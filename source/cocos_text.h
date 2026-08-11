/* System-font text rasterization for Cocos2dxBitmap. */
#ifndef COCOS_TEXT_H
#define COCOS_TEXT_H

#include <stddef.h>

void cocos_text_init(void);

unsigned char *cocos_text_render(const char *text, int px, int cr, int cg, int cb,
                                 int req_w, int req_h, int align,
                                 int *out_w, int *out_h);

#endif
