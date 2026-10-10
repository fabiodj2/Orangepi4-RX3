/* glibc213-compat.h -- force-included (-include) into every shim unit when
 * building on a host whose cross headers are newer than the chroot's glibc
 * 2.13 (Debian GCC 14 / glibc 2.41 headers on the Orange Pi).
 *  - __isoc23_strtol/sscanf: glibc >= 2.38 redirects them under _GNU_SOURCE.
 *  - fstat: a real fstat@GLIBC_2.33 that 2.13 lacks; on ARM the kernel's
 *    struct stat is glibc's, so the raw syscall is byte-exact (see
 *    tools/build-directfb/README.md, "The fstat forward").
 * Time/offset width is pinned by -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <features.h>
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline int rx3_compat_fstat(int fd, struct stat *buf)
{
    return (int)syscall(SYS_fstat, fd, buf);
}
#define fstat(fd, buf) rx3_compat_fstat((fd), (buf))
