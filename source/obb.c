/* obb.c -- optional Android expansion-file (OBB) support
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (original FF4:AY implementation)
 * Where's My Water? port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See obb.h: WMW reads loose files, so this only locates an optional main.obb
 * and lets the engine's own FileManager take it from there.
 */

#include <stddef.h>
#include <sys/stat.h>

#include "obb.h"
#include "config.h"

const char *obb_find(void) {
  struct stat st;
  if (stat(OBB_NAME, &st) == 0 && st.st_size > 0)
    return OBB_NAME;
  return NULL;
}

// --- vestigial AAsset hooks (see obb.h) ------------------------------------

int obb_open(const char *path) { (void)path; return -1; }

void obb_close(void) { }

int obb_exists(const char *name) { (void)name; return 0; }

void *obb_read(const char *name, size_t *out_size) {
  (void)name;
  if (out_size) *out_size = 0;
  return NULL; // fall through to the loose-file lookup
}
