/* wmw_jni.c -- answers for the Java methods libwmw.so calls back into
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The engine reaches Java through com.disney.common.BaseActivity. Every method
 * name and signature below was recovered from libwmw.so's own .rodata (the
 * GetMethodID name/signature string pairs sit adjacent to each other around
 * 0x613d20-0x615200), so this list is the game's real upcall surface rather
 * than a guess.
 *
 * Three groups:
 *
 *   1. Device / display / locale queries. These MUST return sensible values --
 *      the engine sizes its viewport and picks its texture set from them.
 *
 *   2. Capability queries that must answer "no" so the engine never waits on a
 *      service that does not exist here: achievements UI, IAP, network
 *      reachability, and Disney's AMPS content-download service (the
 *      Level-of-the-Week / reward downloads). Answering "no" is what keeps the
 *      port fully offline and self-contained.
 *
 *   3. Fire-and-forget notifications (analytics, social, store links, rate-app,
 *      screenshots) which are correctly no-ops.
 *
 * Anything not listed here falls through to jni_fake.c, which logs it as
 * "JNI: <kind> <name><sig> -> default". If the game ever stalls, that log line
 * names the method to add here.
 */

#include <string.h>
#include <stdio.h>

#include "wmw_jni.h"
#include "config.h"
#include "util.h"
#include "wmw_callbacks.h"
#include "wmw_purchases.h"

volatile float wmw_load_percent  = 0.0f;
volatile int   wmw_quit_requested = 0;

// ---------------------------------------------------------------------------
// numeric answers (()I, ()Z, ()F)
// ---------------------------------------------------------------------------

int wmw_jni_numeric(const char *name, const char *sig, long *out) {
  // --- display geometry, asked during rendererInit ---------------------------
  // The PORTRAIT size, not the landscape window -- the engine lays its whole
  // UI out from these and wmw_tate.c rotates the result onto the panel.
  if (!strcmp(name, "getDisplayWidth"))  { *out = render_width;  return 1; }
  if (!strcmp(name, "getDisplayHeight")) { *out = render_height; return 1; }

  // --- capability queries: everything online / storefront answers "no" ------
  static const char *const say_false[] = {
    "hasAchievementsSupport",     // ()Z -- no Google Play Games here
    "hasAchievementsUI",          // ()Z
    "isAvailable",                // ()Z -- AMPS content service
    "isDownloadAvailable",        // ()Z -- AMPS download
    "hasUpdated",                 // ()Z -- remote data refresh
    "IsConnected",                // ()Z -- reachability
    "installed",                  // ()Z -- companion-app check (WMW2 cross-promo)
    NULL
  };
  for (int i = 0; say_false[i]; i++)
    if (!strcmp(name, say_false[i])) { *out = 0; return 1; }

  if (!strcmp(sig, "()I") || !strcmp(sig, "()Z")) {
    // Unknown query: 0 is the safe answer for both (false / zero).
    return 0; // let jni_fake log it so it can be identified during bring-up
  }
  return 0;
}

// ---------------------------------------------------------------------------
// float answers (()F) -- dispatched through CallFloatMethod, which returns in
// s0. See jni_fake.c: routing these through the integer path hands the engine
// garbage and it fails much later, during resize, as a bad_alloc.
// ---------------------------------------------------------------------------

int wmw_jni_float(const char *name, const char *sig, float *out) {
  if (strcmp(sig, "()F") != 0)
    return 0;

  // Physical panel size in millimetres. The engine divides the pixel size by
  // these to get a DPI, and the DPI selects the texture set.
  //
  // These MUST describe the display as the engine sees it -- which is the
  // PORTRAIT one, because that is what getDisplayWidth/Height report. The
  // Switch panel is 6.2" 16:9, so 137.2 x 77.2 mm in landscape; turned on its
  // side that is 77.2 wide by 137.2 tall. Reporting the landscape figures
  // against a portrait pixel size gives 133 dpi across and 421 dpi down, which
  // is not a real display and makes the engine compute a nonsense scale.
  //
  // Correctly paired, 720/77.2 and 1280/137.2 both come to ~237 dpi.
  if (!strcmp(name, "getDisplayWidthInMM"))  { *out = WMW_PANEL_WIDTH_MM;  return 1; }
  if (!strcmp(name, "getDisplayHeightInMM")) { *out = WMW_PANEL_HEIGHT_MM; return 1; }

  return 0;
}

// ---------------------------------------------------------------------------
// string answers (()Ljava/lang/String;)
// ---------------------------------------------------------------------------

