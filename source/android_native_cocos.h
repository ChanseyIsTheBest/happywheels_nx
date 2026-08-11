/* EGL and input integration. */
#ifndef ANDROID_NATIVE_COCOS_H
#define ANDROID_NATIVE_COCOS_H

#include <stdint.h>
#include "cocos_entrypoints.h"

int   cocos_gl_init(void);
void  cocos_gl_swap(void);
void  cocos_gl_deinit(void);
void  cocos_native_update_mode(void);
uint32_t cocos_native_width(void);
uint32_t cocos_native_height(void);

/* Maximum simultaneous pointers fed to the engine. Must be >= the number of
 * controller bindings in hw_input.c, which each get their own pointer id, and
 * >= the touchscreen finger count. The game's own table is CC_MAX_TOUCHES = 15
 * (verified in GLView::handleTouchesBegin), so 16 is the useful ceiling. */
#define COCOS_MAX_POINTERS 24

typedef struct {
  fn_cocos_touch1 begin;
  fn_cocos_touch1 end;
  fn_cocos_touchN move;
  fn_cocos_touchN cancel;
  fn_cocos_key    key;
  void *env;
  void *thiz;
} CocosInputApi;

void cocos_input_init(const CocosInputApi *api);
void cocos_feed_hid(void);

void android_get_orientation(float *x, float *y, float *z);

#endif /* ANDROID_NATIVE_COCOS_H */
