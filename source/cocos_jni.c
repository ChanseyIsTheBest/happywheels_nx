#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "jni_fake.h"
#include "cocos_jni.h"
#include "cocos_text.h"
#include "cocos_video.h"
#include "opensles.h"

typedef uint64_t juint;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;

extern void *fake_env;
extern volatile int jni_quit_requested;

static char g_root[HW_PATH_MAX] = ".";

#define MAX_PREFS 512
typedef struct { char *k, *v; } Pref;
static Pref  g_prefs[MAX_PREFS];
static int   g_pref_n = 0;
static Mutex g_pref_lock;

static void cocos_video_event(int index, int event) {
  typedef void (*fn_vcb)(void *env, void *thiz, int idx, int event);
  fn_vcb callback = (fn_vcb)so_resolve_external(
      "Java_org_cocos2dx_lib_Cocos2dxVideoHelper_nativeExecuteVideoCallback");
  if (callback)
    callback(fake_env, jni_make_object("Cocos2dxVideoHelper"), index, event);
}

static void prefs_path(char *out, size_t n) { snprintf(out, n, "%s/prefs.kv", g_root); }

static void prefs_load(void) {
  char path[544]; prefs_path(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    char *tab = strchr(line, '\t'); if (!tab) continue;
    *tab = 0;
    char *val = tab + 1;
    size_t vl = strlen(val); if (vl && val[vl-1] == '\n') val[vl-1] = 0;
    if (g_pref_n < MAX_PREFS) { g_prefs[g_pref_n].k = strdup(line); g_prefs[g_pref_n].v = strdup(val); g_pref_n++; }
  }
  fclose(f);
}
static void prefs_save(void) {
  char path[544]; prefs_path(path, sizeof path);
  FILE *f = fopen(path, "w");
  if (!f) return;
  for (int i = 0; i < g_pref_n; i++) fprintf(f, "%s\t%s\n", g_prefs[i].k, g_prefs[i].v);
  fclose(f);
}
static const char *pref_get(const char *k, const char *def) {
  for (int i = 0; i < g_pref_n; i++) if (!strcmp(g_prefs[i].k, k)) return g_prefs[i].v;
  return def;
}
static void pref_set(const char *k, const char *v) {
  mutexLock(&g_pref_lock);
  for (int i = 0; i < g_pref_n; i++)
    if (!strcmp(g_prefs[i].k, k)) { free(g_prefs[i].v); g_prefs[i].v = strdup(v); mutexUnlock(&g_pref_lock); prefs_save(); return; }
  if (g_pref_n < MAX_PREFS) { g_prefs[g_pref_n].k = strdup(k); g_prefs[g_pref_n].v = strdup(v); g_pref_n++; }
  mutexUnlock(&g_pref_lock);
  prefs_save();
}

static void pref_delete(const char *k) {
  mutexLock(&g_pref_lock);
  for (int i = 0; i < g_pref_n; i++) {
    if (!strcmp(g_prefs[i].k, k)) {
      free(g_prefs[i].k); free(g_prefs[i].v);
      g_prefs[i] = g_prefs[g_pref_n - 1];
      g_pref_n--;
      break;
    }
  }
  mutexUnlock(&g_pref_lock);
  prefs_save();
}

void cocos_jni_init(const char *data_root) {
  if (data_root && *data_root) { strncpy(g_root, data_root, sizeof(g_root) - 1); g_root[sizeof(g_root)-1] = 0; }
  mutexInit(&g_pref_lock);
  setInitialize();
  prefs_load();
  cocos_video_init(g_root, cocos_video_event);
}

static int has(const char *h, const char *n) { return strstr(h, n) != NULL; }

