/* wmw_paths.c -- game directory discovery and Android->Switch path mapping
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Two problems this solves, both visible in the boot log of the first run that
 * got as far as asset loading.
 *
 * 1. THE ASSET ROOT IS EMPTY.
 *
 *    The engine builds asset paths as <root> + "/Textures/foo.imagelist". On
 *    Android <root> came from the APK, which it opens with its own statically
 *    linked minizip. We hand it a directory, the zip open fails, and <root>
 *    stays "" -- so every asset request arrives as a bare POSIX absolute path:
 *
 *        fopen(/Script/WC.txt, rb)              -> FAIL
 *        fopen(/Data/textureSettings.xml, rb)   -> FAIL
 *        fopen(/Textures/ui_atlas.imagelist, rb)-> FAIL
 *
 *    The extracted APK has exactly these under assets/, so the mapping is a
 *    straight prefix: "/Script/WC.txt" -> "<gamedir>/assets/Script/WC.txt".
 *
 * 2. THE GAME DIRECTORY WAS HARD-CODED.
 *
 *    It was a #define of "sdmc:/switch/wmw". If the .nro lives anywhere else,
 *    every absolute reference silently misses -- which is why creating a file
 *    that should always succeed failed:
 *
 *        fopen(sdmc:/switch/wmw/checked_water_tmp.db, w) -> FAIL
 *
 *    An fopen("w") into an existing writable directory does not fail. That it
 *    did is proof the directory was not there. The launch directory is now
 *    discovered with getcwd() instead, which hbloader sets to the .nro's own
 *    folder.
 *
 * A third, smaller thing: the engine's bundled SQLite calls getcwd() and
 * prepends it when a path does not start with '/', producing
 * "//sdmc:/switch/wmw/water.db". Any devoptab prefix ("sdmc:", "romfs:") that
 * is not at position 0 means junk was prepended, so we cut back to it.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "wmw_paths.h"
#include "util.h"

static char s_game_dir[WMW_PATH_MAX] = ".";
static char s_assets_dir[WMW_PATH_MAX] = "./assets";

// The engine's hard-coded AMPS root, from libwmw.so's .rodata. Disney's
// content-download service wrote Level-of-the-Week and reward packs here. The
// string in the binary carries trailing spaces, hence the prefix match.
#define AMPS_PREFIX "/mnt/sdcard/Android/data/com.disney.WMW/"

void wmw_paths_init(void) {
  char cwd[WMW_PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) && cwd[0]) {
    // strip a trailing slash so joins stay single-separator
    size_t n = strlen(cwd);
    while (n > 1 && (cwd[n - 1] == '/' || cwd[n - 1] == '\\'))
      cwd[--n] = '\0';
    snprintf(s_game_dir, sizeof(s_game_dir), "%s", cwd);
  }
  snprintf(s_assets_dir, sizeof(s_assets_dir), "%s/assets", s_game_dir);

  // Scratch space for the engine's SQLite. See wmw_shims.c: without a usable
  // temp directory every write transaction fails as "disk I/O error".
  char tmpdir[WMW_PATH_MAX];
  snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", s_game_dir);
  if (mkdir(tmpdir, 0777) == 0)
    debugPrintf("paths: created %s\n", tmpdir);

  debugPrintf("paths: game dir   = %s\n", s_game_dir);
  debugPrintf("paths: asset root = %s\n", s_assets_dir);
  debugPrintf("paths: temp dir   = %s/tmp\n", s_game_dir);
}

const char *wmw_game_dir(void) { return s_game_dir; }

// Find a devoptab prefix ("sdmc:", "romfs:", ...) anywhere in the string.
// Returns its offset, or -1. A device name is [A-Za-z][A-Za-z0-9_-]* ':'.
static int find_device_prefix(const char *p) {
  for (int i = 0; p[i]; i++) {
    if (p[i] != ':')
      continue;
    // a devoptab mount is always "<name>:/", so require the slash. Without
    // this, an ordinary filename containing a colon would be mistaken for a
    // mount point and the path truncated at it.
    if (p[i + 1] != '/')
      continue;
    int j = i - 1;
    while (j >= 0 && (isalnum((unsigned char)p[j]) || p[j] == '_' || p[j] == '-'))
      j--;
    j++;
    if (j > i - 1 || !isalpha((unsigned char)p[j]))
      continue;                       // empty or non-alpha device name
    if (j != 0 && p[j - 1] != '/')
      continue;                       // not at the start of a path component
    return j;
  }
  return -1;
}

const char *wmw_resolve(const char *path, char *buf, size_t buflen) {
  if (!path || !*path || !buf || buflen == 0)
    return path;

  // --- 1. a devoptab prefix that is not at position 0 means something was
  //        prepended (SQLite's getcwd() join). Cut back to it.
  const int dev = find_device_prefix(path);
  if (dev > 0) {
    snprintf(buf, buflen, "%s", path + dev);
    return buf;
  }
  if (dev == 0)
    return path; // already a proper Switch path

  // --- 1b. unix temp directories ------------------------------------------
  // SQLite (and anything else built for a unix host) expects /tmp and friends
  // to exist. Point them all at a real directory inside the game folder.
  {
    static const char *const tmp_roots[] = { "/tmp", "/var/tmp", "/usr/tmp", NULL };
    for (int i = 0; tmp_roots[i]; i++) {
      const size_t n = strlen(tmp_roots[i]);
      if (!strncmp(path, tmp_roots[i], n) && (path[n] == '\0' || path[n] == '/')) {
        snprintf(buf, buflen, "%s/tmp%s", s_game_dir, path + n);
        return buf;
      }
    }
  }

  // --- 2. the AMPS / downloaded-content tree ------------------------------
  if (!strncmp(path, AMPS_PREFIX, sizeof(AMPS_PREFIX) - 1)) {
    const char *rest = path + sizeof(AMPS_PREFIX) - 1;
    // the recorded root has trailing spaces baked into the literal
    char cleaned[WMW_PATH_MAX];
    size_t o = 0;
    for (size_t i = 0; rest[i] && o + 1 < sizeof(cleaned); i++) {
      // collapse the run of spaces that precedes the relative part
      if (rest[i] == ' ' && (rest[i + 1] == '/' || rest[i + 1] == ' '))
        continue;
      cleaned[o++] = rest[i];
    }
    cleaned[o] = '\0';
    snprintf(buf, buflen, "%s/%s", s_game_dir, cleaned);
    return buf;
  }

  // --- 3. a bare POSIX absolute path is an asset request with an empty root
  if (path[0] == '/') {
    snprintf(buf, buflen, "%s%s", s_assets_dir, path);
    return buf;
  }

  // --- 4. relative paths with an asset-tree first component ---------------
  // Not everything arrives absolute: the engine asks for "Curves/ease_out.xml"
  // with no leading slash. Anything whose first component names one of the
  // APK's asset directories belongs under the asset root; everything else
  // (water.db, checked_water_tmp.db, save files) stays in the game directory.
  static const char *const asset_dirs[] = {
    "Animations/", "Audio/", "Curves/", "Data/", "Emitters/", "Fonts/",
    "Levels/", "Objects/", "PuppetShows/", "Script/", "Skeletons/",
    "Sprites/", "Textures/", NULL
  };
  for (int i = 0; asset_dirs[i]; i++) {
    const size_t n = strlen(asset_dirs[i]);
    if (!strncmp(path, asset_dirs[i], n)) {
      snprintf(buf, buflen, "%s/%s", s_assets_dir, path);
      return buf;
    }
  }

  // --- 5. everything else resolves against the launch directory -----------
  return path;
}
