/* wmw_purchases.h -- restore add-on content bought on Google Play
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW_PURCHASES_H__
#define __WMW_PURCHASES_H__

// Read <gamedir>/purchases.txt and tell the engine about each entitlement it
// lists, using the same notifyPurchaseSuccess(sku, isRestore=true) call Google
// Play's queryPurchasesAsync() result would have produced.
//
// Call after rendererInit, once the engine's IAP subsystem exists.
void wmw_purchases_restore(void);

#endif
