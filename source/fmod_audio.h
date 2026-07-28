/* fmod_audio.h -- native replacement for org.fmod.FMODAudioDevice
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FMOD_AUDIO_H__
#define __FMOD_AUDIO_H__

#include <stdint.h>
#include "so_util.h"

// Brings up audout and starts the pump thread.
//
// The two entry points are passed in already resolved rather than looked up
// here, because main.c resolves them alongside the game's own entry points --
// i.e. BEFORE so_finalize(). so_finalize maps the image as code memory, which
// revokes access to the source range the symbol table was read through, so
// symbol lookups belong on the near side of it.
//
// Safe to call before the game has initialised FMOD: the pump thread waits for
// the output device to appear. Returns 0 on success.
int  fmod_audio_start(uintptr_t fn_getinfo, uintptr_t fn_process);

// Feeds silence without stopping the device (applet focus loss / suspend).
void fmod_audio_set_paused(int paused);

void fmod_audio_stop(void);

#endif
