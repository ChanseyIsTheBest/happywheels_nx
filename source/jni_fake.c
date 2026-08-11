/* Minimal JNI environment used by libcocos2dcpp. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "jni_fake.h"
#include "android_native_cocos.h"
#include "libc_shim.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

/* Fake object model. */

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

volatile int jni_quit_requested = 0;

/* Local references. */

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// Pool recurring strings outside local-reference frames.
#define MAX_ISTR 512
static FakeString istr_pool[MAX_ISTR];
static int istr_count = 0;

static void free_ref(void *ref) {
  if (!ref)
    return;
  if ((char *)ref >= (char *)istr_pool && (char *)ref < (char *)&istr_pool[MAX_ISTR])
    return;  // interned string -- pooled, never freed
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are pooled
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

/* Object constructors. */

// Pool opaque objects by class label.
#define MAX_IOBJ 128
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) r = &iobj_pool[0];
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  mutexLock(&locals_lock);
  for (int i = 0; i < istr_count; i++)            // repeats reuse the pooled string
    if (!strcmp(istr_pool[i].utf, u)) { void *r = &istr_pool[i]; mutexUnlock(&locals_lock); return r; }
  if (istr_count < MAX_ISTR) {
    FakeString *s = &istr_pool[istr_count++];
    s->tag = TAG_STRING;
    s->utf = strdup(u);
    mutexUnlock(&locals_lock);
    return s;                                      // pooled, not reg_local'd
  }
  mutexUnlock(&locals_lock);
  FakeString *s = calloc(1, sizeof(*s));           // pool full: one-off local string
  s->tag = TAG_STRING;
  s->utf = strdup(u);
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

/* Transient primitive arrays for Cocos2dxRenderer.nativeTouchesMove/Cancel, which
 * take (int[] ids, float[] xs, float[] ys). The engine reads them SYNCHRONOUSLY
 * during the call (GetIntArrayElements/GetFloatArrayElements + Release), so we
 * hand back small pooled arrays reused round-robin -- NOT reg_local'd, so a
 * defensive DeleteLocalRef won't free static storage (delete_local scans the
 * locals table and simply won't find them). 8 slots >> the ~3 live per frame. */
#define TXA_SLOTS 8
#define TXA_CAP   32
static void *make_transient_array(const void *src, int n, int elem_size) {
  static FakePriArray pool[TXA_SLOTS];
  static uint8_t buf[TXA_SLOTS][TXA_CAP * 4];
  static int next = 0;
  if (n < 0) n = 0;
  if (n > TXA_CAP) n = TXA_CAP;
  int s = next; next = (next + 1) % TXA_SLOTS;
  FakePriArray *a = &pool[s];
  a->tag = TAG_PRIARR; a->len = n; a->elem_size = elem_size; a->data = buf[s];
  if (n) memcpy(buf[s], src, (size_t)n * elem_size);
  return a;
}
void *jni_make_int_array(const int *src, int n)     { return make_transient_array(src, n, 4); }
void *jni_make_float_array(const float *src, int n) { return make_transient_array(src, n, 4); }

/* Owned byte array for software-keyboard results. */
void *jni_make_byte_array(const void *src, int n) {
  if (n < 0) n = 0;
  char *d = malloc(n > 0 ? n : 1);
  if (n > 0 && src) memcpy(d, src, n);
  return make_pri_array_adopt(d, n, 1);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// Java String.length() counts UTF-16 code units.
/* UTF-8 -> UTF-16. Returns the number of UTF-16 code units. If buf is NULL
 * it only counts. GetStringLength, GetStringChars and GetStringRegion all go
 * through this, because cocos sizes its buffer from one and fills from
 * another -- if they disagree it reads off the end.
 *
 * Named jni_* deliberately: libnx declares its own utf8_to_utf16() in
 * switch/runtime/util/utf.h with a different signature, and switch.h pulls
 * it in. */
static juint jni_utf8_to_utf16(const char *str, uint16_t *buf, juint cap) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      if (buf && n + 1 < cap) {
        buf[n]     = (uint16_t)(0xD800u + (cp >> 10));
        buf[n + 1] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
      }
      n += 2;
    } else {
      if (buf && n < cap) buf[n] = (uint16_t)cp;
      n += 1;
    }
    p += adv;
  }
  return n;
}

