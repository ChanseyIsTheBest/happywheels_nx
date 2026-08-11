#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "android_native_cocos.h"
#include "hw_input.h"
#include "nx_pointer.h"
#include "jni_fake.h"
#include "cocos_video.h"

static u32 g_w = 1280, g_h = 720;

void android_get_orientation(float *x, float *y, float *z) {
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

void cocos_native_update_mode(void) {
  int res = (appletGetOperationMode() == AppletOperationMode_Console)
              ? config.docked_res : config.handheld_res;
  if (res == 1080) { g_w = 1920; g_h = 1080; }
  else             { g_w = 1280; g_h = 720;  }
  nxp_set_screen((int)g_w, (int)g_h);
}
uint32_t cocos_native_width(void)  { return g_w; }
uint32_t cocos_native_height(void) { return g_h; }

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLSurface s_surf = EGL_NO_SURFACE;

static void draw_controller_cursor(void);
static void destroy_controller_cursor(void);

static void nx_window_geom(u32 w, u32 h) {
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, w, h);
  nwindowSetCrop(win, 0, 0, w, h);
  nwindowSetTransform(win, 0);
}

int cocos_gl_init(void) {
  cocos_native_update_mode();
  nx_window_geom(g_w, g_h);

  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY) return -1;
  if (!eglInitialize(s_dpy, NULL, NULL)) return -1;
  eglBindAPI(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig config; EGLint n = 0;
  if (!eglChooseConfig(s_dpy, cfg_attr, &config, 1, &n) || n < 1) return -1;
  s_surf = eglCreateWindowSurface(s_dpy, config,
             (EGLNativeWindowType)nwindowGetDefault(), NULL);
  if (s_surf == EGL_NO_SURFACE) return -1;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, config, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT) return -1;
  if (!eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) return -1;
  eglSwapInterval(s_dpy, 1);
  return 0;
}

void cocos_gl_swap(void) {
  nxp_draw();   /* on top of the finished frame, before the real swap */
  if (s_dpy != EGL_NO_DISPLAY) {
    cocos_video_gl_tick((int)g_w, (int)g_h);
    draw_controller_cursor();
    eglSwapBuffers(s_dpy, s_surf);
  }
}

void cocos_gl_deinit(void) {
  if (s_dpy == EGL_NO_DISPLAY) return;
  destroy_controller_cursor();
  eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_ctx != EGL_NO_CONTEXT) eglDestroyContext(s_dpy, s_ctx);
  if (s_surf != EGL_NO_SURFACE) eglDestroySurface(s_dpy, s_surf);
  eglTerminate(s_dpy);
  s_dpy = EGL_NO_DISPLAY; s_ctx = EGL_NO_CONTEXT; s_surf = EGL_NO_SURFACE;
}

#define MAX_PTR COCOS_MAX_POINTERS

static CocosInputApi g_api;
static PadState g_pad;
static HidTouchScreenState g_ts;

static int   g_prev_n = 0;
static int   g_prev_id[MAX_PTR];
static float g_prev_x[MAX_PTR], g_prev_y[MAX_PTR];

static float g_cur_x, g_cur_y;
static int   g_prev_a = 0;
static int   g_cursor_visible = 0;
static int   g_touch_was_down = 0;
static int   g_video_touch_capture = 0;
static int   g_video_a_capture = 0;

static struct {
  GLuint program;
  GLint position;
  GLint local;
  GLint feather;
  int initialized;
} g_cursor_gl;

typedef struct {
  GLint enabled;
  GLint size;
  GLint stride;
  GLint type;
  GLint normalized;
  GLint buffer;
  void *pointer;
} CursorAttribState;

static void save_cursor_attrib(GLuint index, CursorAttribState *state) {
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state->enabled);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state->size);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &state->stride);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state->type);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &state->normalized);
  glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &state->buffer);
  glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &state->pointer);
}

