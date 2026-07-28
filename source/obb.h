/* obb.h -- optional Android expansion-file (OBB) support
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (original FF4:AY implementation)
 * Where's My Water? port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Where's My Water? reads its data as loose files: libwmw.so imports no
 * AAsset* symbols at all, and builds its paths from the "/assets/Data/" literal
 * in its .rodata using plain fopen(). So the normal install -- the APK's
 * assets/ folder copied next to the .nro -- needs nothing from this file.
 *
 * Some distributions of the game additionally ship a main.obb expansion file.
 * libwmw.so exports Java_com_disney_common_BaseActivity_notifyAddObbFilePathToFileManager,
 * which is how the Java layer told the engine's own FileManager where that
 * archive lives. If a main.obb is found next to the .nro, main.c forwards its
 * path through that entry point and the engine reads it itself.
 *
 * obb_read() below remains only because libc_shim.c's AAssetManager emulation
 * references it. That emulation is vestigial for this title (nothing resolves
 * against it) and the stub keeps the link satisfied without pulling in an
 * archive reader the game never asks for.
 */

#ifndef __OBB_H__
#define __OBB_H__

#include <stddef.h>

// Returns the path to main.obb if one sits next to the .nro, else NULL.
const char *obb_find(void);

// Vestigial AAsset support -- see the note above.
int   obb_open(const char *path);
void  obb_close(void);
int   obb_exists(const char *name);
void *obb_read(const char *name, size_t *out_size);

#endif
