/* wmw_bundle.c -- give the engine an openable archive
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The engine treats rendererInit's packagePath as an archive and opens it with
 * its own statically linked minizip ("ZipArchiveReader::readCurrentFile",
 * "wUnzOpen*" in .rodata). On Android that was the APK. Point it at a directory
 * and the open fails -- and the failure is not handled gracefully:
 *
 *     fopen(sdmc:/switch/wmw_nx, rb) -> FAIL
 *     [WMW] RendererResize failed with exception: std::bad_alloc
 *
 * The allocation immediately after the failed open is sized from something the
 * engine never managed to read, so it throws. The same failure breaks
 * ApplicationContext::copyDatabaseFromBundle(), which extracts water.db.
 *
 * The fix is simply to hand it something openable:
 *
 *   1. If an .apk is sitting next to the .nro, use that -- it is the real
 *      thing and contains everything.
 *   2. Otherwise synthesise a small but completely valid ZIP containing
 *      water.db. A successful open with an entry not found is a case the
 *      engine already handles (every loose asset already resolves that way);
 *      a failed open is not.
 *
 * The generated archive stores entries uncompressed, which keeps this to a few
 * dozen lines of header writing and costs nothing at 216 KB.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <zlib.h>

#include "wmw_bundle.h"
#include "wmw_paths.h"
#include "config.h"
#include "util.h"

#define BUNDLE_NAME "bundle.zip"

// --- little-endian writers -------------------------------------------------

static void w16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void w32(FILE *f, uint32_t v) {
  fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
  fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}

typedef struct {
  const char *name;   // path inside the archive
  void *data;
  size_t size;
  uint32_t crc;
  uint32_t local_off;
} ZipEntry;

static void write_local_header(FILE *f, ZipEntry *e) {
  e->local_off = (uint32_t)ftell(f);
  w32(f, 0x04034b50);            // local file header signature
  w16(f, 20);                    // version needed
  w16(f, 0);                     // flags
  w16(f, 0);                     // method: stored
  w16(f, 0); w16(f, 0);          // mod time / date
  w32(f, e->crc);
  w32(f, (uint32_t)e->size);     // compressed size
  w32(f, (uint32_t)e->size);     // uncompressed size
  w16(f, (uint16_t)strlen(e->name));
  w16(f, 0);                     // extra length
  fwrite(e->name, 1, strlen(e->name), f);
  fwrite(e->data, 1, e->size, f);
}

static void write_central_header(FILE *f, const ZipEntry *e) {
  w32(f, 0x02014b50);            // central directory header signature
  w16(f, 20); w16(f, 20);        // version made by / needed
  w16(f, 0); w16(f, 0);          // flags / method
  w16(f, 0); w16(f, 0);          // time / date
  w32(f, e->crc);
  w32(f, (uint32_t)e->size);
  w32(f, (uint32_t)e->size);
  w16(f, (uint16_t)strlen(e->name));
  w16(f, 0); w16(f, 0);          // extra / comment length
  w16(f, 0);                     // disk number start
  w16(f, 0); w32(f, 0);          // internal / external attributes
  w32(f, e->local_off);
  fwrite(e->name, 1, strlen(e->name), f);
}

static void *read_whole(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  void *buf = malloc((size_t)n);
  if (!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) { free(buf); return NULL; }
  *out_size = got;
  return buf;
}

static int build_bundle(const char *out_path) {
  char db_path[WMW_PATH_MAX];
  snprintf(db_path, sizeof(db_path), "%s/assets/Data/water.db", wmw_game_dir());

  size_t db_size = 0;
  void *db = read_whole(db_path, &db_size);
  if (!db) {
    debugPrintf("bundle: no %s to package\n", db_path);
    return 0;
  }
  const uint32_t crc = (uint32_t)crc32(0, (const Bytef *)db, (uInt)db_size);

  // Two names for the same payload: the engine's own prefix convention is not
  // certain from the binary, and a duplicate 216 KB entry is cheap insurance.
  ZipEntry entries[2] = {
    { "assets/Data/water.db", db, db_size, crc, 0 },
    { "Data/water.db",        db, db_size, crc, 0 },
  };
  const int n = (int)(sizeof(entries) / sizeof(*entries));

  FILE *f = fopen(out_path, "wb");
  if (!f) {
    debugPrintf("bundle: could not create %s\n", out_path);
    free(db);
    return 0;
  }

  for (int i = 0; i < n; i++)
    write_local_header(f, &entries[i]);

  const uint32_t cd_off = (uint32_t)ftell(f);
  for (int i = 0; i < n; i++)
    write_central_header(f, &entries[i]);
  const uint32_t cd_size = (uint32_t)ftell(f) - cd_off;

  w32(f, 0x06054b50);            // end of central directory
  w16(f, 0); w16(f, 0);          // disk numbers
  w16(f, (uint16_t)n); w16(f, (uint16_t)n);
  w32(f, cd_size);
  w32(f, cd_off);
  w16(f, 0);                     // comment length

  const long total = ftell(f);
  fclose(f);
  free(db);
  debugPrintf("bundle: generated %s (%ld bytes, %d entries)\n", out_path, total, n);
  return 1;
}

const char *wmw_bundle_path(const char *apk_or_null) {
  // A real APK always wins -- it holds the whole asset tree, not just the
  // database, so the engine's bundle path works exactly as it did on Android.
  if (apk_or_null) {
    debugPrintf("bundle: using %s\n", apk_or_null);
    return apk_or_null;
  }

  static char path[WMW_PATH_MAX];
  snprintf(path, sizeof(path), "%s/" BUNDLE_NAME, wmw_game_dir());

  struct stat st;
  if (stat(path, &st) == 0 && st.st_size > 64)
    return path;                 // already generated on a previous run

  if (build_bundle(path))
    return path;

  debugPrintf("bundle: unavailable -- the engine's archive path will fail\n");
  return NULL;
}