static void restore_cursor_attrib(GLuint index, const CursorAttribState *state) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)state->buffer);
  glVertexAttribPointer(index, state->size, (GLenum)state->type,
                        (GLboolean)state->normalized, state->stride, state->pointer);
  if (state->enabled) glEnableVertexAttribArray(index);
  else                glDisableVertexAttribArray(index);
}

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint create_cursor_program(void) {
  const char *vs_source =
      "attribute vec2 aPos; attribute vec2 aLocal; varying vec2 vLocal;"
      "void main() { vLocal = aLocal; gl_Position = vec4(aPos, 0.0, 1.0); }";
  const char *fs_source =
      "precision mediump float; varying vec2 vLocal; uniform float uFeather;"
      "void main() {"
      "  float d = length(vLocal);"
      "  float a = 1.0 - smoothstep(1.0 - uFeather, 1.0, d);"
      "  float core = 1.0 - smoothstep(0.74 - uFeather, 0.74 + uFeather, d);"
      "  vec3 col = mix(vec3(0.04), vec3(0.98), core);"
      "  gl_FragColor = vec4(col, a * 0.85);"
      "}";
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

static void destroy_controller_cursor(void) {
  if (g_cursor_gl.program)
    glDeleteProgram(g_cursor_gl.program);
  memset(&g_cursor_gl, 0, sizeof(g_cursor_gl));
}

static void draw_controller_cursor(void) {
  if (!g_cursor_visible)
    return;
  if (!g_cursor_gl.initialized) {
    g_cursor_gl.initialized = 1;
    g_cursor_gl.program = create_cursor_program();
    if (g_cursor_gl.program) {
      g_cursor_gl.position = glGetAttribLocation(g_cursor_gl.program, "aPos");
      g_cursor_gl.local = glGetAttribLocation(g_cursor_gl.program, "aLocal");
      g_cursor_gl.feather = glGetUniformLocation(g_cursor_gl.program, "uFeather");
    }
  }
  if (!g_cursor_gl.program)
    return;

  const float radius = 18.0f * ((float)(g_w > g_h ? g_w : g_h) / 1280.0f);
  const float cx = (g_cur_x / (float)g_w) * 2.0f - 1.0f;
  const float cy = 1.0f - (g_cur_y / (float)g_h) * 2.0f;
  const float rx = radius / (float)g_w * 2.0f;
  const float ry = radius / (float)g_h * 2.0f;
  const GLfloat position[8] = {
    cx - rx, cy - ry, cx + rx, cy - ry,
    cx - rx, cy + ry, cx + rx, cy + ry,
  };
  static const GLfloat local[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };

  GLint previous_program, previous_buffer, previous_framebuffer, previous_viewport[4];
  CursorAttribState position_state, local_state;
  GLint src_rgb, dst_rgb, src_alpha, dst_alpha, equation_rgb, equation_alpha;
  GLboolean color_mask[4];
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);
  const GLboolean stencil = glIsEnabled(GL_STENCIL_TEST);
  glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
  glGetIntegerv(GL_VIEWPORT, previous_viewport);
  glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equation_alpha);
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  save_cursor_attrib((GLuint)g_cursor_gl.position, &position_state);
  save_cursor_attrib((GLuint)g_cursor_gl.local, &local_state);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, (GLsizei)g_w, (GLsizei)g_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(g_cursor_gl.program);
  glUniform1f(g_cursor_gl.feather, 2.5f / radius);
  glEnableVertexAttribArray((GLuint)g_cursor_gl.position);
  glEnableVertexAttribArray((GLuint)g_cursor_gl.local);
  glVertexAttribPointer((GLuint)g_cursor_gl.position, 2, GL_FLOAT, GL_FALSE, 0, position);
  glVertexAttribPointer((GLuint)g_cursor_gl.local, 2, GL_FLOAT, GL_FALSE, 0, local);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  restore_cursor_attrib((GLuint)g_cursor_gl.position, &position_state);
  restore_cursor_attrib((GLuint)g_cursor_gl.local, &local_state);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_buffer);
  glUseProgram((GLuint)previous_program);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_framebuffer);
  glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
  glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  glBlendEquationSeparate((GLenum)equation_rgb, (GLenum)equation_alpha);
  glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_alpha, (GLenum)dst_alpha);
  if (!blend) glDisable(GL_BLEND);
  if (depth) glEnable(GL_DEPTH_TEST);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (cull) glEnable(GL_CULL_FACE);
  if (stencil) glEnable(GL_STENCIL_TEST);
}

