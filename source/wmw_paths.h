/* wmw_paths.h -- game directory discovery and Android->Switch path mapping
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW_PATHS_H__
#define __WMW_PATHS_H__

#include <stddef.h>

// Call once at startup, before anything touches the filesystem.
void wmw_paths_init(void);

// The directory the .nro was launched from, e.g. "sdmc:/switch/wmw".
// Discovered at runtime -- never assume a fixed install location.
const char *wmw_game_dir(void);

// Map a path the engine asked for onto a real one. Returns either `path`
// unchanged or a pointer into `buf`. Never returns NULL.
const char *wmw_resolve(const char *path, char *buf, size_t buflen);

// Convenience: the resolver with a caller-side scratch buffer.
#define WMW_PATH_MAX 512

#endif