int cocos_owns_class(const char *cls) {
  if (!cls) return 0;
  /* Cocos passes the dotted spelling in some paths (getSDKVersion among
   * them) and the slashed one in others. Match on the leaf name so routing
   * does not depend on which form arrives. */
  if (has(cls, "Cocos2dxHelper")) return 1;
  return has(cls, "org/cocos2dx/lib/Cocos2dxHelper") ||
         has(cls, "org/cocos2dx/lib/Cocos2dxGLSurfaceView") ||
         has(cls, "org/cocos2dx/lib/Cocos2dxBitmap") ||
         has(cls, "org/cocos2dx/lib/Cocos2dxVideoHelper") ||
         has(cls, "org/cocos2dx/lib/Cocos2dxRenderer") ||
         has(cls, "org/cocos2dx/lib/Cocos2dxActivity") ||
         has(cls, "org/cocos2dx/lib/GameControllerHelper") ||
         has(cls, "org/cocos2dx/cpp/");
}

/* Manager objects are sometimes reported as java/lang/Object. */
int cocos_owns_method(const char *n) {
  if (!n) return 0;
  /* Safety net for calls that arrive with an unhelpful class name. With
   * ClassLoader.loadClass implemented, cocos_owns_class() should now do the
   * routing; these stay so a single missed class lookup cannot silently turn
   * a handler into a zero-returning stub again. */
  if (!strcmp(n, "getSDKVersion")           || !strcmp(n, "getDPI") ||
      !strcmp(n, "getDoubleForKey")         || !strcmp(n, "deleteValueForKey") ||
      !strcmp(n, "getAssetsPath")           || !strcmp(n, "getVersion") ||
      !strcmp(n, "setKeepScreenOn")         || !strcmp(n, "conversionEncoding") ||
      !strcmp(n, "getCocos2dxWritablePath") || !strcmp(n, "getCocos2dxCachePath") ||
      !strcmp(n, "getCocos2dxPackageName")  || !strcmp(n, "getCurrentLanguage") ||
      !strcmp(n, "getDeviceModel")          || !strcmp(n, "getSystemVersion"))
    return 1;
  return !strcmp(n, "mount") || !strcmp(n, "launchDownloader") ||
         !strcmp(n, "getInstance") ||
         !strcmp(n, "getStringForKey") || !strcmp(n, "setStringForKey") ||
         !strcmp(n, "getIntegerForKey") || !strcmp(n, "setIntegerForKey") ||
         !strcmp(n, "getBoolForKey") || !strcmp(n, "setBoolForKey") ||
         !strcmp(n, "getFloatForKey") || !strcmp(n, "setFloatForKey") ||
         !strcmp(n, "setDoubleForKey") ||
         !strcmp(n, "showEditTextDialog") ||
         !strcmp(n, "openIMEKeyboard") || !strcmp(n, "closeIMEKeyboard") ||
         !strcmp(n, "getFontSizeAccordingHeight") ||
         !strcmp(n, "getStringWithEllipsis") ||
         !strcmp(n, "createTextBitmapShadowStroke") ||
         !strcmp(n, "createTextBitmap") ||
         !strcmp(n, "enableAccelerometer") || !strcmp(n, "disableAccelerometer") ||
         !strcmp(n, "setAccelerometerInterval") ||
         !strcmp(n, "substringUtf16") || !strcmp(n, "getUTF16Count") ||
         !strcmp(n, "createVideoWidget") ||
         !strcmp(n, "removeVideoWidget") || !strcmp(n, "setVideoUrl") ||
         !strcmp(n, "setVideoRect") || !strcmp(n, "startVideo") ||
         !strcmp(n, "stopVideo");
}


/* ---- signature-driven variadic unpacking ----
 *
 * On AArch64 a va_list keeps separate general-purpose and floating-point
 * cursors. va_arg must therefore be called with the right type in the right
 * order: pulling a double where the signature says int advances the wrong
 * cursor and every argument after it is garbage. For a call whose shape we
 * did not choose, walking the signature is the only safe way to read it.
 *
 * This matters here because Happy Wheels does not use the stock Cocos2dxBitmap
 * signature. Stock cocos2d-x 3.17 declares
 *     ([BLjava/lang/String;IFFFIIIZFFFFZFFFF)Z
 * with float colour components, while this game uses
 *     ([BLjava/lang/String;IIIIIIIIFZFFFFZIIIIFZI)Z
 * with 0-255 integer RGBA and four extra trailing fields. Reading one layout
 * with the other silently produces nonsense. */

