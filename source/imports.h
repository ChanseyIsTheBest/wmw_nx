/* imports.h -- .so import resolution interface
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen  (original implementation)
 * Where's My Water? port: table contents.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

// The static import table consulted by so_resolve() before it falls back to
// walking the exports of already-loaded modules. Generated from libwmw.so's own
// undefined symbols; see tools/gen_imports.py.
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

// Called once after both modules are relocated and before they are resolved.
void update_imports(void);

#endif
