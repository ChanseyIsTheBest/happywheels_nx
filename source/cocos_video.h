#ifndef COCOS_VIDEO_H
#define COCOS_VIDEO_H

#include <stdint.h>

typedef void (*CocosVideoEventCallback)(int index, int event);

void cocos_video_init(const char *data_root, CocosVideoEventCallback callback);
void cocos_video_shutdown(void);

int  cocos_video_create(void);
void cocos_video_remove(int index);
void cocos_video_set_source(int index, const char *url);
void cocos_video_set_rect(int index, int left, int top, int width, int height);
void cocos_video_start(int index);
void cocos_video_stop(int index);
void cocos_video_skip(void);
int  cocos_video_active(void);

void cocos_video_gl_tick(int screen_width, int screen_height);
void cocos_video_mix_audio(int16_t *samples, int frames);

#endif
