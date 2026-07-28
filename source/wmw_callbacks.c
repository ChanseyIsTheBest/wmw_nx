/* wmw_callbacks.c -- answers for the engine's asynchronous requests
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw_callbacks.h for why these are posted rather than called inline.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "wmw_callbacks.h"
#include "wmw_entrypoints.h"
#include "jni_fake.h"
#include "util.h"

#define MAX_PENDING 32

typedef struct {
  wmw_cb_kind kind;
  int ival;
  char sval[96];
  uint64_t due_ns;   // 0 = deliver at the next drain
} Pending;

static Pending s_queue[MAX_PENDING];
static int s_count;
static Mutex s_lock;
static int s_lock_ready;

extern void *g_thiz_public(void); // provided by main.c

static void post_common(wmw_cb_kind kind, int ival, const char *sval,
                        uint64_t due_ns, int replace) {
  if (!s_lock_ready) { mutexInit(&s_lock); s_lock_ready = 1; }
  mutexLock(&s_lock);

  // Handler.removeCallbacks(): drop any pending callback of the same kind.
  if (replace) {
    for (int i = 0; i < s_count; ) {
      if (s_queue[i].kind == kind) s_queue[i] = s_queue[--s_count];
      else i++;
    }
  }

  if (s_count < MAX_PENDING) {
    Pending *p = &s_queue[s_count++];
    p->kind = kind;
    p->ival = ival;
    p->due_ns = due_ns;
    if (sval) snprintf(p->sval, sizeof(p->sval), "%s", sval);
    else      p->sval[0] = '\0';
  }
  mutexUnlock(&s_lock);
}

void wmw_cb_post(wmw_cb_kind kind, int ival, const char *sval) {
  post_common(kind, ival, sval, 0, 0);
}

void wmw_cb_post_delayed(wmw_cb_kind kind, int ival, const char *sval, int delay_ms) {
  const uint64_t now = armTicksToNs(armGetSystemTick());
  post_common(kind, ival, sval, now + (uint64_t)delay_ms * 1000000ULL, 1);
}

void wmw_cb_drain(void) {
  if (!s_lock_ready) return;

  Pending batch[MAX_PENDING];
  int n = 0;

  const uint64_t now = armTicksToNs(armGetSystemTick());

  mutexLock(&s_lock);
  for (int i = 0; i < s_count; ) {
    if (s_queue[i].due_ns == 0 || s_queue[i].due_ns <= now) {
      batch[n++] = s_queue[i];
      s_queue[i] = s_queue[--s_count];   // take it; not yet due ones stay
    } else {
      i++;
    }
  }
  mutexUnlock(&s_lock);

  void *thiz = g_thiz_public();
  if (!thiz) return;

  for (int i = 0; i < n; i++) {
    const Pending *p = &batch[i];
    switch (p->kind) {
      case WMW_CB_IAP_AVAILABILITY:
        if (wmw.notifyIAPAvailability) {
          debugPrintf("cb: notifyIAPAvailability(%d)\n", p->ival);
          wmw.notifyIAPAvailability(fake_env, thiz, p->ival);
        }
        break;
      case WMW_CB_REACHABILITY:
        if (wmw.notifyReachability) {
          debugPrintf("cb: notifyReachability(%d)\n", p->ival);
          wmw.notifyReachability(fake_env, thiz, p->ival);
        }
        break;
      case WMW_CB_AMPS_AVAILABILITY:
        if (wmw.notifyAMPSAvailability) {
          debugPrintf("cb: notifyAMPSAvailability(%d)\n", p->ival);
          wmw.notifyAMPSAvailability(fake_env, thiz, p->ival);
        }
        break;
      case WMW_CB_PRODUCT_INFO_FAILED:
        if (wmw.notifyProductInfoFailed) {
          debugPrintf("cb: notifyProductInfoFailed(\"%s\")\n", p->sval);
          wmw.notifyProductInfoFailed(fake_env, thiz, jni_make_string(p->sval));
        }
        break;
      case WMW_CB_PURCHASE_FAILED:
        if (wmw.notifyPurchaseFailed) {
          debugPrintf("cb: notifyPurchaseFailed(\"%s\")\n", p->sval);
          wmw.notifyPurchaseFailed(fake_env, thiz, jni_make_string(p->sval));
        }
        break;
      case WMW_CB_PURCHASE_CANCELLED:
        if (wmw.notifyPurchaseCancelled) {
          debugPrintf("cb: notifyPurchaseCancelled(\"%s\")\n", p->sval);
          wmw.notifyPurchaseCancelled(fake_env, thiz, jni_make_string(p->sval));
        }
        break;
      case WMW_CB_LOTW_COUNTDOWN:
        if (wmw.notifyLOTWCountDown) {
          debugPrintf("cb: notifyLOTWCountDown(%d)\n", p->ival);
          wmw.notifyLOTWCountDown(fake_env, thiz, p->ival);
        }
        break;
      case WMW_CB_PURCHASE_SUCCESS:
        if (wmw.notifyPurchaseSuccess) {
          debugPrintf("cb: notifyPurchaseSuccess(\"%s\", restore=%d)\n", p->sval, p->ival);
          wmw.notifyPurchaseSuccess(fake_env, thiz, jni_make_string(p->sval), p->ival);
        }
        break;
      case WMW_CB_ENABLE_BACK_KEY:
        if (wmw.notifyEnableBackKey) {
          debugPrintf("cb: notifyEnableBackKey()\n");
          wmw.notifyEnableBackKey(fake_env, thiz);
        }
        break;
    }
  }
}
