/* Symbols libMyGame.so imports that the base KHUx table does not provide. */

#ifndef HW_IMPORTS_EXTRA_H
#define HW_IMPORTS_EXTRA_H

#include <stddef.h>

void __FD_CLR_chk(int fd, void *set, size_t set_size);
void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...);
int  hw_system(const char *command);

#endif
