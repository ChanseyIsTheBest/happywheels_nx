/* Android-to-Switch import table. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
extern int z_strncasecmp(const char *, const char *, unsigned long);
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "libc_shim.h"
#include "imports.h"
#include "fakefd.h"
#include "cocos_imports.h"
#include "hw_imports_extra.h"
#include "cocos_assets.h"

extern int *__errno(void);              // newlib

/* Silent Android logging. */

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio; (void)tag; (void)text;
  return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio; (void)tag; (void)fmt; (void)va;
  return 0;
}
void __assert2(const char *file, int line, const char *func, const char *expr) {
  (void)file; (void)line; (void)func; (void)expr;
  abort();
}

/* Stack protector and C++ ABI. */

uint64_t __stack_chk_guard_fake = 0x0ull;
void __stack_chk_fail_fake(void) {
  debugLogNote("[fatal] __stack_chk_fail: stack cookie mismatch\n");
  abort_logged();
}

/* The game aborts through here on an uncaught C++ exception (libc++'s default
 * terminate handler), on a failed stack-cookie check, and from
 * __android_log_assert. abort() goes on to run libnx's exit path, which calls
 * fsExit() and unmounts the SD card -- so anything still buffered is lost, and
 * any other thread mid-read faults on a devoptab entry that just became NULL.
 * That secondary fault is what surfaces in the crash report, several frames
 * away from the real cause. Flush and close first so the log survives. */
/* Allocation failure is the other candidate for the uncaught exception:
 * libc++'s operator new throws std::bad_alloc when malloc returns NULL, and
 * nothing in the game catches it. Report the first failure with the size that
 * was asked for -- silence here means the heap is not the problem. */
static int g_alloc_fail_reported;
static void note_alloc_fail(const char *what, size_t n) {
  if (g_alloc_fail_reported) return;
  g_alloc_fail_reported = 1;
  debugLogNote("[heap] %s(%zu) returned NULL -- operator new will throw "
               "std::bad_alloc from here\n", what, n);
}
void *malloc_logged(size_t n) {
  void *p = malloc(n);
  if (!p && n) note_alloc_fail("malloc", n);
  return p;
}
void *calloc_logged(size_t a, size_t b) {
  void *p = calloc(a, b);
  if (!p && a && b) note_alloc_fail("calloc", a * b);
  return p;
}
void *realloc_logged(void *q, size_t n) {
  void *p = realloc(q, n);
  if (!p && n) note_alloc_fail("realloc", n);
  return p;
}

void abort_logged(void) {
  debugLogNote("[fatal] abort() -- process is exiting\n");
  debugLogClose();
  abort();
}

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

/* bionic declares these as `extern FILE* stdout;` -- pointer variables, not
 * the FILE objects themselves. The import has to resolve to the address of
 * the pointer, the same distinction that broke _ctype_. Binding &fake_sF[n]
 * makes the game load the first eight bytes of a FILE struct as the stream
 * pointer, and any fprintf to it dereferences garbage.
 * __sF stays bound to the array: bionic's __sF really is FILE[3]. */
FILE *z_stdin_ptr  = (FILE *)&fake_sF[0];
FILE *z_stdout_ptr = (FILE *)&fake_sF[1];
FILE *z_stderr_ptr = (FILE *)&fake_sF[2];

/* Bionic pthread objects backed by newlib. */

/* Any nonzero return from these lands in libc++ as __throw_system_error, and
 * condition_variable::wait and mutex::lock are noexcept, so it becomes
 * std::terminate with nothing logged. Say what happened before that. */
/* Status codes handed back to the game must use bionic's numbering.
 *
 * The game's libc++ was compiled against bionic, so the errno constants it
 * compares against are baked in. newlib numbers them differently, and the one
 * that matters is ETIMEDOUT: libc++'s condition_variable::__do_timed_wait is
 *
 *     if (ec != 0 && ec != ETIMEDOUT) __throw_system_error(...)
 *
 * and it is noexcept, so a mismatch turns an ordinary timeout into
 * std::terminate. Returning newlib's value made every timed wait that actually
 * timed out fatal -- and because a timeout is not an error, the shims logged
 * nothing, which is why this stayed invisible through several rounds of
 * instrumenting everything else.
 *
 * Credit to the Chrono Trigger port, which documents exactly this. */
#define BIONIC_ETIMEDOUT 110
#define BIONIC_EAGAIN     11
#define BIONIC_EBUSY      16
#define BIONIC_EINVAL     22
#define BIONIC_EPERM       1
#define BIONIC_EDEADLK    35

