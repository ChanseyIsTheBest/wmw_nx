/* wmw_tate.h -- portrait ("TATE") presentation for Where's My Water?
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * WMW is a portrait game -- its UI XML carries forceAspect="384:512" and the
 * playfield is taller than it is wide. The Switch will not give us a portrait
 * window (nwindowSetDimensions fails once mesa has created the EGL surface),
 * so the engine renders into a portrait FBO and we rotate that onto the
 * landscape panel ourselves.
 *
 * This module owns the DISPLAY rotation. The matching INPUT rotation belongs to
 * nx_pointer, which is handed the same mode at init and applies it to touch,
 * stick, mouse and gyro. Two owners of one transform is how they drift apart --
 * and that matters here, because WMW is played as a continuous drag that carves
 * channels through dirt: a pointer rotated the wrong way is not a mis-tap, it
 * is an unplayable game.
 *
 * The cursor is drawn by nxp_draw() BEFORE wmw_tate_present(), i.e. into the
 * portrait target, so the rotation carries it along with the frame. Drawing it
 * after the blit leaves it sideways on the panel.
 */

#ifndef __WMW_TATE_H__
#define __WMW_TATE_H__

// Presentation modes.
enum {
  // UPRIGHT is implemented but no longer selectable from config.txt: the game is
  // portrait and pillarboxing it wastes most of the panel. Kept because it costs
  // nothing and is the obvious starting point if a landscape mode is ever wanted.
  WMW_TATE_UPRIGHT = 0, // portrait centred on the landscape panel, pillarboxed
  WMW_TATE_CW      = 1, // rotated 90 clockwise -- fills the screen
  WMW_TATE_CCW     = 2  // rotated 90 counter-clockwise -- fills the screen
};

// render_* is the portrait size the engine is told it has; window_* is the real
// landscape swapchain. Returns 1 on success, 0 if unavailable -- in which case
// every entry point below is inert and the port renders straight to the window.
int  wmw_tate_init(int render_w, int render_h, int window_w, int window_h, int mode);

int  wmw_tate_active(void);
unsigned wmw_tate_fbo(void);   // substitute whenever the engine binds 0
void wmw_tate_begin(void);     // bind the portrait FBO; call before drawFrame
void wmw_tate_present(void);   // rotated blit; call before eglSwapBuffers
void wmw_tate_shutdown(void);

// NOTE: the inverse transform (panel -> render space) is nx_pointer's job, not
// this module's. It receives the same rotation mode through NxpConfig.rotation
// and applies it to touch, stick, mouse and gyro together. Keeping a second
// copy here is how the two end up disagreeing.

#endif
