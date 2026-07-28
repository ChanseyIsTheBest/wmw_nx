/* wmw_bundle.h -- give the engine an openable archive
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW_BUNDLE_H__
#define __WMW_BUNDLE_H__

// Returns the path to hand rendererInit as its packagePath.
//
// Pass the result of find_apk(), or NULL if there isn't one. With an APK the
// real archive is used; without one a small valid ZIP containing water.db is
// generated next to the .nro. Returns NULL only if neither is possible.
//
// See wmw_bundle.c for why a failed archive open is so much worse than an
// archive with nothing useful in it.
const char *wmw_bundle_path(const char *apk_or_null);

#endif
