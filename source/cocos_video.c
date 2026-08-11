/* Video playback stub.
 *
 * KHUx plays bundled cutscenes through ffmpeg. Happy Wheels has no video --
 * it never resolves Cocos2dxVideoHelper, and nothing in libMyGame.so links
 * against a decoder. Keeping the header contract and stubbing the body drops
 * ffmpeg, dav1d and swscale from the link without touching any caller.
 */

#include <stdint.h>
#include "cocos_video.h"

void cocos_video_init(const char *data_root, CocosVideoEventCallback callback) {
  (void)data_root; (void)callback;
}
void cocos_video_shutdown(void) {}

int  cocos_video_create(void) { return -1; }
void cocos_video_remove(int index) { (void)index; }
void cocos_video_set_source(int index, const char *url) { (void)index; (void)url; }
void cocos_video_set_rect(int index, int left, int top, int width, int height) {
  (void)index; (void)left; (void)top; (void)width; (void)height;
}
void cocos_video_start(int index) { (void)index; }
void cocos_video_stop(int index) { (void)index; }
void cocos_video_skip(void) {}
int  cocos_video_active(void) { return 0; }

void cocos_video_gl_tick(int screen_width, int screen_height) {
  (void)screen_width; (void)screen_height;
}
void cocos_video_mix_audio(int16_t *samples, int frames) {
  (void)samples; (void)frames;
}
