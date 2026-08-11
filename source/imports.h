/* Android import resolution.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

uintptr_t dynlib_find_export(const char *name);

void crx_resolve_imports(so_module *mod);

void abort_logged(void);
void pthread_key_report(void);
void *malloc_logged(size_t n);
void *calloc_logged(size_t a, size_t b);
void *realloc_logged(void *q, size_t n);

#endif