static int to_bionic_errno(int r) {
  if (r == 0) return 0;
  if (r == ETIMEDOUT) return BIONIC_ETIMEDOUT;
  if (r == EAGAIN)    return BIONIC_EAGAIN;
  if (r == EBUSY)     return BIONIC_EBUSY;
  if (r == EINVAL)    return BIONIC_EINVAL;
  if (r == EPERM)     return BIONIC_EPERM;
  if (r == EDEADLK)   return BIONIC_EDEADLK;
  return r;
}

static int pthread_err(const char *what, int r) {
  if (r) debugLogNote("[pthread] %s returned %d -- libc++ will terminate on this\n", what, r);
  return r;
}

/* Accessing a caller-owned pointer slot.
 *
 * Two constraints pull against each other here.
 *
 * The slot lives inside the game's own pthread object, and bionic's
 * pthread_mutex_t on LP64 is int32_t __private[10] -- four-byte alignment. An
 * eight-byte ldar or stlr on a four-byte-aligned address faults on AArch64,
 * while an ordinary ldr or str does not. Using __atomic_load_n here aborted
 * inside the game's static constructors before anything else ran.
 *
 * Dropping to plain accesses plus standalone fences avoids the fault, but
 * fences only order atomic operations: a plain load racing a plain store is
 * still a data race, and ThreadSanitizer says so.
 *
 * So the alignment decides the strategy. Alignment is a property of the slot,
 * so every access to a given slot takes the same branch and the two schemes
 * never mix on one object. Naturally aligned slots -- which is nearly all of
 * them -- get a lock-free acquire load. The rare misaligned slot is handled
 * entirely under the lock, where no atomic instruction is needed at all. */
static Mutex g_lazy_pthread_init;

static inline int slot_aligned(const void *slot) {
  return (((uintptr_t)slot) & 7u) == 0;
}

static inline void *read_slot(void *const *slot) {
  if (slot_aligned(slot))
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  mutexLock(&g_lazy_pthread_init);
  void *v = *slot;
  mutexUnlock(&g_lazy_pthread_init);
  return v;
}

/* Callers either hold g_lazy_pthread_init, or are pthread_*_init, which POSIX
 * already forbids racing against use of the same object. */
static inline void publish_slot(void **slot, void *v) {
  if (slot_aligned(slot)) __atomic_store_n(slot, v, __ATOMIC_RELEASE);
  else                    *slot = v;
}

static pthread_mutex_t *make_mutex(int recursive) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return NULL;
  int ret;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return NULL; }
  return m;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = make_mutex(attr && *attr == 1); // bionic RECURSIVE == 1
  if (!m) return -1;
  publish_slot((void **)uid, m);
  return 0;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) { pthread_mutex_destroy(*uid); free(*uid); *uid = NULL; }
  return 0;
}
/* Turn a bionic static initializer into a real newlib mutex, once.
 *
 * libc++ constexpr-constructs std::mutex to PTHREAD_MUTEX_INITIALIZER, which
 * is 0, so effectively every mutex in the game arrives here as a small value
 * and is created on first lock. Doing that with a plain read-then-store is a
 * race: two threads touching a new mutex at the same moment each build one and
 * the second store wins, leaving the first thread holding an orphan. Mutual
 * exclusion is then silently gone for that object, which corrupts whatever it
 * was protecting -- and libc++ turns the resulting pthread error into a throw
 * from a noexcept function, i.e. std::terminate.
 *
 * The fast path stays a single acquire load; only first touch takes the lock. */
static int ensure_mutex(pthread_mutex_t **uid) {
  if ((uintptr_t)read_slot((void *const *)uid) >= 0x10000)
    return 0;

  int rc = 0;
  mutexLock(&g_lazy_pthread_init);
  const uintptr_t cur = (uintptr_t)*uid;
  if (cur < 0x10000) {
    /* bionic encodes the type in bits 14-15 of the initializer:
     * 0 normal, 1 recursive, 2 errorcheck. */
    pthread_mutex_t *m = make_mutex(((cur >> 14) & 3) == 1);
    if (m) publish_slot((void **)uid, m);
    else   rc = -1;
  }
  mutexUnlock(&g_lazy_pthread_init);
  return rc;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0) return -1;
  return to_bionic_errno(pthread_err("pthread_mutex_lock", pthread_mutex_lock(*uid)));
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1;
  int r = pthread_mutex_trylock(*uid);
  return to_bionic_errno((r == EBUSY) ? r : pthread_err("pthread_mutex_trylock", r)); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0) return -1;
  return to_bionic_errno(pthread_err("pthread_mutex_unlock", pthread_mutex_unlock(*uid)));
}
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  (void)abs;
  if (ensure_mutex(uid) < 0) return -1;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(*uid) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  publish_slot((void **)cnd, c);
  return 0;
}
/* Same race as ensure_mutex: std::condition_variable is also constexpr-built
 * from a static initializer, so it is created on first wait. */
