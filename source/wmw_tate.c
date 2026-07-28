/* wmw_tate.c -- portrait ("TATE") presentation for Where's My Water?
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw_tate.h for why this exists. Implementation notes:
 *
 * The engine renders into a portrait FBO sized so that rotating it 90 degrees
 * lands exactly on the panel -- 720x1280 handheld becomes 1280x720, 1080x1920
 * docked becomes 1920x1080. No letterboxing, no scaling: turn the console on
 * its side and the game fills the screen.
 *
 * Docked is different. You cannot rotate a television, so a rotated blit there
 * would be a sideways game. Docked therefore defaults to UPRIGHT, which centres
 * a 3:4 portrait image on the landscape panel with bars either side.
 *
 * This is GLES 1.1, so the blit is a textured quad through the fixed-function
 * pipeline rather than a shader. The rotation is expressed purely as a
 * permutation of the four texture coordinates -- the destination quad never
 * changes -- which keeps the forward transform trivially comparable against the
 * inverse in wmw_tate_map_pointer() at the bottom of this file.
 *
 * One subtlety: the engine uses GL_OES_framebuffer_object itself for its own
 * render-to-texture effects, and when it finishes it binds framebuffer 0. That
 * would drop it onto the window instead of our portrait target, so imports.c's
 * glBindFramebufferOES wrapper substitutes wmw_tate_fbo() for 0 whenever we are
 * active.
 */

#include <stdio.h>
#include <string.h>
#include <GLES/gl.h>
#include <EGL/egl.h>

#include "wmw_tate.h"
#include "util.h"

// GL_OES_framebuffer_object constants (not all headers expose these).
#ifndef GL_FRAMEBUFFER_OES
#define GL_FRAMEBUFFER_OES            0x8D40
#define GL_RENDERBUFFER_OES           0x8D41
#define GL_COLOR_ATTACHMENT0_OES      0x8CE0
#define GL_DEPTH_ATTACHMENT_OES       0x8D00
#define GL_FRAMEBUFFER_COMPLETE_OES   0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT16_OES
#define GL_DEPTH_COMPONENT16_OES      0x81A5
#endif

