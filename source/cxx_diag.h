/* Name the exception that killed the process. See cxx_diag.c. */

#ifndef CXX_DIAG_H
#define CXX_DIAG_H

#include "so_util.h"

/* Installs a std::terminate handler into the game's own C++ runtime.
 * Returns 1 if it took. Call after so_finalize and before init_array. */
int cxx_diag_install(so_module *mod);

#endif