static int ensure_cond(pthread_cond_t **cnd) {
  if ((uintptr_t)read_slot((void *const *)cnd) >= 0x10000)
    return 0;

  int rc = 0;
  mutexLock(&g_lazy_pthread_init);
  if ((uintptr_t)*cnd < 0x10000) {
    pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
    if (c && pthread_cond_init(c, NULL) == 0) publish_slot((void **)cnd, c);
    else { free(c); rc = -1; }
  }
  mutexUnlock(&g_lazy_pthread_init);
  return rc;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1;
  return to_bionic_errno(pthread_err("pthread_cond_broadcast", pthread_cond_broadcast(*cnd))); }
int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1;
  return to_bionic_errno(pthread_err("pthread_cond_signal", pthread_cond_signal(*cnd))); }
int pthread_cond_destroy_fake(pthread_cond_t **cnd) { if (cnd && (uintptr_t)*cnd >= 0x10000) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; } return 0; }
#define COND_WAIT_CAP_MS 16
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  // Periodic spurious wakeups avoid permanent stalls from lost signals.
  struct timespec cap;
  clock_gettime(CLOCK_MONOTONIC, &cap);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;
  int r = pthread_cond_timedwait(*cnd, *mtx, &cap);
  return (r == ETIMEDOUT) ? 0 : to_bionic_errno(pthread_err("pthread_cond_wait", r));
}
// Clamp timed waits to avoid clock-domain mismatches between bionic and newlib.
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  struct timespec now, cap;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  = now.tv_sec + (now.tv_nsec + add) / 1000000000L;
  cap.tv_nsec = (now.tv_nsec + add) % 1000000000L;
  const struct timespec *use = &cap;
  if (t && (t->tv_sec < cap.tv_sec ||
            (t->tv_sec == cap.tv_sec && t->tv_nsec <= cap.tv_nsec)))
    use = t;
  int r = pthread_cond_timedwait(*cnd, *mtx, use);
  if (r == ETIMEDOUT) {
    /* A timeout is normal, but only if it is spelled the way libc++ expects. */
    return BIONIC_ETIMEDOUT;
  }
  return to_bionic_errno(pthread_err("pthread_cond_timedwait", r));
}

/* pthread_once must block every caller until the initialiser has finished.
 *
 * A plain test-and-set is not enough: the winner runs init() while any
 * concurrent caller returns immediately and uses the not-yet-initialised
 * state. The game's emulated-TLS runtime (__emutls_get_address, used by
 * sdkbox's thread_local data) creates its pthread key inside a pthread_once,
 * and __emutls_get_address calls abort() when it loses that race.
 *
 * The state word belongs to the game, so it is only ever touched by a plain
 * load or store under the lock -- no atomics. AArch64 acquire/release
 * instructions fault on a misaligned address, and nothing here can promise
 * how the game laid its once-word out.
 *
 * States: 0 untouched (bionic PTHREAD_ONCE_INIT), 1 running, 2 done. */
