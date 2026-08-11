/* Bionic TLS helpers.
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int silent_printf(const char *text, ...);

/* ---- file logging ----
 *
 * Ported from the Fruit Ninja port's util.c, which is the only one of the
 * three reference ports that has any logging at all -- both Cocos ports ship
 * a silent_printf() that returns 0. Nothing in it is engine-specific.
 *
 * Deliberately malloc-free: vsnprintf into a fixed buffer plus a raw write().
 * newlib's fopen/vfprintf/fflush all allocate, and this logger is called from
 * inside allocator and dlsym shims, so using stdio here would invert the log
 * lock against the allocator lock and deadlock.
 *
 * DEBUG_LOG 0 compiles the whole thing out: no file is ever created.
 */
int  debugPrintf(const char *text, ...) __attribute__((format(printf, 1, 2)));
int  debugLogNote(const char *text, ...) __attribute__((format(printf, 1, 2)));
void debugLogFlush(void);
void debugLogClose(void);
void debugLogForceOn(void);

#define BIONIC_TLS_SIZE 0x400
#define BIONIC_TLS_TP_OFFSET 0x200
void install_bionic_tls(void *buf);

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

#endif
