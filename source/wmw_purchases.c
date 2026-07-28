/* wmw_purchases.c -- restore add-on content bought on Google Play
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Where's My Water? sells its extra character packs (Allie, Cranky, Mystery
 * Duck) as in-app purchases. On Android the game learns which of them you own
 * at startup, from Google Play: BillingClient.queryPurchasesAsync() returns the
 * entitlements attached to your account, and WMWPurchaseHandler feeds each one
 * to the engine --
 *
 *     for (Purchase p : purchases)
 *         for (String sku : p.getSkus())
 *             activity.notifyPurchaseSuccess(sku, true);   // true = restore
 *
 * -- which is exactly the "Restore Purchases" flow. There is no Play Store on
 * Switch to ask, so this file does the same thing from a list you supply,
 * calling the same entry point with the same restore flag. Nothing is patched
 * and no save data is edited; the engine unlocks the content through its own
 * normal path.
 *
 * You are asserting what you own. Create <gamedir>/purchases.txt with one
 * identifier per line:
 *
 *     # Where's My Water? -- content I own
 *     alliepack01
 *     crankypack01
 *     mysteryduck01
 *
 * The identifiers are the "Internal" names from the game's own IAPInfo table in
 * water.db, which maps them to the per-store SKUs. The engine is told the store
 * SKU, because that is what Play would have handed it -- rendererInit is passed
 * "google" as its SKUMarket, so it resolves against the Google column.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "wmw_purchases.h"
#include "wmw_callbacks.h"
#include "wmw_paths.h"
#include "config.h"
#include "util.h"

/* The game's IAPInfo table, Internal -> Google SKU.
 *
 *   sqlite> select Internal, Google from IAPInfo;
 *
 * Reproduced here rather than read from water.db at runtime: the database is
 * the engine's to open, and a second reader of a file Horizon will not let us
 * stat while it is held open is exactly the trouble documented in §10.2. */
typedef struct { const char *internal; const char *google; const char *what; } Sku;

static const Sku g_skus[] = {
  { "alliepack01",     "com.disney.wmwpaid.goo.2207501", "Allie level pack" },
  { "crankypack01",    "2206519",                        "Cranky level pack" },
  { "mysteryduck01",   "2206625",                        "Mystery Duck level pack" },

  { "locksmith_one",     "2206619",                        "Locksmith: base" },
  { "locksmith_swampy",  "2206620",                        "Locksmith: Swampy" },
  { "locksmith_cranky",  "2206621",                        "Locksmith: Cranky" },
  { "locksmith_mystery", "2206624",                        "Locksmith: Mystery Duck" },
  { "locksmith_allie",   "com.disney.wmwpaid.goo.2207502", "Locksmith: Allie" },

  { "bundle01",        "2207053",                        "Bundle" },
  { "alliebundle01",   "com.disney.wmwpaid.goo.2208451", "Allie bundle" },
  { "crankybundle01",  "com.disney.wmwpaid.goo.2208452", "Cranky bundle" },
  { "mysterybundle01", "com.disney.wmwpaid.goo.2208453", "Mystery Duck bundle" },
  { "megabundle01",    "com.disney.wmwpaid.goo.2208454", "Mega bundle" },
  { NULL, NULL, NULL }
};

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static const Sku *find_sku(const char *name) {
  for (int i = 0; g_skus[i].internal; i++) {
    if (!strcasecmp(g_skus[i].internal, name)) return &g_skus[i];
    if (!strcasecmp(g_skus[i].google, name))   return &g_skus[i]; // accept raw SKUs too
  }
  return NULL;
}

void wmw_purchases_restore(void) {
  char path[WMW_PATH_MAX];
  snprintf(path, sizeof(path), "%s/purchases.txt", wmw_game_dir());

  FILE *f = fopen(path, "r");
  if (!f) {
    debugPrintf("purchases: no purchases.txt -- add-on content stays locked\n");
    return;
  }

  int restored = 0;
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    trim(line);
    if (!line[0]) continue;

    const Sku *s = find_sku(line);
    if (!s) {
      debugPrintf("purchases: unknown entry \"%s\" -- ignored\n", line);
      continue;
    }

    // Exactly what onQueryPurchasesResponse() does per owned SKU: the store
    // identifier, with the restore flag set.
    wmw_cb_post(WMW_CB_PURCHASE_SUCCESS, 1 /* isRestore */, s->google);
    debugPrintf("purchases: restoring %s (%s)\n", s->what, s->internal);
    restored++;
  }
  fclose(f);

  debugPrintf("purchases: %d entitlement(s) restored\n", restored);
}