int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;

  for (;;) {
    mutexLock(&g_lazy_pthread_init);
    const int state = *once;
    if (state == 0) *once = 1;          /* claim it */
    mutexUnlock(&g_lazy_pthread_init);

    if (state == 2) return 0;           /* already done */

    if (state == 0) {
      (*init)();
      mutexLock(&g_lazy_pthread_init);
      *once = 2;
      mutexUnlock(&g_lazy_pthread_init);
      return 0;
    }

    svcSleepThread(1000);               /* 1us; another thread is initialising */
  }
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct { void *(*entry)(void *); void *arg; uint8_t tls[BIONIC_TLS_SIZE]; } ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;
  /* The bionic TLS block lives inside this allocation, so it has to stay
   * mapped for the whole life of the thread -- it cannot be freed on entry.
   * Copy what is needed and release it at exit instead; the previous version
   * never freed it at all, leaking BIONIC_TLS_SIZE plus the header for every
   * thread the game ever created. */
  void *(*entry)(void *) = ts->entry;
  void *arg = ts->arg;
  install_bionic_tls(ts->tls);
  void *ret = entry(arg);
  free(ts);
  return ret;
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  if (stack < (2u << 20)) stack = 2u << 20;
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  const int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);

  /* libc++ compares pthread_t against _LIBCPP_NULL_THREAD, which is
   * (pthread_t)0, to decide whether a std::thread is joinable. A valid thread
   * whose id happens to be 0 therefore looks already-joined, and join() and
   * detach() both throw system_error(EINVAL) *without calling pthread at all*
   * -- which is exactly the shape seen here: std::system_error with none of
   * the pthread shims reporting an error. */
  if (r == 0 && thread && *(uintptr_t *)thread == 0)
    debugLogNote("[pthread] pthread_create returned a thread id of 0 -- libc++ "
                 "will treat this thread as null and throw on join/detach\n");

  if (r != 0) {
    debugLogNote("[pthread] pthread_create returned %d for a %zu KB stack -- "
                 "std::thread turns this into system_error\n", r, stack / 1024);
    free(ts);
    return r;
  }
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  return to_bionic_errno(pthread_err("pthread_join", pthread_join(thread, retval)));
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }

/* Bionic TLS keys. */
/* bionic's own PTHREAD_KEYS_MAX is 128, so a well-behaved Android build never
 * needs more -- it would have failed on the device it shipped for. Exhausting
 * this table therefore points at a leak rather than a genuine requirement, and
 * raising the ceiling only buys time.
 *
 * It buys that time very cheaply though: 16 KB globally and 8 KB per thread at
 * 1024, against five threads. Worth having so a slow leak degrades instead of
 * killing the process, but the peak is logged precisely so the leak is still
 * visible -- a healthy run should sit well under 128. */
#define FAKE_KEYS_MAX 1024
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_keys_live, g_keys_peak;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      debugLogNote("[pthread] the underlying pthread_key_create failed -- "
                   "newlib is out of real TLS keys\n");
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;
      g_keys_live++;
      if (g_keys_live > g_keys_peak) {
        g_keys_peak = g_keys_live;
        /* 128 is what the game would have had on Android. Passing it means
         * something is not giving keys back. */
        if (g_keys_peak == 129)
          debugLogNote("[pthread] TLS keys passed 128, which is bionic's own "
                       "limit -- keys are leaking somewhere\n");
        if (g_keys_peak == FAKE_KEYS_MAX * 3 / 4)
          debugLogNote("[pthread] TLS key table three-quarters full (%d of %d)\n",
                       g_keys_live, FAKE_KEYS_MAX);
      }
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  debugLogNote("[pthread] pthread_key_create: all %d TLS keys in use -- "
               "libc++ turns this into system_error\n", FAKE_KEYS_MAX);
  return EAGAIN;
}

void pthread_key_report(void) {
  debugLogNote("[pthread] TLS keys: %d live, %d peak, of %d\n",
               g_keys_live, g_keys_peak, FAKE_KEYS_MAX);
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  if (g_key_table[key - 1].used && g_keys_live > 0) g_keys_live--;
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  /* The other place libc++'s __thread_specific_ptr throws system_error. */
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) {
    debugLogNote("[pthread] pthread_setspecific: bad key %u (master ready=%d)\n",
                 key, g_master_key_ready);
    return EINVAL;
  }
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) {
      debugLogNote("[pthread] pthread_setspecific: out of memory for the "
                   "per-thread key table\n");
      return ENOMEM;
    }
    const int r = pthread_setspecific(g_master_key, kv);
    if (r != 0) {
      /* Left unchecked this leaks kv and every later get returns NULL, so the
       * table is rebuilt on each call and the values never stick. */
      debugLogNote("[pthread] the underlying pthread_setspecific returned %d -- "
                   "thread-local values will not persist\n", r);
      free(kv);
      return r;
    }
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}

/* Small compatibility shims. */

