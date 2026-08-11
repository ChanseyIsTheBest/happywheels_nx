/* watchdog.c -- makes a hang report itself.
 *
 * A crash leaves an Atmosphere report; a hang leaves nothing at all, which is
 * exactly the case that is hardest to diagnose remotely. This is a cut-down
 * version of the idea in the Fruit Ninja port's diag.c, without the Unity and
 * Mono coupling: the render loop publishes a frame counter and a short
 * breadcrumb string, and an independent libnx thread notices when frame
 * progress stops and writes what the last breadcrumb was.
 *
 * The watchdog thread is a raw libnx Thread, not a pthread, so it does not go
 * through the pthread shim -- if the hang is a deadlock in that shim, the
 * watchdog still runs.
 *
 * Overhead on the happy path is two relaxed stores per frame.
 */

#include <switch.h>
#include <string.h>
#include <stdatomic.h>

#include "util.h"
#include "watchdog.h"

static atomic_uint  g_frame;
static atomic_uint  g_breadcrumb_seq;
static char         g_breadcrumb[192];
static Mutex        g_crumb_lock;

static Thread g_thread;
static atomic_bool g_running;
static atomic_bool g_started;

void watchdog_frame(unsigned frame) {
  atomic_store_explicit(&g_frame, frame, memory_order_relaxed);
}

void watchdog_mark(const char *what) {
  if (!what) return;
  mutexLock(&g_crumb_lock);
  strncpy(g_breadcrumb, what, sizeof g_breadcrumb - 1);
  g_breadcrumb[sizeof g_breadcrumb - 1] = 0;
  mutexUnlock(&g_crumb_lock);
  atomic_fetch_add_explicit(&g_breadcrumb_seq, 1, memory_order_relaxed);
}

static void watchdog_main(void *arg) {
  (void)arg;
  const unsigned STALL_SECONDS = 5;

  unsigned last_frame = 0;
  unsigned last_seq   = 0;
  unsigned quiet      = 0;   /* seconds without frame or breadcrumb progress */
  int      reported   = 0;

  while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
    svcSleepThread(1000000000ULL);   /* 1s */

    unsigned f   = atomic_load_explicit(&g_frame, memory_order_relaxed);
    unsigned seq = atomic_load_explicit(&g_breadcrumb_seq, memory_order_relaxed);

    if (f != last_frame || seq != last_seq) {
      last_frame = f;
      last_seq   = seq;
      quiet      = 0;
      reported   = 0;
      continue;
    }

    quiet++;
    if (quiet < STALL_SECONDS || reported) continue;

    char crumb[sizeof g_breadcrumb];
    mutexLock(&g_crumb_lock);
    memcpy(crumb, g_breadcrumb, sizeof crumb);
    mutexUnlock(&g_crumb_lock);

    debugLogNote("\n*** STALL: no progress for %us. frame=%u last=\"%s\" ***\n",
                 quiet, f, crumb[0] ? crumb : "(none)");
    debugLogFlush();
    reported = 1;   /* one report per stall, not one per second */
  }
}

void watchdog_start(void) {
  if (atomic_exchange(&g_started, true)) return;
  atomic_store(&g_running, true);
  if (R_FAILED(threadCreate(&g_thread, watchdog_main, NULL, NULL, 0x4000, 0x2C, -2)))
    return;
  threadStart(&g_thread);
}

void watchdog_stop(void) {
  if (!atomic_load(&g_started)) return;
  atomic_store(&g_running, false);
  threadWaitForExit(&g_thread);
  threadClose(&g_thread);
}
