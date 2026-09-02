/* main.c -- Where's My Water? Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * On Android, Where's My Water? (com.disney.WMW) is a native C++ game: a custom
 * Creature Feep engine drawing through OpenGL ES 1.1 fixed-function, with FMOD
 * Ex for audio and a statically-linked libxml2 for its level data. Its Java
 * layer is thin -- an Activity (com.disney.common.BaseActivity) plus a
 * GLSurfaceView.Renderer (com.disney.common.WMWRenderer) that forward lifecycle,
 * resize, per-frame draw and touch into the .so.
 *
 * This file recreates that Java layer in C: load and link libfmodex.so +
 * libwmw.so, stand up a GLES1 context and a fake JNI activity, then run a libnx
 * loop that calls the same entry points in the same order the renderer did.
 *
 * Data files (extracted from a copy of the game you own, never bundled):
 *   libwmw.so, libfmodex.so and the assets/ tree -- placed next to the .nro.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "libc_shim.h"
#include "nx_pointer.h"
#include "wmw_paths.h"
#include "wmw_tate.h"
#include "wmw_bundle.h"
#include "wmw_callbacks.h"
#include "wmw_assetindex.h"
#include "opensles.h"
#include "wmw_purchases.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "wmw_jni.h"
#include "wmw_entrypoints.h"
#include "fmod_audio.h"
#include "obb.h"
#include "nx_pointer.h"

size_t g_heap_total, g_heap_newlib, g_heap_so;  /* reported once at boot */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module fmod_mod; // libfmodex.so (loaded first -- libwmw imports 42 symbols from it)
so_module game_mod; // libwmw.so

wmw_entry_points wmw;

// libfmodex.so's two audio entry points. Resolved in resolve_entry_points(),
// which runs BEFORE so_finalize() -- see fmod_audio.h for why that matters.
static uintptr_t fmod_getinfo_addr, fmod_process_addr;

static void *g_thiz; // the fake BaseActivity / WMWRenderer instance
static PadState g_pad;

// wmw_callbacks.c needs the activity object to deliver answers with.
void *g_thiz_public(void) { return g_thiz; }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  /* Split the process heap between newlib and the .so load zone.
   *
   * The load zone only has to hold the game's native libraries -- libwmw.so is
   * about 7.6 MB and libfmodex.so about 1.4 MB -- so a fixed reservation is
   * right, and everything else belongs to newlib.
   *
   * That distribution matters more than it looks. Mesa allocates GPU memory
   * from the SAME newlib heap (nouveau_bo_new -> memalign -> malloc), so every
   * texture the engine uploads competes with the engine's own allocations.
   * Capping newlib at a fixed size and giving the remainder to the load zone
   * leaves that memory almost entirely unused while starving the allocator
   * actually under pressure -- on a title-override launch, multiple gigabytes
   * sat idle. The failure arrives as a fault inside malloc, several frames
   * deep in mesa's texture path:
   *
   *     _malloc_r <- _memalign_r <- nouveau_bo_new <- nvc0_miptree_create
   *               <- st_texture_create <- st_TexImage
   */
  size_t so_zone = SO_ZONE_MB * 1024 * 1024;
  if (so_zone > size / 4)          /* pathologically small heap: stay sane */
    so_zone = size / 4;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = size - so_zone;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)((char *)addr + fake_heap_size), 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_heap_total   = size;
  g_heap_newlib  = fake_heap_size;
  g_heap_so      = heap_so_limit;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.\nLaunch through a title override, not the album.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static const char *find_apk(void);

static void check_data(void) {
  struct stat st;
  const char *files[] = { FMOD_SO_NAME, SO_NAME };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nExtract it from your APK's\nlib/arm64-v8a/ next to the .nro.", files[i]);
  // Assets may be loose (assets/Data/) or still inside an .apk next to the .nro.
  if (stat("assets/Data", &st) < 0 && !find_apk())
    fatal_error("No game data found.\nCopy the APK's assets/ folder --\nor the .apk itself -- next to the .nro.");
}

