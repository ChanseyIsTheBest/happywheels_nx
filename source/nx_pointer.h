/* nx_pointer.h -- on-screen cursor. See nx_pointer.c for what was cut from the
 * original and why.
 *
 * Owns the cursor's position, visibility and its GL overlay. It does not read
 * the touchscreen and does not emit pointer events: hw_input.c already builds
 * the engine's pointer set and emits the cursor's tap alongside the gameplay
 * bindings.
 *
 * If <data_dir>/cursor.png exists and is at most 64x64 it is used, alpha
 * respected; otherwise a built-in vector arrow is drawn.
 */

#ifndef NX_POINTER_H
#define NX_POINTER_H

#include <stdio.h>
#include <stdint.h>
#include <switch.h>

typedef struct {
  int   screen_w, screen_h;      /* render space, e.g. 1920x1080 */
  const char *data_dir;          /* where cursor.png lives */
  void (*log)(const char *msg);  /* optional */

  /* Optional locked file I/O. devkitPro's handle table is not thread-safe and
   * the engine's workers use it constantly, so a port with locked wrappers
   * should pass them; cursor.png is read through these. */
  FILE *(*fopen_fn)(const char *path, const char *mode);
  int   (*fclose_fn)(FILE *f);
} NxpConfig;

/* Call once, before the engine spawns threads: it reads cursor.png from the SD
 * card. Decoding and GL upload happen lazily on the render thread. */
void nxp_init(const NxpConfig *cfg);

/* Once per frame, with a PadState the caller has already padUpdate()d. */
void nxp_update(PadState *pad);

/* Tell the cursor the render space changed. Cheap no-op when unchanged. */
void nxp_set_screen(int w, int h);

/* Draw the cursor with the engine's GL context current, just before the real
 * eglSwapBuffers. Saves and restores all GL state it touches. */
void nxp_draw(void);

int  nxp_cursor_visible(void);
void nxp_cursor_pos(float *x, float *y);
void nxp_set_visible(int on);
void nxp_warp(float x, float y);   /* move it, e.g. onto the control being bound */

/* 1 = shader built, 0 = not built yet (never drawn), -1 = compile/link failed */
int  nxp_gl_state(void);

#endif /* NX_POINTER_H */
