/* wmw_callbacks.h -- answers for the engine's asynchronous requests
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Several of the engine's platform calls are request/response pairs, not
 * fire-and-forget. It calls out to Java, Java goes away and does something, and
 * the ANSWER arrives later through a separate native entry point. Both halves
 * are in classes.dex:
 *
 *     BaseActivity.requestNetworkConnectionTest()
 *         -> notifyReachability(hasInternetConnection())        [synchronous]
 *
 *     WMWPurchaseHandler.requestRestore()
 *         -> notifyIAPAvailability(false)   when billing is unsupported
 *     WMWPurchaseHandler.onBillingSetupFinished()
 *         -> notifyIAPAvailability(true)    when it connects
 *
 * Treating the outbound half as a no-op -- which is the obvious thing to do for
 * a storefront that cannot exist on Switch -- leaves the engine waiting for a
 * reply that never comes. It does not crash or log anything: the screen simply
 * never advances, and the button still animates because the tap was received.
 * That is what the in-app-purchase warning screen was doing.
 *
 * The answers are POSTED rather than called inline. The request arrives on the
 * render thread from inside engine code, and calling straight back into the
 * engine from there re-enters it at an arbitrary point. On Android these were
 * genuinely asynchronous -- billing callbacks landed on the UI thread on a
 * later turn of the loop -- so draining the queue at the top of the next frame
 * is both safer and more faithful.
 */

#ifndef __WMW_CALLBACKS_H__
#define __WMW_CALLBACKS_H__

typedef enum {
  WMW_CB_IAP_AVAILABILITY,   // notifyIAPAvailability(Z)
  WMW_CB_REACHABILITY,       // notifyReachability(Z)
  WMW_CB_AMPS_AVAILABILITY,  // notifyAMPSAvailability(Z)
  WMW_CB_PRODUCT_INFO_FAILED,// notifyProductInfoFailed(String)
  WMW_CB_PURCHASE_FAILED,    // notifyPurchaseFailed(String)
  WMW_CB_PURCHASE_CANCELLED, // notifyPurchaseCancelled(String)
  WMW_CB_LOTW_COUNTDOWN,     // notifyLOTWCountDown(I)
  WMW_CB_ENABLE_BACK_KEY,    // notifyEnableBackKey()
  WMW_CB_PURCHASE_SUCCESS    // notifyPurchaseSuccess(String, Z); ival = isRestore
} wmw_cb_kind;

// Queue an answer. Safe to call from the JNI dispatch. `sval` may be NULL.
void wmw_cb_post(wmw_cb_kind kind, int ival, const char *sval);

// As above, but delivered no sooner than `delay_ms` from now, and REPLACING any
// pending callback of the same kind. This mirrors Android's
// Handler.removeCallbacks() + postDelayed() pair, which BaseActivity uses for
// the back key: delayBackpressEvent() cancels any pending re-enable and
// schedules a fresh one 1000 ms out. Delivering it on the next frame instead
// would defeat the debounce and let a held or repeated press double-fire.
void wmw_cb_post_delayed(wmw_cb_kind kind, int ival, const char *sval, int delay_ms);

// Deliver everything queued. Call once per frame, before rendererDrawFrame.
void wmw_cb_drain(void);

#endif
