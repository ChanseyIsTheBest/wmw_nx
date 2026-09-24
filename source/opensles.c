/* opensles.c -- minimal OpenSL ES shim backed by SDL2 audio
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 that FMOD Ex's "OpenSL ES Output"
 * uses: the Object interface (Realize/GetInterface/Destroy), the Engine
 * interface (CreateOutputMix/CreateAudioPlayer), and on each player the Play,
 * Volume, AndroidConfiguration and AndroidSimpleBufferQueue interfaces.
 * Players are software-mixed into one SDL2 audio device.
 *
 * Buffer-queue semantics -- read this before changing the mixer.
 *
 * An OpenSL buffer queue holds POINTERS. Enqueue() does not copy: the audio
 * system reads the buffer when that buffer reaches the head of the queue and
 * is actually played, then fires the completion callback. FMOD Ex depends on
 * this. From libfmodex.so's fmod_output_opensl.cpp (disassembled):
 *
 *   - init allocates a ring of dspNumBuffers blocks (default 4 x 512 frames at
 *     24 kHz, i.e. 21.3 ms each) and enqueues every block up front;
 *   - the completion callback does no mixing at all. It re-enqueues the block
 *     that just finished and advances an offset P;
 *   - getposition() reports P, and FMOD's polled mixer thread (10 ms period)
 *     mixes fresh audio into the blocks BEHIND P -- blocks that are already
 *     sitting in the queue, waiting to be played.
 *
 * So FMOD writes into a block after enqueuing it and relies on it not being
 * read until its turn comes. A shim that copies at Enqueue() time captures the
 * block's previous contents instead, and every sound comes out one full ring
 * (~85 ms) late. The previous version of this file did exactly that, and then
 * parked the copies behind a ~330 ms decoupling ring fed by a pump thread
 * (a design inherited from a BASS port, where the callback does the mixing).
 * Together with a 2048-frame SDL period that added up to the ~0.5 s delay
 * between tapping a button and hearing its click.
 *
 * Now the SDL audio callback pulls straight from the queued buffers, the way
 * AudioFlinger does, and fires the completion callback as each buffer is
 * finished. The end-to-end latency is FMOD's own ring (as on Android) plus
 * the SDL device period (WMW_AUDIO_SAMPLES).
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "opensles.h"
#include "config.h"
#include "util.h"

// Frames per SDL audio callback at 48 kHz. The Switch SDL backend double-
// buffers at this size, so it is also most of the output latency: 1024 frames
// is 21.3 ms per buffer. See config.h to override.
#ifndef WMW_AUDIO_SAMPLES
#define WMW_AUDIO_SAMPLES 1024
#endif

// --- OpenSL ES constants ----------------------------------------------------

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C
#define SL_RESULT_BUFFER_INSUFFICIENT  0x07

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

// callback: (SLAndroidSimpleBufferQueueItf caller, void *pContext)
typedef void (*slBufferQueueCallback)(void *caller, void *context);

// --- interface-id sentinels -------------------------------------------------

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

// --- vtable structs (method order matches the OpenSL ES 1.0.1 spec) ---------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

// only CreateAudioPlayer (slot 2) and CreateOutputMix (slot 7) are used; the
// rest keep the correct layout but are generic so a shared stub assigns
// cleanly. The engine calls each slot with its own typed vtable.
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

// --- objects ----------------------------------------------------------------

#define MAX_PLAYERS 32
#define BQ_SLOTS 128   // queue depth limit; FMOD uses dspNumBuffers (4)

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;

  int in_use;
  int channels;
  int rate;
  int playing;
  int bytes_per_sample; // 1/2/4, per the source PCM format
  int is_float;         // PCM_EX float representation
  int frame_bytes;      // bytes_per_sample * channels
  float gain; // linear, from SetVolumeLevel (millibels)
  int locator_buffers;  // numBuffers the creator declared (diagnostics only)

  slBufferQueueCallback cb;
  void *cb_ctx;

  // The buffer queue proper: pointers only, read at play time (see the top of
  // this file). q[q_head] is the buffer being played; cur_pos is how far into
  // it the mixer has read.
  BQBuffer q[BQ_SLOTS];
  int q_head, q_count;
  SLuint32 cur_pos;
  SLuint32 played;      // buffers completed, for GetState's 'index'

  // Streaming linear resampler from p->rate to the device rate. rs_phase is
  // 32.32 fixed point: the position between rs_prev and rs_next. It and the
  // two frames persist across callbacks and across buffer boundaries, so the
  // interpolation never restarts and never clicks at a boundary.
  uint64_t rs_phase;
  int32_t  rs_prev[2];
  int32_t  rs_next[2];

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

// --- global SDL device + player registry ------------------------------------

static SDL_AudioDeviceID g_dev = 0;
static int g_dev_rate = 44100;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

// diagnostics (audio thread + FMOD threads; exact atomicity not needed)
static volatile long g_enq_count = 0;      // total bq_Enqueue calls
static volatile long g_cb_count = 0;       // total completion callbacks fired
static volatile long g_underrun_count = 0; // callbacks where a playing player ran dry
static volatile long g_enq_rejected = 0;   // Enqueue calls refused (queue full / bad args)
static volatile int  g_q_min = 1 << 30;    // min queued buffers seen since last heartbeat

#define MOVIE_RING_FRAMES 65536
static SDL_mutex *g_movie_lock = NULL;
static int16_t *g_movie_pcm = NULL;
static int g_movie_active = 0;
static int g_movie_paused = 0;
static int g_movie_head = 0;
static int g_movie_count = 0;
static uint64_t g_movie_samples_queued = 0;
static uint64_t g_movie_samples_played = 0;
static int g_movie_opened_dev = 0;

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// Read one source sample and return it scaled to the S16 range, regardless of
// the source PCM width/representation. BASS on Android frequently outputs 32-bit
// float (SL_ANDROID_DATAFORMAT_PCM_EX); plain SL_DATAFORMAT_PCM gives 8/16/32-bit
// signed ints. memcpy avoids any alignment assumptions on the queued buffer.
static inline int32_t read_sample(const uint8_t *src, int bps, int is_float) {
  if (is_float) {
    float f;
    memcpy(&f, src, sizeof(f));
    float v = f * 32768.0f;
    if (v > 32767.0f) v = 32767.0f;
    else if (v < -32768.0f) v = -32768.0f;
    return (int32_t)v;
  }
  if (bps == 2) { int16_t v; memcpy(&v, src, 2); return v; }
  if (bps == 4) { int32_t v; memcpy(&v, src, 4); return v >> 16; }   // 32-bit int -> S16
  if (bps == 1) { return ((int32_t)src[0] - 128) << 8; }             // unsigned 8-bit -> S16
  return 0;
}


// Retire the buffer at the head of the queue and fire the completion callback,
// exactly as OpenSL does when a buffer finishes playing. Called with p->lock
// held; the lock is dropped around the callback because FMOD's callback calls
// straight back into bq_Enqueue() to re-queue the block.
//
// FMOD's callback is a handful of instructions (re-enqueue, advance an offset)
// so running it on the SDL audio thread is safe -- it is also where Android
// runs it.
static void retire_head(Player *p) {
  p->q_head = (p->q_head + 1) % BQ_SLOTS;
  p->q_count--;
  p->cur_pos = 0;
  p->played++;

  slBufferQueueCallback cb = p->cb;
  void *ctx = p->cb_ctx;
  if (cb) {
    g_cb_count++;
    SDL_UnlockMutex(p->lock);
    cb(&p->bq_vt, ctx);
    SDL_LockMutex(p->lock);
  }
}

// Read the next source frame from the queue as S16 stereo. Returns 0 when there
// is nothing to read (queue empty, or the player was stopped while the lock was
// dropped for a callback). Called with p->lock held.
static int bq_pull_frame(Player *p, int32_t out[2]) {
  for (;;) {
    // Re-checked every time round: retire_head() drops the lock, and FMOD's
    // stop path is SetPlayState(STOPPED) -> Clear -> free its ring. Once we see
    // 'stopped' under the lock, the queued pointers must not be touched.
    if (!p->playing || p->q_count <= 0)
      return 0;

    const BQBuffer *b = &p->q[p->q_head];
    const SLuint32 fb = (SLuint32)p->frame_bytes;
    if (p->cur_pos + fb <= b->size) {
      const uint8_t *s = (const uint8_t *)b->data + p->cur_pos;
      const int bps = p->bytes_per_sample;
      int32_t l = read_sample(s, bps, p->is_float);
      int32_t r = (p->channels >= 2) ? read_sample(s + bps, bps, p->is_float) : l;
      out[0] = l;
      out[1] = r;
      p->cur_pos += fb;
      // Last whole frame of this buffer consumed: it has "finished playing".
      if (p->cur_pos + fb > b->size)
        retire_head(p);
      return 1;
    }
    // Empty buffer or a sub-frame tail -- nothing playable, just retire it.
    retire_head(p);
  }
}

// Mix one player into the S16 stereo accumulator, resampling from the player's
// rate to the device rate. Pulls directly from the queued buffers.
static void mix_player(Player *p, int32_t *acc, int frames) {
  SDL_LockMutex(p->lock);
  if (!p->playing) {
    SDL_UnlockMutex(p->lock);
    return;
  }

  if (p->q_count < g_q_min) g_q_min = p->q_count;

  const uint64_t ONE = 1ULL << 32;
  const uint64_t step = ((uint64_t)p->rate << 32) / (uint64_t)g_dev_rate;
  const float g = p->gain;

  for (int i = 0; i < frames; i++) {
    // Slide the interpolation window forward one source frame for every whole
    // source frame the phase has passed.
    while (p->rs_phase >= ONE) {
      int32_t f[2];
      if (!bq_pull_frame(p, f)) {
        // Ran dry. Leave rs_phase >= ONE so the next callback resumes by
        // pulling, and leave the rest of this block silent.
        if (p->playing) g_underrun_count++;
        SDL_UnlockMutex(p->lock);
        return;
      }
      p->rs_prev[0] = p->rs_next[0];
      p->rs_prev[1] = p->rs_next[1];
      p->rs_next[0] = f[0];
      p->rs_next[1] = f[1];
      p->rs_phase -= ONE;
    }

    const int32_t w1 = (int32_t)((uint32_t)p->rs_phase >> 17); // 0..32767
    const int32_t w0 = 32768 - w1;
    const int32_t sl = (p->rs_prev[0] * w0 + p->rs_next[0] * w1) >> 15;
    const int32_t sr = (p->rs_prev[1] * w0 + p->rs_next[1] * w1) >> 15;
    acc[i * 2 + 0] += (int32_t)(sl * g);
    acc[i * 2 + 1] += (int32_t)(sr * g);
    p->rs_phase += step;
  }

  SDL_UnlockMutex(p->lock);
}

static void mix_movie(int32_t *acc, int frames) {
  if (!g_movie_lock)
    return;

  SDL_LockMutex(g_movie_lock);
  if (!g_movie_active || g_movie_paused || !g_movie_pcm) {
    SDL_UnlockMutex(g_movie_lock);
    return;
  }

  const int n = g_movie_count < frames ? g_movie_count : frames;
  for (int i = 0; i < n; i++) {
    const int idx = (g_movie_head + i) % MOVIE_RING_FRAMES;
    acc[i * 2 + 0] += g_movie_pcm[idx * 2 + 0];
    acc[i * 2 + 1] += g_movie_pcm[idx * 2 + 1];
  }
  g_movie_head = (g_movie_head + n) % MOVIE_RING_FRAMES;
  g_movie_count -= n;
  g_movie_samples_played += (uint64_t)n;
  SDL_UnlockMutex(g_movie_lock);
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;
  static int first = 1;
  if (first) { debugPrintf("opensles: SDL audio callback is running\n"); first = 0; }
  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  // heartbeat: every ~2 s of audio, report the pipeline counters so a stall is
  // visible in the log. 'qmin' is the fewest buffers FMOD had queued at the
  // start of a callback -- it should sit at dspNumBuffers-1 or so; 0 plus a
  // rising underrun count means FMOD's mixer is not keeping up.
  static long hb_frames = 0;
  hb_frames += frames;
  if (hb_frames >= (long)g_dev_rate * 2) {
    hb_frames = 0;
    debugPrintf("opensles: hb enq=%ld cb=%ld underrun=%ld rejected=%ld qmin=%d players=%d\n",
                g_enq_count, g_cb_count, g_underrun_count, g_enq_rejected,
                (g_q_min == (1 << 30) ? -1 : g_q_min), g_player_count);
    g_q_min = 1 << 30;
  }

  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] && g_players[i]->in_use)
      mix_player(g_players[i], acc, frames);
  SDL_UnlockMutex(g_reg_lock);

  mix_movie(acc, frames);

  int16_t *out = (int16_t *)stream;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
  }
}

// Set as soon as the game's audio library creates an OpenSL engine. FMOD Ex
// in this build ships only two outputs -- "FMOD NoSound Output" and "FMOD
// OpenSL ES Output" -- so OpenSL is the real one and the org.fmod.FMODAudioDevice
// entry points are vestigial. main.c checks this before starting the
// FMODAudioDevice pump, which would otherwise contend for the audio device.
static int g_engine_created;

int opensles_in_use(void) { return g_engine_created; }

static void ensure_device(int rate) {
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (g_dev)
    return;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    debugPrintf("opensles: SDL audio init failed: %s\n", SDL_GetError());
    return;
  }
  // Always open at the Switch's native output rate.
  //
  // The obvious thing is to ask SDL for the player's rate and let it convert.
  // That works, but it puts SDL's internal resampler in the path for every
  // sample, and on this platform that is where the audio broke up: FMOD mixes
  // at 24000 Hz, the hardware is fixed at 48000, and the conversion produced
  // audible repeats and glitching.
  //
  // Opening at the hardware rate means SDL converts nothing, and mix_player()
  // does the conversion itself with a per-player phase accumulator that
  // persists across callbacks and buffer boundaries.
  const int dev_rate = 48000;
  (void)rate;

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = dev_rate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = WMW_AUDIO_SAMPLES;
  want.callback = audio_callback;

  // allowed_changes = 0: we want exactly this spec. Anything SDL had to adapt
  // would go through an SDL_AudioStream, which adds both a resampler and a
  // buffer -- latency this file is trying to keep out.
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

  if (!g_dev) {
    // Fall back to letting SDL pick a rate rather than ending up silent.
    // mix_player() resamples to whatever g_dev_rate ends up being.
    debugPrintf("opensles: strict open failed (%s); retrying with rate changes allowed\n",
                SDL_GetError());
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  }

  if (!g_dev) {
    debugPrintf("opensles: SDL_OpenAudioDevice failed: %s -- NO AUDIO\n", SDL_GetError());
    return;
  }

  g_dev_rate = have.freq;
  SDL_PauseAudioDevice(g_dev, 0);
  debugPrintf("opensles: audio device opened -- %d Hz, %d ch, %d samples (%.1f ms/period, double-buffered)\n",
              have.freq, have.channels, have.samples,
              have.freq ? have.samples * 1000.0 / have.freq : 0.0);
  if (have.channels != 2)
    debugPrintf("opensles: WARNING got %d channels, expected 2\n", have.channels);
}

int opensles_movie_begin(int requested_rate) {
  if (!g_movie_lock)
    g_movie_lock = SDL_CreateMutex();
  if (!g_movie_pcm)
    g_movie_pcm = calloc(MOVIE_RING_FRAMES * 2, sizeof(int16_t));
  if (!g_movie_lock || !g_movie_pcm)
    return 0;

  const int had_dev = (g_dev != 0);
  ensure_device(requested_rate > 0 ? requested_rate : 44100);
  if (!g_dev)
    return 0;
  g_movie_opened_dev = !had_dev;

  SDL_LockMutex(g_movie_lock);
  g_movie_active = 1;
  g_movie_paused = 1;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_samples_queued = 0;
  g_movie_samples_played = 0;
  SDL_UnlockMutex(g_movie_lock);
  return g_dev_rate;
}

int opensles_movie_queue(const int16_t *pcm, int frames) {
  int done = 0;
  while (done < frames) {
    if (!g_movie_lock)
      return done;

    SDL_LockMutex(g_movie_lock);
    if (!g_movie_active || !g_movie_pcm) {
      SDL_UnlockMutex(g_movie_lock);
      return done;
    }

    const int space = MOVIE_RING_FRAMES - g_movie_count;
    int n = frames - done;
    if (n > space)
      n = space;
    for (int i = 0; i < n; i++) {
      const int idx = (g_movie_head + g_movie_count + i) % MOVIE_RING_FRAMES;
      g_movie_pcm[idx * 2 + 0] = pcm[(done + i) * 2 + 0];
      g_movie_pcm[idx * 2 + 1] = pcm[(done + i) * 2 + 1];
    }
    g_movie_count += n;
    g_movie_samples_queued += (uint64_t)n;
    SDL_UnlockMutex(g_movie_lock);

    done += n;
    if (done < frames)
      SDL_Delay(2);
  }
  return done;
}

void opensles_movie_set_paused(int paused) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  if (g_movie_active)
    g_movie_paused = paused != 0;
  SDL_UnlockMutex(g_movie_lock);
}

uint64_t opensles_movie_samples_queued(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_queued;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

uint64_t opensles_movie_samples_played(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_played;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

int opensles_movie_buffered_frames(void) {
  int ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_count;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

void opensles_movie_end(void) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  g_movie_active = 0;
  g_movie_paused = 0;
  g_movie_head = 0;
  g_movie_count = 0;
  SDL_UnlockMutex(g_movie_lock);

  // Release the device the movie opened so the game re-opens it at its own rate.
  // Outside g_movie_lock: SDL_CloseAudioDevice waits on the audio callback, which
  // itself takes g_movie_lock.
  if (g_movie_opened_dev) {
    g_movie_opened_dev = 0;
    if (g_dev) {
      SDL_CloseAudioDevice(g_dev);
      g_dev = 0;
    }
  }
}

// --- buffer queue interface -------------------------------------------------
//
// Enqueue stores the POINTER. The buffer is read by mix_player() when it
// reaches the head of the queue -- see the top of this file for why copying
// here is wrong for FMOD.

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (!pBuffer || size == 0) {
    g_enq_rejected++;
    return SL_RESULT_PARAMETER_INVALID;
  }

  SDL_LockMutex(p->lock);
  // Android caps the queue at the numBuffers the player was created with.
  const int cap = (p->locator_buffers > 0 && p->locator_buffers < BQ_SLOTS)
                    ? p->locator_buffers : BQ_SLOTS;
  if (p->q_count >= cap) {
    SDL_UnlockMutex(p->lock);
    g_enq_rejected++;
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  const int tail = (p->q_head + p->q_count) % BQ_SLOTS;
  p->q[tail].data = pBuffer;
  p->q[tail].size = size;
  p->q_count++;
  g_enq_count++;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_count = 0;
  p->cur_pos = 0;
  // Restart the resampler so nothing from before the Clear bleeds through.
  p->rs_phase = 1ULL << 32;
  p->rs_prev[0] = p->rs_prev[1] = 0;
  p->rs_next[0] = p->rs_next[1] = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (SLuint32)p->q_count;  // buffers still queued
    st->index = p->played;             // buffers completed so far
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->cb = cb;
  p->cb_ctx = ctx;
  SDL_UnlockMutex(p->lock);
  debugPrintf("opensles: bq RegisterCallback cb=%p\n", (void *)cb);
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// --- play interface ---------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  SDL_LockMutex(p->lock);
  p->playing = (state == SL_PLAYSTATE_PLAYING);
  // Report the latency this player's queue implies, once per start: FMOD keeps
  // the queue full, so the newest mixed block is (queued - 1) blocks from the
  // head. The SDL device adds one to two periods on top.
  if (p->playing && p->q_count > 0 && p->frame_bytes > 0 && p->rate > 0) {
    const int blk = (int)(p->q[p->q_head].size / (SLuint32)p->frame_bytes);
    debugPrintf("opensles: SetPlayState PLAYING -- %d x %d-frame buffers @ %d Hz queued "
                "(~%d ms ahead of the mixer) + device %d frames\n",
                p->q_count, blk, p->rate,
                (p->q_count - 1) * blk * 1000 / p->rate, WMW_AUDIO_SAMPLES);
  } else {
    debugPrintf("opensles: SetPlayState %u\n", (unsigned)state);
  }
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pState) *pState = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

// --- volume interface -------------------------------------------------------

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  p->gain = mb_to_linear(level);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (m) p->gain = 0.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

// --- player object ----------------------------------------------------------

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

/* SLAndroidConfigurationItf.
 *
 * FMOD Ex resolves SL_IID_ANDROIDCONFIGURATION (confirmed: it is one of only
 * six OpenSL symbols in libfmodex.so's string table) and uses it to set the
 * stream type and performance mode on the player. None of that means anything
 * here, but returning FEATURE_UNSUPPORTED from GetInterface can make a caller
 * treat player creation as failed. Accept the calls and ignore them.
 */
