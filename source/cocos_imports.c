/* Additional imports required by libcocos2dcpp.so. */
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>
#include <signal.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <zlib.h>
#include <switch.h>
#include <unistd.h>
#include "imports.h"
#include "opensles.h"
#include "cocos_assets.h"

extern long z_lseek(int fd, long off, int whence);

// Avoid the setjmp macro when exposing it as sigsetjmp.
extern int setjmp();

/* bionic declares _ctype_ as a POINTER to a 257-entry table, and classifies
 * with table[c + 1] -- index 0 is the EOF slot. Two things follow, and both
 * were wrong here before:
 *
 *   1. The import must resolve to the address of a pointer VARIABLE, not to
 *      the table. The game emits `ldr x9,[got]; ldr x9,[x9]` -- a double load
 *      -- so binding the table makes it read the first eight table bytes as
 *      if they were the pointer. Entries 0..7 are all control characters
 *      (0x20), which is how a dereference of 0x2020202020202020 happens.
 *   2. Character c lives at index c + 1, not c.
 *
 * Flag values are the shared BSD set, identical between bionic and newlib:
 * upper 0x01, lower 0x02, digit 0x04, space 0x08, punct 0x10, cntrl 0x20,
 * xdigit 0x40, blank 0x80. */
static unsigned char z_ctype_tab[257];
const unsigned char *z_ctype_ptr = z_ctype_tab;

__attribute__((constructor)) static void z_ctype_init(void) {
  z_ctype_tab[0] = 0;                     /* EOF slot */
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c))  f |= 0x01;
    if (islower(c))  f |= 0x02;
    if (isdigit(c))  f |= 0x04;
    if (isspace(c))  f |= 0x08;
    if (ispunct(c))  f |= 0x10;
    if (iscntrl(c))  f |= 0x20;
    if (isxdigit(c)) f |= 0x40;
    if (c == ' ')    f |= 0x80;
    z_ctype_tab[c + 1] = f;
  }
}

static long z_stub0(void) { return 0; }
static long z_stubneg1(void) { return -1; }

static long z_prctl(int option, unsigned long a2, unsigned long a3,
                    unsigned long a4, unsigned long a5) {
  (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
  return 0;
}
static int z_pthread_setname_np(void *t, const char *n) { (void)t; (void)n; return 0; }

int z_pthread_attr_getstack(const void *attr, void **stackaddr, size_t *stacksize) {
  (void)attr;
  uintptr_t sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)sp)) && mi.size) {
    if (stackaddr) *stackaddr = (void *)(uintptr_t)mi.addr;
    if (stacksize) *stacksize = (size_t)mi.size;
  } else {
    if (stackaddr) *stackaddr = (void *)(sp & ~0xFFFFFull);
    if (stacksize) *stacksize = 0x100000;
  }
  return 0;
}
int z_pthread_getattr_np(void *t, void *a) { (void)t; (void)a; return 0; }

int z_getpagesize(void) { return 0x1000; }
int z_pthread_equal(unsigned long a, unsigned long b) { return a == b; }
int z_gettid(void) { return 1; }
int z_dup(int fd) { int n = dup(fd); return n < 0 ? fd : n; }

int z_strcasecmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcasecmp(a, b);
}
int z_strncasecmp(const char *a, const char *b, unsigned long n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncasecmp(a, b, n);
}
char *z_basename(const char *path) {
  if (!path || !*path) return (char *)".";
  const char *s = strrchr(path, '/');
  return (char *)(s ? s + 1 : path);
}
int z_isdigit_l (int c,void*l){ (void)l; return isdigit(c); }
int z_islower_l (int c,void*l){ (void)l; return islower(c); }
int z_isupper_l (int c,void*l){ (void)l; return isupper(c); }
int z_isxdigit_l(int c,void*l){ (void)l; return isxdigit(c); }
int z_tolower_l (int c,void*l){ (void)l; return tolower(c); }
int z_toupper_l (int c,void*l){ (void)l; return toupper(c); }
void z_sincos(double x,double*s,double*c){ if(s)*s=sin(x); if(c)*c=cos(x); }
void*z_memrchr(const void*s,int c,unsigned long n){ const unsigned char*p=(const unsigned char*)s+n; while(n--){ if(*--p==(unsigned char)c) return (void*)p; } return 0; }

