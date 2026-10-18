/* memshim.c - LD_PRELOAD: Chromebit fixups that must run before rbp's own code.
 *
 * 1) /dev/mem denial + broken mmap redirect
 *    rbp's user_space_rtc_init() does:
 *        fd = open("/dev/mem", O_RDONLY);
 *        p  = mmap(NULL, 64, PROT_READ, MAP_SHARED, fd, 0x2098000);   // i.MX6 SRC
 *    On the Chromebit (RK3288) that physical address is not the i.MX6 SRC, so
 *    the read is garbage; rbp turns it into a bogus pointer and SIGSEGVs in a
 *    loop, tripping the watchdog.  chmod 000 /dev/mem is useless (root has
 *    CAP_DAC_OVERRIDE: open() succeeded).  We deny the open here; rbp then
 *    falls back to mmap(..., MAP_SHARED, fd=-1, addr), which we redirect to
 *    private anonymous memory so sched_clock/v2_get_cycles read zero.
 *
 * 2) /dev/gpiodrv read + poll shim (formerly gpioshim.so)
 *    On the RX3 /dev/gpiodrv is a GPIO driver; reads return pin state.  Our
 *    stub is a regular file, so read() hits EOF and rbp's GpioManager busy-
 *    spins (load average >50, ~50s CPU).  Return a constant 1 zero byte like
 *    gpioshim did.  Folded in here so there is a single open()/read() layer
 *    (two LD_PRELOADs both defining open() would shadow each other).
 *
 *    GpioManager also does poll(fd, POLLIN, -1) on /dev/gpiodrv.  A regular
 *    file is *always* poll-ready, so that poll returns instantly and two
 *    GpioManager threads spin at ~11k polls/s each (load ~20).  We park them:
 *    never report the gpio fd ready, sleep for the requested (or 50 ms) time
 *    and return 0 (timeout), so the threads idle at ~20 wakeups/s total.
 *    Same trick as PrimeBox fbshim-tsc.c / rb2go fbshim-window.c.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#ifndef SYS_poll
#define SYS_poll __NR_poll
#endif

#ifndef SYS_mmap2
#define SYS_mmap2 __NR_mmap2
#endif
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef O_TMPFILE
#define O_TMPFILE 020000000
#endif

#define MAX_FDS 4096
static char is_gpio[MAX_FDS];
static char is_tsc[MAX_FDS];

static int is_dev_mem(const char *path)
{
    return path && strcmp(path, "/dev/mem") == 0;
}

static int is_gpiodrv(const char *path)
{
    size_t n;
    if (!path)
        return 0;
    n = strlen(path);
    if (n < 7)
        return 0;
    return strcmp(path + n - 7, "gpiodrv") == 0;
}

/* XDJ-RX3 custom tsc2007 touch device (we run with no touchscreen) */
static int is_tscdev(const char *path)
{
    return path && strstr(path, "tsc2007") != NULL;
}

static int deny_mem(void)
{
    errno = EACCES;
    return -1;
}

static mode_t take_mode(int flags, va_list ap)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    return mode;
}

static int track(int fd, const char *path)
{
    if (fd >= 0 && fd < MAX_FDS) {
        is_gpio[fd] = is_gpiodrv(path);
        is_tsc[fd]  = is_tscdev(path);
    }
    return fd;
}

/* --- deny /dev/mem; track /dev/gpiodrv --- */
int open(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    int fd;

    if (is_dev_mem(path))
        return deny_mem();

    va_start(ap, flags);
    mode = take_mode(flags, ap);
    va_end(ap);

    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
    return track(fd, path);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    int fd;

    if (is_dev_mem(path))
        return deny_mem();

    va_start(ap, flags);
    mode = take_mode(flags, ap);
    va_end(ap);

    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
    return track(fd, path);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    int fd;

    if (is_dev_mem(path))
        return deny_mem();

    va_start(ap, flags);
    mode = take_mode(flags, ap);
    va_end(ap);

    fd = (int)syscall(SYS_openat, dirfd, path, flags, mode);
    return track(fd, path);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    int fd;

    if (is_dev_mem(path))
        return deny_mem();

    va_start(ap, flags);
    mode = take_mode(flags, ap);
    va_end(ap);

    fd = (int)syscall(SYS_openat, dirfd, path, flags, mode);
    return track(fd, path);
}

int close(int fd)
{
    if (fd >= 0 && fd < MAX_FDS) {
        is_gpio[fd] = 0;
        is_tsc[fd]  = 0;
    }
    return (int)syscall(SYS_close, fd);
}

/* --- /dev/gpiodrv: constant GPIO input (never changes) --- */
ssize_t read(int fd, void *buf, size_t count)
{
    if (fd >= 0 && fd < MAX_FDS && is_gpio[fd] && buf && count >= 1) {
        memset(buf, 0, 1);   /* simulate a pin that never changes */
        return 1;
    }
    /* When the host touch bridge is active, read its native six-byte RX3
     * reports from the FIFO. Without it, retain the old no-touch fallback. */
    if (fd >= 0 && fd < MAX_FDS && is_tsc[fd] && buf && count >= 6) {
        const char *bridge = getenv("RX3_TOUCH_BRIDGE");

        if (bridge && bridge[0] == '1')
            return (ssize_t)syscall(SYS_read, fd, buf, count);

        {
            struct timespec ts = { 0, 20000000 };
            memset(buf, 0, 6);
            nanosleep(&ts, NULL);
            return 6;
        }
    }
    return (ssize_t)syscall(SYS_read, fd, buf, count);
}

/* --- /dev/gpiodrv: park GpioManager instead of spinning on a regular file ---
 * A regular file is always poll-ready, so rbp's GpioManager threads return from
 * poll() instantly and re-poll (~11k/s each).  Report "no event": clear revents
 * for the gpio fds, sleep, and return 0 (timeout).  GpioManager treats that as
 * "nothing happened" and just polls again slowly. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (fds && nfds >= 1) {
        nfds_t i;
        int all_gpio = 1;

        for (i = 0; i < nfds; i++) {
            if (fds[i].fd >= 0 && fds[i].fd < MAX_FDS && is_gpio[fds[i].fd])
                fds[i].revents = 0;          /* never ready */
            else
                all_gpio = 0;
        }

        if (all_gpio) {
            if (timeout < 0) {
                struct timespec ts = { 0, 50000000 };       /* 50 ms */
                nanosleep(&ts, NULL);
            } else if (timeout > 0) {
                struct timespec ts = { timeout / 1000,
                                       (long)(timeout % 1000) * 1000000L };
                nanosleep(&ts, NULL);
            }
            return 0;
        }
    }
    return (int)syscall(SYS_poll, fds, nfds, timeout);
}

/* --- redirect the broken mmap(fd=-1, MAP_SHARED) --- */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
        offset = 0;
    }
    return (void *)syscall(SYS_mmap2, addr, length, prot, flags, fd,
                           (unsigned long)offset >> 12);
}
