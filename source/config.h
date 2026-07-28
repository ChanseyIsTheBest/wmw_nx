/* config.h -- build-time constants for the Where's My Water? Switch port
 *
 * Where's My Water? (com.disney.WMW) -- the Premium build.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory reserved for the .so load zone -- the game's native libraries and
// nothing else. libwmw.so is ~7.6 MB and libfmodex.so ~1.4 MB, so this is
// generous; everything NOT reserved here goes to newlib.
//
// newlib is deliberately not capped. It carries the engine's own allocations
// (decoded WebP/PNG atlases, the fluid simulation's particle pools, FMOD's
// mixer buffers, libxml2's working set) AND every GPU buffer mesa allocates,
// since nouveau_bo_new() ends up in memalign(). Reserving a fixed slab for the
// engine and handing the rest to the load zone gets this exactly backwards.
// See __libnx_initheap in main.c.
#define SO_ZONE_MB 32

// Two native modules, loaded in the order the Android activity loads them.
// libfmodex MUST be loaded and relocated before libwmw is resolved: libwmw
// imports 42 FMOD entry points and so_resolve binds them by walking the
// already-loaded modules once the static import table has been consulted.
#define FMOD_SO_NAME "libfmodex.so"
#define SO_NAME      "libwmw.so"

// Game data. The engine builds its paths from a data root and appends
// "/assets/Data/..." itself (the literal "/assets/Data/" lives in libwmw's
// .rodata). The launch directory is the folder that *contains* "assets".
// The game directory is DISCOVERED AT RUNTIME with getcwd() -- hbloader sets
// the working directory to the .nro's own folder. Do not hard-code it: an
// install anywhere other than the assumed path silently breaks every absolute
// reference, including the engine's save-database writes. See wmw_paths.c.
#define OBB_NAME    "main.obb"           // optional; only if your copy ships one
#define LOG_NAME    "debug.log"

// rendererInit's 4th argument. On Android this came from the "SKUMarket"
// manifest metadata and selected the storefront; "google" is the default the
// Java code falls back to. It only gates IAP, which is answered "unavailable".
#define WMW_SKU_MARKET "google"

// rendererInit's 5th argument: Android DisplayMetrics.density. 1.0 = mdpi
// (160 dpi), 1.5 = hdpi, 2.0 = xhdpi. The Switch handheld panel is ~237 dpi,
// which is hdpi territory, but the engine's HD art was authored for xhdpi
// tablets -- 2.0 asks for the sharper set. Drop to 1.5 if atlases fail to fit.
#define WMW_DISPLAY_DENSITY 2.0f

// Per-line SD-card writes are slow; set to 0 for release builds.
#define DEBUG_LOG 0

// Frame rate the main loop is held to. The panel is 60 Hz and the engine has no
// internal limiter, so this is what actually paces the game.
#define WMW_TARGET_FPS 60

// Reported to the engine through BaseActivity.getAppVersion().
#define WMW_APP_VERSION "1.18.0"

// Physical panel size in millimetres, reported through getDisplayWidthInMM() /
// getDisplayHeightInMM(). The engine divides the pixel size by these to get a
// DPI, and that DPI selects which texture/atlas set it loads.
//
// These describe the display AS THE ENGINE SEES IT, which is portrait: it was
// told 720x1280 by getDisplayWidth/Height. The Switch panel is 6.2" 16:9 --
// 137.2 x 77.2 mm held normally, so 77.2 wide by 137.2 tall turned on its side.
// Both axes then work out to ~237 dpi, which is the bucket the HD tablet art
// targets. Pairing landscape millimetres with a portrait pixel size instead
// gives 133 dpi across and 421 dpi down: not a real display, and the engine
// derives a nonsense scale from it.
//
// If the art comes out visibly low-resolution, lower both (raising the computed
// DPI); if atlas loading runs out of memory, raise both.
#define WMW_PANEL_WIDTH_MM   77.2f
#define WMW_PANEL_HEIGHT_MM 137.2f

// WMW is a portrait game. The engine renders into a portrait framebuffer which
// is then rotated 90 degrees onto the landscape panel, so you turn the console
// on its side and play it like a phone. See wmw_tate.c.
//
// Two coordinate spaces, and it matters which is which:
//
//   screen_* -- the real landscape swapchain and the space the touchscreen
//               reports in. Locked to 1280x720 in BOTH handheld and docked, so
//               the presentation is identical either way and the engine is
//               never asked to re-lay-out on a dock/undock.
//   render_* -- the portrait size the ENGINE believes it has (720x1280).
//               Reported through getDisplayWidth/getDisplayHeight. Rotated, it
//               lands 1:1 on the panel: no scaling, no letterboxing.
extern int screen_width;
extern int screen_height;
extern int render_width;
extern int render_height;

// Which way to rotate. Overridable at runtime in <gamedir>/config.txt with
//   rotation = cw | ccw | upright
// "cw" puts the portrait image's top edge on the right of the panel. If the
// game ends up upside-down for how you like to hold the console, use "ccw".
// "upright" is the non-rotated fallback: portrait centred with bars either
// side, for playing docked where you cannot turn the screen.
int wmw_rotation_mode(void);

// Language / country reported to the engine, derived from the Switch system
// language at boot (see wmw_language.c). WMW ships localized text keyed off
// these two.
const char *wmw_language_code(void);
const char *wmw_country_code(void);

#endif
