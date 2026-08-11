/* Symbols libMyGame.so imports that the base loader table does not provide.
 * The table entries themselves are inline in imports.c, in the block
 * marked "Happy Wheels additions over the base table". */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/select.h>

#include "hw_imports_extra.h"

/* Bionic fortify wrapper. The base table has __FD_SET_chk and __FD_ISSET_chk
 * but not the CLR variant; Happy Wheels imports all three.
 *
 * This must treat fd_set exactly as its two siblings in libc_shim.c do -- a
 * flat array of unsigned long with a 1024 bound, which is bionic's layout.
 * Using newlib's FD_CLR macro and FD_SETSIZE instead would disagree with them
 * on both the bit positions and the usable range. */
void __FD_CLR_chk(int fd, void *set, size_t set_size) {
  (void)set_size;
  if (!set || fd < 0 || fd >= 1024) return;
  ((unsigned long *)set)[fd / (8 * sizeof(long))] &=
      ~(1ul << (fd % (8 * sizeof(long))));
}

/* Never returns on Android. Keep that contract -- returning here would turn
 * a caught assertion into corruption somewhere later and much harder to find. */
void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...) {
  (void)tag;

  char msg[512];
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
  } else {
    snprintf(msg, sizeof msg, "assertion failed: %s", cond ? cond : "?");
  }

  fprintf(stderr, "[assert] %s\n", msg);
  abort();
}

/* No shell. Returning 0 would mean "the command succeeded", so return -1
 * and let the caller take its failure path. */
int hw_system(const char *command) {
  (void)command;
  return -1;
}