static juint utf16_len(const char *str) { return jni_utf8_to_utf16(str, NULL, 0); }

/* Interned classes and singletons. */

#define MAX_CLASSES 128
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) return &class_pool[0];
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  return c;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && c->tag == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;
static FakeObject *g_asset_mgr = NULL;

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// Cache the ClassLoader outside local-reference frames.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

/* Method and field IDs. */

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) return &id_pool[0];
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls ? cls : "", sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

/* Dispatch. */

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

static const char *lang_code(void) {
  // Use Japanese when selected by the system; otherwise use English.
  static int ja = -1;
  if (ja < 0) {
    ja = 0;
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl)))
        ja = (sl == SetLanguage_JA);
      setExit();
    }
  }
  return ja ? "ja" : "en";
}

// Return the first non-empty String argument.
static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

const char *jni_string_utf(void *jstr);

// AudioManager property selected by the preceding static-field lookup.
static int g_last_output_prop = 0;

static void *getproperty_value(const char *key) {
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  if (which == 1) return jni_make_string("48000");
  if (which == 2) return jni_make_string("256");
  return jni_make_string("");
}

static void *act_object(const FakeID *id, va_list va) {
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  /* ClassLoader.loadClass / findClass.
   *
   * cocos's JniHelper::getClassID() resolves every class through the class
   * loader rather than FindClass once a loader is available -- and this fake
   * environment does provide one. It passes the dotted name.
   *
   * Answering with a generic java/lang/Object threw the requested identity
   * away, so every method looked up through JniHelper arrived at the
   * dispatchers with the wrong class and fell through to the generic
   * fallbacks. Cocos2dxHelper methods only worked at all because
   * cocos_owns_method() matches a hand-maintained list of names; anything not
   * on that list silently returned zero. getSDKVersion was one of those, which
   * is why audio stayed on the UrlAudioPlayer path.
   *
   * Interning the real name makes cocos_owns_class() work as intended and the
   * name list becomes a safety net rather than the only routing mechanism. */
  if (name_has(id->name, "loadClass") || name_has(id->name, "findClass")) {
    const char *dotted = jni_string_utf(va_arg(va, void *));
    if (dotted && *dotted) {
      char slashed[128];
      size_t i = 0;
      for (; dotted[i] && i + 1 < sizeof slashed; i++)
        slashed[i] = (dotted[i] == '.') ? '/' : dotted[i];
      slashed[i] = 0;
      return intern_class(slashed);
    }
    return intern_class("java/lang/Object");
  }
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  if (name_has(id->name, "VersionName")) return jni_make_string(HW_VERSION_NAME);
  if (name_has(id->name, "PackageName")) return jni_make_string(HW_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  if (name_has(id->cls, "Locale")) {
    int ja = !strcmp(lang_code(), "ja");
    if (!strcmp(id->name, "getCountry"))     return jni_make_string(ja ? "JP" : "US");
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string(ja ? "jpn" : "eng");
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string(ja ? "JPN" : "USA");
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string(ja ? "ja_JP" : "en_US");
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(hw_data_root()));
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  if (sig_returns(id->sig, "Ljava/lang/String;"))
    return jni_make_string("");
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    return (juint)(s ? strtol(s, NULL, 10) : 0);
  }
  (void)va;
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp"))
    jni_quit_requested = 1;
}

/* Class dispatch. */