// The engine can read its assets out of a zip. If the user dropped their APK
// next to the .nro, prefer it -- that is exactly what Android passed.
static const char *find_apk(void) {
  static char found[256];
  struct stat st;

  // the usual names first
  static const char *const candidates[] = { "base.apk", "WMW.apk", "game.apk", NULL };
  for (int i = 0; candidates[i]; i++)
    if (stat(candidates[i], &st) == 0 && st.st_size > 0)
      return candidates[i];

  // otherwise take any .apk sitting next to the .nro
  DIR *d = opendir(".");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      const char *dot = strrchr(e->d_name, '.');
      if (dot && strcasecmp(dot, ".apk") == 0) {
        snprintf(found, sizeof(found), "%s", e->d_name);
        closedir(d);
        return found;
      }
    }
    closedir(d);
  }
  return NULL;
}

// The engine keeps its level/progress data in an SQLite database. On Android,
// ApplicationContext::copyDatabaseFromBundle() extracted water.db out of the
// APK on first run. We have no APK to extract from when the assets are loose,
// and the log shows that path failing:
//
//     [Init] ApplicationContext::copyDatabaseFromBundle()
//            - couldn't open water.db for writing
//     [WMW]  Can't open database: unable to open database file .../water.db
//
// The shipped copy lives at assets/Data/water.db, so seed the writable one
// ourselves if it is not there yet. Done once, before the engine starts.
static void ensure_database(void) {
  char dst[WMW_PATH_MAX], src[WMW_PATH_MAX];
  snprintf(dst, sizeof(dst), "%s/water.db", wmw_game_dir());
  snprintf(src, sizeof(src), "%s/assets/Data/water.db", wmw_game_dir());

  // Presence is not enough. If copyDatabaseFromBundle() ran without a readable
  // bundle it leaves a zero-byte or truncated file behind, and the engine then
  // reports "file is encrypted or is not a database" on every launch from then
  // on. Check the SQLite magic and re-seed if it is not a real database, so a
  // bad run repairs itself on the next start instead of wedging permanently.
  struct stat st;
  if (stat(dst, &st) == 0 && st.st_size > 0) {
    char magic[16] = {0};
    FILE *chk = fopen(dst, "rb");
    if (chk) {
      const size_t got = fread(magic, 1, sizeof(magic), chk);
      fclose(chk);
      if (got == sizeof(magic) && memcmp(magic, "SQLite format 3", 15) == 0)
        return; // valid -- keep it, it holds the player's progress
    }
    debugPrintf("db: %s is not a valid database -- reseeding\n", dst);
    remove(dst);
  }

  // copyDatabaseFromBundle() stages through checked_water_tmp.db. A leftover
  // from an interrupted run is a state variable we do not want, so clear it.
  {
    char tmp[WMW_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/checked_water_tmp.db", wmw_game_dir());
    if (remove(tmp) == 0) debugPrintf("db: cleared stale checked_water_tmp.db\n");
  }

  FILE *in = fopen(src, "rb");
  if (!in) {
    debugPrintf("db: no bundled database at %s\n", src);
    return;
  }
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    debugPrintf("db: could not create %s -- is the SD card writable?\n", dst);
    return;
  }

  char buf[16 * 1024];
  size_t n, total = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { total = 0; break; }
    total += n;
  }
  fclose(in);
  fclose(out);
  debugPrintf("db: seeded water.db (%zu bytes)\n", total);
}

static void set_screen_size(void) {
  // Locked to the same size in both modes -- docking changes nothing: the
  // engine is never told its resolution moved, and there is no re-layout to
  // go wrong halfway through a level.
  //
  // 1080p rather than the handheld panel's native 720p: the OS downscales for
  // free when handheld (a supersampled, slightly antialiased image on the
  // 1280x720 panel) and presents 1:1, unscaled, when docked. WMW_PANEL_WIDTH_MM
  // / HEIGHT_MM in config.h are scaled right alongside render_width/height so
  // the engine's computed DPI -- and therefore which art-asset tier it loads
  // -- is unchanged from the 720p numbers; only supersample it further without
  // scaling those two together too.
  //
  // (A 720p-render / 1080p-window diagnostic variant briefly lived here while
  // chasing the menu-transition stalls in the frame-timing log -- ruled out:
  // the stalls persisted unchanged at 720p, so they are not resolution-linked
  // and turned out to predate this work entirely.)
  screen_width  = 1920;
  screen_height = 1080;
  render_width  = 1080;
  render_height = 1920;
  debugPrintf("screen: window %dx%d, engine renders %dx%d (portrait)\n",
              screen_width, screen_height, render_width, render_height);
}