int   z_isnan (double x){ return x!=x; }
int   z_isnanf(float  x){ return x!=x; }

// Resolve OES map-buffer functions lazily from Mesa.
void *glMapBufferOES(GLenum target, GLenum access) {
  static void *(*p)(GLenum, GLenum) = NULL; static int tried = 0;
  if (!tried) { tried = 1; p = (void *(*)(GLenum, GLenum))eglGetProcAddress("glMapBufferOES"); }
  return p ? p(target, access) : NULL;
}
unsigned char glUnmapBufferOES(GLenum target) {
  static unsigned char (*p)(GLenum) = NULL; static int tried = 0;
  if (!tried) { tried = 1; p = (unsigned char (*)(GLenum))eglGetProcAddress("glUnmapBufferOES"); }
  return p ? p(target) : 0;
}
int   z_putchar(int c){ return fputc(c, stdout); }
char *z_stpcpy(char *d, const char *s){ while((*d=*s)){d++;s++;} return d; }
time_t z_timegm(struct tm *tm){ return mktime(tm); }  /* switch TZ is UTC */

// IPv4-only formatting for the offline network shim.
const char *z_inet_ntop(int af, const void *src, char *dst, unsigned int size){
  if(!src||!dst) return NULL;
  const unsigned char *p=src;
  if(af==2) {
    int n=snprintf(dst,size,"%u.%u.%u.%u",p[0],p[1],p[2],p[3]);
    return (n<0||(unsigned)n>=size)?NULL:dst;
  }
  if(size) dst[0]=0;
  return dst;
}
void *z_gethostbyname(const char *name){ (void)name; return NULL; }
int z_sigdelset(void *set, int sig){ (void)set;(void)sig; return 0; }
int z_sigfillset(void *set){ (void)set; return 0; }