// Resolved through eglGetProcAddress so this file does not depend on whether
// the headers declare the extension or the library exports the suffixed names.
static void   (*p_GenFramebuffers)(GLsizei, GLuint *);
static void   (*p_BindFramebuffer)(GLenum, GLuint);
static void   (*p_DeleteFramebuffers)(GLsizei, const GLuint *);
static void   (*p_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (*p_CheckFramebufferStatus)(GLenum);
static void   (*p_GenRenderbuffers)(GLsizei, GLuint *);
static void   (*p_BindRenderbuffer)(GLenum, GLuint);
static void   (*p_DeleteRenderbuffers)(GLsizei, const GLuint *);
static void   (*p_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void   (*p_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

static int s_ready;
static int s_mode;
static int s_rw, s_rh;   // render (portrait) size
static int s_ww, s_wh;   // window (landscape) size
static GLuint s_tex, s_fbo, s_depth;

// Destination rectangle in window space. Full screen for the rotated modes,
// pillarboxed for upright.
static float s_dx0, s_dy0, s_dx1, s_dy1;

static void *gl_proc(const char *oes, const char *core) {
  void *p = (void *)eglGetProcAddress(oes);
  if (!p && core) p = (void *)eglGetProcAddress(core);
  return p;
}

static int resolve_fbo_procs(void) {
  p_GenFramebuffers         = (void (*)(GLsizei, GLuint *))gl_proc("glGenFramebuffersOES", "glGenFramebuffers");
  p_BindFramebuffer         = (void (*)(GLenum, GLuint))gl_proc("glBindFramebufferOES", "glBindFramebuffer");
  p_DeleteFramebuffers      = (void (*)(GLsizei, const GLuint *))gl_proc("glDeleteFramebuffersOES", "glDeleteFramebuffers");
  p_FramebufferTexture2D    = (void (*)(GLenum, GLenum, GLenum, GLuint, GLint))gl_proc("glFramebufferTexture2DOES", "glFramebufferTexture2D");
  p_CheckFramebufferStatus  = (GLenum (*)(GLenum))gl_proc("glCheckFramebufferStatusOES", "glCheckFramebufferStatus");
  p_GenRenderbuffers        = (void (*)(GLsizei, GLuint *))gl_proc("glGenRenderbuffersOES", "glGenRenderbuffers");
  p_BindRenderbuffer        = (void (*)(GLenum, GLuint))gl_proc("glBindRenderbufferOES", "glBindRenderbuffer");
  p_DeleteRenderbuffers     = (void (*)(GLsizei, const GLuint *))gl_proc("glDeleteRenderbuffersOES", "glDeleteRenderbuffers");
  p_RenderbufferStorage     = (void (*)(GLenum, GLenum, GLsizei, GLsizei))gl_proc("glRenderbufferStorageOES", "glRenderbufferStorage");
  p_FramebufferRenderbuffer = (void (*)(GLenum, GLenum, GLenum, GLuint))gl_proc("glFramebufferRenderbufferOES", "glFramebufferRenderbuffer");

  return p_GenFramebuffers && p_BindFramebuffer && p_FramebufferTexture2D &&
         p_CheckFramebufferStatus && p_GenRenderbuffers && p_BindRenderbuffer &&
         p_RenderbufferStorage && p_FramebufferRenderbuffer;
}

static void compute_dest_rect(void) {
  if (s_mode == WMW_TATE_UPRIGHT) {
    // Fit the portrait image inside the landscape panel, preserving aspect.
    const float scale = (float)s_wh / (float)s_rh;
    const float w = (float)s_rw * scale;
    s_dx0 = ((float)s_ww - w) * 0.5f;
    s_dx1 = s_dx0 + w;
    s_dy0 = 0.0f;
    s_dy1 = (float)s_wh;
  } else {
    // Rotated: the portrait target was sized to land exactly on the panel.
    s_dx0 = 0.0f; s_dy0 = 0.0f;
    s_dx1 = (float)s_ww; s_dy1 = (float)s_wh;
  }
}

int wmw_tate_init(int render_w, int render_h, int window_w, int window_h, int mode) {
  wmw_tate_shutdown();

  if (render_w <= 0 || render_h <= 0 || window_w <= 0 || window_h <= 0)
    return 0;
  if (!resolve_fbo_procs()) {
    debugPrintf("tate: GL_OES_framebuffer_object unavailable -- portrait disabled\n");
    return 0;
  }

  s_mode = mode;
  s_rw = render_w; s_rh = render_h;
  s_ww = window_w; s_wh = window_h;

  glGenTextures(1, &s_tex);
  glBindTexture(GL_TEXTURE_2D, s_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_rw, s_rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  p_GenRenderbuffers(1, &s_depth);
  p_BindRenderbuffer(GL_RENDERBUFFER_OES, s_depth);
  p_RenderbufferStorage(GL_RENDERBUFFER_OES, GL_DEPTH_COMPONENT16_OES, s_rw, s_rh);

  p_GenFramebuffers(1, &s_fbo);
  p_BindFramebuffer(GL_FRAMEBUFFER_OES, s_fbo);
  p_FramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, s_tex, 0);
  p_FramebufferRenderbuffer(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, s_depth);

  const GLenum status = p_CheckFramebufferStatus(GL_FRAMEBUFFER_OES);
  p_BindFramebuffer(GL_FRAMEBUFFER_OES, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE_OES) {
    debugPrintf("tate: framebuffer incomplete (0x%x) -- portrait disabled\n", status);
    wmw_tate_shutdown();
    return 0;
  }

  compute_dest_rect();
  s_ready = 1;
  debugPrintf("tate: %s, render %dx%d -> window %dx%d\n",
              s_mode == WMW_TATE_UPRIGHT ? "upright (pillarboxed)"
              : s_mode == WMW_TATE_CCW   ? "rotated 90 CCW (fullscreen)"
                                         : "rotated 90 CW (fullscreen)",
              s_rw, s_rh, s_ww, s_wh);
  return 1;
}

int wmw_tate_active(void) { return s_ready; }
unsigned wmw_tate_fbo(void) { return s_fbo; }

void wmw_tate_begin(void) {
  if (!s_ready) return;
  p_BindFramebuffer(GL_FRAMEBUFFER_OES, s_fbo);
  glViewport(0, 0, s_rw, s_rh);
}

void wmw_tate_shutdown(void) {
  if (s_fbo && p_DeleteFramebuffers) p_DeleteFramebuffers(1, &s_fbo);
  if (s_depth && p_DeleteRenderbuffers) p_DeleteRenderbuffers(1, &s_depth);
  if (s_tex) glDeleteTextures(1, &s_tex);
  s_fbo = s_depth = s_tex = 0;
  s_ready = 0;
}

// ---------------------------------------------------------------------------
// present -- the FORWARD transform
// ---------------------------------------------------------------------------
//
// The destination quad is always the same four window-space corners; only the
// texture coordinates change. The FBO texture has its origin at the bottom
// left, so the rendered image's visual top edge is at t = 1.
//
//   UPRIGHT   window TL <- portrait TL, i.e. (0,1)
//   CW        the portrait's top edge goes to the window's RIGHT edge
//   CCW       the portrait's top edge goes to the window's LEFT edge
//
// Vertex order is TL, BL, TR, BR to suit GL_TRIANGLE_STRIP.

/* Full state of one GLES1 client array, so restoring it cannot leave the engine
 * reading OUR quad on its next draw. */
typedef struct { GLint size, type, stride, buf; GLvoid *ptr; } ClientArray;

static void carray_save(GLenum size_e, GLenum type_e, GLenum stride_e,
                        GLenum ptr_e, ClientArray *a) {
  glGetIntegerv(size_e,   &a->size);
  glGetIntegerv(type_e,   &a->type);
  glGetIntegerv(stride_e, &a->stride);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &a->buf);
  glGetPointerv(ptr_e, &a->ptr);
}

void wmw_tate_present(void) {
  if (!s_ready) return;

  /* STATIC, not stack locals.
   *
   * glVertexPointer stores the address, it does not copy the data. A quad in
   * automatic storage is dangling the moment this function returns, and the
   * engine's next draw then reads whatever the stack has become. The arrays are
   * also restored below, but both protections are wanted: one keeps the pointer
   * valid, the other keeps the engine from using it at all. */
  static const GLfloat uv_upright[8] = { 0,1,  0,0,  1,1,  1,0 };
  static const GLfloat uv_cw[8]      = { 0,0,  1,0,  0,1,  1,1 };
  static const GLfloat uv_ccw[8]     = { 1,1,  0,1,  1,0,  0,0 };
  static GLfloat verts[8];

  verts[0] = s_dx0; verts[1] = s_dy0;
  verts[2] = s_dx0; verts[3] = s_dy1;
  verts[4] = s_dx1; verts[5] = s_dy0;
  verts[6] = s_dx1; verts[7] = s_dy1;

  const GLfloat *uv = (s_mode == WMW_TATE_UPRIGHT) ? uv_upright
                    : (s_mode == WMW_TATE_CCW)     ? uv_ccw
                                                   : uv_cw;

  // ---- save everything we are about to touch -----------------------------
  //
  // GLES 1.1 has no glPushAttrib, so this is manual. It is not optional: the
  // engine configures its pipeline incrementally and carries state across
  // frames, so anything left changed here corrupts the NEXT frame's drawing.
  // The symptom is the engine's own art failing to appear while the blit
  // itself still looks correct -- which reads as missing assets, not as a
  // presentation bug.
  GLint  prev_viewport[4];
  GLint  prev_tex = 0, prev_matrix_mode = 0, prev_env_mode = 0;
  GLint  prev_active = GL_TEXTURE0, prev_cactive = GL_TEXTURE0;
  GLint  prev_blend_src = GL_ONE, prev_blend_dst = GL_ZERO;
  GLint  prev_arr_buf = 0;
  GLfloat prev_color[4], prev_clear[4];
  ClientArray sv_v, sv_t;

  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &prev_cactive);
  glGetIntegerv(GL_BLEND_SRC, &prev_blend_src);
  glGetIntegerv(GL_BLEND_DST, &prev_blend_dst);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_arr_buf);
  glGetFloatv(GL_CURRENT_COLOR, prev_color);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
  carray_save(GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE,
              GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER, &sv_v);
  carray_save(GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE,
              GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_POINTER, &sv_t);

  const GLboolean had_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean had_blend   = glIsEnabled(GL_BLEND);
  const GLboolean had_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean had_light   = glIsEnabled(GL_LIGHTING);
  const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean had_alpha   = glIsEnabled(GL_ALPHA_TEST);
  const GLboolean had_varr    = glIsEnabled(GL_VERTEX_ARRAY);
  const GLboolean had_tarr    = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  const GLboolean had_carr    = glIsEnabled(GL_COLOR_ARRAY);
  const GLboolean had_narr    = glIsEnabled(GL_NORMAL_ARRAY);

  // Texture units. The engine multitextures (it imports glActiveTexture and
  // glClientActiveTexture), so "the" texture binding is per-unit and there is
  // no single value to save. Force unit 0 for the blit, and switch off
  // texturing on any higher unit the engine left enabled -- otherwise its
  // second stage modulates our quad.
  GLint max_units = 2;
  glGetIntegerv(GL_MAX_TEXTURE_UNITS, &max_units);
  if (max_units > 8) max_units = 8;
  if (max_units < 1) max_units = 1;
  GLboolean had_unit_tex[8];
  for (GLint u = 0; u < max_units; u++) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + u));
    had_unit_tex[u] = glIsEnabled(GL_TEXTURE_2D);
    if (u > 0) glDisable(GL_TEXTURE_2D);
  }
  glActiveTexture(GL_TEXTURE0);
  glClientActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &prev_env_mode);
  glGetIntegerv(GL_MATRIX_MODE, &prev_matrix_mode);

  // ---- the rotated blit ---------------------------------------------------
  p_BindFramebuffer(GL_FRAMEBUFFER_OES, 0);
  glViewport(0, 0, s_ww, s_wh);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, (GLfloat)s_ww, (GLfloat)s_wh, 0.0f, -1.0f, 1.0f); // y down
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_LIGHTING);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_ALPHA_TEST);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, s_tex);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

  glBindBuffer(GL_ARRAY_BUFFER, 0);   // client arrays, not offsets into a VBO
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // ---- put it all back ----------------------------------------------------
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode((GLenum)prev_matrix_mode);

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, prev_env_mode);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

  // Client arrays: re-bind the buffer each was sourced from, then its pointer.
  // Restoring the pointer without the buffer it belonged to would silently turn
  // a VBO offset into a client-memory address, or the reverse.
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)sv_v.buf);
  if (sv_v.size > 0) glVertexPointer(sv_v.size, (GLenum)sv_v.type, sv_v.stride, sv_v.ptr);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)sv_t.buf);
  if (sv_t.size > 0) glTexCoordPointer(sv_t.size, (GLenum)sv_t.type, sv_t.stride, sv_t.ptr);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_arr_buf);

  // Texture-unit enables, then the units the engine had selected.
  for (GLint u = 0; u < max_units; u++) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + u));
    if (had_unit_tex[u]) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
  }
  glClientActiveTexture((GLenum)prev_cactive);
  glActiveTexture((GLenum)prev_active);

  glBlendFunc((GLenum)prev_blend_src, (GLenum)prev_blend_dst);
  glColor4f(prev_color[0], prev_color[1], prev_color[2], prev_color[3]);
  glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

  #define RESTORE(flag, had) do { if (had) glEnable(flag); else glDisable(flag); } while (0)
  RESTORE(GL_DEPTH_TEST,   had_depth);
  RESTORE(GL_BLEND,        had_blend);
  RESTORE(GL_CULL_FACE,    had_cull);
  RESTORE(GL_LIGHTING,     had_light);
  RESTORE(GL_SCISSOR_TEST, had_scissor);
  RESTORE(GL_ALPHA_TEST,   had_alpha);
  #undef RESTORE

  #define RESTORE_CS(flag, had) do { if (had) glEnableClientState(flag); else glDisableClientState(flag); } while (0)
  RESTORE_CS(GL_VERTEX_ARRAY,        had_varr);
  RESTORE_CS(GL_TEXTURE_COORD_ARRAY, had_tarr);
  RESTORE_CS(GL_COLOR_ARRAY,         had_carr);
  RESTORE_CS(GL_NORMAL_ARRAY,        had_narr);
  #undef RESTORE_CS
}

/* The inverse transform -- panel coordinates back into render space -- lives in
 * nx_pointer.c, which is told the same rotation mode through NxpConfig.rotation
 * and applies it to touch, stick, mouse and gyro alike. It is deliberately NOT
 * duplicated here: two implementations of the same transform are exactly what
 * drifts apart, and when they do the symptom (input offset or mirrored) looks
 * nothing like the cause. One owner. */
