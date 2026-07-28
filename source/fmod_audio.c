/* fmod_audio.c -- native replacement for org.fmod.FMODAudioDevice
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * WMW plays all of its audio through FMOD Ex (libfmodex.so). On Android, FMOD
 * Ex's output backend is *half Java*: the native library exposes two JNI entry
 * points and a Java thread in org.fmod.FMODAudioDevice drives them --
 *
 *     private native int fmodGetInfo(int info);
 *     private native int fmodProcess(ByteBuffer buffer);
 *
 * ...allocating a direct ByteBuffer, calling fmodProcess() to have the native
 * mixer fill it with PCM, and writing the result to an AudioTrack.
 *
 * That is very good news for this port: FMOD does not have to be reimplemented
 * or replaced, and no FMOD Switch licence is involved. We simply write that
 * Java thread in C. This file is the whole audio backend.
 *
 * The contract is exact, not inferred. classes.dex gives the constants:
 *
 *     FMOD_INFO_SAMPLERATE      = 0
 *     FMOD_INFO_DSPBUFFERLENGTH = 1
 *     FMOD_INFO_DSPNUMBUFFERS   = 2
 *     FMOD_INFO_MIXERRUNNING    = 3
 *     NUMCHANNELS               = 2      (a constant -- never queried)
 *
 * ...and run() gives the algorithm:
 *
 *     while (!initialised) {
 *         rate = fmodGetInfo(SAMPLERATE);
 *         if (rate <= 0) { sleep(100); continue; }
 *         dspLen  = fmodGetInfo(DSPBUFFERLENGTH);
 *         dspBufs = fmodGetInfo(DSPNUMBUFFERS);
 *         trackBytes  = max(dspBufs * dspLen * 2 * NUMCHANNELS,
 *                           AudioTrack.getMinBufferSize(rate, STEREO, PCM16));
 *         bufferBytes = dspLen * 2 * NUMCHANNELS;
 *         ...AudioTrack(STREAM_MUSIC, rate, STEREO, PCM16, trackBytes, STREAM)
 *     }
 *     while (running)
 *         if (fmodGetInfo(MIXERRUNNING) == 1) {
 *             fmodProcess(buffer);
 *             track.write(buffer, 0, buffer.capacity());
 *         }
 *
 * Two details matter and are easy to get wrong:
 *   - the staging buffer must be exactly dspLen * 2 * 2 bytes, because
 *     fmodProcess fills the buffer's full capacity;
 *   - fmodProcess must only be called while MIXERRUNNING reports 1.
 *
 * fmodProcess reaches the destination through env->GetDirectBufferAddress()
 * (JNIEnv slot 0x730/8 = 230, confirmed by disassembly), so jni_fake.c's fake
 * direct buffer is all the plumbing required.
 *
 * The one thing Android did for free and we must do ourselves is rate
 * conversion: libnx's audout is fixed at 48 kHz stereo PCM16, whereas FMOD
 * hands us whatever rate the game asked System::init for. See resample() below.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <switch.h>

#include "fmod_audio.h"
#include "so_util.h"
#include "jni_fake.h"
#include "util.h"
#include "config.h"

// classes.dex: org.fmod.FMODAudioDevice
#define FMOD_INFO_SAMPLERATE       0
#define FMOD_INFO_DSPBUFFERLENGTH  1
#define FMOD_INFO_DSPNUMBUFFERS    2
#define FMOD_INFO_MIXERRUNNING     3
#define FMOD_NUMCHANNELS           2

#define OUT_RATE      48000   // libnx audout is fixed at 48 kHz stereo PCM16
#define OUT_CHANNELS  2
#define NUM_OUT_BUFS  3

typedef int32_t (*fn_fmod_getinfo)(void *env, void *thiz, int32_t info);
typedef int32_t (*fn_fmod_process)(void *env, void *thiz, void *bytebuffer);

static fn_fmod_getinfo s_getinfo;
static fn_fmod_process s_process;

static Thread s_thread;
static volatile bool s_running;
static volatile bool s_paused;

static void *s_thiz;      // fake FMODAudioDevice instance
static void *s_bytebuf;   // fake direct ByteBuffer over s_pcm
static int16_t *s_pcm;    // FMOD's staging buffer, dsp_len frames

static AudioOutBuffer s_abuf[NUM_OUT_BUFS];
static void *s_abuf_mem[NUM_OUT_BUFS];

static int s_rate, s_dsp_len, s_dsp_bufs;
static int s_out_frames;  // frames per audout buffer after rate conversion

// ---------------------------------------------------------------------------
// rate conversion
// ---------------------------------------------------------------------------
//
// Linear interpolation, stereo, driven by a fractional phase accumulator that
// persists across calls so no click appears at buffer boundaries. FMOD is
// almost certainly already mixing at a modest rate for a 2011 mobile title, so
// this is an upsample; linear is more than adequate for the material and costs
// a few thousand cycles per buffer.
//
// If s_rate == OUT_RATE this is bypassed entirely.

static uint64_t s_phase;      // 32.32 fixed point, index into the source buffer
static int16_t  s_prev[2];    // last source frame, for interpolation across calls

static int resample(const int16_t *src, int src_frames, int16_t *dst, int dst_cap) {
  const uint64_t step = ((uint64_t)s_rate << 32) / OUT_RATE;
  int out = 0;
  while (out < dst_cap) {
    const uint64_t idx = s_phase >> 32;
    if ((int)idx >= src_frames) break;

    const uint32_t frac = (uint32_t)(s_phase & 0xffffffffu);
    const int32_t  w1 = (int32_t)(frac >> 17);       // 0..32767
    const int32_t  w0 = 32768 - w1;

    for (int ch = 0; ch < 2; ch++) {
      const int32_t a = (idx == 0) ? s_prev[ch] : src[(idx - 1) * 2 + ch];
      const int32_t b = src[idx * 2 + ch];
      dst[out * 2 + ch] = (int16_t)((a * w0 + b * w1) >> 15);
    }
    out++;
    s_phase += step;
  }
  s_prev[0] = src[(src_frames - 1) * 2];
  s_prev[1] = src[(src_frames - 1) * 2 + 1];
  s_phase -= (uint64_t)src_frames << 32;
  return out;
}

// ---------------------------------------------------------------------------
// pump thread
// ---------------------------------------------------------------------------

static int wait_for_device(void) {
  // FMOD::System::init() has not necessarily run yet; fmodGetInfo returns -1
  // until the output device exists. This is the same 100 ms poll the Java
  // thread used.
  while (s_running) {
    const int32_t rate = s_getinfo(fake_env, s_thiz, FMOD_INFO_SAMPLERATE);
    if (rate > 0) {
      s_rate = rate;
      return 1;
    }
    svcSleepThread(100ull * 1000 * 1000);
  }
  return 0;
}

static void audio_thread(void *arg) {
  (void)arg;

  // The engine's stack-protector canary lives at tpidr_el0+0x28 and FMOD code
  // runs on this thread, so the guard must be installed here too.
  tls_setup_guard();

  if (!wait_for_device()) return;

  s_dsp_len  = s_getinfo(fake_env, s_thiz, FMOD_INFO_DSPBUFFERLENGTH);
  s_dsp_bufs = s_getinfo(fake_env, s_thiz, FMOD_INFO_DSPNUMBUFFERS);
  if (s_dsp_len <= 0)  s_dsp_len  = 1024; // FMOD's own Android fallback
  if (s_dsp_bufs <= 0) s_dsp_bufs = 4;

  debugPrintf("fmod: rate=%d dspBufferLength=%d dspNumBuffers=%d channels=%d\n",
              s_rate, s_dsp_len, s_dsp_bufs, FMOD_NUMCHANNELS);

  // Exactly the Java allocation: dspBufferLength * 2 bytes * NUMCHANNELS.
  const size_t pcm_bytes = (size_t)s_dsp_len * 2 * FMOD_NUMCHANNELS;
  s_pcm = memalign(0x1000, (pcm_bytes + 0xfff) & ~0xfff);
  if (!s_pcm) { debugPrintf("fmod: pcm alloc failed\n"); return; }
  memset(s_pcm, 0, pcm_bytes);
  s_bytebuf = jni_make_direct_buffer(s_pcm, pcm_bytes);

  // The fake buffer wraps memory this module owns and frees itself. Left as a
  // plain local ref, a PopLocalFrame would call free_ref() on it -- which frees
  // the WRAPPED allocation as well -- and fmod_audio_stop() would then free it a
  // second time. Pin it; the pump owns its lifetime.
  jni_pin(s_bytebuf);

  // Worst case one source frame maps to (OUT_RATE / s_rate) output frames.
  s_out_frames = (int)(((int64_t)s_dsp_len * OUT_RATE) / s_rate) + 2;
  const size_t out_bytes = (size_t)s_out_frames * OUT_CHANNELS * sizeof(int16_t);
  const size_t out_aligned = (out_bytes + 0xfff) & ~0xfff;

  if (s_rate != OUT_RATE)
    debugPrintf("fmod: resampling %d -> %d Hz (%d -> %d frames/buffer)\n",
                s_rate, OUT_RATE, s_dsp_len, s_out_frames);

  for (int i = 0; i < NUM_OUT_BUFS; i++) {
    s_abuf_mem[i] = memalign(0x1000, out_aligned);
    if (!s_abuf_mem[i]) { debugPrintf("fmod: out buffer alloc failed\n"); return; }
    memset(s_abuf_mem[i], 0, out_aligned);
    s_abuf[i].next = NULL;
    s_abuf[i].buffer = s_abuf_mem[i];
    s_abuf[i].buffer_size = out_aligned;
    s_abuf[i].data_size = out_bytes;
    s_abuf[i].data_offset = 0;
    audoutAppendAudioOutBuffer(&s_abuf[i]);
  }

  debugPrintf("fmod: pump started\n");

  while (s_running) {
    AudioOutBuffer *released = NULL;
    uint32_t count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, UINT64_MAX)))
      break;
    if (!released) continue;

    size_t bytes = out_bytes;

    // The Java thread gated every fmodProcess() on this. Calling it while the
    // mixer is down is what produces garbage or a fault.
    const int mixer_up =
        (s_getinfo(fake_env, s_thiz, FMOD_INFO_MIXERRUNNING) == 1);

    if (s_paused || !mixer_up) {
      memset(released->buffer, 0, out_bytes);
      static int quiet_reported;
      if (!s_paused && quiet_reported < 3) {
        quiet_reported++;
        debugPrintf("fmod: mixer not running (getInfo(MIXERRUNNING)=%d) -- silence\n",
                    s_getinfo(fake_env, s_thiz, FMOD_INFO_MIXERRUNNING));
      }
    } else {
      const int32_t got = s_process(fake_env, s_thiz, s_bytebuf);

      // First few passes: report what came back, and whether it is actually
      // non-zero audio. FMOD returning success while filling the buffer with
      // silence looks identical to a broken pump from the outside.
      static int reported;
      if (reported < 8) {
        reported++;
        int32_t peak = 0;
        for (int i = 0; i < s_dsp_len * FMOD_NUMCHANNELS; i++) {
          const int32_t v = s_pcm[i] < 0 ? -s_pcm[i] : s_pcm[i];
          if (v > peak) peak = v;
        }
        debugPrintf("fmod: process -> %d, peak sample %d\n", (int)got, (int)peak);
      }

      if (got <= 0) {
        memset(released->buffer, 0, out_bytes);
      } else if (s_rate == OUT_RATE) {
        memcpy(released->buffer, s_pcm, pcm_bytes);
        bytes = pcm_bytes;
      } else {
        const int frames = resample(s_pcm, s_dsp_len,
                                    (int16_t *)released->buffer, s_out_frames);
        // Submit exactly what was produced. Padding to a fixed size instead
        // leaves a few zero samples at the end of every buffer, which is an
        // audible tick at ~47 buffers a second.
        bytes = (size_t)frames * OUT_CHANNELS * sizeof(int16_t);
        if (bytes == 0) { memset(released->buffer, 0, out_bytes); bytes = out_bytes; }
      }
    }

    released->data_size = bytes;
    released->data_offset = 0;
    audoutAppendAudioOutBuffer(released);
  }

  debugPrintf("fmod: pump stopped\n");
}

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------

int fmod_audio_start(uintptr_t fn_getinfo, uintptr_t fn_process) {
  s_getinfo = (fn_fmod_getinfo)fn_getinfo;
  s_process = (fn_fmod_process)fn_process;

  if (!s_getinfo || !s_process) {
    debugPrintf("fmod: audio entry points missing (getInfo=%p process=%p)\n",
                s_getinfo, s_process);
    return -1;
  }

  if (R_FAILED(audoutInitialize())) { debugPrintf("fmod: audoutInitialize failed\n"); return -1; }
  if (R_FAILED(audoutStartAudioOut())) { debugPrintf("fmod: audoutStartAudioOut failed\n"); return -1; }

  s_thiz = jni_make_object("org/fmod/FMODAudioDevice");
  jni_pin(s_thiz);   // held for the life of the pump thread
  s_running = true;
  s_paused = false;

  // Java set this thread to Thread.MAX_PRIORITY (10). Keep it just above the
  // main thread's default 0x2c so the mixer never starves behind a long frame,
  // and put it on core 1 to stay off the render core.
  if (R_FAILED(threadCreate(&s_thread, audio_thread, NULL, NULL, 64 * 1024, 0x2b, 1)) ||
      R_FAILED(threadStart(&s_thread))) {
    debugPrintf("fmod: could not start pump thread\n");
    s_running = false;
    return -1;
  }
  return 0;
}

void fmod_audio_set_paused(int paused) { s_paused = paused ? true : false; }

void fmod_audio_stop(void) {
  if (!s_running) return;
  s_running = false;
  audoutStopAudioOut();
  threadWaitForExit(&s_thread);
  threadClose(&s_thread);
  audoutExit();
  for (int i = 0; i < NUM_OUT_BUFS; i++) free(s_abuf_mem[i]);
  free(s_pcm);
}