#include "cocos_jni.h"
#include "hw_sdkbox.h"
#include "util.h"

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  // String.getBytes([charset]).
  if (recv && *(uint32_t *)recv == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  if (cocos_owns_class(id->cls) || cocos_owns_method(id->name)) return cocos_dispatch_object(recv, id, va);
  return act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  if (recv && *(uint32_t *)recv == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
  }
  if (cocos_owns_class(id->cls) || cocos_owns_method(id->name)) return cocos_dispatch_int(recv, id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  if (cocos_owns_class(id->cls) || cocos_owns_method(id->name)) return cocos_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static double dispatch_double(void *recv, const FakeID *id, va_list va) {
  if (cocos_owns_class(id->cls) || cocos_owns_method(id->name)) return cocos_dispatch_double(recv, id, va);
  return 0.0;
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  if (cocos_owns_class(id->cls) || cocos_owns_method(id->name)) { cocos_dispatch_void(recv, id, va); return; }
  act_void(id, va);
}

/* JNIEnv methods. */

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  if (name && hw_sdkbox_owns_class(name)) return hw_sdkbox_class(name);
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

// Decode String(byte[][,charset]) constructors as UTF-8.
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

// Fake objects are compatible with any non-boxed class.
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  if (obj && *(uint32_t *)obj == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
  }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

/* Instance and static calls share class-aware dispatch. */

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)
CALL_VARIADIC(j_CallDoubleMethod, double, dispatch_double)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticDoubleMethod   j_CallDoubleMethod
#define j_CallStaticDoubleMethodV  j_CallDoubleMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

/* jvalue[] call variants. */
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  return j_CallObjectMethod(e, r, id);
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallBooleanMethod(e, r, id); }
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    return (juint)(s ? strtol(s, NULL, 10) : 0);
  }
  (void)a; return j_CallIntMethod(e, r, id);
}
static juint j_CallLongMethodA   (void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallLongMethod(e, r, id); }
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallFloatMethod(e, r, id); }
static double j_CallDoubleMethodA(void *e, void *r, FakeID *id, const void *a){ (void)a; return j_CallDoubleMethod(e, r, id); }
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){ (void)a; j_CallVoidMethod(e, r, id); }
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticDoubleMethodA  j_CallDoubleMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

/* Strings. */

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// Native paths are ASCII, so UTF-16 offsets equal UTF-8 byte offsets here.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf || len <= 0) return;
  const char *utf = obj_str(jstr);

  juint total = jni_utf8_to_utf16(utf, NULL, 0);
  uint16_t *tmp = malloc((size_t)(total + 1) * sizeof(uint16_t));
  if (!tmp) return;
  jni_utf8_to_utf16(utf, tmp, total);

  /* start and len are in UTF-16 units, matching GetStringLength. */
  if (start < 0) start = 0;
  if ((juint)start > total) start = (int)total;
  if ((juint)(start + len) > total) len = (int)total - start;
  for (int i = 0; i < len; i++) buf[i] = tmp[start + i];
  free(tmp);
}
/* cocos2d-x StringUtils::getStringUTFCharsJNI() calls this, then builds a
 * std::u16string from the result and GetStringLength. Returning NULL here
 * makes that constructor memcpy from address 0. */
static uint16_t g_empty_utf16 = 0;

static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *utf = obj_str(jstr);
  juint n = jni_utf8_to_utf16(utf, NULL, 0);

  uint16_t *buf = malloc((size_t)(n + 1) * sizeof(uint16_t));
  if (!buf) {
    /* Never hand back NULL -- callers memcpy from it without checking.
     * An empty string is wrong but survivable; NULL is not. */
    if (is_copy) *is_copy = 1;
    return &g_empty_utf16;
  }
  jni_utf8_to_utf16(utf, buf, n);
  buf[n] = 0;
  if (is_copy) *is_copy = 1;
  return buf;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  if (chars && chars != &g_empty_utf16) free((void *)chars);
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

/* Arrays. */


/* Primitive array access.
 *
 * FakePriArray carries elem_size, so one generic implementation covers every
 * primitive type. Elements are handed out by reference (isCopy = 0), so the
 * matching Release is a no-op -- there is nothing to write back.
 *
 * Leaving these unwired is not harmless: the zero stub returns NULL and every
 * caller memcpys straight from it. nativeTouchesMove reads the int/float
 * arrays this port builds for pointer events, and BitmapDC reads the glyph
 * bitmap out of a byte array, so both paths land here. */
static void *j_GetPrimitiveArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  FakePriArray *a = arr;
  if (is_copy) *is_copy = 0;
  if (a && a->tag == TAG_PRIARR) return a->data;
  return NULL;
}

static void j_ReleasePrimitiveArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
  /* Elements were not copied, so there is nothing to commit or free. */
}

static int pri_clamp(FakePriArray *a, int start, int len) {
  if (!a || a->tag != TAG_PRIARR || !a->data) return 0;
  if (start < 0 || len <= 0 || start > a->len) return 0;
  if (start + len > a->len) len = a->len - start;
  return len;
}