#define MAX_JARGS 32

typedef struct {
  char    t;      /* JNI type letter; array/object refs are reported as 'L' */
  int64_t i;
  double  d;
  void   *p;
} JArg;

static int jni_unpack_args(const char *sig, va_list va, JArg *out, int max) {
  if (!sig) return 0;
  const char *p = strchr(sig, '(');
  if (!p) return 0;
  p++;

  int n = 0;
  while (*p && *p != ')' && n < max) {
    char t = *p;

    if (t == '[' || t == 'L') {
      while (*p == '[') p++;
      if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
      else if (*p) p++;
      out[n].t = 'L';
      out[n].p = va_arg(va, void *);
      n++;
      continue;
    }

    p++;
    out[n].t = t;
    switch (t) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out[n].i = va_arg(va, int);          break;
      case 'J':
        out[n].i = va_arg(va, int64_t);      break;
      case 'F': case 'D':
        out[n].d = va_arg(va, double);       break;  /* float promotes */
      default:
        out[n].i = 0;                        break;
    }
    n++;
  }
  return n;
}

static uint32_t u8next(const char **p, const char *end) {
  const unsigned char *s = (const unsigned char *)*p;
  if ((const char *)s >= end) { return 0; }
  uint32_t c = *s; int n;
  if (c < 0x80)      { n = 0; }
  else if (c < 0xE0) { c &= 0x1F; n = 1; }
  else if (c < 0xF0) { c &= 0x0F; n = 2; }
  else               { c &= 0x07; n = 3; }
  s++;
  for (int i = 0; i < n && (const char *)s < end && (*s & 0xC0) == 0x80; i++) { c = (c << 6) | (*s & 0x3F); s++; }
  *p = (const char *)s;
  return c;
}

static void cocos_show_keyboard(const char *title, const char *initial, int maxlen) {
  char out[576] = {0};
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  if (title && *title)   swkbdConfigSetGuideText(&kbd, title);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  if (maxlen > 0 && maxlen < 500) swkbdConfigSetStringLenMax(&kbd, (u32)maxlen);
  Result rc = swkbdShow(&kbd, out, sizeof out);
  swkbdClose(&kbd);
  if (R_FAILED(rc)) return;

  typedef void (*fn_setres)(void *env, void *thiz, void *jbytes);
  fn_setres setResult = (fn_setres)so_resolve_external(
      "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");
  if (setResult)
    setResult(fake_env, jni_make_object("thiz"), jni_make_byte_array(out, (int)strlen(out)));
}

static const char *current_language(void) {
  u64 lc = 0; SetLanguage sl;
  if (R_SUCCEEDED(setGetSystemLanguage(&lc)) && R_SUCCEEDED(setMakeLanguage(lc, &sl))) {
    switch (sl) {
      case SetLanguage_JA:  return "ja";
      case SetLanguage_FR: case SetLanguage_FRCA: return "fr";
      case SetLanguage_DE:  return "de";
      case SetLanguage_IT:  return "it";
      case SetLanguage_ES: case SetLanguage_ES419: return "es";
      case SetLanguage_ZHCN: case SetLanguage_ZHHANS: return "zh";
      case SetLanguage_ZHTW: case SetLanguage_ZHHANT: return "zh";
      case SetLanguage_KO:  return "ko";
      default: return "en";
    }
  }
  return "en";
}

/* Happy Wheels ships its assets in the base APK, so there is no OBB to
 * mount and no BGObbFileManager in the binary. The mount request is answered
 * by doing nothing. */
static void drive_obb_mount(int patch) { (void)patch; }