// ---------------------------------------------------------------------------
// EGL / GLES1 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  // GLES1: EGL_OPENGL_ES_BIT, not ES2_BIT. The engine logs "Preparing ES 1.1".
  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no GLES1 config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);

  debugPrintf("gl vendor:   %s\n", (const char *)glGetString(GL_VENDOR));
  debugPrintf("gl renderer: %s\n", (const char *)glGetString(GL_RENDERER));
  debugPrintf("gl version:  %s\n", (const char *)glGetString(GL_VERSION));
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// module loading
// ---------------------------------------------------------------------------

#define RESOLVE(field, sym)                                                    \
  do {                                                                         \
    wmw.field = (void *)so_try_find_addr_rx(&game_mod, sym);                    \
    if (!wmw.field) debugPrintf("entry point missing: %s\n", sym);              \
  } while (0)

static void resolve_entry_points(void) {
  wmw.JNI_OnLoad = (fn_onload)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");

  RESOLVE(rendererInit,              "Java_com_disney_common_WMWRenderer_rendererInit");
  RESOLVE(rendererResized,           "Java_com_disney_common_WMWRenderer_rendererResized");
  RESOLVE(notifyScreenResized,       "Java_com_disney_common_WMWRenderer_notifyScreenResized");
  RESOLVE(rendererDrawFrame,         "Java_com_disney_common_WMWRenderer_rendererDrawFrame");
  RESOLVE(rendererReloadContextData, "Java_com_disney_common_WMWRenderer_rendererReloadContextData");
  RESOLVE(rendererTouchBegan,        "Java_com_disney_common_WMWRenderer_rendererTouchBegan");
  RESOLVE(rendererTouchMoved,        "Java_com_disney_common_WMWRenderer_rendererTouchMoved");
  RESOLVE(rendererTouchEnded,        "Java_com_disney_common_WMWRenderer_rendererTouchEnded");

  RESOLVE(onGamePause,          "Java_com_disney_common_BaseActivity_onGamePause");
  RESOLVE(onGameResume,         "Java_com_disney_common_BaseActivity_onGameResume");
  RESOLVE(onLostFocus,          "Java_com_disney_common_BaseActivity_onLostFocus");
  RESOLVE(onRegainedFocus,      "Java_com_disney_common_BaseActivity_onRegainedFocus");
  RESOLVE(backKeyPressed,       "Java_com_disney_common_BaseActivity_backKeyPressed");
  RESOLVE(accelerometerChanged, "Java_com_disney_common_BaseActivity_accelerometerChanged");
  RESOLVE(getLocalizedText,     "Java_com_disney_common_BaseActivity_getLocalizedText");
  RESOLVE(notifyIAPAvailability,   "Java_com_disney_common_BaseActivity_notifyIAPAvailability");
  RESOLVE(notifyReachability,      "Java_com_disney_common_BaseActivity_notifyReachability");
  RESOLVE(notifyAMPSAvailability,  "Java_com_disney_common_BaseActivity_notifyAMPSAvailability");
  RESOLVE(notifyProductInfoFailed, "Java_com_disney_common_BaseActivity_notifyProductInfoFailed");
  RESOLVE(notifyPurchaseFailed,    "Java_com_disney_common_BaseActivity_notifyPurchaseFailed");
  RESOLVE(notifyPurchaseCancelled, "Java_com_disney_common_BaseActivity_notifyPurchaseCancelled");
  RESOLVE(notifyLOTWCountDown,     "Java_com_disney_common_BaseActivity_notifyLOTWCountDown");
  RESOLVE(notifyEnableBackKey,     "Java_com_disney_common_BaseActivity_notifyEnableBackKey");
  RESOLVE(notifyPurchaseSuccess,   "Java_com_disney_common_BaseActivity_notifyPurchaseSuccess");
  RESOLVE(notifyAddObbFilePathToFileManager,
          "Java_com_disney_common_BaseActivity_notifyAddObbFilePathToFileManager");

  fmod_getinfo_addr = so_try_find_addr_rx(&fmod_mod,
      "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
  fmod_process_addr = so_try_find_addr_rx(&fmod_mod,
      "Java_org_fmod_FMODAudioDevice_fmodProcess");
  if (!fmod_getinfo_addr || !fmod_process_addr)
    debugPrintf("fmod: audio entry points not found in %s\n", FMOD_SO_NAME);

  if (!wmw.rendererInit || !wmw.rendererDrawFrame)
    fatal_error("libwmw.so is missing its renderer\nentry points. Wrong library or\nunsupported game version?");
}

static void load_two_modules(void) {
  if (so_load(&fmod_mod, FMOD_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", FMOD_SO_NAME);

  const size_t used = ALIGN_MEM(fmod_mod.load_size, 0x1000);
  void *base2 = (char *)heap_so_base + used;
  size_t avail2 = (heap_so_limit > used) ? heap_so_limit - used : 0;

  if (so_load(&game_mod, SO_NAME, base2, avail2) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  so_relocate(&fmod_mod);
  so_relocate(&game_mod);

  update_imports();

  // FMOD first: its libc imports come straight from the table.
  so_resolve(&fmod_mod, dynlib_functions, dynlib_numfunctions, 1);
  // Then the engine: the table is consulted first, then sibling modules, so the
  // 42 FMOD_* / FMOD::* imports bind to the real libfmodex sitting in so_list.
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  resolve_entry_points();

  so_finalize(&fmod_mod);
  so_finalize(&game_mod);
  so_flush_caches(&fmod_mod);
  so_flush_caches(&game_mod);

  // libwmw was built with -mstack-protector-guard=tls: guarded functions read
  // the canary from tpidr_el0 + 0x28 (visible in rendererInit and the touch
  // entry points). Install it before any engine code runs.
  tls_setup_guard();

  so_execute_init_array(&fmod_mod);
  so_execute_init_array(&game_mod); // C++ static constructors

  so_free_temp(&fmod_mod);
  so_free_temp(&game_mod);
}

// ---------------------------------------------------------------------------
// touch: the renderer takes parallel float arrays, Android multitouch style
// ---------------------------------------------------------------------------

#define MAX_TOUCH 8

// The renderer takes Android-style parallel arrays. classes.dex declares:
//   rendererTouchBegan(int, float[], float[], int[])
//   rendererTouchEnded(int, float[], float[], int[])
//   rendererTouchMoved(int, float[], float[], float[], float[], int[])
// -- so "moved" also wants each contact's PREVIOUS position, and the id array
// is int[], not float[]. Allocated once and reused, as the Java renderer did.
static void *a_x, *a_y, *a_px, *a_py, *a_id;
static float *t_x, *t_y, *t_px, *t_py;
static int32_t *t_id;

// Last reported position per pointer id, so "moved" can supply prevX/prevY.
static float last_x[MAX_TOUCH], last_y[MAX_TOUCH];
static int   last_valid[MAX_TOUCH];

static void touch_arrays_init(void) {
  a_x  = jni_make_float_array(MAX_TOUCH, &t_x);
  a_y  = jni_make_float_array(MAX_TOUCH, &t_y);
  a_px = jni_make_float_array(MAX_TOUCH, &t_px);
  a_py = jni_make_float_array(MAX_TOUCH, &t_py);
  a_id = jni_make_int_array(MAX_TOUCH, &t_id);

  // These live for the whole process and we hold raw pointers into their
  // backing stores (t_x, t_y, ...), refilled every frame. As plain local refs
  // the engine could discard them at any PopLocalFrame -- free_ref() releases
  // the backing store too, after which every touch writes into freed memory and
  // the heap fails later inside free(), with a stack that points at the engine
  // rather than at us. Pin them.
  jni_pin(a_x);
  jni_pin(a_y);
  jni_pin(a_px);
  jni_pin(a_py);
  jni_pin(a_id);
}

static void nxp_log(const char *m) { debugPrintf("%s", (char *)m); }

// nx_pointer reads cursor.png and persists pointer.cfg. Those opens allocate
// newlib handles like any other, so they must take the same lock as the rest of
// the port -- FMOD's threads are live by then. See libc_shim.c.
static FILE *nxp_fopen(const char *path, const char *mode) {
  wmw_file_lock();
  FILE *f = fopen(path, mode);
  wmw_file_unlock();
  return f;
}
static int nxp_fclose(FILE *f) {
  wmw_file_lock();
  const int r = fclose(f);
  wmw_file_unlock();
  return r;
}

static void input_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);

  NxpConfig pcfg;
  memset(&pcfg, 0, sizeof(pcfg));

  // The cursor lives in RENDER space -- the portrait framebuffer the engine
  // draws into -- because nxp_draw() runs before wmw_tate_present() and is
  // therefore rotated along with everything else. Physical input is what needs
  // converting, and `rotation` tells the module to do it: pushing the stick
  // right moves the cursor right as the player sees it, not as the panel is
  // wired.
  pcfg.screen_w = render_width;
  pcfg.screen_h = render_height;

  // The touch panel is bonded to the physical glass and does not rotate just
  // because the image does, so it stays in landscape panel space -- the
  // digitizer's own FIXED native resolution, not screen_width/screen_height
  // (our chosen swapchain size, which is a display-only concept the touch
  // hardware has never heard of). See WMW_TOUCH_PANEL_W/H in config.h.
  pcfg.panel_w  = WMW_TOUCH_PANEL_W;
  pcfg.panel_h  = WMW_TOUCH_PANEL_H;

  pcfg.rotation     = wmw_tate_mode();
  pcfg.handle_touch = 1;   // one owner for the input rotation, not two
  pcfg.data_dir     = wmw_game_dir();
  pcfg.log          = nxp_log;
  pcfg.fopen_fn     = nxp_fopen;
  pcfg.fclose_fn    = nxp_fclose;
  nxp_init(&pcfg);
}

// Group this frame's events by phase and issue one batched call per phase,
// which is what WMWView.copyTouches() did with MotionEvent's pointer arrays.
static void dispatch_phase(int phase, const NxpEvent *ev, int n) {
  int count = 0;
  for (int i = 0; i < n && count < MAX_TOUCH; i++) {
    if (ev[i].phase != phase) continue;
    const int id = ev[i].id;
    const int slot = (id >= 0 && id < MAX_TOUCH) ? id : 0;

    // nx_pointer already delivers render-space coordinates -- it applies the
    // inverse of the display rotation itself, for touch and cursor alike.
    // Rotating again here would undo it; one owner for that transform.
    float mx = ev[i].x, my = ev[i].y;

    // The engine wants NORMALISED coordinates, not pixels. WMWView.java
    // divided by the view size before every call:
    //     _xPos[i] = event.getX(i) / getWidth();
    //     _yPos[i] = event.getY(i) / getHeight();
    // Passing pixels puts every contact far off the bottom-right corner, which
    // reads as "touch does nothing at all".
    mx /= (float)render_width;
    my /= (float)render_height;

    t_x[count]  = mx;
    t_y[count]  = my;
    t_id[count] = id;

    // prev = where this id was last seen; on a fresh contact, itself.
    t_px[count] = last_valid[slot] ? last_x[slot] : mx;
    t_py[count] = last_valid[slot] ? last_y[slot] : my;

    last_x[slot] = mx;
    last_y[slot] = my;
    last_valid[slot] = (phase != NXP_UP);
    count++;
  }
  if (!count) return;

  // First few contacts only: enough to confirm the rotation and the 0..1
  // normalisation are right, without flooding the log during play.
  static int touch_logged = 0;
  if (touch_logged < 12) {
    touch_logged++;
    debugPrintf("touch: %s n=%d  panel(%.0f,%.0f) -> engine(%.3f,%.3f)\n",
                phase == NXP_DOWN ? "down" : phase == NXP_UP ? "up" : "move",
                count, (double)ev[0].x, (double)ev[0].y,
                (double)t_x[0], (double)t_y[0]);
  }

  if (phase == NXP_DOWN) {
    if (wmw.rendererTouchBegan)
      wmw.rendererTouchBegan(fake_env, g_thiz, count, a_x, a_y, a_id);
  } else if (phase == NXP_UP) {
    if (wmw.rendererTouchEnded)
      wmw.rendererTouchEnded(fake_env, g_thiz, count, a_x, a_y, a_id);
  } else {
    if (wmw.rendererTouchMoved)
      wmw.rendererTouchMoved(fake_env, g_thiz, count, a_x, a_y, a_px, a_py, a_id);
  }
}

// Holding LS for 2s (see nx_pointer's gesture) flips between the rotated
// fullscreen presentation and the pillarboxed upright one, live. Runs here --
// the top of feed_pointer(), called once per frame before wmw_tate_begin()
// rebinds for the frame -- so the FBO rebuild inside wmw_tate_init() never
// leaves a frame rendering into a target that was just torn down.
static void handle_mode_toggle(void) {
  if (!nxp_toggle_requested()) return;

  wmw_set_pillarbox_enabled(!wmw_pillarbox_enabled());
  const int mode = wmw_tate_mode();
  debugPrintf("main: mode toggle -- now %s\n",
              mode == WMW_TATE_UPRIGHT ? "upright (pillarboxed)" : "rotated (fullscreen)");

  if (!wmw_tate_init(render_width, render_height, screen_width, screen_height, mode))
    debugPrintf("tate: re-init failed on live toggle -- portrait presentation disabled\n");
  nxp_set_rotation(mode);
}

static void feed_pointer(void) {
  nxp_update();
  handle_mode_toggle();
  NxpEvent ev[16];
  const int n = nxp_poll(ev, 16);
  if (!n) return;
  dispatch_phase(NXP_DOWN, ev, n);
  dispatch_phase(NXP_MOVE, ev, n);
  dispatch_phase(NXP_UP,   ev, n);
}

// ---------------------------------------------------------------------------
// applet lifecycle -> BaseActivity lifecycle
// ---------------------------------------------------------------------------

static void applet_hook_fn(AppletHookType type, void *param) {
  (void)param;
  switch (type) {
    case AppletHookType_OnExitRequest:
      wmw_quit_requested = 1;
      break;
    case AppletHookType_OnFocusState: {
      const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
      debugPrintf("applet: focus %s\n", focused ? "gained" : "lost");
      fmod_audio_set_paused(!focused);
      if (focused) {
        if (wmw.onRegainedFocus) wmw.onRegainedFocus(fake_env, g_thiz);
        if (wmw.onGameResume)    wmw.onGameResume(fake_env, g_thiz);
        // A real Android context loss would need this; harmless here and it is
        // what the Java renderer did on surface recreation.
        if (wmw.rendererReloadContextData) wmw.rendererReloadContextData(fake_env, g_thiz);
      } else {
        if (wmw.onGamePause) wmw.onGamePause(fake_env, g_thiz);
        if (wmw.onLostFocus) wmw.onLostFocus(fake_env, g_thiz);
      }
      break;
    }
    case AppletHookType_OnOperationMode:
      // Dock/undock changes the panel size; tell the engine to re-lay-out.
      set_screen_size();
      nwindowSetDimensions(nwindowGetDefault(), screen_width, screen_height);
      if (wmw.rendererResized)     wmw.rendererResized(fake_env, g_thiz, screen_width, screen_height);
      if (wmw.notifyScreenResized) wmw.notifyScreenResized(fake_env, g_thiz, screen_width, screen_height);
          {
        NxpConfig pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.screen_w = screen_width;
        pcfg.screen_h = screen_height;
        pcfg.data_dir = wmw_game_dir();
        pcfg.log      = nxp_log;
        nxp_init(&pcfg);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------

int main(void) {
  AppletHookCookie cookie;

  socketInitializeDefault();
  debugPrintf("=== Where's My Water? NX loader ===\n");
  debugPrintf("heap: %zu MB total -- %zu MB newlib (engine + mesa textures), %zu MB modules\n",
              g_heap_total >> 20, g_heap_newlib >> 20, g_heap_so >> 20);

  // Discover where we were launched from before anything touches the disk.
  wmw_paths_init();

  // SDL2 backs the OpenSL ES shim that FMOD plays through (see opensles.c).
  // SDL_SetMainReady() is required because this port provides its own main()
  // rather than SDL_main; without it SDL_Init refuses to start and the only
  // symptom is silence.
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s -- there will be no sound\n", SDL_GetError());
  else
    debugPrintf("SDL audio subsystem ready\n");

  check_syscalls();
  check_data();
  wmw_assetindex_build();
  ensure_database();
  set_screen_size();

  if (!egl_init())
    fatal_error("Could not create a GLES 1.1 context.\nInstall switch-mesa and\nswitch-libdrm_nouveau.");

  // Portrait presentation. Must come after egl_init() (it needs a current
  // context) and before the engine is told its size.
  if (!wmw_tate_init(render_width, render_height,
                     screen_width, screen_height, wmw_tate_mode()))
    debugPrintf("tate: portrait unavailable -- rendering landscape as-is\n");

  load_two_modules();

  jni_init();
  g_thiz = jni_make_thiz();
  touch_arrays_init();

  // Dalvik called this the moment System.loadLibrary() returned.
  if (wmw.JNI_OnLoad) {
    int32_t v = wmw.JNI_OnLoad(fake_vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  }

  // If a main.obb sits next to the .nro, hand its path to the engine's own
  // FileManager exactly as the Java layer did. Most copies do not have one --
  // the assets/ tree is read as loose files -- so this is a no-op by default.
  {
    const char *obb = obb_find();
    if (obb && wmw.notifyAddObbFilePathToFileManager) {
      debugPrintf("registering obb: %s\n", obb);
      wmw.notifyAddObbFilePathToFileManager(fake_env, g_thiz, jni_make_string(obb));
    }
  }

  // FMOD's pump waits for FMOD::System::init(), so starting it now is safe.

  input_init();
  appletHook(&cookie, applet_hook_fn, NULL);

  // The GLSurfaceView.Renderer contract: onSurfaceCreated -> onSurfaceChanged
  // -> onDrawFrame repeatedly.
  // rendererInit(String apkPath, String dataPath, Context ctx, String sku, float density)
  //
  // apkPath was ApplicationInfo.sourceDir on Android -- the APK itself. The
  // engine has a statically-linked minizip ("ZipArchiveReader::readCurrentFile",
  // "wUnzOpen*" in .rodata), so it can read assets straight out of a zip. If a
  // .apk is sitting next to the .nro we hand it that; otherwise we hand it the
  // game directory and it falls back to loose files under assets/Data/.
  {
    // The engine opens this as an archive. Handing it a directory makes the
    // open fail, and it then allocates from a size it never read -- which
    // surfaces as "RendererResize failed with exception: std::bad_alloc".
    const char *apk = find_apk();
    const char *bundle = wmw_bundle_path(apk);
    const char *pkg_path = bundle ? bundle : wmw_game_dir();
    debugPrintf("calling rendererInit(pkg=\"%s\", data=\"%s\", sku=\"%s\", density=%.2f)\n",
                pkg_path, wmw_game_dir(), WMW_SKU_MARKET, (double)WMW_DISPLAY_DENSITY);
    wmw.rendererInit(fake_env, g_thiz,
                     jni_make_string(pkg_path),
                     jni_make_string(wmw_game_dir()),
                     g_thiz,                       // the Context is the activity
                     jni_make_string(WMW_SKU_MARKET),
                     WMW_DISPLAY_DENSITY);
  }

  debugPrintf("calling rendererResized(%d, %d)\n", render_width, render_height);
  if (wmw.rendererResized)     wmw.rendererResized(fake_env, g_thiz, render_width, render_height);
  if (wmw.notifyScreenResized) wmw.notifyScreenResized(fake_env, g_thiz, render_width, render_height);

  // Audio backend choice has to come AFTER rendererInit: FMOD::System::init()
  // runs in there, and that is when it dlopen()s libOpenSLES and creates its
  // engine. Deciding earlier always sees "no OpenSL engine" and starts the
  // FMODAudioDevice pump as well, putting two writers on the audio device.
  if (opensles_in_use()) {
    debugPrintf("audio: FMOD is using the OpenSL ES output\n");
  } else {
    debugPrintf("audio: no OpenSL engine -- falling back to the FMODAudioDevice pump\n");
    fmod_audio_start(fmod_getinfo_addr, fmod_process_addr);
  }

  // Tell the engine which add-on content is owned, the same way Google Play's
  // queryPurchasesAsync() result would have. Must follow rendererInit: the IAP
  // subsystem does not exist before it.
  wmw_purchases_restore();

  if (wmw.onGameResume)    wmw.onGameResume(fake_env, g_thiz);
  if (wmw.onRegainedFocus) wmw.onRegainedFocus(fake_env, g_thiz);

  // On Android the billing client connected shortly after startup and called
  // notifyIAPAvailability(). Nothing will ever connect here, so say so -- the
  // engine holds screens open waiting for this answer. Same for reachability:
  // this port is entirely offline.
  wmw_cb_post(WMW_CB_IAP_AVAILABILITY, 0, NULL);
  wmw_cb_post(WMW_CB_REACHABILITY, 0, NULL);

  debugPrintf("entering main loop\n");

  /* Frame pacer.
   *
   * eglSwapInterval(1) is set, but mesa's present is asynchronous --
   * eglSwapBuffers does not block on the panel. Left alone the loop free-runs
   * slightly fast, drifts a fraction of a millisecond every frame, and laps the
   * display roughly once a second. That reads as a periodic hitch rather than
   * as "too fast", which makes it easy to misdiagnose as a performance problem.
   *
   * Holding an explicit deadline fixes the pacing regardless of what the
   * present does. */
  const u64 frame_ticks = armNsToTicks(1000000000ull / WMW_TARGET_FPS);
  u64 frame_deadline = armGetSystemTick() + frame_ticks;

  int frame = 0;
  u64 acc_render = 0, acc_swap = 0, max_render = 0, max_swap = 0;
  u64 win_start = armGetSystemTick();

  while (appletMainLoop() && !wmw_quit_requested) {
    padUpdate(&g_pad);

    // No button is mapped to the Android back key. The game has its own on-screen
    // back buttons, and B is close enough to the tap buttons (A / ZL / ZR) to be
    // pressed by accident, which backs out of a level rather than doing nothing.
    // wmw.backKeyPressed is still resolved and can be bound here if wanted.

    feed_pointer();
    wmw_cb_drain();   // deliver answers to last frame's async requests

    const u64 t_render0 = armGetSystemTick();
    wmw_tate_begin();                     // bind the portrait target
    wmw.rendererDrawFrame(fake_env, g_thiz);
    nxp_draw();                           // cursor INTO the portrait target...
    wmw_tate_present();                   // ...so the rotation carries it too
    const u64 t_render1 = armGetSystemTick();

    eglSwapBuffers(s_display, s_surface);
    const u64 t_swap1 = armGetSystemTick();

    const u64 d_render = t_render1 - t_render0;
    const u64 d_swap   = t_swap1  - t_render1;
    acc_render += d_render; acc_swap += d_swap;
    if (d_render > max_render) max_render = d_render;
    if (d_swap   > max_swap)   max_swap   = d_swap;

    // --- hold to the target rate --------------------------------------------
    {
      const s64 remain = (s64)(frame_deadline - armGetSystemTick());
      if (remain > 0) {
        const u64 remain_ns = armTicksToNs((u64)remain);
        // Sleep the bulk of it -- cheap, and lets the CPU drop clocks -- but
        // busy-wait the final millisecond, which is finer than the scheduler
        // can reliably deliver.
        if (remain_ns > 1500000ull)
          svcSleepThread((s64)(remain_ns - 1000000ull));
        while ((s64)(frame_deadline - armGetSystemTick()) > 0)
          ; // spin the last <=1ms
        frame_deadline += frame_ticks;
      } else {
        // A genuine spike put us behind. Resync to now rather than advancing the
        // deadline: trying to "catch up" races several frames back to back,
        // which looks worse than the hitch it is compensating for.
        frame_deadline = armGetSystemTick() + frame_ticks;
      }
    }

    frame++;

    // Timing report: the first few frames, then once every ~2 seconds. Render
    // and swap are separated because they fail differently -- render climbing
    // means the engine is working harder, swap climbing means it is waiting on
    // the display.
    if (frame < 5 || (frame % 120) == 0) {
      const u64 now = armGetSystemTick();
      const u64 win_ns = armTicksToNs(now - win_start);
      const int n = (frame % 120 == 0 && frame) ? 120 : (frame ? frame : 1);
      debugPrintf("frame %d: fps=%.1f render avg=%.1fms max=%.1fms | swap avg=%.1fms max=%.1fms\n",
                  frame, n * 1e9 / (double)(win_ns ? win_ns : 1),
                  armTicksToNs(acc_render) / 1e6 / n, armTicksToNs(max_render) / 1e6,
                  armTicksToNs(acc_swap)   / 1e6 / n, armTicksToNs(max_swap)   / 1e6);
      acc_render = acc_swap = max_render = max_swap = 0;
      win_start = now;
    }

    debugLogFlush();
  }

  debugPrintf("shutting down\n");
  if (wmw.onGamePause) wmw.onGamePause(fake_env, g_thiz);
  nxp_save_settings();
  fmod_audio_stop();
  appletUnhook(&cookie);
  egl_deinit();
  socketExit();
  return 0;
}