static void j_GetPrimitiveArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  len = pri_clamp(a, start, len);
  if (!len || !buf) return;
  memcpy(buf, (char *)a->data + (size_t)start * a->elem_size,
         (size_t)len * a->elem_size);
}

static void j_SetPrimitiveArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  len = pri_clamp(a, start, len);
  if (!len || !buf) return;
  memcpy((char *)a->data + (size_t)start * a->elem_size, buf,
         (size_t)len * a->elem_size);
}

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
/* The remaining primitive types. Left unwired these returned NULL, and every
 * caller feeds the result straight to Get<Type>ArrayElements and copies from
 * it -- the same NULL-deref shape that broke GetStringChars. */
static void *j_NewBooleanArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewCharArray  (void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewShortArray (void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewLongArray  (void *env, int len) { (void)env; return new_pri_array(len, 8); }
static void *j_NewDoubleArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }

/* Field reads that return a double. The generic stub returns uint64_t in x0,
 * but the caller reads d0 -- so an unwired slot yields whatever happened to be
 * in the FP register, not zero. Same trap as CallDoubleMethod. */
static double j_GetDoubleFieldZero(void *env, void *obj, void *fid) {
  (void)env; (void)obj; (void)fid; return 0.0;
}

/* Direct ByteBuffers: pointer-returning, so NULL is unsafe for the same
 * reason. Nothing here can back one, so report a zero-capacity buffer. */
static void *j_NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
  (void)env; (void)cap; (void)addr; return NULL;
}
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; (void)buf; return NULL;
}
static int64_t j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; (void)buf; return -1;   /* -1 = not a direct buffer, per JNI */
}

/* Ignoring FatalError lets a condition the game considers unrecoverable
 * continue silently into much stranger failures. */
static void j_FatalError(void *env, const char *msg) {
  (void)env;
  fprintf(stderr, "[jni] FatalError: %s\n", msg ? msg : "(null)");
  abort();
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

/* Fields. */
#define APP_VERSION_NAME HW_VERSION_NAME
#define APP_VERSION_CODE HW_VERSION_CODE
#define NX_SDK_INT 31

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return 720;
    if (!strcmp(n, "heightPixels")) return 1280;
    if (!strcmp(n, "densityDpi"))   return 320;
  }
  return 0;
}

static float field_float(const FakeID *id) {
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }

/* Reflection. */
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env; (void)m; return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env; (void)f; return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n;
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

/* JNI table indices follow the specification. */


/* ---- unimplemented-slot diagnostics ----
 *
 * A slot left unwired used to return 0 silently. That is the single failure
 * mode that has cost the most here: GetStringChars, the primitive-array
 * family and GetDoubleField all returned a quiet zero that the game then
 * dereferenced somewhere else entirely, so the crash pointed at the victim
 * rather than the cause.
 *
 * Each slot now gets its own thunk that records its index the first time it
 * is touched. jni_dump_unimplemented() writes them next to the NRO, so an
 * unimplemented slot names itself instead of having to be inferred backwards
 * from a fault address. */

static uint8_t g_unimpl_hit[233];

static uint64_t jni_unimpl_note(int slot) {
  if (slot >= 0 && slot < (int)(sizeof g_unimpl_hit) && !g_unimpl_hit[slot]) {
    g_unimpl_hit[slot] = 1;
    /* Written through immediately: the previous version only dumped after the
     * main loop, so a hang produced no file at all. */
    debugLogNote("[jni] unimplemented JNIEnv slot %d (returned 0)\n", slot);
  }
  return 0;
}

void jni_dump_unimplemented(const char *data_root) {
  int any = 0;
  for (size_t i = 0; i < sizeof g_unimpl_hit; i++) if (g_unimpl_hit[i]) { any = 1; break; }
  if (!any) return;

  char path[600];
  snprintf(path, sizeof path, "%s/jni_unimplemented.txt", data_root ? data_root : ".");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "JNIEnv slots the game called that this port does not implement.\n"
             "Each returned 0/NULL. Indices are JNINativeInterface_ positions.\n\n");
  for (size_t i = 0; i < sizeof g_unimpl_hit; i++)
    if (g_unimpl_hit[i]) fprintf(f, "  slot %zu\n", i);
  fclose(f);
}

