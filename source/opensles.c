/* opensles.c -- minimal OpenSL ES shim backed by SDL2 audio
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 the SQEX "Sd" sound driver uses:
 * the Object interface (Realize/GetInterface/Destroy), the Engine interface
 * (CreateOutputMix/CreateAudioPlayer), and on each player the Play, Volume and
 * AndroidSimpleBufferQueue interfaces. Players are software-mixed into one SDL2
 * audio device; the buffer-queue completion callback is fired from the SDL
 * audio thread, exactly like Android's fast-track callback.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "opensles.h"
#include "util.h"

// --- OpenSL ES constants ----------------------------------------------------

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

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
#define BQ_SLOTS 128

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
  int is_float;         // PCM_EX float representation (BASS often uses this)
  float gain; // linear, from SetVolumeLevel (millibels)

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers (legacy fields kept for GetState/Clear shape)
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail;
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;

  // Decoupling ring: bq_Enqueue converts BASS's PCM to S16 stereo and writes it
  // here; the audio callback drains it. A pump thread keeps it topped up by
  // firing the completion callback, so playback never depends on BASS's timing.
  int16_t *ring;     // interleaved S16 stereo, ring_cap frames
  int ring_cap;      // capacity in frames
  int ring_head;     // read index (frames)
  int ring_count;    // frames currently buffered
  SDL_Thread *pump;
  volatile int alive;

  // Resampling state. The device runs at the Switch's native rate; a player
  // mixing at a different rate is converted here rather than by SDL. See
  // ensure_device() for why.
  uint64_t rs_phase;      // 32.32 fixed point, position within the ring
  int16_t  rs_prev[2];    // last frame consumed, for interpolation across calls

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

// diagnostics (audio thread + BASS callback thread; exact atomicity not needed)
static volatile long g_enq_count = 0;   // total bq_Enqueue calls
static volatile long g_cb_count = 0;    // total completion callbacks fired
static volatile long g_underrun_count = 0; // times a player ran dry
static volatile int  g_ring_last = 0;   // ring fill (frames) at last drain
static volatile int  g_ring_min = 1 << 30; // min ring fill since last heartbeat
static volatile int  g_enq_peak = 0;     // max |sample| enqueued since last heartbeat
static volatile long g_enq_silent = 0;   // cumulative near-silent frames BASS fed us

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

// Mix one player's buffered audio into the S16 stereo accumulator. This only
// drains the ring; it never calls back into BASS (the pump thread does that),
// so the audio callback stays short and never blocks on BASS's mixing.
static void mix_player(Player *p, int32_t *acc, int frames) {
  if (!p->playing || !p->ring)
    return;

  const float g = p->gain;
  SDL_LockMutex(p->lock);

  const int avail = p->ring_count;
  g_ring_last = avail;
  if (avail < g_ring_min) g_ring_min = avail;

  if (p->rate == g_dev_rate) {
    // Rates agree: straight copy.
    const int n = p->ring_count < frames ? p->ring_count : frames;
    for (int i = 0; i < n; i++) {
      const int rd = (p->ring_head + i) % p->ring_cap;
      acc[i * 2 + 0] += (int32_t)(p->ring[rd * 2 + 0] * g);
      acc[i * 2 + 1] += (int32_t)(p->ring[rd * 2 + 1] * g);
    }
    p->ring_head = (p->ring_head + n) % p->ring_cap;
    p->ring_count -= n;
    if (n < frames) g_underrun_count++;
    SDL_UnlockMutex(p->lock);
    return;
  }

  /* Linear resample from p->rate to the device rate.
   *
   * rs_phase is a 32.32 fixed-point read position measured in SOURCE frames
   * from the current ring head, and it persists across callbacks. That
   * continuity is the whole point: a resampler that restarts at phase 0 every
   * callback drops or repeats a fraction of a frame at each boundary, which at
   * ~12 callbacks a second is precisely the repeating, glitchy artefact this
   * replaces. rs_prev carries the last source frame across the same boundary so
   * interpolation has something to work from.
   */
  const uint64_t step = ((uint64_t)p->rate << 32) / (uint64_t)g_dev_rate;
  int produced = 0;

  for (; produced < frames; produced++) {
    const uint64_t idx = p->rs_phase >> 32;
    if ((int)idx >= p->ring_count)
      break;                                  // ran out of source material

    const uint32_t frac = (uint32_t)(p->rs_phase & 0xffffffffu);
    const int32_t w1 = (int32_t)(frac >> 17);  // 0..32767
    const int32_t w0 = 32768 - w1;

    const int rd = (p->ring_head + (int)idx) % p->ring_cap;
    for (int ch = 0; ch < 2; ch++) {
      const int32_t b = p->ring[rd * 2 + ch];
      const int32_t a = (idx == 0)
                          ? p->rs_prev[ch]
                          : p->ring[(((p->ring_head + (int)idx - 1) % p->ring_cap) * 2) + ch];
      const int32_t s = (a * w0 + b * w1) >> 15;
      acc[produced * 2 + ch] += (int32_t)(s * g);
    }
    p->rs_phase += step;
  }

  // Retire the source frames fully consumed, and keep the fractional remainder
  // in rs_phase so the next callback resumes mid-sample rather than snapping.
  const int consumed = (int)(p->rs_phase >> 32);
  if (consumed > 0) {
    const int last = (p->ring_head + consumed - 1) % p->ring_cap;
    p->rs_prev[0] = p->ring[last * 2 + 0];
    p->rs_prev[1] = p->ring[last * 2 + 1];
    p->ring_head = (p->ring_head + consumed) % p->ring_cap;
    p->ring_count -= consumed;
    p->rs_phase &= 0xffffffffULL;             // keep only the fraction
  }

  if (produced < frames) g_underrun_count++;
  SDL_UnlockMutex(p->lock);
}

