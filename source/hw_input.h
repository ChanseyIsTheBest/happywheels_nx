/* Controller -> synthetic touch mapping. */

#ifndef HW_INPUT_H
#define HW_INPUT_H

#include <switch.h>

void hw_input_init(const char *data_root);
void hw_input_save(void);
int  hw_input_learn_active(void);

void hw_input_update(PadState *pad, int w, int h,
                     int touch_active, float touch_x, float touch_y);

/* Implemented in android_native_cocos.c -- hands a pointer set to the
 * engine, diffing against the previous frame to produce begin/move/end. */
void hw_input_feed(int n, const int *ids, const float *xs, const float *ys);

#endif