#define UT_(n) static uint64_t j_unimpl_##n(void *e, ...) { (void)e; return jni_unimpl_note(n); }
UT_(0) UT_(1) UT_(2) UT_(3) UT_(4) UT_(5) UT_(6) UT_(7)
UT_(8) UT_(9) UT_(10) UT_(11) UT_(12) UT_(13) UT_(14) UT_(15)
UT_(16) UT_(17) UT_(18) UT_(19) UT_(20) UT_(21) UT_(22) UT_(23)
UT_(24) UT_(25) UT_(26) UT_(27) UT_(28) UT_(29) UT_(30) UT_(31)
UT_(32) UT_(33) UT_(34) UT_(35) UT_(36) UT_(37) UT_(38) UT_(39)
UT_(40) UT_(41) UT_(42) UT_(43) UT_(44) UT_(45) UT_(46) UT_(47)
UT_(48) UT_(49) UT_(50) UT_(51) UT_(52) UT_(53) UT_(54) UT_(55)
UT_(56) UT_(57) UT_(58) UT_(59) UT_(60) UT_(61) UT_(62) UT_(63)
UT_(64) UT_(65) UT_(66) UT_(67) UT_(68) UT_(69) UT_(70) UT_(71)
UT_(72) UT_(73) UT_(74) UT_(75) UT_(76) UT_(77) UT_(78) UT_(79)
UT_(80) UT_(81) UT_(82) UT_(83) UT_(84) UT_(85) UT_(86) UT_(87)
UT_(88) UT_(89) UT_(90) UT_(91) UT_(92) UT_(93) UT_(94) UT_(95)
UT_(96) UT_(97) UT_(98) UT_(99) UT_(100) UT_(101) UT_(102) UT_(103)
UT_(104) UT_(105) UT_(106) UT_(107) UT_(108) UT_(109) UT_(110) UT_(111)
UT_(112) UT_(113) UT_(114) UT_(115) UT_(116) UT_(117) UT_(118) UT_(119)
UT_(120) UT_(121) UT_(122) UT_(123) UT_(124) UT_(125) UT_(126) UT_(127)
UT_(128) UT_(129) UT_(130) UT_(131) UT_(132) UT_(133) UT_(134) UT_(135)
UT_(136) UT_(137) UT_(138) UT_(139) UT_(140) UT_(141) UT_(142) UT_(143)
UT_(144) UT_(145) UT_(146) UT_(147) UT_(148) UT_(149) UT_(150) UT_(151)
UT_(152) UT_(153) UT_(154) UT_(155) UT_(156) UT_(157) UT_(158) UT_(159)
UT_(160) UT_(161) UT_(162) UT_(163) UT_(164) UT_(165) UT_(166) UT_(167)
UT_(168) UT_(169) UT_(170) UT_(171) UT_(172) UT_(173) UT_(174) UT_(175)
UT_(176) UT_(177) UT_(178) UT_(179) UT_(180) UT_(181) UT_(182) UT_(183)
UT_(184) UT_(185) UT_(186) UT_(187) UT_(188) UT_(189) UT_(190) UT_(191)
UT_(192) UT_(193) UT_(194) UT_(195) UT_(196) UT_(197) UT_(198) UT_(199)
UT_(200) UT_(201) UT_(202) UT_(203) UT_(204) UT_(205) UT_(206) UT_(207)
UT_(208) UT_(209) UT_(210) UT_(211) UT_(212) UT_(213) UT_(214) UT_(215)
UT_(216) UT_(217) UT_(218) UT_(219) UT_(220) UT_(221) UT_(222) UT_(223)
UT_(224) UT_(225) UT_(226) UT_(227) UT_(228) UT_(229) UT_(230) UT_(231)
UT_(232)
#undef UT_

