/* Fixed configuration. See config.h for what each value does. */

#include <stdio.h>
#include <string.h>

#include "config.h"

int screen_width  = 1920;
int screen_height = 1080;

const Config config = {
  .handheld_res        = 1080,
  .docked_res          = 1080,
  .dpi                 = 320,
  .remove_ads          = 1,
  .decode_stream_audio = 1,
};

static char g_data_root[HW_PATH_MAX] = "";

int hw_init_data_root(const char *nro_path) {
  if (!nro_path || !*nro_path) {
    snprintf(g_data_root, sizeof g_data_root, "sdmc:/switch/happywheels");
    return 0;
  }
  snprintf(g_data_root, sizeof g_data_root, "%s", nro_path);
  char *slash = strrchr(g_data_root, '/');
  if (!slash) slash = strrchr(g_data_root, '\\');
  if (!slash) return -1;
  *slash = '\0';
  return 0;
}

const char *hw_data_root(void) { return g_data_root; }
