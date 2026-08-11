/* SDKBOX neutralisation.
 *
 * Happy Wheels links SDKBOX for in-app purchase, Google Analytics, Firebase
 * Analytics and the rate-this-app prompt. None of it is reachable or wanted
 * on a Switch running offline, and the shipped sdkbox_config.json contains a
 * live analytics tracking ID, so the whole plugin surface is answered here
 * instead of being allowed to reach the network.
 *
 * Every com/sdkbox class resolves to a single inert object. Calls against
 * it return zero/NULL/empty, which is the same shape the engine sees when a
 * plugin is absent on Android, so the game takes its "plugin unavailable"
 * path rather than dereferencing something unexpected.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hw_sdkbox.h"

static int g_sdkbox_class;   /* address used as an opaque jclass handle */

int hw_sdkbox_owns_class(const char *cls) {
  if (!cls) return 0;
  return strstr(cls, "com/sdkbox/")  != NULL ||
         strstr(cls, "com/fancyforce/AdHelper")  != NULL ||
         strstr(cls, "com/fancyforce/IAPHelper") != NULL ||
         strstr(cls, "org/cocos2dx/lib/Cocos2dxDownloader") != NULL;
}

void *hw_sdkbox_class(const char *cls) {
  (void)cls;
  return &g_sdkbox_class;
}

int hw_sdkbox_is_object(const void *obj) {
  return obj == (const void *)&g_sdkbox_class;
}

/* The one product in sdkbox_config.json is com.fancyforce.happywheels.removeads.
 *
 * This reports the plugin as unavailable rather than reporting the product as
 * owned. The game then behaves as it does on a device with no billing service:
 * no store, and no ad plugin to serve anything either, because the ad classes
 * above are inert too. Answering "purchased" would be a licence-check bypass,
 * which is a different thing from running your own copy through a wrapper. */
int hw_sdkbox_query_bool(const char *method) {
  if (!method) return 0;
  if (!strcmp(method, "isInitialized"))   return 0;
  if (!strcmp(method, "isEnabled"))       return 0;
  if (!strcmp(method, "isAvailable"))     return 0;
  if (!strcmp(method, "isPurchased"))     return 0;
  if (!strcmp(method, "isAdAvailable"))   return 0;
  return 0;
}
