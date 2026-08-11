/* cxx_diag.c -- name the exception that killed the process.
 *
 * The game statically links its own libc++/libc++abi, so `__cxa_throw` is not
 * one of our imports and cannot be hooked. Every uncaught exception has
 * therefore surfaced as a bare `abort()` several frames from the cause, and
 * the type has had to be guessed at -- badly, twice.
 *
 * It does however *export* the ABI entry points, so a terminate handler can be
 * installed into its runtime and ask what is in flight:
 *
 *   std::set_terminate            install the handler
 *   __cxa_current_exception_type  the std::type_info of the live exception
 *   __cxa_demangle                turn its mangled name into something readable
 *
 * type_info::name() is a virtual call, which would mean calling through the
 * game's vtable. The Itanium ABI lays type_info out as { vptr, const char*
 * __type_name }, so the mangled name is simply the second word -- no call
 * needed, and nothing to go wrong if the object is damaged.
 *
 * For a std::exception the what() string usually carries the real detail
 * ("mutex lock failed", "std::bad_alloc"). Reaching it needs __cxa_begin_catch
 * plus a virtual call, so it is attempted only after the type name is safely
 * recorded -- if that part faults, the type is already in the log.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "cxx_diag.h"
#include "so_util.h"
#include "util.h"
#include "imports.h"

typedef void  (*fn_set_terminate)(void (*)(void));
typedef void *(*fn_current_exception_type)(void);
typedef char *(*fn_demangle)(const char *, char *, size_t *, int *);
typedef void *(*fn_begin_catch)(void *);

static fn_current_exception_type g_current_type;
static fn_demangle               g_demangle;
static fn_begin_catch            g_begin_catch;
static void                    (*g_prev_terminate)(void);

/* The what() text is what actually names the throw site. libc++ raises
 * std::system_error from several places that never touch pthread at all --
 *
 *   "thread::join failed"                          thread already joined
 *   "thread::detach failed"                        thread already detached
 *   "condition_variable::wait: mutex not locked"   unique_lock not owning
 *   "mutex lock failed"                            pthread_mutex_lock error
 *   "condition_variable wait failed"               pthread_cond_wait error
 *
 * -- and the shims report only the last two. The string separates them.
 *
 * Reaching it means a virtual call into the game's own vtable, so it is
 * attempted last, after the type is already recorded and flushed. Only for
 * types known to derive from std::exception, where what() is the third
 * virtual slot (after the two destructors). */
static int derives_from_std_exception(const char *mangled) {
  static const char *const known[] = {
    "system_error", "runtime_error", "logic_error", "bad_alloc",
    "length_error", "out_of_range", "invalid_argument", "bad_cast",
    "bad_function_call", "future_error", "ios_base", "range_error",
    "overflow_error", "underflow_error", "domain_error", "bad_typeid",
  };
  for (size_t i = 0; i < sizeof known / sizeof *known; i++)
    if (strstr(mangled, known[i])) return 1;
  return 0;
}

static void log_exception_message(const char *mangled) {
  if (!g_begin_catch || !derives_from_std_exception(mangled)) return;

  void *obj = g_begin_catch(NULL);   /* the in-flight exception object */
  if (!obj) return;

  void *const *vtable = *(void *const **)obj;
  if (!vtable) return;

  /* Itanium ABI vtable for std::exception: [0] complete dtor, [1] deleting
   * dtor, [2] what(). */
  const char *(*what)(void *) = (const char *(*)(void *))vtable[2];
  if (!what) return;

  const char *msg = what(obj);
  if (msg) debugLogNote("[c++]   what(): %s\n", msg);
}

static void hw_terminate_handler(void) {
  const char *mangled = NULL;

  if (g_current_type) {
    void *ti = g_current_type();
    if (ti) {
      /* Itanium ABI: { vptr, const char *__type_name }. A leading '*' means
       * the name is not unique across DSOs; skip it. */
      mangled = ((const char *const *)ti)[1];
      if (mangled && *mangled == '*') mangled++;
    }
  }

  if (!mangled) {
    debugLogNote("[c++] std::terminate with no active exception "
                 "(noexcept violation, or terminate called directly)\n");
    debugLogFlush();
  } else {
    /* Record the mangled name before anything else. __cxa_demangle allocates,
     * and if the reason we are here is a failed allocation then calling into
     * malloc again may deadlock or fault -- at which point the type would be
     * lost with it. Mangled is ugly but unambiguous: _ZSt9bad_alloc reads
     * clearly enough. */
    debugLogNote("[c++] uncaught exception: %s\n", mangled);
    debugLogFlush();

    if (g_demangle) {
      int status = -1;
      char *pretty = g_demangle(mangled, NULL, NULL, &status);
      if (status == 0 && pretty) {
        debugLogNote("[c++]   demangled: %s\n", pretty);
        debugLogFlush();
      }
      /* Deliberately not freed: the process is on its way out and free() is
       * no safer here than malloc was. */
    }

    log_exception_message(mangled);
    debugLogFlush();
  }

  if (g_prev_terminate) g_prev_terminate();
  /* If the previous handler returned, fall through to the port's abort so the
   * log is closed rather than lost to fsExit. */
  abort_logged();
}

int cxx_diag_install(so_module *mod) {
  fn_set_terminate set_terminate =
      (fn_set_terminate)so_try_find_addr_rx(mod, "_ZSt13set_terminatePFvvE");
  g_current_type = (fn_current_exception_type)so_try_find_addr_rx(
      mod, "__cxa_current_exception_type");
  g_demangle = (fn_demangle)so_try_find_addr_rx(mod, "__cxa_demangle");
  g_begin_catch = (fn_begin_catch)so_try_find_addr_rx(mod, "__cxa_begin_catch");

  if (!set_terminate) {
    debugLogNote("[c++] std::set_terminate not exported -- uncaught exceptions "
                 "will stay anonymous\n");
    return 0;
  }
  set_terminate(hw_terminate_handler);
  debugLogNote("[c++] terminate handler installed (type=%s demangle=%s what=%s)\n",
               g_current_type ? "yes" : "no", g_demangle ? "yes" : "no",
               g_begin_catch ? "yes" : "no");
  return 1;
}