static SLresult cfg_SetConfiguration(void *self, const char *key,
                                     const void *value, SLuint32 size) {
  (void)self; (void)key; (void)value; (void)size;
  return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const char *key,
                                     SLuint32 *size, void *value) {
  (void)self; (void)key; (void)value;
  if (size) *size = 0;
  return SL_RESULT_SUCCESS;
}
static const struct {
  SLresult (*SetConfiguration)(void *, const char *, const void *, SLuint32);
  SLresult (*GetConfiguration)(void *, const char *, SLuint32 *, void *);
} g_cfg_itf = { cfg_SetConfiguration, cfg_GetConfiguration };
static const void *g_cfg_itf_ptr = &g_cfg_itf;

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(const void **)pInterface = &g_cfg_itf_ptr;
  } else {
    *(void **)pInterface = NULL;
    debugPrintf("opensles: player GetInterface UNSUPPORTED iid=%p\n", iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  // audio_callback() mixes every player while holding g_reg_lock, so once the
  // player is unregistered under that lock the audio thread cannot be inside it
  // (or inside its completion callback) any more.
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  if (p->lock) SDL_DestroyMutex(p->lock);
  free(p);
}

// --- engine interface -------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(*p));
  if (!p)
    return SL_RESULT_PARAMETER_INVALID;
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->channels = 2;
  p->rate = 44100;
  p->bytes_per_sample = 2; // safe default: 16-bit signed
  p->is_float = 0;
  p->lock = SDL_CreateMutex();

  if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    // 2 = SL_DATAFORMAT_PCM, 4 = SL_ANDROID_DATAFORMAT_PCM_EX (adds a trailing
    // 'representation' word: 1=signed int, 2=unsigned int, 3=float).
    if (fmt->formatType == 2 || fmt->formatType == 4) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
      int bits = fmt->bitsPerSample ? (int)fmt->bitsPerSample : 16;
      p->bytes_per_sample = bits / 8;
      if (fmt->formatType == 4) {
        SLuint32 rep = ((const SLuint32 *)fmt)[7]; // representation field
        p->is_float = (rep == 3 /* FLOAT */);
      }
      // float is always 32-bit; guard against odd/zero widths
      if (p->is_float) p->bytes_per_sample = 4;
      if (p->bytes_per_sample < 1) p->bytes_per_sample = 2;
    }
  }
  // The locator is {locatorType, numBuffers} for both SL_DATALOCATOR_BUFFERQUEUE
  // and SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE. FMOD passes dspNumBuffers.
  if (src && src->pLocator) {
    const SLDataLocator_BufferQueue *loc = src->pLocator;
    p->locator_buffers = (int)loc->numBuffers;
  }
  if (p->channels < 1) p->channels = 2;
  p->frame_bytes = p->bytes_per_sample * p->channels;

  // Empty interpolation window: the first output frame pulls a source frame.
  p->rs_phase = 1ULL << 32;

  ensure_device(p->rate);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);
  if (slot < 0)
    debugPrintf("opensles: WARNING player registry full -- this player will be silent\n");

  *pPlayer = &p->obj_vt;
  debugPrintf("opensles: CreateAudioPlayer (%d Hz, %d ch, %d-bit %s, %d buffers)\n",
              p->rate, p->channels, p->bytes_per_sample * 8,
              p->is_float ? "float" : "int", p->locator_buffers);
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_PARAMETER_INVALID;
  m->obj_vt = &mix_obj_vtable;
  if (pMix) *pMix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

// --- entry point ------------------------------------------------------------

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_PARAMETER_INVALID;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  g_engine_created = 1;
  debugPrintf("opensles: slCreateEngine\n");
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  opensles_movie_end();
  free(g_movie_pcm);
  g_movie_pcm = NULL;
  if (g_movie_lock) {
    SDL_DestroyMutex(g_movie_lock);
    g_movie_lock = NULL;
  }
  if (g_dev) {
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
  }
}