#define UT(n) (void *)j_unimpl_##n
static void *const g_unimpl_thunks[233] = {
  UT(0),UT(1),UT(2),UT(3),UT(4),UT(5),UT(6),UT(7),
  UT(8),UT(9),UT(10),UT(11),UT(12),UT(13),UT(14),UT(15),
  UT(16),UT(17),UT(18),UT(19),UT(20),UT(21),UT(22),UT(23),
  UT(24),UT(25),UT(26),UT(27),UT(28),UT(29),UT(30),UT(31),
  UT(32),UT(33),UT(34),UT(35),UT(36),UT(37),UT(38),UT(39),
  UT(40),UT(41),UT(42),UT(43),UT(44),UT(45),UT(46),UT(47),
  UT(48),UT(49),UT(50),UT(51),UT(52),UT(53),UT(54),UT(55),
  UT(56),UT(57),UT(58),UT(59),UT(60),UT(61),UT(62),UT(63),
  UT(64),UT(65),UT(66),UT(67),UT(68),UT(69),UT(70),UT(71),
  UT(72),UT(73),UT(74),UT(75),UT(76),UT(77),UT(78),UT(79),
  UT(80),UT(81),UT(82),UT(83),UT(84),UT(85),UT(86),UT(87),
  UT(88),UT(89),UT(90),UT(91),UT(92),UT(93),UT(94),UT(95),
  UT(96),UT(97),UT(98),UT(99),UT(100),UT(101),UT(102),UT(103),
  UT(104),UT(105),UT(106),UT(107),UT(108),UT(109),UT(110),UT(111),
  UT(112),UT(113),UT(114),UT(115),UT(116),UT(117),UT(118),UT(119),
  UT(120),UT(121),UT(122),UT(123),UT(124),UT(125),UT(126),UT(127),
  UT(128),UT(129),UT(130),UT(131),UT(132),UT(133),UT(134),UT(135),
  UT(136),UT(137),UT(138),UT(139),UT(140),UT(141),UT(142),UT(143),
  UT(144),UT(145),UT(146),UT(147),UT(148),UT(149),UT(150),UT(151),
  UT(152),UT(153),UT(154),UT(155),UT(156),UT(157),UT(158),UT(159),
  UT(160),UT(161),UT(162),UT(163),UT(164),UT(165),UT(166),UT(167),
  UT(168),UT(169),UT(170),UT(171),UT(172),UT(173),UT(174),UT(175),
  UT(176),UT(177),UT(178),UT(179),UT(180),UT(181),UT(182),UT(183),
  UT(184),UT(185),UT(186),UT(187),UT(188),UT(189),UT(190),UT(191),
  UT(192),UT(193),UT(194),UT(195),UT(196),UT(197),UT(198),UT(199),
  UT(200),UT(201),UT(202),UT(203),UT(204),UT(205),UT(206),UT(207),
  UT(208),UT(209),UT(210),UT(211),UT(212),UT(213),UT(214),UT(215),
  UT(216),UT(217),UT(218),UT(219),UT(220),UT(221),UT(222),UT(223),
  UT(224),UT(225),UT(226),UT(227),UT(228),UT(229),UT(230),UT(231),
  UT(232)
};
#undef UT

static void *env_table[233];
static void **env_table_ptr = env_table;
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (size_t i = 0; i < sizeof(env_table) / sizeof(*env_table); i++)
    env_table[i] = g_unimpl_thunks[i];

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // jvalue[] instance variants
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[60]  = (void *)j_CallDoubleMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // jvalue[] static variants
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[140] = (void *)j_CallStaticDoubleMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[175] = (void *)j_NewBooleanArray;
  env_table[177] = (void *)j_NewCharArray;
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[180] = (void *)j_NewLongArray;
  env_table[182] = (void *)j_NewDoubleArray;
  env_table[103] = (void *)j_GetDoubleFieldZero;        // GetDoubleField
  env_table[153] = (void *)j_GetDoubleFieldZero;        // GetStaticDoubleField
  env_table[18]  = (void *)j_FatalError;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  /* Primitive arrays: Get/Release elements 183-198, Get/Set region 199-214.
   * One generic implementation per group; elem_size lives in the array. */
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPrimitiveArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePrimitiveArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPrimitiveArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPrimitiveArrayRegion;
  env_table[222] = (void *)j_GetPrimitiveArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePrimitiveArrayElements; // ReleasePrimitiveArrayCritical

  env_table[220] = (void *)j_GetStringRegion;
  env_table[224] = (void *)j_GetStringChars;      // GetStringCritical
  env_table[225] = (void *)j_ReleaseStringChars;  // ReleaseStringCritical
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