static int ret0_i(void) { return 0; }
static int retm1_i(void) { return -1; }
static unsigned ret0_u(void) { return 0; }
static int signal_stub(int s, void *h) { (void)s; (void)h; return 0; }
static int sigaction_stub(int s, const void *a, void *o) { (void)s; (void)a; (void)o; return 0; }
static int ioctl_stub(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
static int fcntl_stub(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int tcgetattr_stub(int fd, void *t) { (void)fd; if (t) memset(t, 0, 60); return 0; }
static int tcsetattr_stub(int fd, int opt, const void *t) { (void)fd; (void)opt; (void)t; return 0; }

// POSIX file ops that devkitA64 newlib/libnx may not provide -- implement or
// stub them locally so the link never depends on their presence.
static int access_impl(const char *path, int mode) {
  (void)mode;
  struct stat st;
  return stat(path, &st) == 0 ? 0 : -1;
}
static int chmod_stub(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int truncate_stub(const char *path, long len) { (void)path; (void)len; return 0; }
static int ftruncate_stub(int fd, long len) { (void)fd; (void)len; return 0; }
static int fsync_stub(int fd) { (void)fd; return 0; }
static int dup2_stub(int a, int b) { (void)a; return b; }
static long pread_impl(int fd, void *buf, size_t n, long off) {
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, off, SEEK_SET) < 0) return -1;
  size_t total = 0;
  while (total < n) {
    long r = read(fd, (char *)buf + total, n - total);
    if (r <= 0) break;
    total += (size_t)r;
  }
  lseek(fd, cur, SEEK_SET);
  return (long)total;
}
static long pwrite_impl(int fd, const void *buf, size_t n, long off) {
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}
static int uname_fake(void *buf) { if (buf) memset(buf, 0, 390); return 0; }
static long sysconf_pass(int n) { return sysconf_fake(n); }
static char *g_tzname_fake[2] = { (char *)"UTC", (char *)"UTC" };
static int readlink_stub(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
static int link_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int symlink_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int fchmod_stub(int fd, int m) { (void)fd; (void)m; return 0; }
static int fchmodat_stub(int d, const char *p, int m, int f) { (void)d; (void)p; (void)m; (void)f; return 0; }
static int utimensat_stub(int d, const char *p, const void *t, int f) { (void)d; (void)p; (void)t; (void)f; return 0; }
static long sendfile_stub(int o, int i, long *off, size_t c) { (void)o; (void)i; (void)off; (void)c; return -1; }
static void *fdopendir_stub(int fd) { (void)fd; return NULL; }
static char *inet_ntoa_stub(uint32_t in) { (void)in; static char s[] = "0.0.0.0"; return s; }
static void _exit_fake(int code) { (void)code; extern void NX_NORETURN __libnx_exit(int rc); __libnx_exit(0); }

// JNI can supply null strings to these bionic-compatible helpers.
static int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
static int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
static char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
static char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
static char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
static size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }

/* Graphics compatibility. */

static char *shader_fixups(const char *src) {
  const char *find = "monoCol = vec3";
  char *pos = strstr(src, find);
  if (!pos) return NULL;
  const char *repl = "vec3 monoCol = vec3";
  const size_t pre = (size_t)(pos - src), flen = strlen(find), rlen = strlen(repl);
  char *out = malloc(strlen(src) + (rlen - flen) + 1);
  if (!out) return NULL;
  memcpy(out, src, pre);
  memcpy(out + pre, repl, rlen);
  strcpy(out + pre + rlen, pos + flen);
  return out;
}
static void gl_ShaderSource_compat(GLuint sh, GLsizei count, const GLchar *const *strs, const GLint *lens) {
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
  char *buf = malloc(total + 1);
  if (!buf) { glShaderSource(sh, count, strs, lens); return; }
  size_t off = 0;
  for (GLsizei i = 0; i < count; i++) {
    size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
    memcpy(buf + off, strs[i], l); off += l;
  }
  buf[off] = 0;
  char *fixed = shader_fixups(buf);
  const GLchar *final_src = fixed ? fixed : buf;
  glShaderSource(sh, 1, &final_src, NULL);
  free(buf);
  free(fixed);
}

static EGLBoolean egl_QuerySurface_compat(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (val) {
    if (attr == EGL_WIDTH  && *val <= 0) { *val = screen_width;  r = EGL_TRUE; }
    if (attr == EGL_HEIGHT && *val <= 0) { *val = screen_height; r = EGL_TRUE; }
  }
  return r;
}

/* Import table. */

DynLibFunction dynlib_functions[] = {
  /* ---- Happy Wheels additions over the base table ---- */
  { "AAssetManager_openDir",       (uintptr_t)&AAssetManager_openDir },
  { "AAssetDir_getNextFileName",   (uintptr_t)&AAssetDir_getNextFileName },
  { "AAssetDir_rewind",            (uintptr_t)&AAssetDir_rewind },
  { "AAssetDir_close",             (uintptr_t)&AAssetDir_close },
  { "AAsset_openFileDescriptor",   (uintptr_t)&AAsset_openFileDescriptor },
  { "AAsset_openFileDescriptor64", (uintptr_t)&AAsset_openFileDescriptor64 },
  { "__FD_CLR_chk",                (uintptr_t)&__FD_CLR_chk },
  { "__android_log_assert",        (uintptr_t)&__android_log_assert },
  { "system",                      (uintptr_t)&hw_system },
  { "frexpf",                      (uintptr_t)&frexpf },
  { "glFrontFace",                 (uintptr_t)&glFrontFace },
  { "glIsRenderbuffer",            (uintptr_t)&glIsRenderbuffer },
  { "glLineWidth",                 (uintptr_t)&glLineWidth },
  /* ---------------------------------------------------- */

  // --- liblog / cxxabi / fortify markers ---
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "__assert2", (uintptr_t)&__assert2 },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__get_h_errno", (uintptr_t)&__get_h_errno_fake },

  // --- fortify wrappers ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__pread_chk", (uintptr_t)&__pread_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  // --- bionic misc ---
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "sysconf", (uintptr_t)&sysconf_pass },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "uname", (uintptr_t)&uname_fake },
  { "openlog", (uintptr_t)&ret0_i },
  { "closelog", (uintptr_t)&ret0_i },
  { "syslog", (uintptr_t)&ret0_i },
  { "abort", (uintptr_t)&abort_logged },
  { "_exit", (uintptr_t)&_exit_fake },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc_logged },
  { "calloc", (uintptr_t)&calloc_logged },
  { "realloc", (uintptr_t)&realloc_logged },
  { "free", (uintptr_t)&free },
  { "memalign", (uintptr_t)&memalign },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr }, { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy }, { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat }, { "strchr", (uintptr_t)&z_strchr },
  { "strcmp", (uintptr_t)&z_strcmp }, { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&z_strlen }, { "strncasecmp", (uintptr_t)&z_strncasecmp },
  { "strncmp", (uintptr_t)&z_strncmp }, { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&z_strrchr }, { "strstr", (uintptr_t)&z_strstr },
  { "strtod", (uintptr_t)&strtod }, { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol }, { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll }, { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull }, { "atoi", (uintptr_t)&atoi },
  { "qsort", (uintptr_t)&qsort }, { "rand", (uintptr_t)&rand }, { "srand", (uintptr_t)&srand },
  { "isalnum", (uintptr_t)&isalnum }, { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper }, { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },

  // --- wide / multibyte / locale ---
  { "wcslen", (uintptr_t)&wcslen }, { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp }, { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof }, { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold }, { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul }, { "wcstoull", (uintptr_t)&wcstoull },
  { "btowc", (uintptr_t)&btowc }, { "wctob", (uintptr_t)&wctob },
  { "mbrlen", (uintptr_t)&mbrlen }, { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbtowc", (uintptr_t)&mbtowc }, { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb }, { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale }, { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake }, { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake }, { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake }, { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake }, { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake }, { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake }, { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake }, { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake }, { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake }, { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake }, { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake }, { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // --- printf family ---
  { "printf", (uintptr_t)&silent_printf }, { "puts", (uintptr_t)&puts },
  { "snprintf", (uintptr_t)&snprintf }, { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf }, { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf }, { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf }, { "vsscanf", (uintptr_t)&vsscanf },

  // --- math ---
  { "acosf", (uintptr_t)&acosf }, { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f }, { "cosf", (uintptr_t)&cosf },
  { "sinf", (uintptr_t)&sinf }, { "tanf", (uintptr_t)&tanf },
  { "expf", (uintptr_t)&expf }, { "logf", (uintptr_t)&logf },
  { "powf", (uintptr_t)&powf }, { "pow", (uintptr_t)&pow },
  { "fmodf", (uintptr_t)&fmodf }, { "sincosf", (uintptr_t)&sincosf_fake },

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime_fake }, { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime }, { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime }, { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime }, { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep }, { "usleep", (uintptr_t)&usleep },
  { "tzset", (uintptr_t)&tzset }, { "tzname", (uintptr_t)&g_tzname_fake },
  { "getenv", (uintptr_t)&getenv_fake }, { "putenv", (uintptr_t)&putenv },

  // --- stdio (fake __sF aware) ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&z_stdin_ptr }, { "stdout", (uintptr_t)&z_stdout_ptr }, { "stderr", (uintptr_t)&z_stderr_ptr },
  { "fopen", (uintptr_t)&fopen_fake }, { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake }, { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell_fake }, { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake }, { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake }, { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake }, { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake }, { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getc_fake }, { "fputwc", (uintptr_t)&fputc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "ungetwc", (uintptr_t)&ungetc_fake },
  { "feof", (uintptr_t)&feof_fake }, { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake }, { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename_fake },

  // --- filesystem ---
  { "open", (uintptr_t)&open_fake }, { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake }, { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write_fake }, { "pwrite", (uintptr_t)&pwrite_impl },
  { "pread", (uintptr_t)&pread_impl },
  { "lseek", (uintptr_t)&z_lseek }, { "pipe", (uintptr_t)&pipe_fake },
  { "poll", (uintptr_t)&poll_fake }, { "select", (uintptr_t)&select_fake },
  { "dup2", (uintptr_t)&dup2_stub }, { "fcntl", (uintptr_t)&fcntl_stub },
  { "ioctl", (uintptr_t)&ioctl_stub }, { "isatty", (uintptr_t)&isatty },
  { "tcgetattr", (uintptr_t)&tcgetattr_stub }, { "tcsetattr", (uintptr_t)&tcsetattr_stub },
  { "stat", (uintptr_t)&stat_fake }, { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake }, { "statfs", (uintptr_t)&statfs_fake },
  { "statvfs", (uintptr_t)&statvfs_fake }, { "access", (uintptr_t)&access_impl },
  { "mkdir", (uintptr_t)&mkdir_fake }, { "rmdir", (uintptr_t)&rmdir },
  { "unlink", (uintptr_t)&unlink }, { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "chdir", (uintptr_t)&chdir }, { "getcwd", (uintptr_t)&getcwd_fake },
  { "chmod", (uintptr_t)&chmod_stub }, { "fchmod", (uintptr_t)&fchmod_stub },
  { "fchmodat", (uintptr_t)&fchmodat_stub }, { "truncate", (uintptr_t)&truncate_stub },
  { "ftruncate", (uintptr_t)&ftruncate_stub }, { "fsync", (uintptr_t)&fsync_stub },
  { "link", (uintptr_t)&link_stub }, { "symlink", (uintptr_t)&symlink_stub },
  { "readlink", (uintptr_t)&readlink_stub }, { "utime", (uintptr_t)&ret0_i },
  { "utimensat", (uintptr_t)&utimensat_stub }, { "sendfile", (uintptr_t)&sendfile_stub },
  { "opendir", (uintptr_t)&opendir }, { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake }, { "fdopendir", (uintptr_t)&fdopendir_stub },
  { "realpath", (uintptr_t)&realpath_fake },
  { "strerror", (uintptr_t)&strerror }, { "strerror_r", (uintptr_t)&strerror_r_fake },

  // --- signals / setjmp ---
  { "signal", (uintptr_t)&signal_stub }, { "sigaction", (uintptr_t)&sigaction_stub },
  { "sigaddset", (uintptr_t)&ret0_i }, { "sigemptyset", (uintptr_t)&ret0_i },
  { "setjmp", (uintptr_t)&setjmp }, { "longjmp", (uintptr_t)&longjmp },
  { "siglongjmp", (uintptr_t)&longjmp },

  // --- process / ids ---
  { "getpid", (uintptr_t)&getpid_fake }, { "getuid", (uintptr_t)&ret0_u },
  { "geteuid", (uintptr_t)&ret0_u }, { "getegid", (uintptr_t)&ret0_u },
  { "getpwuid", (uintptr_t)&getpwuid_fake }, { "getrusage", (uintptr_t)&getrusage_fake },
  { "fork", (uintptr_t)&fork_fake }, { "execvp", (uintptr_t)&execvp_fake },
  { "waitpid", (uintptr_t)&waitpid_fake }, { "kill", (uintptr_t)&kill_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },

  // --- dynamic loader ---
  { "dlopen", (uintptr_t)&dlopen_fake }, { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake }, { "dlsym", (uintptr_t)&dlsym_fake },

  // --- networking (offline) ---
  { "socket", (uintptr_t)&socket_fake }, { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake }, { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake }, { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake }, { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake }, { "shutdown", (uintptr_t)&shutdown_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake }, { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getsockname", (uintptr_t)&getsockname_fake }, { "getpeername", (uintptr_t)&getpeername_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake }, { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake }, { "gethostname", (uintptr_t)&gethostname_fake },
  { "getservbyname", (uintptr_t)&getservbyname_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake }, { "if_indextoname", (uintptr_t)&if_indextoname_fake },
  // inet_aton returns 0 on FAILURE (nonzero == success); use ret0_i so callers
  // see a clean failure. inet_pton returns <=0 on error, so retm1_i is correct.
  { "inet_aton", (uintptr_t)&ret0_i }, { "inet_pton", (uintptr_t)&retm1_i },
  { "inet_ntoa", (uintptr_t)&inet_ntoa_stub },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake }, { "pthread_join", (uintptr_t)&pthread_join_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake }, { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake }, { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake }, { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0_i },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },
  { "sem_init", (uintptr_t)&sem_init_fake }, { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake }, { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },

  // --- EGL ---
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay }, { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglTerminate", (uintptr_t)&eglTerminate }, { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglCreateContext", (uintptr_t)&eglCreateContext }, { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers }, { "eglQuerySurface", (uintptr_t)&egl_QuerySurface_compat },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext }, { "eglDestroySurface", (uintptr_t)&eglDestroySurface },

  // --- GLES2 ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture }, { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer }, { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer }, { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate }, { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData }, { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor }, { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil }, { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader }, { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram }, { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace }, { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers }, { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers }, { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures }, { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask }, { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray }, { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements }, { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D }, { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers }, { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures }, { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError }, { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv }, { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram }, { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset }, { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage }, { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&gl_ShaderSource_compat }, { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask }, { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexImage2D", (uintptr_t)&glTexImage2D }, { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D }, { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i }, { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv }, { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv }, { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer }, { "glViewport", (uintptr_t)&glViewport },

};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// ctype functions are macros in newlib, so wrappers provide import addresses.
#define EXTRA_CTYPE(name) static int extra_##name(int c) { return name(c); }
EXTRA_CTYPE(isalpha)
EXTRA_CTYPE(islower)
EXTRA_CTYPE(toupper)
static char *extra_strpbrk(const char *s, const char *accept) { return strpbrk(s, accept); }
static void extra_perror(const char *s) { (void)s; }
static int extra_execl(const char *path, const char *arg, ...) {
  (void)path; (void)arg; errno = ENOSYS; return -1;
}
static const char *extra_gai_strerror(int err) { (void)err; return "getaddrinfo error"; }