const char *wmw_jni_string(const char *name, const char *sig) {
  if (strcmp(sig, "()Ljava/lang/String;") != 0)
    return NULL;

  if (!strcmp(name, "getLanguageCode")) return wmw_language_code();
  if (!strcmp(name, "getCountryCode"))  return wmw_country_code();

  // Any stable non-empty id works; it only ever keys local save data here.
  if (!strcmp(name, "getInstallationId")) return "switch-homebrew";

  if (!strcmp(name, "getAppVersion"))   return WMW_APP_VERSION;
  if (!strcmp(name, "getAppInfo"))      return WMW_APP_VERSION;
  if (!strcmp(name, "getAppBuildInfo")) return WMW_APP_VERSION;

  return NULL; // jni_fake falls back to "" and logs it
}

// ---------------------------------------------------------------------------
// void notifications
// ---------------------------------------------------------------------------

int wmw_jni_void(const char *name, const char *sig) {
  // The engine drives its own loading bar through this.
  if (!strcmp(name, "setDisplayPercent") && !strcmp(sig, "(F)V")) {
    // The float argument is not forwarded by the generic dispatcher; the value
    // is cosmetic here (the port shows no separate loading screen), so simply
    // acknowledge it to keep it out of the log.
    return 1;
  }

  if (!strcmp(name, "closeActivity")) {
    debugPrintf("engine requested closeActivity()\n");
    wmw_quit_requested = 1;
    return 1;
  }

  // --- request/response pairs ---------------------------------------------
  // These are NOT fire-and-forget. The engine blocks a screen until the
  // matching notify* arrives; see wmw_callbacks.c. classes.dex shows what the
  // Java side answered with when the service was unavailable, which is always
  // our case: no storefront, no network, no content service.
  if (!strcmp(name, "requestRestorePurchases")) {
    // The in-game "Restore Purchases" button. On Android this re-queries Play
    // and replays every entitlement through notifyPurchaseSuccess(sku, true);
    // do the same from purchases.txt, then report that the storefront itself
    // is unavailable (WMWPurchaseHandler.requestRestore() ->
    // notifyIAPAvailability(false) when billing is unsupported).
    wmw_purchases_restore();
    wmw_cb_post(WMW_CB_IAP_AVAILABILITY, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "requestIAPAvailability")) {
    wmw_cb_post(WMW_CB_IAP_AVAILABILITY, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "requestNetworkConnectionTest")) {
    // BaseActivity.requestNetworkConnectionTest() -> notifyReachability(...)
    wmw_cb_post(WMW_CB_REACHABILITY, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "queryAMPSForDownloads") || !strcmp(name, "requestAMPSDownload")) {
    wmw_cb_post(WMW_CB_AMPS_AVAILABILITY, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "requestIAPProductInfo")) {
    // WMWPurchaseHandler.onSkuDetailsResponse() -> notifyProductInfoFailed(sku)
    wmw_cb_post(WMW_CB_PRODUCT_INFO_FAILED, 0, "");
    return 1;
  }
  if (!strcmp(name, "requestPurchase")) {
    wmw_cb_post(WMW_CB_PURCHASE_CANCELLED, 0, "");
    return 1;
  }
  if (!strcmp(name, "delayBackpressEvent")) {
    // BaseActivity posts a Runnable that calls notifyEnableBackKey() after a
    // short delay; until it arrives the engine ignores the back key. Treating
    // this as a no-op meant the first press disabled the key permanently --
    // which presents as "backKeyPressed() from Screen_WorldSelect!" appearing
    // in the log while nothing happens on screen.
    // 1000 ms, matching Handler.postDelayed() in BaseActivity, and replacing
    // any pending re-enable exactly as removeCallbacks() does.
    wmw_cb_post_delayed(WMW_CB_ENABLE_BACK_KEY, 0, NULL, 1000);
    return 1;
  }
  if (!strcmp(name, "requestLOTWCountDown") || !strcmp(name, "swampyTime")) {
    wmw_cb_post(WMW_CB_LOTW_COUNTDOWN, 0, NULL);
    return 1;
  }

  // Fire-and-forget platform calls with no Switch equivalent. Acknowledged so
  // they do not fill debug.log; none of them affect gameplay.
  static const char *const noops[] = {
    // storefront / promo / external links
    "openURL", "rateApp", "purchaseSuccessConfirmation",
    // achievements
    "reportAchievement", "showAchievementsUI",
    // Disney AMPS content service + cloud save
    "synchronizePlayerGameData", "initLocalPlayerData",
    // misc platform
    "saveScreenshot", "checkAudioStatus",
    "notifyDoneGraphicContextRestore",
    NULL
  };
  for (int i = 0; noops[i]; i++)
    if (!strcmp(name, noops[i])) return 1;

  return 0;
}