// Pump thread: keep the ring topped up to ~330ms by prompting BASS to render and
// enqueue. Crucially, when a prompt produces nothing (BASS's stream decoder is
// behind or blocked), it must YIELD the CPU generously rather than spin -- a busy
// pump starves BASS's own update/decode thread and turns a brief gap into a long
// stall (music drops out while pre-decoded SFX keep playing). Runs off the audio
// thread so BASS's mixing can't blow the audio deadline.
static int pump_thread(void *arg) {
  Player *p = (Player *)arg;
  const int target = p->rate / 3; // ~330 ms of headroom to ride out the mixer's bursts
  int misses = 0;
  while (p->alive) {
    if (!p->playing || !p->cb) { SDL_Delay(5); misses = 0; continue; }

    int fill;
    SDL_LockMutex(p->lock);
    fill = p->ring_count;
    SDL_UnlockMutex(p->lock);

    if (fill >= target) { SDL_Delay(3); misses = 0; continue; }

    g_cb_count++;
    p->cb(&p->bq_vt, p->cb_ctx); // -> BASS renders + bq_Enqueue -> ring grows

    int fill2;
    SDL_LockMutex(p->lock);
    fill2 = p->ring_count;
    SDL_UnlockMutex(p->lock);

    if (fill2 > fill) {
      // Made progress; keep filling with only a tiny yield.
      misses = 0;
      SDL_Delay(1);
    } else {
      // No data produced: hand the CPU to BASS's decode thread, escalating the
      // back-off if it stays stalled so we never busy-wait on an empty stream.
      misses++;
      SDL_Delay(misses < 3 ? 8 : 20);
    }
  }
  return 0;
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

  // heartbeat: every ~2s of callbacks, report the pipeline counters so a stall
  // is visible in the log (does the enqueue/callback chain keep moving?).
  static int hb = 0;
  if (++hb >= 48) { // 48 * 2048 / 48000 ~= 2.05s
    hb = 0;
    debugPrintf("opensles: hb enq=%ld cb=%ld underrun=%ld ring=%d min=%d peak=%d silent=%ld players=%d\n",
                g_enq_count, g_cb_count, g_underrun_count,
                g_ring_last, (g_ring_min == (1 << 30) ? -1 : g_ring_min),
                g_enq_peak, g_enq_silent, g_player_count);
    g_ring_min = 1 << 30;
    g_enq_peak = 0;
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
  // audible repeats and glitching even though the pipeline never underran
  // (underrun=0, ring fill stable at ~340 ms).
  //
  // Opening at the hardware rate means SDL converts nothing, and mix_player()
  // does the conversion itself with a per-player phase accumulator that
  // persists across callbacks -- so there is no discontinuity at buffer
  // boundaries, which is exactly what a per-callback resampler gets wrong.
  const int dev_rate = 48000;
  (void)rate;

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = dev_rate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 2048;
  want.callback = audio_callback;

  // allowed_changes = 0 asks SDL to guarantee the format we requested and do
  // any conversion itself. That matters here: FMOD mixes at 24000 Hz and the
  // Switch's audio output is fixed at 48000, so something must resample. SDL
  // doing it means the mixer below never has to.
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

  if (!g_dev) {
    // Fall back to letting SDL pick a rate rather than ending up silent. The
    // mixer has no resampler, so this will play at the wrong pitch -- but a
    // wrong-pitch game is diagnosable and a silent one is not.
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
  debugPrintf("opensles: audio device opened -- %d Hz, %d ch, %d samples (players resampled to this)\n",
              have.freq, have.channels, have.samples);
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

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  const uint8_t *src = (const uint8_t *)pBuffer;
  const int bps = p->bytes_per_sample > 0 ? p->bytes_per_sample : 2;
  const int stereo = (p->channels >= 2);
  const int frame_bytes = bps * (stereo ? 2 : 1);
  const int frames = frame_bytes ? (int)(size / frame_bytes) : 0;

  SDL_LockMutex(p->lock);
  for (int i = 0; i < frames; i++) {
    if (p->ring_count >= p->ring_cap) break; // ring full: drop excess (pump paces)
    const uint8_t *s = src + (size_t)i * frame_bytes;
    int32_t l = read_sample(s, bps, p->is_float);
    int32_t r = stereo ? read_sample(s + bps, bps, p->is_float) : l;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    // silence meter: is BASS actually feeding audio, or zeros? (a full ring of
    // zeros = audible cutout with underrun=0, which the other counters miss)
    int a = l < 0 ? -l : l;
    int b = r < 0 ? -r : r;
    if (b > a) a = b;
    if (a > g_enq_peak) g_enq_peak = a;
    if (a < 16) g_enq_silent++; // ~ -66 dBFS: effectively silent
    const int w = (p->ring_head + p->ring_count) % p->ring_cap;
    p->ring[w * 2 + 0] = (int16_t)l;
    p->ring[w * 2 + 1] = (int16_t)r;
    p->ring_count++;
  }
  g_enq_count++;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_pos = p->cur_size = 0;
  p->ring_head = p->ring_count = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    // Report "empty" so BASS always feels free to render the next block when we
    // prompt it; the pump thread is what actually paces production via the ring.
    (void)p;
    st->count = 0;
    st->index = 0;
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  p->cb = cb;
  p->cb_ctx = ctx;
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
  SDL_UnlockMutex(p->lock);
  debugPrintf("opensles: SetPlayState %u\n", (unsigned)state);
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
  // Stop the pump first so it can't fire the callback into a half-freed player.
  p->alive = 0;
  if (p->pump) { SDL_WaitThread(p->pump, NULL); p->pump = NULL; }
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  free(p->ring);
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

  ensure_device(p->rate);

  // Allocate the decoupling ring (~1s) and start the pump thread.
  p->ring_cap = p->rate > 0 ? p->rate : 44100; // 1 second of frames
  p->ring = (int16_t *)calloc((size_t)p->ring_cap * 2, sizeof(int16_t));
  p->ring_head = p->ring_count = 0;
  p->alive = 1;
  p->pump = SDL_CreateThread(pump_thread, "bass_pump", p);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  *pPlayer = &p->obj_vt;
  debugPrintf("opensles: CreateAudioPlayer (%d Hz, %d ch, %d-bit %s)\n",
              p->rate, p->channels, p->bytes_per_sample * 8,
              p->is_float ? "float" : "int");
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