static DynLibFunction extra_functions[] = {
  { "isalpha", (uintptr_t)&extra_isalpha },
  { "islower", (uintptr_t)&extra_islower },
  { "toupper", (uintptr_t)&extra_toupper },
  { "strpbrk", (uintptr_t)&extra_strpbrk },
  { "perror", (uintptr_t)&extra_perror },
  { "execl", (uintptr_t)&extra_execl },
  { "gai_strerror", (uintptr_t)&extra_gai_strerror },
};
static const size_t extra_numfunctions = sizeof(extra_functions) / sizeof(*extra_functions);

// Combined table used by import resolution and dlsym().
static DynLibFunction *g_combined = NULL;
static int g_combined_n = 0;
static void build_combined(void) {
  if (g_combined) return;
  g_combined_n = (int)dynlib_numfunctions + cocos_dynlib_numfunctions
               + (int)extra_numfunctions;
  g_combined = malloc((size_t)g_combined_n * sizeof(DynLibFunction));
  size_t off = 0;
  memcpy(g_combined + off, dynlib_functions, dynlib_numfunctions * sizeof(DynLibFunction));
  off += dynlib_numfunctions;
  memcpy(g_combined + off, cocos_dynlib_functions,
         (size_t)cocos_dynlib_numfunctions * sizeof(DynLibFunction));
  off += cocos_dynlib_numfunctions;
  memcpy(g_combined + off, extra_functions,
         extra_numfunctions * sizeof(DynLibFunction));
}

/* Search the shim table by name -- used by dlsym() for the GLES/EGL/libc
 * entry points the engine resolves dynamically (not module exports). */
uintptr_t dynlib_find_export(const char *name) {
  if (!name) return 0;
  build_combined();
  for (int i = 0; i < g_combined_n; i++)
    if (strcmp(name, g_combined[i].symbol) == 0)
      return g_combined[i].func;
  return 0;
}

void crx_resolve_imports(so_module *mod) {
  so_relocate(mod);
  build_combined();
  if (so_resolve(mod, g_combined, g_combined_n) != 0)
    fatal_error("Unresolved imports in %s", mod->name);
}
