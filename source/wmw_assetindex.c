/* wmw_assetindex.c -- in-memory index of the asset tree
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw_assetindex.h for why this exists.
 *
 * Implementation is deliberately plain: one recursive walk at startup into an
 * open-addressed hash set of path strings. Roughly 4000 entries for this game,
 * a few hundred KB including the strings. No deletion, no resizing after build,
 * no locking needed on lookup because the table is immutable once built.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include "wmw_assetindex.h"
#include "wmw_paths.h"
#include "libc_shim.h"
#include "util.h"

#define IDX_CAP 16384          // power of two, comfortably over ~4000 entries

static char *s_slot[IDX_CAP];
static int s_count;
static int s_built;
static char s_root[WMW_PATH_MAX];
static size_t s_root_len;

static uint64_t hash_path(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void index_insert(const char *path) {
  if (s_count >= IDX_CAP / 2) return;   // keep the table sparse
  uint64_t i = hash_path(path) & (IDX_CAP - 1);
  while (s_slot[i]) {
    if (strcmp(s_slot[i], path) == 0) return;
    i = (i + 1) & (IDX_CAP - 1);
  }
  s_slot[i] = strdup(path);
  if (s_slot[i]) s_count++;
}

static int index_contains(const char *path) {
  uint64_t i = hash_path(path) & (IDX_CAP - 1);
  while (s_slot[i]) {
    if (strcmp(s_slot[i], path) == 0) return 1;
    i = (i + 1) & (IDX_CAP - 1);
  }
  return 0;
}

// Recursive walk. Depth-limited purely as a guard against a symlink loop or a
// malformed tree; the real asset tree is three levels deep.
static void walk(const char *dir, int depth) {
  if (depth > 8) return;

  wmw_file_lock();
  DIR *d = opendir(dir);
  wmw_file_unlock();
  if (!d) return;

  for (;;) {
    wmw_file_lock();
    struct dirent *e = readdir(d);
    wmw_file_unlock();
    if (!e) break;

    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
      continue;

    char child[WMW_PATH_MAX];
    if ((int)snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child))
      continue;

    int is_dir = 0;
#ifdef DT_DIR
    if (e->d_type == DT_DIR)       is_dir = 1;
    else if (e->d_type == DT_UNKNOWN)
#endif
    {
      struct stat st;
      wmw_file_lock();
      const int ok = (stat(child, &st) == 0);
      wmw_file_unlock();
      if (ok && S_ISDIR(st.st_mode)) is_dir = 1;
    }

    if (is_dir) walk(child, depth + 1);
    else        index_insert(child);
  }

  wmw_file_lock();
  closedir(d);
  wmw_file_unlock();
}

void wmw_assetindex_build(void) {
  if (s_built) return;
  s_built = 1;

  snprintf(s_root, sizeof(s_root), "%s/assets", wmw_game_dir());
  s_root_len = strlen(s_root);

  struct stat st;
  if (stat(s_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    debugPrintf("assetindex: %s not found -- lookups will hit the filesystem\n", s_root);
    s_root[0] = '\0';
    return;
  }

  walk(s_root, 0);
  debugPrintf("assetindex: %d files indexed under %s\n", s_count, s_root);
}

int wmw_assetindex_lookup(const char *path) {
  if (!s_built || !s_root[0] || !path) return -1;

  // Only authoritative inside the tree we walked. Anything else -- save data,
  // the database, the generated bundle, downloaded content -- can change while
  // the game runs, so it must still go to the filesystem.
  if (strncmp(path, s_root, s_root_len) != 0 || path[s_root_len] != '/')
    return -1;

  return index_contains(path) ? 1 : 0;
}
