/* Bionic TLS helpers.
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"
#include "config.h"

int silent_printf(const char *text, ...) {
  (void)text;
  return 0;
}

#if DEBUG_LOG

static Mutex g_log_lock;      /* libnx Mutex: 0 == unlocked, needs no init */
static int   g_log_fd = -1;
static char  g_log_buf[8192];
static int   g_log_used = 0;
static volatile int g_log_on = 1;

static void log_flush_locked(void) {
  int off = 0;
  while (off < g_log_used) {
    ssize_t w = write(g_log_fd, g_log_buf + off, (size_t)(g_log_used - off));
    if (w <= 0) break;
    off += (int)w;
  }
  g_log_used = 0;
}

void debugLogFlush(void) {
  mutexLock(&g_log_lock);
  if (g_log_fd >= 0) log_flush_locked();
  mutexUnlock(&g_log_lock);
}

/* Flush and close so the bytes are committed rather than left with the size
 * not yet written back. For the crash/exit path: the next debugPrintf would
 * reopen anyway. */
void debugLogClose(void) {
  mutexLock(&g_log_lock);
  if (g_log_fd >= 0) {
    log_flush_locked();
    fsync(g_log_fd);
    close(g_log_fd);
    g_log_fd = -1;
  }
  mutexUnlock(&g_log_lock);
}

void debugLogForceOn(void) { g_log_on = 1; }

int debugPrintf(const char *text, ...) {
  if (!g_log_on) return 0;
  static char line[2048];
  va_list list;

  mutexLock(&g_log_lock);
  if (g_log_fd < 0) g_log_fd = open(LOG_NAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_log_fd >= 0) {
    va_start(list, text);
    int n = vsnprintf(line, sizeof line, text, list);
    va_end(list);
    if (n > 0) {
      if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
      /* Coalesce and flush in one write() rather than a syscall per line. */
      if (g_log_used + n > (int)sizeof g_log_buf) log_flush_locked();
      if (n > (int)sizeof g_log_buf) {
        ssize_t off = 0;
        while (off < n) {
          ssize_t w = write(g_log_fd, line + off, (size_t)(n - off));
          if (w <= 0) break;
          off += w;
        }
      } else {
        memcpy(g_log_buf + g_log_used, line, (size_t)n);
        g_log_used += n;
      }
    }
  }
  mutexUnlock(&g_log_lock);
  return 0;
}

/* Rare, important events: written straight through, because the interesting
 * ones are followed by a crash or a hang. Never use per-frame. */
int debugLogNote(const char *text, ...) {
  const int was = g_log_on;
  g_log_on = 1;
  va_list ap; va_start(ap, text);
  char line[512];
  int n = vsnprintf(line, sizeof line, text, ap);
  va_end(ap);
  if (n > 0) debugPrintf("%s", line);
  debugLogFlush();
  g_log_on = was;
  return n;
}

#else

int  debugPrintf(const char *text, ...)  { (void)text; return 0; }
int  debugLogNote(const char *text, ...) { (void)text; return 0; }
void debugLogFlush(void)  { }
void debugLogClose(void)  { }
void debugLogForceOn(void){ }

#endif

void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}
