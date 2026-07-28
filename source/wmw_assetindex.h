/* wmw_assetindex.h -- in-memory index of the asset tree
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The engine probes hard for every asset it loads. A single sprite is looked
 * for as:
 *
 *     assets/Sprites/foo-HD.sprite        (high-res variant)
 *     amps/lotw/Sprites/foo-HD.sprite     (downloaded-content override)
 *     assets/Sprites/foo.sprite
 *     amps/lotw/Sprites/foo.sprite
 *
 * Three of those four normally miss. On Android a miss costs a page-cache
 * lookup; on Horizon every one is a round trip to the filesystem service, and
 * the engine does this thousands of times during a level load. One boot log
 * recorded 1996 failed opens -- and that counts only the ones that failed, not
 * the successes alongside them.
 *
 * Indexing the tree once at startup turns every one of those misses into a hash
 * lookup. The index is built from a single recursive directory walk, which the
 * filesystem serves far more cheaply than several thousand individual opens.
 */

#ifndef __WMW_ASSETINDEX_H__
#define __WMW_ASSETINDEX_H__

// Walk <gamedir>/assets once and remember every file. Safe to call more than
// once; only the first call does work. Failure is not fatal -- the index simply
// reports "unknown" and every lookup falls through to the filesystem.
void wmw_assetindex_build(void);

// Fast existence test for a fully-resolved path.
//   1  = the index knows this file exists
//   0  = the index knows it does NOT exist (skip the syscall entirely)
//  -1  = outside the indexed tree, or no index; ask the filesystem
int wmw_assetindex_lookup(const char *resolved_path);

#endif