void cocos_input_init(const CocosInputApi *api) {
  g_api = *api;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  g_cur_x = (float)g_w * 0.5f;
  g_cur_y = (float)g_h * 0.5f;
}

static int prev_index_of(int id) {
  for (int i = 0; i < g_prev_n; i++) if (g_prev_id[i] == id) return i;
  return -1;
}

static void feed_pointers(int cur_n, const int *cur_id, const float *cur_x, const float *cur_y) {
  int   mid[MAX_PTR]; float mx[MAX_PTR], my[MAX_PTR]; int mN = 0;
  /* Defensive: mid/mx/my are sized MAX_PTR and mN can reach cur_n. */
  if (cur_n > MAX_PTR) cur_n = MAX_PTR;
  if (cur_n < 0) cur_n = 0;
  for (int i = 0; i < cur_n; i++) {
    int p = prev_index_of(cur_id[i]);
    if (p < 0) {
      if (g_api.begin) g_api.begin(g_api.env, g_api.thiz, cur_id[i], cur_x[i], cur_y[i]);
    } else {
      mid[mN] = cur_id[i]; mx[mN] = cur_x[i]; my[mN] = cur_y[i]; mN++;
    }
  }
  if (mN && g_api.move) {
    void *jids = jni_make_int_array(mid, mN);
    void *jxs  = jni_make_float_array(mx, mN);
    void *jys  = jni_make_float_array(my, mN);
    g_api.move(g_api.env, g_api.thiz, jids, jxs, jys);
  }
  for (int i = 0; i < g_prev_n; i++) {
    int gone = 1;
    for (int j = 0; j < cur_n; j++) if (cur_id[j] == g_prev_id[i]) { gone = 0; break; }
    if (gone && g_api.end)
      g_api.end(g_api.env, g_api.thiz, g_prev_id[i], g_prev_x[i], g_prev_y[i]);
  }
  g_prev_n = cur_n > MAX_PTR ? MAX_PTR : cur_n;
  for (int i = 0; i < g_prev_n; i++) { g_prev_id[i] = cur_id[i]; g_prev_x[i] = cur_x[i]; g_prev_y[i] = cur_y[i]; }
}

void hw_input_feed(int n, const int *ids, const float *xs, const float *ys) {
  feed_pointers(n, ids, xs, ys);
}

void cocos_feed_hid(void) {
  padUpdate(&g_pad);

  /* Before the touchscreen branches below, every one of which can return: the
   * cursor toggle and its movement must not depend on whether a finger happens
   * to be down. */
  nxp_update(&g_pad);

  int n = hidGetTouchScreenStates(&g_ts, 1);
  if (n > 0 && g_ts.count > 0) {
    g_cursor_visible = 0;
    const float PANEL_W = 1280.0f, PANEL_H = 720.0f;
    int   ids[MAX_PTR]; float xs[MAX_PTR], ys[MAX_PTR];
    int c = g_ts.count > MAX_PTR ? MAX_PTR : g_ts.count;
    for (int i = 0; i < c; i++) {
      ids[i] = (int)g_ts.touches[i].finger_id;
      xs[i]  = (float)g_ts.touches[i].x * ((float)g_w / PANEL_W);
      ys[i]  = (float)g_ts.touches[i].y * ((float)g_h / PANEL_H);
    }
    if (!g_touch_was_down && cocos_video_active()) {
      g_video_touch_capture = 1;
      if (g_prev_n) feed_pointers(0, NULL, NULL, NULL);
      cocos_video_skip();
    }
    g_touch_was_down = 1;
    if (g_video_touch_capture)
      return;
    if (hw_input_learn_active()) {
      hw_input_update(&g_pad, (int)g_w, (int)g_h, 1, xs[0], ys[0]);
      return;
    }
    feed_pointers(c, ids, xs, ys);
    return;
  }
  if (g_touch_was_down) {
    g_touch_was_down = 0;
    if (g_video_touch_capture) {
      g_video_touch_capture = 0;
      if (g_prev_n) feed_pointers(0, NULL, NULL, NULL);
      return;
    }
  }
  hw_input_update(&g_pad, (int)g_w, (int)g_h, 0, 0.0f, 0.0f);

}