DynLibFunction cocos_dynlib_functions[] = {
  /* SD-backed assets. */
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
  { "AAsset_close", (uintptr_t)&AAsset_close },
  { "AAsset_read", (uintptr_t)&AAsset_read },
  { "AAsset_seek", (uintptr_t)&AAsset_seek },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64 },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64 },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64 },
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava },

  /* OpenSL ES. */
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  IID(3DCOMMIT), IID(3DDOPPLER), IID(3DGROUPING), IID(3DLOCATION), IID(3DMACROSCOPIC),
  IID(3DSOURCE), IID(ANDROIDCONFIGURATION), IID(ANDROIDEFFECT), IID(ANDROIDEFFECTCAPABILITIES),
  IID(ANDROIDEFFECTSEND), IID(ANDROIDSIMPLEBUFFERQUEUE), IID(AUDIODECODERCAPABILITIES),
  IID(AUDIOENCODER), IID(AUDIOENCODERCAPABILITIES), IID(AUDIOIODEVICECAPABILITIES),
  IID(BASSBOOST), IID(BUFFERQUEUE), IID(DEVICEVOLUME), IID(DYNAMICINTERFACEMANAGEMENT),
  IID(DYNAMICSOURCE), IID(EFFECTSEND), IID(ENGINE), IID(ENGINECAPABILITIES),
  IID(ENVIRONMENTALREVERB), IID(EQUALIZER), IID(LED), IID(METADATAEXTRACTION),
  IID(METADATATRAVERSAL), IID(MIDIMESSAGE), IID(MIDIMUTESOLO), IID(MIDITEMPO), IID(MIDITIME),
  IID(MUTESOLO), IID(NULL), IID(OBJECT), IID(OUTPUTMIX), IID(PITCH), IID(PLAY),
  IID(PLAYBACKRATE), IID(PREFETCHSTATUS), IID(PRESETREVERB), IID(RATEPITCH), IID(RECORD),
  IID(SEEK), IID(THREADSYNC), IID(VIBRA), IID(VIRTUALIZER), IID(VISUALIZATION), IID(VOLUME),
  #undef IID

  /* GLES2. */
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
  { "glGetString", (uintptr_t)&glGetString },
  { "glIsBuffer", (uintptr_t)&glIsBuffer },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glMapBufferOES", (uintptr_t)&glMapBufferOES },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform2f", (uintptr_t)&glUniform2f },
  { "glUniform2i", (uintptr_t)&glUniform2i },
  { "glUniform2iv", (uintptr_t)&glUniform2iv },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3i", (uintptr_t)&glUniform3i },
  { "glUniform3iv", (uintptr_t)&glUniform3iv },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniform4i", (uintptr_t)&glUniform4i },
  { "glUniform4iv", (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "glUnmapBufferOES", (uintptr_t)&glUnmapBufferOES },

  /* ---- libc, libm, ctype, pthread ---- */
  { "_ctype_", (uintptr_t)&z_ctype_ptr },
  { "acos", (uintptr_t)&acos }, { "asin", (uintptr_t)&asin }, { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 }, { "atanf", (uintptr_t)&atanf }, { "atol", (uintptr_t)&atol },
  { "basename", (uintptr_t)&z_basename }, { "bsearch", (uintptr_t)&bsearch },
  { "cbrtf", (uintptr_t)&cbrtf }, { "clearerr", (uintptr_t)&clearerr },
  { "clock", (uintptr_t)&clock }, { "clock_getres", (uintptr_t)&z_stub0 },
  { "cos", (uintptr_t)&cos }, { "difftime", (uintptr_t)&difftime }, { "div", (uintptr_t)&div },
  { "dladdr", (uintptr_t)&z_stub0 }, { "dup", (uintptr_t)&z_dup },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "exit", (uintptr_t)&exit }, { "exp", (uintptr_t)&exp }, { "exp2f", (uintptr_t)&exp2f },
  { "fdopen", (uintptr_t)&fdopen }, { "flock", (uintptr_t)&z_stub0 },
  { "fmod", (uintptr_t)&fmod }, { "fnmatch", (uintptr_t)&z_stub0 },
  { "fscanf", (uintptr_t)&fscanf },
  { "getpagesize", (uintptr_t)&z_getpagesize }, { "getpriority", (uintptr_t)&z_stub0 },
  { "getpwuid_r", (uintptr_t)&z_stub0 }, { "gettid", (uintptr_t)&z_gettid },
  { "hypot", (uintptr_t)&hypot },
  { "inflate", (uintptr_t)&inflate }, { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ }, { "inflateReset", (uintptr_t)&inflateReset },
  { "isdigit_l", (uintptr_t)&z_isdigit_l }, { "islower_l", (uintptr_t)&z_islower_l },
  { "isupper_l", (uintptr_t)&z_isupper_l }, { "isxdigit_l", (uintptr_t)&z_isxdigit_l },
  { "ldexp", (uintptr_t)&ldexp }, { "ldexpf", (uintptr_t)&ldexpf }, { "lldiv", (uintptr_t)&lldiv },
  { "log", (uintptr_t)&log }, { "log10", (uintptr_t)&log10 }, { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 }, { "log2f", (uintptr_t)&log2f }, { "logb", (uintptr_t)&logb },
  { "lseek64", (uintptr_t)&z_lseek }, { "memrchr", (uintptr_t)&z_memrchr },
  { "modf", (uintptr_t)&modf }, { "modff", (uintptr_t)&modff }, { "prctl", (uintptr_t)&z_prctl },
  { "pthread_atfork", (uintptr_t)&z_stub0 },
  { "pthread_attr_getstack", (uintptr_t)&z_pthread_attr_getstack },
  { "pthread_condattr_destroy", (uintptr_t)&z_stub0 },
  { "pthread_condattr_init", (uintptr_t)&z_stub0 },
  { "pthread_condattr_setclock", (uintptr_t)&z_stub0 },
  { "pthread_equal", (uintptr_t)&z_pthread_equal },
  { "pthread_getattr_np", (uintptr_t)&z_pthread_getattr_np },
  { "pthread_rwlock_init", (uintptr_t)&z_stub0 },
  { "pthread_setname_np", (uintptr_t)&z_pthread_setname_np },
  { "raise", (uintptr_t)&raise }, { "scalbn", (uintptr_t)&scalbn },
  { "sched_getaffinity", (uintptr_t)&z_stub0 }, { "sched_setaffinity", (uintptr_t)&z_stub0 },
  { "setbuf", (uintptr_t)&setbuf }, { "setenv", (uintptr_t)&setenv },
  { "setpriority", (uintptr_t)&z_stub0 }, { "setvbuf", (uintptr_t)&setvbuf },
  { "sigaltstack", (uintptr_t)&z_stub0 }, { "sin", (uintptr_t)&sin },
  { "sincos", (uintptr_t)&z_sincos }, { "sqrtf", (uintptr_t)&sqrtf },
  { "strcasecmp", (uintptr_t)&z_strcasecmp }, { "strncasecmp", (uintptr_t)&z_strncasecmp },
  { "strcspn", (uintptr_t)&strcspn }, { "strdup", (uintptr_t)&strdup },
  { "strftime", (uintptr_t)&strftime }, { "strlcpy", (uintptr_t)&strlcpy },
  { "strnlen", (uintptr_t)&strnlen }, { "strspn", (uintptr_t)&strspn },
  { "strtok_r", (uintptr_t)&strtok_r }, { "tan", (uintptr_t)&tan },
  { "tolower_l", (uintptr_t)&z_tolower_l }, { "toupper_l", (uintptr_t)&z_toupper_l },
  { "towlower", (uintptr_t)&towlower }, { "unsetenv", (uintptr_t)&unsetenv },
  { "vprintf", (uintptr_t)&vprintf },
  { "wmemcpy", (uintptr_t)&wmemcpy }, { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },

  /* Additional libc and math functions. */
  { "sqrt", (uintptr_t)&sqrt }, { "atof", (uintptr_t)&atof }, { "frexp", (uintptr_t)&frexp },
  { "isnan", (uintptr_t)&z_isnan }, { "__isnanf", (uintptr_t)&z_isnanf },
  { "isgraph", (uintptr_t)&isgraph }, { "isprint", (uintptr_t)&isprint },
  { "ldiv", (uintptr_t)&ldiv }, { "getgid", (uintptr_t)&z_stub0 }, { "mlock", (uintptr_t)&z_stub0 },
  { "pthread_attr_setschedparam", (uintptr_t)&z_stub0 },
  { "pthread_rwlock_destroy", (uintptr_t)&z_stub0 },
  { "putchar", (uintptr_t)&z_putchar }, { "random", (uintptr_t)&random },
  { "sigprocmask", (uintptr_t)&z_stub0 }, { "sigsetjmp", (uintptr_t)&setjmp },
  { "socketpair", (uintptr_t)&z_stubneg1 }, { "stpcpy", (uintptr_t)&z_stpcpy },
  { "strtoimax", (uintptr_t)&strtoll }, { "strtoumax", (uintptr_t)&strtoull },
  { "timegm", (uintptr_t)&z_timegm }, { "alarm", (uintptr_t)&z_stub0 },

  { "inet_ntop", (uintptr_t)&z_inet_ntop },
  { "gethostbyname", (uintptr_t)&z_gethostbyname },
  { "sigdelset", (uintptr_t)&z_sigdelset },
  { "sigfillset", (uintptr_t)&z_sigfillset },
};
int cocos_dynlib_numfunctions = (int)(sizeof(cocos_dynlib_functions)/sizeof(cocos_dynlib_functions[0]));
