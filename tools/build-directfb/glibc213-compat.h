/* glibc213-compat.h -- force-included (-include) into every DirectFB unit.
 *
 * The cross toolchain's glibc >= 2.38 headers redirect strtol/strtoul/sscanf
 * to __isoc23_* whenever _GNU_SOURCE is set. glibc 2.13 in the RX3 chroot has
 * none of those, so libdirect/libfusion came out unloadable. features.h is
 * include-guarded, so clearing the switch right after it sticks for the unit.
 * Same family of problem as compat_shim.c's fstat (Orange Pi / Debian GCC 14).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* DirectFB is built as GNU C; must precede features.h */
#endif
#include <features.h>
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
