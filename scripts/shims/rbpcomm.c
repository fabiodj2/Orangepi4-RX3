/*
 * rbpcomm.so -- name the player process "rbp", as it is on the XDJ-RX3.
 *
 * On the deck rbp is exec'ed directly, so its comm is "rbp". This port starts it
 * through the chroot's loader (`/lib/ld-linux.so.3 /root/pdj/rbp -a`, a command line
 * every stop/kill pattern of both launchers matches), so its comm is
 * "ld-linux.so.3". The rx3-toolkit core decides "am I inside the player?" from
 * /proc/self/comm == "rbp" (rx3_core_hook.c, is_player_process) and otherwise does
 * nothing at all -- no log, no ready file (measured 2026-10-13).
 *
 * This constructor renames the main thread to "rbp" with PR_SET_NAME, only when the
 * command line is exactly that loader + /root/pdj/rbp, so an exec'ed utility that
 * inherits LD_PRELOAD keeps its own name. The command line is untouched. It must be
 * LAST in LD_PRELOAD: preloaded constructors run in reverse list order, so last runs
 * first, before the core asks.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>

__attribute__((constructor)) static void rbpcomm_init(void)
{
    static const char want[] = "/lib/ld-linux.so.3\0/root/pdj/rbp";
    char buf[64];
    ssize_t n;
    int fd = open("/proc/self/cmdline", O_RDONLY);

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < (ssize_t)sizeof want || memcmp(buf, want, sizeof want) != 0)
        return;
    prctl(PR_SET_NAME, "rbp", 0, 0, 0);
}
