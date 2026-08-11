/* Happy Wheels Switch wrapper configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stddef.h>

#define MMAP_ARENA_ALIGN    ((size_t)2 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)384 * 1024 * 1024)

#define HW_PACKAGE        "com.fancyforce.happywheels"
#define HW_VERSION_CODE   113
#define HW_VERSION_NAME   "1.1.3"

#define SO_COCOS   "libMyGame.so"

/* 1 = write debug.log next to the NRO and keep the stall watchdog running.
 * 0 = absolute silence: the logger is compiled out, no file is ever created,
 * and the watchdog thread is not started. Set to 1 to diagnose anything. */
#define DEBUG_LOG 0
#define LOG_NAME      "debug.log"
#define CONTROLS_NAME "controls.cfg"

#define HW_PATH_MAX 512

/* Runtime data directory, derived from the path of the launched NRO. */
int hw_init_data_root(const char *nro_path);
const char *hw_data_root(void);

extern int screen_width;
extern int screen_height;

/* Fixed settings. There is no config.txt: these were the only values anyone
 * was choosing, and a file that regenerates itself with stale defaults caused
 * more trouble than it was worth -- an old copy silently re-enabled a code
 * path that had already been removed for crashing. Change them here and
 * rebuild.
 *
 *   handheld_res / docked_res  720 or 1080. Equal on purpose: the game then
 *                              lays out identically in both, so controls.cfg
 *                              stays valid and docking raises no
 *                              surface-changed event mid-session.
 *   dpi                        reported to Cocos2dxHelper.getDPI(); the game
 *                              uses it to pick an asset bucket.
 *   remove_ads                 report the entitlement as held.
 *   decode_stream_audio        decode the streamed music in-port.
 */
typedef struct {
  int handheld_res;
  int docked_res;
  int dpi;
  int remove_ads;
  int decode_stream_audio;
} Config;

extern const Config config;

#endif