void *cocos_dispatch_object(void *recv, const void *idv, va_list va) {
  (void)recv;
  const FakeID *id = idv;
  const char *n = id->name;

  if (!strcmp(n, "getCocos2dxWritablePath") || !strcmp(n, "getCocos2dxCachePath"))
    return jni_make_string(g_root);
  if (!strcmp(n, "getCocos2dxPackageName"))
    return jni_make_string(HW_PACKAGE);
  if (!strcmp(n, "getAssetsPath")) {
    char p[HW_PATH_MAX + 8];
    snprintf(p, sizeof p, "%s/assets", g_root);
    return jni_make_string(p);
  }
  if (!strcmp(n, "getVersion"))
    return jni_make_string(HW_VERSION_NAME);
  if (!strcmp(n, "getCurrentLanguage"))
    return jni_make_string(current_language());
  if (!strcmp(n, "getSystemVersion"))
    return jni_make_string("11");
  if (!strcmp(n, "getAuxiliaryInfo") || !strcmp(n, "getDeviceModel"))
    return jni_make_string("");
  if (!strcmp(n, "getStringForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    const char *d = jni_string_utf(va_arg(va, void *));
    return jni_make_string(pref_get(k, d ? d : ""));
  }
  if (!strcmp(n, "getStringWithEllipsis")) {
    void *s = va_arg(va, void *);
    return s;
  }
  if (!strcmp(n, "substringUtf16")) {
    int blen = 0; const char *src = (const char *)jni_bytearray_data(va_arg(va, void *), &blen);
    int start = va_arg(va, int), count = va_arg(va, int);
    char out[600]; int oi = 0;
    if (src && count > 0) {
      const char *p = src, *end = src + blen; int idx = 0;
      while (p < end && idx < start + count && oi < (int)sizeof(out) - 4) {
        const char *cs = p; uint32_t cp = u8next(&p, end);
        int units = (cp >= 0x10000) ? 2 : 1;
        int bl = (int)(p - cs);
        if (idx >= start && oi + bl <= (int)sizeof(out)) { memcpy(out + oi, cs, bl); oi += bl; }
        idx += units;
      }
    }
    return jni_make_byte_array(out, oi);
  }
  if (!strcmp(n, "getInstance"))
    return jni_make_object(id->cls);
  return jni_make_object(id->cls);
}

juint cocos_dispatch_int(void *recv, const void *idv, va_list va) {
  (void)recv;
  const FakeID *id = idv;
  const char *n = id->name;

  if (!strcmp(n, "getIntegerForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    int def = va_arg(va, int);
    const char *v = pref_get(k, NULL);
    return v ? (juint)(int64_t)atoll(v) : (juint)def;
  }
  if (!strcmp(n, "getBoolForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    int def = va_arg(va, int);
    const char *v = pref_get(k, NULL);
    return v ? (juint)(atoi(v) != 0) : (juint)(def != 0);
  }
  if (!strcmp(n, "getDPI"))               return (juint)config.dpi;

  /* The whole reason audio was silent.
   *
   * AudioPlayerProvider::getAudioPlayer() begins with, in effect:
   *     if (sdk <= 16) return createUrlAudioPlayer(info);
   * because the PCM buffer-queue path needs API 17. Unhandled, this method
   * returned 0, so every single sound took the UrlAudioPlayer branch: the
   * compressed file handed to a platform decoder that does not exist here.
   * The bundled Tremor decoder was never even reached.
   *
   * Reporting a modern API level lets cocos decode with AudioDecoderOgg and
   * play through a buffer queue, which this port does implement. The only
   * three callers are in AudioPlayerProvider, so nothing else is affected. */
  if (!strcmp(n, "getSDKVersion")) {
    static int logged = 0;
    if (!logged) { logged = 1; debugLogNote("[jni] getSDKVersion -> 25 (PCM audio path)\n"); }
    return 25;   /* Android 7.1 */
  }
  if (!strcmp(n, "getFontSizeAccordingHeight")) { return (juint)va_arg(va, int); }
  if (!strcmp(n, "getUTF16Count")) {
    int blen = 0; const char *src = (const char *)jni_bytearray_data(va_arg(va, void *), &blen);
    int cnt = 0;
    if (src) { const char *p = src, *end = src + blen; while (p < end) { uint32_t cp = u8next(&p, end); cnt += (cp >= 0x10000) ? 2 : 1; } }
    return (juint)cnt;
  }
  if (!strcmp(n, "createVideoWidget")) return (juint)cocos_video_create();
  if (!strcmp(n, "isBackgroundMusicPlaying")) return 0;
  if (!strcmp(n, "playEffect")) {
    static int sid = 1; return (juint)(sid++);
  }
  if (!strcmp(n, "createTextBitmapShadowStroke") ||
      !strcmp(n, "createTextBitmap")) {
    JArg a[MAX_JARGS];
    int na = jni_unpack_args(id->sig, va, a, MAX_JARGS);
    if (na < 9) return 0;

    /* a[0] text bytes, a[1] font name, a[2] point size are common to both
     * layouts. What follows differs, so key off the colour argument's type:
     * integer 0-255 components mean this game's variant, float means stock. */
    int int_colours = (a[3].t != 'F' && a[3].t != 'D');

    int cr, cg, cb, align, w, h;
    if (int_colours) {
      cr = (int)a[3].i; cg = (int)a[4].i; cb = (int)a[5].i;  /* a[6] is alpha */
      if (na < 10) return 0;
      align = (int)a[7].i; w = (int)a[8].i; h = (int)a[9].i;
    } else {
      double r = a[3].d, g = a[4].d, b = a[5].d;
      double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      int sc = (mx <= 1.0) ? 255 : 1;
      cr = (int)(r * sc + 0.5); cg = (int)(g * sc + 0.5); cb = (int)(b * sc + 0.5);
      align = (int)a[6].i; w = (int)a[7].i; h = (int)a[8].i;
    }
    if (cr < 0) cr = 0;
    if (cr > 255) cr = 255;
    if (cg < 0) cg = 0;
    if (cg > 255) cg = 255;
    if (cb < 0) cb = 0;
    if (cb > 255) cb = 255;

    /* The text arrives as a byte[], not a String, and is not NUL-terminated. */
    int blen = 0;
    const char *bytes = (const char *)jni_bytearray_data(a[0].p, &blen);
    if (!bytes || blen <= 0) return 0;
    char *text = malloc((size_t)blen + 1);
    if (!text) return 0;
    memcpy(text, bytes, (size_t)blen);
    text[blen] = 0;

    /* Device::TextAlign packs horizontal in the low nibble: 1 left, 2 right,
     * 3 centre. cocos_text_render wants 0 centre, 1 left, 2 right. */
    int ha = align & 0xF;
    int ra = (ha == 1) ? 0x01 : (ha == 2) ? 0x02 : 0;

    int W = 0, H = 0;
    unsigned char *rgba = cocos_text_render(text, (int)a[2].i, cr, cg, cb, w, h, ra, &W, &H);
    free(text);
    if (!rgba) return 0;

    typedef void (*fn_init)(void *env, void *thiz, int w, int h, void *jbytes);
    fn_init nativeInit = (fn_init)so_resolve_external(
        "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
    if (nativeInit)
      nativeInit(fake_env, jni_make_object("thiz"), W, H, jni_make_byte_array(rgba, W * H * 4));
    free(rgba);
    return 1;
  }
  return 0;
}

float cocos_dispatch_float(void *recv, const void *idv, va_list va) {
  (void)recv;
  const FakeID *id = idv;
  const char *n = id->name;
  if (!strcmp(n, "getFloatForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    double def = va_arg(va, double);
    const char *v = pref_get(k, NULL);
    return v ? (float)atof(v) : (float)def;
  }
  if (!strcmp(n, "getEffectsVolume") || !strcmp(n, "getBackgroundMusicVolume"))
    return 1.0f;
  return 0.0f;
}

double cocos_dispatch_double(void *recv, const void *idv, va_list va) {
  (void)recv;
  const FakeID *id = idv;
  const char *n = id->name;

  if (!strcmp(n, "getDoubleForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    double def = va_arg(va, double);
    const char *v = pref_get(k, NULL);
    return v ? atof(v) : def;
  }
  return 0.0;
}

void cocos_dispatch_void(void *recv, const void *idv, va_list va) {
  (void)recv;
  const FakeID *id = idv;
  const char *n = id->name;
  char buf[64];

  if (!strcmp(n, "setStringForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    const char *v = jni_string_utf(va_arg(va, void *));
    pref_set(k, v ? v : ""); return;
  }
  if (!strcmp(n, "setIntegerForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    int v = va_arg(va, int);
    snprintf(buf, sizeof buf, "%d", v); pref_set(k, buf); return;
  }
  if (!strcmp(n, "setBoolForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    int v = va_arg(va, int);
    pref_set(k, v ? "1" : "0"); return;
  }
  if (!strcmp(n, "setFloatForKey") || !strcmp(n, "setDoubleForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    double v = va_arg(va, double);
    snprintf(buf, sizeof buf, "%.9g", v); pref_set(k, buf); return;
  }

  if (!strcmp(n, "showEditTextDialog")) {
    const char *title = jni_string_utf(va_arg(va, void *));
    void *jcontent    = va_arg(va, void *);
    (void)va_arg(va, int); (void)va_arg(va, int); (void)va_arg(va, int);
    int maxLength     = va_arg(va, int);
    int clen = 0; const char *content = (const char *)jni_bytearray_data(jcontent, &clen);
    char init[512] = {0};
    if (content && clen > 0) { int k = clen < (int)sizeof(init) - 1 ? clen : (int)sizeof(init) - 1; memcpy(init, content, k); }
    cocos_show_keyboard(title, init, maxLength);
    return;
  }

  if (!strcmp(n, "removeVideoWidget")) {
    cocos_video_remove(va_arg(va, int));
    return;
  }
  if (!strcmp(n, "setVideoUrl")) {
    int index = va_arg(va, int);
    (void)va_arg(va, int);
    const char *url = jni_string_utf(va_arg(va, void *));
    cocos_video_set_source(index, url);
    return;
  }
  if (!strcmp(n, "setVideoRect")) {
    int index = va_arg(va, int);
    int left = va_arg(va, int);
    int top = va_arg(va, int);
    int width = va_arg(va, int);
    int height = va_arg(va, int);
    cocos_video_set_rect(index, left, top, width, height);
    return;
  }
  if (!strcmp(n, "startVideo")) {
    opensles_ensure_output();
    cocos_video_start(va_arg(va, int));
    return;
  }
  if (!strcmp(n, "stopVideo")) {
    cocos_video_stop(va_arg(va, int));
    return;
  }

  if (!strcmp(n, "mount")) {
    int patch = va_arg(va, int);
    drive_obb_mount(patch != 0);
    return;
  }

  if (!strcmp(n, "setKeepScreenOn")) { (void)va_arg(va, int); return; }

  /* The game exports Cocos2dxAccelerometer_onSensorChanged, so it will accept
   * sensor data, but nothing here feeds it. Accept the enable/disable calls
   * so they stop landing in the generic fallback; wiring them to the console
   * accelerometer via android_get_orientation() is a later job. */
  if (!strcmp(n, "enableAccelerometer") || !strcmp(n, "disableAccelerometer")) return;
  if (!strcmp(n, "setAccelerometerInterval")) { (void)va_arg(va, double); return; }
  if (!strcmp(n, "deleteValueForKey")) {
    const char *k = jni_string_utf(va_arg(va, void *));
    if (k) pref_delete(k);
    return;
  }
  if (!strcmp(n, "conversionEncoding")) { return; }

  if (!strcmp(n, "terminateProcess") || !strcmp(n, "end")) { jni_quit_requested = 1; return; }
}
