/*
 * pointsrc.c — see pointsrc.h.
 *
 * Layout: discovery (find a device whose name/capabilities match) -> a reader
 * loop per device kind -> tscfake_emit(). The reader thread wraps discovery in a
 * retry loop, so "no device yet" and "device unplugged" are the same state and
 * a replug needs no special case.
 *
 * The absolute loop is the SC Live 4's, carried over: same event codes (it
 * accepted both ABS_X/ABS_Y and the multitouch ABS_MT_POSITION_* pair), same
 * "down" sources (BTN_TOUCH and ABS_MT_TRACKING_ID), same emit-on-SYN_REPORT.
 * The relative loop is new and exists because an HDMI monitor has no digitiser.
 */
#define _GNU_SOURCE
#include "pointsrc.h"
#include "point_xform.h"
#include "tscfake.h"
#include "touch_zone.h"
#include "menu_zone.h"
#include "menu_window.h"
#include "side_zone.h"
#include "util_zone.h"
#include "wave_zone.h"
#include "prompt_zone.h"
#include "fx_zone.h"
#include "fxpad_zone.h"  /* the BPM cell's momentary X/Y pad -- the operator's ask of
                          * 2026-10-07; fxpad_zone.h carries the rectangle, the two
                          * wire forms and the momentary restore */
#include "hc_zone.h"
#include "drmband.h"
#include "syscalls.h"
#include "envutil.h"
#include "usb_label.h"   /* what a media device's name may contain -- the same rules
                          * knobshim writes into rbp's own device record with */

/* The one rbp thing this file knows: the keycode a QUANTIZE tap becomes, and
 * how to send it. rbp_key.h is a leaf (rbp_abi.h and libc, nothing more), which
 * is why fbshim can reach rbp's key path without linking anything that carries
 * rbp's meter hook -- see rbp_key.h. Every send is a no-op unless the process
 * is rbp and its KeyManager exists yet, so this needs no guard of its own. */
#include "rbp_abi.h"
#include "rbp_key.h"
#include "plinn_scan.h"

/* The channel faders the drawers drive. The *send* is rbp_key's, but a fader is
 * also the meter bridge's input, so the position has to be recorded where the
 * meter (knobshim, rbp_vu.c) will read it -- and fader_state.o is linked into both
 * shims so that "where it reads it" is this same array. fader_state.c says why that
 * object exists at all. */
#include "rbp_vu.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <elf.h>
#include <link.h>
#include <poll.h>
#include <unistd.h>

/* --- the kernel's input ABI, declared here rather than taken from
 * <linux/input.h> ----------------------------------------------------------
 *
 * The shims are cross-compiled against armel headers of one vintage and run
 * against a kernel of another, and <linux/input.h> has already changed shape
 * under us once: since Linux 5.4 it selects between a `struct timeval time` and
 * a `__sec/__usec` pair depending on __USE_TIME_BITS64, which is why
 * tools/evdevdump.c has to name its fields through input_event_sec/usec. These
 * definitions are the small, stable subset the shim needs, so the shim does not
 * care which branch the build host's headers take.
 */
struct ev_input_event {
    struct { uint32_t sec, usec; } time;
    uint16_t type, code;
    int32_t  value;
};

struct ev_absinfo {
    int32_t value, minimum, maximum, fuzz, flat, resolution;
};

/* EVIOCGABS encodes sizeof(struct input_absinfo) in the request, so a mismatch
 * would silently ask the kernel for a different thing than we read back. */
_Static_assert(sizeof(struct ev_absinfo) == 24,
               "struct input_absinfo must be 24 bytes for EVIOCGABS");

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define SYN_REPORT 0

#define ABS_X                 0x00
#define ABS_Y                 0x01
/* ABS_MT_SLOT (0x2f, below) is what makes the per-slot state below meaningful: the
 * panel sends it before every contact's own axes, and reading it is the difference
 * between two fingers and one pointer that teleports. It was read once before, while
 * the waveform gesture was a two-finger pinch; that gesture was rejected and the
 * read went with it (docs/07-touch.md has that history), and it is back now for the
 * drawers' faders, which are the one thing here that genuinely wants two hands. */
#define ABS_MT_SLOT           0x2f
#define ABS_MT_POSITION_X     0x35
#define ABS_MT_POSITION_Y     0x36
#define ABS_MT_TRACKING_ID    0x39

/* TWO CONTACTS, AND NO MORE. The panel's HID descriptor declares ten fingers, and
 * the reader keeps per-slot state for exactly two of them: slot 0, the PRIMARY
 * contact every other surface in this shim is built around, and slot 1, which the
 * edge drawers take and nothing else does. A third finger's events are recognised
 * and DROPPED at their own slot index -- the number is not clamped, because
 * clamping would land a third finger's position in the second one's slot and
 * re-create the very defect this dimension exists to remove. */
#define POINT_SLOTS 2

#define REL_X 0x00
#define REL_Y 0x01

#define BTN_LEFT  0x110
#define BTN_TOUCH 0x14a

/* _IOC(_IOC_READ, 'E', nr, size): 'E' is 0x45. */
#define EVIOCGNAME(len)     (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | 0x06UL)
#define EVIOCGBIT(ev, len)  (0x80000000UL | ((unsigned long)(len) << 16) | (0x45UL << 8) | (0x20UL + (unsigned long)(ev)))
#define EVIOCGABS(abs)      (0x80000000UL | (24UL << 16) | (0x45UL << 8) | (0x40UL + (unsigned long)(abs)))

#define POINT_KIND_NONE 0
#define POINT_KIND_ABS  1
#define POINT_KIND_REL  2
#define POINT_KIND_AUTO 3

#define POINT_SCAN_MAX 32          /* /dev/input/event0 .. event31 */
#define POINT_NAME_MAX 256
#define POINT_PATH_MAX 64

#define POINT_DWELL_DEFAULT_MS 45
/* The same number as the dwell, for the same measured reason: rbp's
 * TouchAdValueHysteresis discards the first frame of a down burst, so a press rbp
 * is to act on has to be held for a few of its ~57 Hz frames. This one is the
 * replay of a swallowed strip tap (menu_replay_tap below), which rbp never saw the
 * down edge of and so has to be given a whole press. POINT_MENU_TAP_MS overrides it
 * for the bench -- deliberately *not* in start-rb.sh's SHIM_VARS, like the menu's
 * other bench knobs, so it cannot be set from rb.conf by accident. */
#define POINT_MENU_TAP_DEFAULT_MS 45
/* The panel's press-and-hold, as two numbers, both bench-overridable and both
 * deliberately *not* in start-rb.sh's SHIM_VARS -- the same rule as the menu's
 * other bench knobs, so neither can be set from rb.conf by accident:
 *
 *   POINT_MENU_HOLD_MS       how long the FINGER must be down (MZ_HOLD_FINGER_MS;
 *                            0 turns the hold off and every press is a tap again)
 *   POINT_MENU_HOLD_SEND_MS  how long the KEY is then held (MZ_HOLD_KEY_MS)
 *
 * menu_zone.h's MZ_HOLD_FINGER_MS carries the measurement behind both: rbp gives
 * one keycode two meanings, press = the menu and hold = UTILITY, and its own timer
 * is what decides -- between 300 and 400 ms, measured 2026-09-29. */
#define POINT_RESCAN_MS        500

/* How often read_loop_abs() looks at the clock while a press is still down on a menu
 * button -- i.e. the resolution of the moment the panel lets go of the screen by
 * itself. It exists only so that loop does not have to block through that moment,
 * and it is deliberately not a bench knob: it is how often a question is asked, not
 * an answer to it, and there is no feel to tune. 20 ms is under the point where a
 * delay is visible and coarse enough that a hold costs fifty wake-ups a second on a
 * thread that is idle the rest of the time. */
#define POINT_MENU_HOLD_TICK_MS 20

static pthread_t reader_tid;
static int reader_started = 0;

/* Written by the reader thread, read by pointsrc_status() from another thread.
 * Deliberately not synchronised: this is a diagnostic snapshot, and a torn
 * string would still be a readable one. The alternative (a mutex taken on every
 * pointer event) would put a lock in the path this file exists to keep cheap. */
static volatile int  st_kind = POINT_KIND_NONE;
static volatile int  st_down = 0;
static volatile int  st_cursor_x = POINT_LOGICAL_W / 2;
static volatile int  st_cursor_y = POINT_LOGICAL_H / 2;
static char st_path[POINT_PATH_MAX];
static char st_name[POINT_NAME_MAX];

/* --- logging ---------------------------------------------------------------
 *
 * One FILE* opened on first use and never closed, rather than an fopen/fclose
 * per line: the log is written from the pointer path, and a per-event open is
 * exactly the mistake Milestone 1 is removing from the DirectFB driver. */
static FILE *log_fp = NULL;
static int  log_debug = 0;

/* POINT_QUANTIZE_TAP: whether a press on a deck QUANTIZE box is turned into
 * rbp's keycode for it. On by default -- it is the operator's ask, and the box
 * is inert without it. It is not, however, a part of the screen no other screen
 * uses: on the BROWSE screen rbp binds its own control over the same rectangle
 * (the deck strip's time display, TIME/REMAIN), so a press there does both
 * things at once. That is measured and written down rather than discovered --
 * docs/07-touch.md, docs/13-raspberrypi4.md -- and this gate is what lets the
 * operator end it from the unit without a rebuild if the double action is worse
 * than the feature is worth. */
static int  quantize_enabled = 1;

/* POINT_MENU: whether the swipe-down top menu exists at all. On by default, like
 * POINT_QUANTIZE_TAP, because it is the operator's ask. With it off, no report is
 * ever offered to menu_zone.c -- so rbp's input stream is byte-identical to what
 * it is without this feature, which is the property the drill checks and the
 * reason the gate is one call site rather than a parallel path.
 *
 * POINT_MENU_MOUSE: whether the menu also answers the *relative* device. Off by
 * default, and that is a design choice rather than an omission: a mouse has no
 * natural swipe, and more to the point the menu and the mouse arrow both write
 * the one framebuffer page this port has (cursor_paint.c's conditional restore
 * assumes it is the only writer in its rectangle). Off means ABS-only, i.e. the
 * two never overlap. With it on they can, and they self-heal against each other
 * at the cost of a flickering arrow inside the panel -- see docs/07-touch.md. */
static int  menu_enabled = 1;
/* The BEAT FX panel's three taps (fx_zone.h), their own switch and NOT POINT_MENU's: they
 * are rbp's own drawing rather than a shim surface, so an operator who turns the band
 * off may still want a finger on the FX panel to work -- and one who wants the panel left
 * entirely to rbp sets POINT_FX_TOUCH=0 and gets exactly that. */
static int  fx_touch = 1;
/* The HOT CUE pad row (hc_zone.h), its own switch for fx_touch's reason: it is rbp's own
 * drawing rather than a shim surface, and an operator who wants only the panel left alone
 * sets POINT_HOTCUE_TOUCH=0 and gets exactly that. Off means the sixteen reports this
 * module would have taken go to rbp untouched, byte for byte. */
static int  hc_touch = 1;
/* Touch on the pad strip while it shows STATUS (the pads): off by default, the
 * operator's call of 2026-10-13 ("retirar o touch da parte de STATUS e deixar apenas
 * para BEATFX"). Off, every gesture that STARTS on the strip (y STRIP_Y0..STRIP_Y1)
 * while strip_page() reads the pads is swallowed whole -- no HOT CUE row, no rbp
 * pad-mode change from the caption. POINT_STATUS_TOUCH=1 gives both back. */
static int  status_touch = 0;
static int  menu_mouse = 0;

/* The buttons' keycodes, in menu_zone.c's column order: the first six are rbp's own
 * browse keycodes, all six now measured (rbp_abi.h carries the derivation and the
 * runs), and they are the same six the keyboard map sends from its digit row. The
 * seventh is USB STOP, which no keyboard row has and which is here alone.
 *
 * PLAYLIST and SEARCH were 0 here for one release: the two slots the derivation
 * named and nothing had measured, drawn and named and visibly pending, firing
 * nothing and saying so. POINT_MENU_KEY_EXTRA is how they were measured, and it
 * survives the promotion -- it is the instrument, and the next unnamed key will
 * want it again. */
static const int menu_key[MZ_COLS] = {
    K_SOURCE, K_BROWSE, K_TAGLIST, K_PLAYLIST, K_SEARCH, K_MENU, K_USBSTOP
};

/* Which of the keys above rbp only finishes on when it sees a SECOND edge. One
 * entry today, so this is a predicate and not a table -- but it is asked by keycode
 * rather than by column, deliberately: POINT_MENU_KEY_EXTRA repoints a column at
 * some other key, and the edge sequence a key needs belongs to the KEY. Overriding
 * USB STOP's column with something ordinary should stop sending it a repeat, and it
 * does.
 *
 * K_USBSTOP is the odd one of the seven: it is not "repeat-only" but "press AND
 * repeat", because its handler does half the job on each edge (rbp_abi.h's entry
 * has the branch table and the measurement). map_flx4.c's note table carries the
 * same idea as a per-note flag; this is the same mechanism one layer down. */
static int menu_key_needs_repeat(int code)
{
    return code == K_USBSTOP;
}

/* The table actually used: menu_key, overridden in column order by the bench knob
 * POINT_MENU_KEY_EXTRA (a comma-separated list) at start-up.
 *
 * It is deliberately NOT in start-rb.sh's SHIM_VARS and not in rb.conf. That list
 * exports every name it holds -- *including* the ones nobody set, as the empty
 * string -- so a shipped default would overwrite the ad-hoc value with "" on the
 * next service restart, which is exactly the trap start-rb.sh:176-180 documents.
 * An operator setting it in the environment by hand is the only user it has, and
 * the service does pass one through: the unit file has no EnvironmentFile, but
 * `systemctl set-environment POINT_MENU_KEY_EXTRA=0x204,0x205` reaches rbp on the
 * next restart and `unset-environment` takes it away again.
 *
 * Note the *spelling*, because it is the whole reason this works: POINT_MENU_* is
 * the shim's name, which SHIM_VARS does not list and therefore does not rewrite.
 * The RB_POINT_MENU_* spelling is the deployed-config one, and it only becomes
 * POINT_MENU_* through that same SHIM_VARS loop -- so on a unit whose
 * start-rb.sh predates the knob, `systemctl set-environment RB_POINT_MENU=0`
 * reaches the service and stops there, with the shim silently using its in-source
 * default. Measured 2026-09-29, and it cost three runs before the launcher was
 * read rather than the shim. `grep POINT_MENU /opt/rblive4/start-rb.sh` is the
 * one-line check. */
static int  menu_key_eff[MZ_COLS];

#define POINT_LOG "/tmp/pointsrc.log"

/* Also called by fb_cursor.c, so the pointer's two halves land in one file in
 * the order the events happened. That makes this the one place in the pointer
 * path that takes a lock — the file's whole design keeps locks out of the reader
 * loop, and a vfprintf racing another thread's is undefined rather than merely
 * interleaved.
 *
 * The per-event lines are written only when POINT_DEBUG is on: a report per
 * SYN_REPORT would be the flood this file exists to avoid. What is written
 * unconditionally is the rare, one-off event — attach, detach, a device that
 * went away, and a recognized QUANTIZE tap — because those are the lines a
 * drill reads back afterwards and a silent feature is not evidence. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* THE OPEN IS A ONE-SHOT, AND IT USED TO BE ABLE TO EAT A LINE.
 *
 * This was:
 *
 *     if (!log_tried) { log_tried = 1; log_fp = fopen(POINT_LOG, "a"); }
 *     if (log_fp == NULL) return;
 *
 * which publishes its two halves in the wrong order and under no lock at all.
 * Thread A sets log_tried = 1 and is still inside fopen(); thread B arrives,
 * reads log_tried, concludes somebody has already opened the file, and then
 * consults log_fp -- which is still NULL -- and returns having written nothing.
 * Silently, because that same NULL is also how a legitimate "the file could not
 * be opened, say nothing" is spelled. The window is one fopen wide, which is
 * microseconds, and it is open only at process start, because before it every
 * call is a candidate and after it log_fp is set. Three threads log in that
 * window (the reader's attach line, the cursor thread's, and whichever of rbp's
 * own threads called in), so it is a coin toss rather than a rarity.
 *
 * WHY IT COST WHOLE DRILL PHASES. The lines a drill gates on are one-shot: the
 * reader attaches once, the cursor opens the fb once, and the menu's "drawing
 * the panel" is written once when that thread opens the fb. A phase whose
 * signature line is the one that loses the race never produces it, at all, for
 * the rest of the process's life -- so the drill waits out its timeout and
 * reports "the phase never reached the state it claimed". That is exactly the
 * anomaly that has been read as a mystery since menu11: phases failing their own
 * gate with the rest of the log healthy, and by preference the FIRST phase of a
 * run, which is the one whose signature is written soonest after start.
 *
 * Measured on the unit 2026-09-29 (work/menu19.sh, both phases lost theirs in one
 * run): the log held the reader's attach line, the raw-range line and the cursor
 * line -- all written by threads that log *later* -- and no menu line of either
 * kind, in a run where rbp's environ has no POINT_MENU and so the ON branch had
 * to run. The survivors are the tell: the loser is whichever call is in flight
 * while the winner is inside its fopen.
 *
 * pthread_once() is the whole fix: one thread runs log_open, every other caller
 * blocks until it has returned, and NULL then means only what it says.
 *
 * shimutil.c's klog() has the same missing lock and is *not* wrong, which is why
 * this one deserves the paragraph: klog's flag and its value are one variable
 * (`log_fd < 0`, then `log_fd = open(...)`), so a thread that loses the race
 * opens a second fd and still writes its line -- a leaked fd, no lost line. Here
 * the flag and the value are two variables, and that is what makes it a race with
 * a victim. */
static pthread_once_t log_once = PTHREAD_ONCE_INIT;

static void log_open(void)
{
    log_fp = fopen(POINT_LOG, "a");
}

void pointsrc_log(const char *fmt, ...)
{
    va_list ap;
    pthread_once(&log_once, log_open);
    if (log_fp == NULL)
        return;
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', log_fp);
    fflush(log_fp);
    pthread_mutex_unlock(&log_lock);
}

/* --- capability/name helpers ---------------------------------------------- */

static int bit_set(const unsigned char *bits, unsigned int bit)
{
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

static int want_kind(void)
{
    const char *k = env_str("POINT_KIND", "auto");
    if (strcmp(k, "none") == 0) return POINT_KIND_NONE;
    if (strcmp(k, "abs") == 0)  return POINT_KIND_ABS;
    if (strcmp(k, "rel") == 0)  return POINT_KIND_REL;
    if (strcmp(k, "auto") == 0) return POINT_KIND_AUTO;
    pointsrc_log("pointsrc: unknown POINT_KIND '%s'; treating as auto", k);
    return POINT_KIND_AUTO;
}

/* auto means "abs, then rel": on a machine that has both, the touchscreen is the
 * device the operator is reaching for and the mouse is the fallback. Expanded
 * here into the list open_pointer() walks, so the retry loop has one shape. */
static int kind_candidates(int out[2], int kind)
{
    if (kind == POINT_KIND_AUTO) {
        out[0] = POINT_KIND_ABS;
        out[1] = POINT_KIND_REL;
        return 2;
    }
    out[0] = kind;
    out[1] = POINT_KIND_NONE;
    return 1;
}

static int device_has_kind(int fd, int kind)
{
    unsigned char evbits[8];
    unsigned char absbits[16];
    unsigned char keybits[96];

    memset(evbits, 0, sizeof evbits);
    if (real_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
        return 0;

    if (kind == POINT_KIND_ABS) {
        if (!bit_set(evbits, EV_ABS) || !bit_set(evbits, EV_KEY))
            return 0;
        memset(absbits, 0, sizeof absbits);
        memset(keybits, 0, sizeof keybits);
        if (real_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
            return 0;
        if (real_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
            return 0;
        /* Both ABS_X and ABS_Y, plus something that can say "pressed". The
         * BTN_TOUCH requirement is what keeps a joystick or an accelerometer
         * from being adopted as a pointer. */
        return bit_set(absbits, ABS_X) && bit_set(absbits, ABS_Y) &&
               bit_set(keybits, BTN_TOUCH);
    }

    if (!bit_set(evbits, EV_REL) || !bit_set(evbits, EV_KEY))
        return 0;
    memset(absbits, 0, sizeof absbits);
    memset(keybits, 0, sizeof keybits);
    if (real_ioctl(fd, EVIOCGBIT(EV_REL, sizeof absbits), absbits) < 0)
        return 0;
    if (real_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0)
        return 0;
    /* BTN_LEFT is required for the same reason BTN_TOUCH is above: REL_X/REL_Y
     * are also reported by scroll wheels, which are not pointers. */
    return bit_set(absbits, REL_X) && bit_set(absbits, REL_Y) &&
           bit_set(keybits, BTN_LEFT);
}

static void read_abs_range(int fd, int code, int *min, int *max)
{
    struct ev_absinfo ai;
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return;
    /* A degenerate range (max == min) is what a device reports when the kernel
     * has no calibration for that axis; the caller's default is better than a
     * divide-by-zero. */
    if (ai.maximum > ai.minimum) {
        *min = ai.minimum;
        *max = ai.maximum;
    }
}

/* Where the reader starts from is the device's own position -- not zero, and not
 * the screen's centre.
 *
 * The kernel drops an ABS event whose value is the one the device already holds
 * (measured: a report can carry a y and no x at all), so a loop that has never
 * been handed an x cannot tell "the finger is at x 0" from "no x was reported".
 * It reads x 0, which is the far-left column. Measured on the unit 2026-09-28:
 * after a service restart, 18 consecutive injected taps at raw x 83 were read as
 * `raw=(0,y)` -- the shim's x was still the zero it started with -- and every one
 * was lost, while the same taps landed as soon as one report moved x. The same
 * hole applies to a finger: a first touch at the x the panel last reported
 * arrives with no x event, and the tap is delivered to rbp at logical x 0.
 *
 * Seeding gives the loop one invariant, which then holds for its whole life:
 * `rx`/`ry` are the device's stored values. True at the start because of this
 * call; preserved afterwards because a value that changes always arrives as an
 * event, and a value that does not is already held here. */
static void seed_abs_position(int fd, int *rx, int *ry)
{
    struct ev_absinfo ai;

    /* The abs candidate filter requires ABS_X and ABS_Y (read_caps above), so
     * both ioctls name an axis this device has. A failure leaves the default. */
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0)
        *rx = ai.value;
    memset(&ai, 0, sizeof ai);
    if (real_ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0)
        *ry = ai.value;
}

static int open_matching_device(int kind, const char *match, char *path_out,
                                unsigned long path_len, char *name_out,
                                unsigned long name_len)
{
    int i;

    /* An explicit node path skips the scan entirely. It is the escape hatch for
     * a device whose capabilities are unusual enough that the filters above
     * reject it. */
    if (match != NULL && match[0] == '/') {
        int fd = real_open(match, O_RDONLY, 0);
        if (fd < 0)
            return -1;
        snprintf(path_out, path_len, "%s", match);
        name_out[0] = '\0';
        (void)real_ioctl(fd, EVIOCGNAME(name_len - 1), name_out);
        name_out[name_len - 1] = '\0';
        return fd;
    }

    for (i = 0; i < POINT_SCAN_MAX; i++) {
        char path[POINT_PATH_MAX];
        char name[POINT_NAME_MAX];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = real_open(path, O_RDONLY, 0);
        if (fd < 0)
            continue;

        memset(name, 0, sizeof name);
        if (real_ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0)
            name[0] = '\0';
        name[sizeof name - 1] = '\0';

        if (match != NULL && strstr(name, match) == NULL) {
            real_close(fd);
            continue;
        }
        if (!device_has_kind(fd, kind)) {
            real_close(fd);
            continue;
        }

        snprintf(path_out, path_len, "%s", path);
        snprintf(name_out, name_len, "%s", name);
        return fd;
    }
    return -1;
}

/* --- reader loops ---------------------------------------------------------- */

static void log_attach(int kind, const char *path, const char *name)
{
    pointsrc_log("pointsrc: %s device %s name='%s'",
         kind == POINT_KIND_ABS ? "absolute" : "relative", path, name);
}

static void absorb_into_xform(struct point_xform *x, int fd, int kind)
{
    point_xform_init(x);
    if (kind == POINT_KIND_ABS) {
        /* Only the absolute path reads the device's range; a relative device
         * reports counts with no range at all, and point_xform_rel() never looks
         * at these fields. */
        read_abs_range(fd, ABS_X, &x->raw_min_x, &x->raw_max_x);
        read_abs_range(fd, ABS_Y, &x->raw_min_y, &x->raw_max_y);
        pointsrc_log("pointsrc: raw range x=[%d..%d] y=[%d..%d] swap=%d inv_x=%d inv_y=%d",
             x->raw_min_x, x->raw_max_x, x->raw_min_y, x->raw_max_y,
             x->swap_xy, x->invert_x, x->invert_y);
        if (x->fit_fb_w > 0)
            pointsrc_log("pointsrc: abs fit: panel %dx%d -> picture %dx%d at %d,%d",
                 x->fit_fb_w, x->fit_fb_h, x->fit_dw, x->fit_dh, x->fit_bx, x->fit_by);
        else
            pointsrc_log("pointsrc: abs fit: whole panel (fb is the logical size, or POINT_ABS_FIT=0)");
    }
}

/* The deck QUANTIZE boxes are the one place a pointer position is ours to
 * interpret rather than rbp's: rbp draws the boxes and never lets a touch on
 * one mean anything (touch_zone.h has the measurement). So a press that starts
 * on one is turned into the keycode rbp has for that button -- K_QUANTIZE,
 * which is what a hardware press sends -- and rbp does the rest, its own screen
 * included.
 *
 * Called from pointer_report() -- one call per report that reaches rbp at all,
 * which is the contract with the zone module (touch_zone_feed: one call per
 * report, in order), and it is the module that knows which reports can mean
 * anything: a drag is the same press at a new position, and a resting finger is
 * the same press again. A report the menu swallowed never gets here at all, which
 * is deliberate: see pointer_report().
 *
 * The key goes out as a press and a release together, exactly as aloop_apply()
 * sends a pad. The gesture is complete at the press -- rbp's handler toggles on
 * the press alone -- so there is no held key to release later, and nothing to
 * lose if the device is unplugged mid-touch.
 *
 * CALL THIS BEFORE tscfake_emit(). pointer_report() is now the one place that
 * order is written down, which is why it takes both calls. It is not cosmetic.
 * uif::IKeyManager::sendKey (0x37ad64) fills ONE
 * shared IKeyInput -- the pointer at KeyManager+132 -- and only then walks the
 * listener list, handing every listener that same mutable struct (`this->vt[7]`
 * at the tail of sendKey; the press/release split before it is shift-key
 * bookkeeping, not the dispatch). So a key sent while rbp's own touch thread is
 * working through a report can have its keycode overwritten before the deck's
 * PlayerInnards reads it -- and rbp's touch thread is exactly what runs when the
 * report carrying this gesture arrives.
 *
 * The menu's keys are not exposed to that race, and it is worth saying why they
 * need no special handling: menu_fire() runs on a report the menu *swallowed*, so
 * there is no tscfake_emit() in flight and rbp's touch thread is not working
 * through anything. The ordering rule below is satisfied by construction rather
 * than by being careful.
 *
 * Where the numbers are, and what they do not say. Two runs on 2026-09-28
 * measured 2/8 and 7/10 taps flipping deck 1's flag (Player+3804, whose only
 * runtime writer is PlayerInnards::onKey_Quantize), and that is what put the
 * race above on the table. They are not evidence for it: the instrument that
 * produced them never moved x, so its reports carried no x at all and the reader
 * held the 0 it started with (seed_abs_position, above) for the whole run --
 * those taps were judged at logical x 0, outside the zone, and the mix within a
 * run is not explained by anything the shim did. The controls were clean and
 * still stand: KEY_6 (K_BROWSE, global) toggled the screen 8/8, and KEY_SPACE
 * (K_PLAY, channel 1, a deck key) started and stopped deck 1 8/8 with no touch
 * in flight.
 *
 * With the reader's x fixed, the same instrument measures 20+20 taps across both
 * decks landing on the press with 0 misses, and a 10-tap control outside the zone
 * landing none. So the ordering below rests on the structure of sendKey, which is
 * where it was found, and not on those runs -- it costs one reordering and it
 * closes a window the structure says is there. */
static void quantize_tap(int down, int x, int y)
{
    int deck = touch_zone_feed(down, x, y);
    if (!deck)
        return;
    pointsrc_log("pointsrc: deck%d QUANTIZE tap at (%d,%d)%s",
                 deck, x, y, quantize_enabled ? "" : " (disabled)");
    if (!quantize_enabled)
        return;
    send_rx_key(K_QUANTIZE, OP_PRESS, deck, 0);
    send_rx_key(K_QUANTIZE, OP_RELEASE, deck, 0);
}

/* Fill menu_key_eff from menu_key and the bench knob. Defined here rather than
 * beside its table because it logs, and pointsrc_log() is defined further down.
 *
 * The knob *overrides* the table in column order, and it keeps the ability to say
 * 0: that is what puts a button back on the sentinel this table shipped with
 * while PLAYLIST and SEARCH were unnamed, so the negative half of a measurement
 * -- "with no keycode, a tap changes nothing" -- stays reproducible. It stops
 * when its list runs out, so one number in the environment moves one button and
 * leaves the other five as shipped. */
static void menu_keys_init(void)
{
    const char *p = env_str("POINT_MENU_KEY_EXTRA", NULL);
    int i;

    for (i = 0; i < MZ_COLS; i++)
        menu_key_eff[i] = menu_key[i];

    if (p == NULL || *p == '\0')
        return;                    /* no knob: the shipped table, untouched */

    for (i = 0; i < MZ_COLS; i++) {
        char *end;
        long n;

        n = strtol(p, &end, 0);    /* base 0, so 0x204 and 204 are both accepted */
        if (end == p) {
            pointsrc_log("pointsrc: POINT_MENU_KEY_EXTRA '%s' is not a keycode list;"
                         " menu button %d '%s' and the rest keep the shipped keys",
                         p, i + 1, menu_label(i + 1));
            return;
        }
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        if (n < 0 || n > 0xffff) {
            pointsrc_log("pointsrc: POINT_MENU_KEY_EXTRA %ld is not a keycode; menu"
                         " button %d '%s' keeps the shipped key", n, i + 1,
                         menu_label(i + 1));
        } else {
            menu_key_eff[i] = (int)n;
            pointsrc_log("pointsrc: menu button %d '%s' -> keycode %#lx (bench)",
                         i + 1, menu_label(i + 1), (unsigned long)n);
        }
        if (*p == '\0')
            break;                 /* knob exhausted: the rest keep the shipped keys */
    }
}

/* Both defined further down, and both used by menu_fire() and by the USB STOP
 * chooser's block below it. Declared here rather than beside their definitions
 * because the chooser sits beside the function that raises it. The third --
 * pointsrc_usb_state() -- is public (pointsrc.h) because menu_draw.c's builder needs
 * the same answer at paint time, and it is forward-declared there. */
static int rbp_i32(unsigned long addr);
static unsigned long long shim_now_ms(void);

/* A tap that landed on a menu button. Same shape as quantize_tap()'s: the
 * gesture is complete at the tap -- rbp's handler for a *press* of every one of
 * these keys acts on the press alone, with USB STOP the one exception (it is
 * finished by the repeat edge, and menu_key_needs_repeat() below sends it) -- so
 * the key goes out as a press and a release together and there is nothing left to
 * release if the panel is unplugged mid-touch.
 *
 * AND USB STOP NO LONGER GETS THIS FAR unless POINT_USBSTOP_PROMPT=0: menu_fire()
 * returns for it and raises the chooser instead. The three-edge shape
 * menu_key_needs_repeat() describes is then usb_stop_send()'s, on the chosen
 * device's OWN channel rather than CH_GLOBAL.
 *
 * A *held* finger is the one case where the two edges are separated on purpose,
 * and it is the operator's second ask on the glass: *"if i long press menu it
 * should go to utility"* -- which is the MENU button's own second meaning on this
 * hardware, implemented by rbp itself on the same keycode (measurement in
 * menu_zone.h's MZ_HOLD_FINGER_MS). This function does not implement UTILITY; it
 * holds the key and rbp's timer does the rest, which is why the rule here is a
 * duration and not a screen. Two callers reach this arm and it does the same thing
 * for both: the release, and read_loop_abs() the moment the threshold passes with
 * the finger still down -- the operator's follow-up ask, *"the menu should disappear
 * and it should just go to utility by itself"*, which is menu_hold_pending().
 *
 * CH_GLOBAL, not a deck channel, because these are the browse/library family: on
 * hardware they are global keys with no deck of their own (map_kbd.c sends all
 * six browse keys the same way, and it is measured -- see docs/08-controls.md).
 * USB STOP IS NO LONGER ONE OF THEM: the block below catches it before this and
 * raises the chooser instead, and the eject goes out from there on the chosen
 * DEVICE's own channel -- CH_GLOBAL is 1, which is USB 1's number, and that is why
 * the old binding could never have reached the second device.
 *
 * A button whose keycode is 0 fires nothing and *says so*, and the line names the
 * keycode when it does fire. That is not politeness: a tap that sent nothing and
 * a gesture that never arrived look identical from the outside, so the log is the
 * only thing that separates them -- it is what the PLAYLIST/SEARCH measurement
 * read, and what a re-check reads with the same two buttons put back on 0 through
 * POINT_MENU_KEY_EXTRA. */
static void menu_fire(int button, int held_ms)
{
    const char *label = menu_label(button);
    int hold_ms, send_ms, code;

    /* A button with no label fires nothing and says nothing. The menu's eighth cell
     * -- the globe that opened the browser window -- used to be the one case here;
     * it is gone with the browser (menu_zone.h's block), so menu_button_at() now
     * answers 1..MZ_COLS or 0 and this is belt-and-braces against a future cell
     * being added to the hit test before it is added to the label table. */
    if (label == NULL)
        return;
    code = menu_key_eff[button - 1];
    if (code == 0) {
        pointsrc_log("pointsrc: menu '%s' tapped (button %d): no keycode yet,"
                     " nothing sent", label, button);
        return;
    }

    /* THE SEVENTH COLUMN RAISES THE CHOOSER INSTEAD OF FIRING -- the operator's
     * "when you have USB stop, put up a prompt for USB1, USB2 or Cancel"
     * (2026-10-05). The far right of the band used to stop the operator's stick on
     * one tap, and the stick is production media whose FAT already carries damage
     * (docs/10-usb.md); now the eject takes a three-second hold on a lit button
     * (the operator's ask of 2026-10-07, prompt_zone.h).
     *
     * THE TEST IS BY KEYCODE AND NOT BY COLUMN, which is the same choice
     * menu_key_needs_repeat() makes and for the same reason: POINT_MENU_KEY_EXTRA
     * can repoint a column, and whatever column carries K_USBSTOP is the one that
     * raises the box -- while a column that no longer carries it is unaffected.
     *
     * BOTH PATHS ARE CAUGHT, the tap and the hold, before either: there is no second
     * meaning for a held USB STOP that is worth the operator's media, and the eject
     * is then reachable ONLY from the box. POINT_USBSTOP_PROMPT=0 puts the immediate
     * eject back for bench work.
     *
     * The box is raised on the RELEASE of the column tap -- this function is called
     * from the release -- so no press is inherited, and prompt_open() clears its
     * latches for the same reason. */
    if (code == K_USBSTOP && env_int("POINT_USBSTOP_PROMPT", 1)) {
        struct prompt_state S;

        pointsrc_usb_state(&S);
        /* And the Beat FX picker goes away if it was up. The funnel's own ladder already
         * keeps the chooser from being raised UNDER the picker -- it is asked first, so a
         * tap that would open the picker while the chooser is up is the chooser's -- but
         * this path is a KEYCODE and not a tap, so it arrives with the picker possibly
         * still on the glass. Both boxes are centred and both are opaque, so leaving the
         * two up would show one through the other. Closing here rather than hiding a
         * plane in menu_draw.c is the honest fix: there is one box at a time, and this is
         * the line that says so. */
        if (fxlist_is_open()) {
            fxlist_close();
            pointsrc_log("pointsrc: the USB STOP chooser displaces the Beat FX picker");
        }
        prompt_open(shim_now_ms());
        /* The two labels are the ones the buttons will READ, word for word, so a drill's
         * log says which stick each button offered without a screenshot of the glass. */
        pointsrc_log("pointsrc: menu '%s' -> the USB STOP chooser (usb 1 %s '%s',"
                     " usb 2 %s '%s')", label,
                     S.live[0] ? "ready" : "absent",
                     prompt_cell_text(&S, PR_CELL_USB1),
                     S.live[1] ? "ready" : "absent",
                     prompt_cell_text(&S, PR_CELL_USB2));
        return;
    }

    /* A finger held long enough is rbp's *other* meaning for this key. The panel
     * does not know what that meaning is -- it knows the operator held the button,
     * and it sends a key down long enough for rbp's own timer to decide (see
     * menu_zone.h's MZ_HOLD_FINGER_MS for the measurement and the two numbers).
     *
     * The send blocks this reader thread for send_ms, which is the price of a
     * synthesized hold: the key's edges are not the finger's edges. rbp's timer needs
     * a key-down of its own threshold or longer, and a finger that is a hold at all
     * is only guaranteed to be *just* past MZ_HOLD_FINGER_MS -- inside rbp's measured
     * 300..400 ms band -- so replaying the finger's own duration would reach UTILITY
     * by luck. A fixed span that clears the band with room does it every time.
     * Bounded, and once per hold -- the same trade menu_replay_tap() makes for its
     * 45 ms, with the same justification (evdev buffers, so a touch arriving during
     * it is delayed and not lost). Send 0 and the two edges go out back to back: a
     * plain tap, which is the second A/B lever. */
    hold_ms = env_int("POINT_MENU_HOLD_MS", MZ_HOLD_FINGER_MS);
    if (menu_hold_fires(held_ms, hold_ms)) {
        send_ms = env_int("POINT_MENU_HOLD_SEND_MS", MZ_HOLD_KEY_MS);
        pointsrc_log("pointsrc: menu '%s' held %dms (hold is %dms) -> key %#x"
                     " down for %dms, rbp's own hold action", label, held_ms,
                     hold_ms, code, send_ms);
        send_rx_key(code, OP_PRESS, CH_GLOBAL, 0);
        if (menu_key_needs_repeat(code))
            send_rx_key(code, OP_REPEAT, CH_GLOBAL, 0);
        if (send_ms > 0)
            usleep((useconds_t)send_ms * 1000);
        send_rx_key(code, OP_RELEASE, CH_GLOBAL, 0);
        return;
    }

    pointsrc_log("pointsrc: menu '%s' tapped (button %d) -> key %#x (finger %dms"
                 " of %dms)", label, button, code, held_ms, hold_ms);
    send_rx_key(code, OP_PRESS, CH_GLOBAL, 0);
    /* The middle edge, for a key that needs one, goes out immediately after the
     * press and before the release -- the order map_flx4.c uses for K_TRACKFILTER,
     * and the order the handler expects (rbp_abi.h's K_USBSTOP: the press mutes and
     * notifies, the repeat is what asks the db to stop the device, the release
     * unwinds). A tap therefore carries all three edges back to back, which is a
     * different thing from the panel's own gap between them -- if rbp turns out to
     * want the gap, the hold path below is the shape that has one. */
    if (menu_key_needs_repeat(code))
        send_rx_key(code, OP_REPEAT, CH_GLOBAL, 0);
    send_rx_key(code, OP_RELEASE, CH_GLOBAL, 0);
}

/* ---------------------------------------------------------------------------
 * THE USB STOP CHOOSER -- the part of it that is not pure.
 *
 * The band's seventh column is rbp's own safe eject (K_USBSTOP) and it used to be one
 * tap sent on CH_GLOBAL. CH_GLOBAL is 1, which is USB 1's own channel number -- so
 * that binding could stop the first device and could never have reached the second.
 * The operator's ask of 2026-10-05 --
 *
 *     "when you have USB stop, put up a prompt for USB1, USB2 or Cancel"
 *
 * -- is answered by prompt_zone.c (the box, the rows, the gesture, the labels) and
 * prompt_paint.c (its image). This block is the other half: where rbp's answer about the
 * two devices is read, where each device's own NAME is read, and what a chosen row
 * actually sends.
 * ------------------------------------------------------------------------- */

/* One slot's device name, off the file the watcher leaves beside that slot's FIFO.
 *
 * THE FILE IS usb-watch.sh's -- `/tmp/udev_usbN.label`, written at attach with blkid's
 * LABEL and removed at detach -- and it is the same string rbp's own SOURCE screen shows
 * in its DEVICE NAME column, by the same route (docs/10-usb.md, section *The device
 * name*). The chroot's /tmp IS the host's, which is how both reaches work.
 *
 * READ THROUGH THE RAW CALLS for the reason syscalls.h gives: rbp's own open/read are
 * interposed by this same process. Absent, empty and whitespace-only are one answer --
 * no name -- and prompt_state_name() then leaves the cell its own "HOLD USB n", which is
 * byte for byte the label this box shipped with before names existed. There is no
 * memoisation on purpose: a stick that goes away takes its name with it, and a stale name
 * over an empty slot is worse than reading two small files again.
 *
 * NOT AN ERROR AND NOT LOGGED. This is asked on every tick of the builder's thread as
 * well as from this one, so a line here would be the log-filling trap
 * ([[rblive4-verbose-log-fills-tmpfs]]); what the box draws is the proof, and it says so
 * in menu_draw.c's own verbose line. */
static void usb_name_read(int dev, struct prompt_state *S)
{
    char path[64];
    char raw[USB_LABEL_MAX + 1];
    char text[USB_LABEL_MAX + 1];
    int fd, n;

    raw[0] = '\0';
    snprintf(path, sizeof path, "/tmp/udev_usb%d.label", dev + 1);

    fd = real_open(path, O_RDONLY, 0);
    n = 0;
    if (fd >= 0) {
        n = (int)real_read(fd, raw, sizeof raw - 1);
        real_close(fd);
    }
    if (n < 0)
        n = 0;
    else
        raw[n] = '\0';

    n = usb_label_sanitize(raw, n, text, USB_LABEL_MAX);
    prompt_state_name(S, dev, n > 0 ? text : 0);
}

/* rbp's two UsbStorageManager objects, walked exactly the way
 * getUsbStorageManager() does (rbp_abi.h carries the disassembly).
 *
 * EVERY STEP IS GUARDED, and that is the difference from util_read_state() above.
 * uiBrowse is a plain .bss singleton read at one fixed address, so that one needs no
 * guard beyond "am I rbp at all"; these are POINTERS into objects rbp allocates while
 * it is starting up, so an unguarded walk from this thread can follow a null or a
 * half-built pointer and take the input thread down with it -- the same hazard
 * rbp_led.c's guarded walks exist for.
 *
 * A miss is not fatal and must not be. A caller that cannot read rbp gets both
 * devices absent, which draws both rows dim and sends nothing: a refusal, not an
 * assumption (prompt_zone.h).
 *
 * THE TWO NAMES ARE FILLED FIRST, before any of the guards below, so every caller of
 * this leaves with a struct that is filled in full whether or not rbp could be read --
 * the names are the host's answer and have nothing to do with rbp's. A caller that
 * cannot walk rbp still gets buttons that name the devices they are refusing.
 *
 * PUBLIC (pointsrc.h) because the chooser's picture depends on it: a row whose
 * device rbp reports absent is drawn dim (prompt_paint.c), so menu_draw.c's builder
 * reads this too, on every build, and the answer it was read at is part of the
 * published image tuple. Two callers, two threads, and it reads only rbp's memory --
 * nothing here is written, so there is nothing to serialise. */
int pointsrc_usb_state(struct prompt_state *S)
{
    static int am_rbp = -1;
    unsigned long objmgr, array;
    int count, i;

    S->live[0] = 0;
    S->live[1] = 0;
    for (i = 0; i < PROMPT_DEVICES; i++)
        usb_name_read(i, S);

    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return 0;

    objmgr = *(volatile unsigned long *)(uintptr_t)UI_OBJ_MGR_HOLDER;
    if (!objmgr)
        return 0;
    count = *(volatile int *)(uintptr_t)(objmgr + UIOBJMGR_OFF_MGRNUM);
    array = *(volatile unsigned long *)(uintptr_t)(objmgr + UIOBJMGR_OFF_MGRARR);
    if (!array || count < 1)
        return 0;
    /* This build has two; a larger count is not this binary and is not walked. Sized
     * by PROMPT_DEVICES and not by the box's button count -- today's box has one button
     * per device, but the two-line box it had until 2026-10-07 had OK and CANCEL under
     * the devices, and a walk that grew with the buttons would have started reading
     * managers that are not there (prompt_zone.h). */
    if (count > PROMPT_DEVICES)
        count = PROMPT_DEVICES;

    for (i = 0; i < count; i++) {
        unsigned long mgr = ((volatile unsigned long *)(uintptr_t)array)[i];

        if (!mgr)
            continue;
        /* The channel word, which is what decides which manager a key reaches. A
         * manager at index i that does not say it is channel i+1 is not one this row
         * could fire at, so it is left absent. */
        if (rbp_i32(mgr + USB_MGR_OFF_CHANNEL) != i + 1)
            continue;
        S->live[i] = rbp_i32(mgr + USB_MGR_OFF_MEDIA) == USB_MGR_MEDIA_READY;
    }
    return 1;
}

/* The eject, on the chosen device's OWN channel.
 *
 * THREE EDGES, and rbp_abi.h's K_USBSTOP block is the derivation: the press mutes
 * both players and notifies the media went away, the REPEAT is the one that reaches
 * request_usb_stop -- the actual eject -- and the release unwinds. A press+release tap
 * is a silent no-op; that is a measurement on the unit, and it is why
 * menu_key_needs_repeat() exists at all.
 *
 * THE CHANNEL IS THE DEVICE'S OWN NUMBER and not CH_GLOBAL, for the reason
 * rbp_abi.h's USB_MGR_OFF_CHANNEL records: onKey drops any key whose channel byte
 * differs from the manager's own. prompt_act_channel() is what names it -- 1 for the
 * left button, 2 for the right -- and it answers 0 for PR_ACT_NONE, which sends
 * nothing: a box that stopped nothing sends nothing at all, which is what a release
 * let go before its three seconds, and a press that missed, both are.
 *
 * Back to back, no sleep: one synthesized tap, exactly as menu_fire() sends its own.
 * A wait here would starve the one input loop the band, the window, the drawers and
 * this box all share (docs/07-touch.md's TRAP 2). */
static void usb_stop_send(int ch)
{
    if (ch != 1 && ch != 2)
        return;
    send_rx_key(K_USBSTOP, OP_PRESS, ch, 0);
    send_rx_key(K_USBSTOP, OP_REPEAT, ch, 0);
    send_rx_key(K_USBSTOP, OP_RELEASE, ch, 0);
}

/* --- THE BEAT FX PANEL'S THREE CONTROLS, on their way to rbp -----------------
 *
 * WHY THEY ARE HERE. rbp's own `ui::touch_panel` family has no control that picks the
 * effect, assigns its channel or powers it (fx_zone.h's header), so all three gestures
 * exist only as keycodes -- and all three wire forms are ALREADY PROVEN in this tree by
 * the FLX4, which is the whole reason this is wiring rather than discovery:
 *
 *   map_flx4.c:493    send_rx_key(K_BFXCH,   OP_VALUE, CH_GLOBAL, want)  <- the lever
 *   map_flx4.c:520    send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, pos)   <- FX SELECT
 *   map_flx4.c:1087   K_BFX, PRESS+RELEASE, CH_GLOBAL                    <- FX ON/OFF
 *
 * These are the same calls, raised from a finger on rbp's own drawing instead of from
 * the controller. Back to back, NO SLEEP, like every other send in this file: a wait here
 * would starve the one input loop the band, the window, the drawers and both popups share
 * (docs/07-touch.md's TRAP 2). */
static void fx_ch_send(int want)
{
    send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, want);
}

static void fx_type_send(int pos)
{
    send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, pos);
}

/* The Beat FX ON/OFF toggle, on the FLX4's own wire form: a PRESS then a RELEASE of
 * K_BFX on CH_GLOBAL, which is exactly what the panel's FX ON/OFF button sends
 * (map_flx4.c:1087 `add_note(CH_FXA, N_FX_ONOFF, K_BFX, CH_GLOBAL)`, pinned by
 * test_flx4.c:506-509). So a tap on the effect-name cell is one toggle, whatever the
 * operator's last toggle was -- rbp owns the on/off state, this shim keeps none, and the
 * two cannot disagree. No value is carried: K_BFX is a keycode, not an absolute. */
static void fx_power_send(void)
{
    send_rx_key(K_BFX, OP_PRESS,   CH_GLOBAL, 0);
    send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
}

/* Where BeatEffectManager sits inside MixerEngine, and where the selected channel sits
 * inside THAT. rbp's own offsets, disassembled off work/rbp.nm.txt; they live here rather
 * than in rbp_abi.h because this is the only place in the tree that walks them -- the
 * two shims that already talk to Beat FX (map_flx4.c, map_jp21.c) do it through
 * rbp_beatfx_type() in rbp_bridge.o, which fbshim.so does NOT link (Makefile:292). */
#define FX_BEM_IN_MIXER 0x58        /* MixerEngine* -> BeatEffectManager* */
#define FX_BEM_OFF_CH   0x00        /* BeatEffectManager* -> the select channel */

/* WHAT rbp's Beat FX CH SELECT currently is, or -1 when it cannot be asked.
 *
 * A plain .bss word for the singleton, then ONE pointer chase -- so unlike rbp_i32()'s
 * reads of uiBrowse the two interior words are NULL-checked, and a half-built or
 * torn-down engine answers -1 rather than faulting. The chain, disassembled and then
 * MEASURED on the unit 2026-10-06:
 *
 *   DjEngineIF::getBeatEffectSelectChannel @0x4d314
 *     ldr r4,[pc] -> ME_SINGLETON (0x011493c0)
 *     ldr r0,[r4] -> MixerEngine*
 *     ldr r0,[r0,#0x58] -> BeatEffectManager*
 *     ldr r0,[r0,#0x00] -> the channel      0 = deck 1, 1 = deck 2, 5 = MASTER
 *
 * Measured by driving the FLX4's OWN lever: CH1 -> 0, CH2 -> 1, MASTER -> 5, and back to
 * 0 -- with `+0x04` (the NEXT word setBeatEffectSelectChannel writes) tracking it
 * exactly, so `+0x00` does NOT lag and the cycle can trust what it reads.
 *
 * READING rbp BEATS KEEPING A CURSOR OF OUR OWN, which is the whole point of the chase:
 * the FLX4's lever writes this same word, so a private cursor would desync the moment the
 * operator touched the hardware. -1 is "not rbp, or not started, or torn down"; the caller
 * answers it by starting at deck 1, rbp's measured cold-start default. */
static int fx_ch_read(void)
{
    static int am_rbp = -1;
    unsigned int mixer, bem;

    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return -1;
    mixer = *(volatile unsigned int *)(uintptr_t)ME_SINGLETON;
    if (!mixer)
        return -1;
    bem = *(volatile unsigned int *)(uintptr_t)(mixer + FX_BEM_IN_MIXER);
    if (!bem)
        return -1;
    return *(volatile int *)(uintptr_t)(bem + FX_BEM_OFF_CH);
}

/* ------------------------------------------- the Beat FX's own VALUES (fxpad)
 *
 * The chase one link further than fx_ch_read(): past BeatEffectManager's select channel
 * and into the CURRENT BeatEffect, which is where the level, the rung and the on/off
 * actually live. rbp_abi.h carries the whole disassembly and the values this was checked
 * against on the unit; what matters here is the SHAPE, which is fx_ch_read()'s: a plain
 * .bss word for the singleton, then three interior pointers, EACH NULL-CHECKED, so a
 * half-built or torn-down engine answers 0 rather than faulting.
 *
 * THE TYPE WORD IS READ TOO, AND NOT AS DECORATION. `BeatEffect+0x3c` reads 1 on an
 * effect that is switched off -- rbp then has a `BeatEffectOff` at +0x08, a class whose
 * ON/OFF, level and time virtuals are all `bx lr`, so nothing maintains its flag. Reading
 * the flag alone made the pad believe a dead effect was live and send no toggle at all,
 * which is the operator's "when i switch an effect and then press the x/y pad it doesn't
 * engage" of 2026-10-07. `BEM_OFF_TYPE` is rbp's own `getBeatEffectType()`, type 0 IS the
 * off state (measured: one step of the effect selector while the effect is off leaves the
 * type at 0 and flips +0x3c to 1), and fxpad_zone.c's `live_on()` owns the rule that joins
 * the two words -- this function's job is only to hand both of them over, unchanged.
 *
 * A REFUSAL IS NOT A NUISANCE, IT IS THE PAD'S SAFETY PROPERTY. This read is what the
 * momentary pad restores TO: a pad that cannot read the old level would put the effect
 * back at a number it invented, on the operator's own track, mid-set. So a missing link
 * refuses the whole read, and the caller refuses to engage.
 *
 * A REFUSAL NAMES THE LINK THAT FAILED. There was no line here at all until the operator
 * reported a pad that did nothing, and the absence of a line is the worst possible
 * evidence: "it never engaged", "it engaged and sent nothing" and "the shim was not
 * running" all looked identical. The caller logs `why` on the way past. */
static int fx_float_to_depth(float f)
{
    int d = (int)(f * (float)BFX_DEPTH_FULL + 0.5f);

    if (d < 0)
        d = 0;
    if (d > BFX_DEPTH_FULL)
        d = BFX_DEPTH_FULL;
    return d;
}

static int fx_state_read_why(struct fxpad_live *out, const char **why)
{
    static int am_rbp = -1;
    unsigned int mixer, bem, be;

    if (why)
        *why = "not asked";
    if (!out) {
        if (why) *why = "no out buffer";
        return 0;
    }
    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp) {
        if (why) *why = "this process is not rbp";
        return 0;
    }
    mixer = *(volatile unsigned int *)(uintptr_t)ME_SINGLETON;
    if (!mixer) {
        if (why) *why = "MixerEngine singleton is null";
        return 0;
    }
    bem = *(volatile unsigned int *)(uintptr_t)(mixer + BEM_IN_MIXER);
    if (!bem) {
        if (why) *why = "MixerEngine+0x58 (BeatEffectManager) is null";
        return 0;
    }
    be = *(volatile unsigned int *)(uintptr_t)(bem + BEM_OFF_EFFECT);
    if (!be) {
        if (why) *why = "BeatEffectManager+0x08 (the current effect) is null";
        return 0;
    }

    out->type = *(volatile int *)(uintptr_t)(bem + BEM_OFF_TYPE);
    out->on = *(volatile unsigned char *)(uintptr_t)(be + BE_OFF_ON) ? 1 : 0;
    out->depth = fx_float_to_depth(*(volatile float *)(uintptr_t)(be + BE_OFF_DEPTH));
    out->beat = *(volatile int *)(uintptr_t)(be + BE_OFF_BEAT);
    out->beat_min = *(volatile int *)(uintptr_t)(be + BE_OFF_BEAT_MIN);
    out->beat_max = *(volatile int *)(uintptr_t)(be + BE_OFF_BEAT_MAX);
    if (why)
        *why = 0;
    return 1;
}

static int fx_state_read(struct fxpad_live *out)
{
    return fx_state_read_why(out, 0);
}

/* The two value sends the pad drives, each the SAME call the controller map already
 * makes -- so nothing here is a wire form invented for the shim:
 *
 *   K_DEPTH    OP_VALUE, an absolute 10-bit value plus the normalised float. Exactly
 *              what add_abs(CH_FXA, CC_FX_DEPTH, K_DEPTH, CH_GLOBAL) puts on the wire
 *              when the FLX4's LEVEL/DEPTH knob turns (map_flx4.c:628).
 *   K_BEAT*    OP_PRESS ONLY, with the delta in BOTH the param and the l argument,
 *              exactly map_flx4.c:535. The op is not a style choice: rbp's asEventCode
 *              gates 0x4490/0x4491 on (op & 0xf) == 0, so a RELEASE would be dropped
 *              and a level that climbed one way could never climb back.
 *
 * Back to back and with NO SLEEP, like every other send in this file: a wait here would
 * starve the one input loop the band, the window, the drawers and all three boxes share
 * (docs/07-touch.md's TRAP 2). */
static void fx_depth_send(int v)
{
    send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL, (long)v,
                  (float)v / (float)BFX_DEPTH_FULL);
}

static void fx_beat_step_send(int d)
{
    send_rx_key_fl(d > 0 ? K_BEATNEXT : K_BEATPREV, OP_PRESS, CH_GLOBAL,
                   (long)d, 0.0f, (long)d);
}

/* deck 1 -> deck 2 -> MASTER -> deck 1, from whatever rbp says it is right now. The step
 * is 0 -> 1 -> 5 -> 0, the order the operator asked for and the order rbp's own values
 * run in. Anything rbp answers that is none of the three -- it cannot today, but another
 * build could -- steps to deck 1 rather than inventing a value rbp has no meaning for. */
static void fx_ch_cycle(void){
    int cur = fx_ch_read();
    int next;

    if (cur < 0) {
        pointsrc_log("pointsrc: beat fx ch unreadable -- starting the cycle at deck 1");
        cur = 0;
    }
    next = (cur == 0) ? 1 : (cur == 1) ? BFX_CH_MASTER : 0;
    fx_ch_send(next);
    pointsrc_log("pointsrc: beat fx ch select %d -> %s", cur,
                 next == 0 ? "deck 1" : next == 1 ? "deck 2" : "MASTER");
}

/* Is rbp on the performance screen, where these three controls are drawn? The gate
 * wave_zone.c already uses -- rbp_abi.h's BROWSE_MODE_PLAY, measured 1 with the
 * performance screen on the glass and 5 with INFO over it -- and it is re-read on EVERY
 * report because rbp can leave the screen under a press that is already down. */
static int rbp_perf_screen(void)
{
    static int am_rbp = -1;

    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return 0;
    return rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE) == BROWSE_MODE_PLAY;
}

/* Which page does the pad strip SHOW: 0 the HOT CUE grid (STATUS), 1 the BEAT FX BANK
 * + X-PAD, -1 unknown. Read off the scanout, because what the finger lands on is what
 * is drawn there and nothing in rbp's memory tracks that reliably: a GUI window's
 * page word (s_WinscapeWorkMemory, +0x58) agreed with the screen for 90 s and then,
 * on 2026-10-13, read STATUS under a drawn BEAT FX page while two STATUS | BEAT FX
 * taps flipped the glass -- and the HOT CUE row fired cue B for two taps on ECHO.
 * IsBeatFxSelected's word lags the strip as well (rbp_abi.h's STRIP_* block).
 *
 * The test is the strip's caption row, y 500..501: under the BEAT FX page rbp fills
 * it with its grey hatch (alternating 0x21/0x31), under the pads it is black. The
 * eight points were chosen from 245 captures (109 BEAT FX, 136 pads, every pad mode,
 * lit and unlit cells, both decks) as pixels whose whole 3x3 neighbourhood was hatch
 * in every BEAT FX frame and black in every pads frame -- the neighbourhood is what
 * keeps the answer when the picture is scaled. 6 of 8 decide; anything else
 * (another screen, a frame mid-redraw, no scanout) is -1, which the callers read as
 * "do nothing": the HOT CUE row stands down, the y band is not moved. */
static const short strip_probe[8][2] = {
    { 102, 500 }, { 230, 501 }, { 390, 501 }, { 550, 501 },
    { 670, 501 }, { 830, 501 }, { 1007, 501 }, { 1150, 501 },
};

/* The STATUS | BEAT FX toggle's two halves, as drawn: the selected half is filled
 * grey 0x7b7d7b, the other black. With the KEY/STEMS core active (librx3_core) the
 * strip has two more pages, KEY and STEMS, on the same grey hatch as BEAT FX, and the
 * toggle then shows NEITHER half lit -- so the hatch alone does not mean BEAT FX
 * (2026-10-13: the shim took taps on KEY- / 0 / KEY+ for bank slots). */
static const short toggle_status_probe[2][2] = { { 1140, 445 }, { 1140, 470 } };
static const short toggle_bfx_probe[2][2]    = { { 1225, 445 }, { 1225, 470 } };

/* The scanout pixel under a logical point, as r,g,b; -1 when it cannot be read. */
static int scan_px(const unsigned char *pix, int w, int h, int pitch, int lx, int ly,
                   int *r, int *g, int *b)
{
    /* The picture is 1280x800 logical, fitted by height and centred: 1:1 with
     * KMS_MODE=1280x800, 1728x1080 at x 96 on the scaled 1920x1080 route. */
    long pw = (long)POINT_LOGICAL_W * h / POINT_LOGICAL_H;
    long ox = (w - pw) / 2;
    long sx = ox + (long)lx * pw / POINT_LOGICAL_W;
    long sy = (long)ly * h / POINT_LOGICAL_H;
    const unsigned char *q;

    if (sx < 0 || sx >= w || sy < 0 || sy >= h)
        return -1;
    q = pix + sy * pitch + sx * 4;              /* XRGB8888: B G R X */
    *b = q[0];
    *g = q[1];
    *r = q[2];
    return 0;
}

static int toggle_lit(const unsigned char *pix, int w, int h, int pitch,
                      const short pr[2][2])
{
    int i, r, g, b;

    for (i = 0; i < 2; i++)
        if (scan_px(pix, w, h, pitch, pr[i][0], pr[i][1], &r, &g, &b) < 0 ||
            r < 0x68 || r > 0x90 || g < 0x68 || g > 0x90 || b < 0x68 || b > 0x90)
            return 0;
    return 1;
}

static int strip_page(void)
{
    const unsigned char *pix;
    const void *base;
    int w, h, pitch, i, hatch = 0, black = 0, st, bf;

    if (rx3_kms_peek(&base, &w, &h, &pitch) < 0 || w <= 0 || h <= 0)
        return -1;
    pix = base;
    for (i = 0; i < 8; i++) {
        int r, g, b;

        if (scan_px(pix, w, h, pitch, strip_probe[i][0], strip_probe[i][1],
                    &r, &g, &b) < 0)
            return -1;
        if (r >= 0x18 && r <= 0x38 && g >= 0x18 && g <= 0x38 && b >= 0x18 && b <= 0x38)
            hatch++;
        else if (r < 8 && g < 8 && b < 8)
            black++;
    }
    st = toggle_lit(pix, w, h, pitch, toggle_status_probe);
    bf = toggle_lit(pix, w, h, pitch, toggle_bfx_probe);
    /* Both terms, so neither a KEY/STEMS page (hatch, no half lit) nor a half-drawn
     * frame is ever read as one of the two pages this file has rules for. */
    if (hatch >= 6 && bf && !st)
        return 1;
    if (black >= 6 && st && !bf)
        return 0;
    return -1;
}

static int strip_shows_pads(void)
{
    return strip_page() == 0;
}

/* --- THE DDJ-400'S STEMS PAGE, as taps on the rx3-toolkit's own STEMS panel ------
 *
 * With RX3_CORE=toolkit the KEY/STEMS core is the rx3-toolkit's, and it takes no
 * controller command: its stems answer SLIP LOOP pads 5-7 (which put the deck in
 * SLIP LOOP, and a pad-mode key closes its panel) or a finger on its STEMS tab and
 * controls. The operator's ask (2026-10-14): SHIFT+BEAT JUMP opens STEMS on the
 * screen and pads 1-3 toggle DRUMS/VOCAL/INST there, as on the legacy core. So the
 * bridge (RX3_STEMS_PADS=touch) writes a command to a FIFO and this thread acts on
 * it: the page buttons tap the panel's tab where a finger would, the pads change
 * the toolkit's state directly (stems_command, below).
 *
 * Tab geometry is the toolkit's (rx3_core_hook.c performance_tab_touch): KEY is
 * the left tab, x 1090..1179, STEMS the right, x 1181..1270, y 384..431, drawn grey
 * 0x7b7d7b when selected (a tap on a selected tab closes it, so it is tapped only
 * when not lit). */
#define STEMS_FIFO          "/tmp/rx3-stems-touch.fifo"
#define STEMS_CMD_MAGIC     0x53434d44u     /* the bridge's stems_pad_command_packet */
#define STEMS_CMD_OPEN      0x20
#define KEY_CMD_OPEN        0x21
#define KEY_CMD_PART        0x30            /* 0x30 -1, 0x31 own key, 0x32 +1 */
#define HOTCUE_DELETE_BASE  0x40            /* 0x40..0x47 = SHIFT+HOTCUE pad 1..8 */
#define STEMS_TAB_X         1225
#define STEMS_TAB_Y         407

struct stems_cmd { uint32_t magic, sequence; uint8_t deck, control; uint16_t reserved; };

static const short stems_tab_probe[2][2] = { { 1186, 392 }, { 1265, 424 } };
static const short key_tab_probe[2][2]   = { { 1095, 392 }, { 1170, 424 } };

static int tab_lit(const short probe[2][2])
{
    const void *base;
    int w, h, pitch;

    if (rx3_kms_peek(&base, &w, &h, &pitch) < 0 || w <= 0 || h <= 0)
        return 0;
    return toggle_lit(base, w, h, pitch, probe);
}

static void stems_tap(int x, int y)
{
    int i;

    /* never through a finger that is on the glass: wait it out, up to 0.5 s */
    for (i = 0; i < 25 && st_down; i++)
        usleep(20000);
    if (st_down)
        return;
    tscfake_emit(1, x, y);
    usleep(40000);
    tscfake_emit(0, x, y);
}

/* Open the toolkit panel behind tab (x, y) unless it is already the selected one:
 * a tap on a selected tab closes it. KEY is the left tab, STEMS the right. */
static int open_panel(int x, int y, const short probe[2][2])
{
    int i;

    if (!rbp_perf_screen())
        return 0;
    if (tab_lit(probe))
        return 1;
    stems_tap(x, y);
    for (i = 0; i < 25; i++) {
        usleep(20000);
        if (tab_lit(probe))
            return 1;
    }
    return 0;
}

static int stems_open_panel(void)
{
    return open_panel(STEMS_TAB_X, STEMS_TAB_Y, stems_tab_probe);
}

/* KEY (SHIFT+SAMPLER on the DDJ-400, 2026-10-14): its pads are -1 / the track's own
 * key / +1, the toolkit's stepper parts 0/1/2 (keyshift_fire). */
#define KEY_TAB_X           1135
#define KEY_TAB_Y           407

/* THE OTHER DIRECTION: the pads' LEDs. ddj400-bridge already lights PAD FX 2 pads
 * 1-3 from /tmp/rx3-stems-pad-state.bin (stems_pad_state_tick: magic "SMTS", a
 * sequence, active[deck] bit n = pad n+1 lit, armed[deck]) -- the file the legacy
 * core wrote. Here it is written from the toolkit core's own state, so a tap on the
 * glass, a pad, or a new track all reach the LEDs. The toolkit's role bits are
 * already the pads' order: 1 INST, 2 VOCAL, 4 DRUMS (rx3_stems_panel.h).
 *
 * Where: its `stems_decks[2]` (rx3_stems_decl.h), found by name in the loaded
 * librx3_core.so's own .symtab (the build is not stripped), so a rebuilt core moves
 * nothing here. Per deck: +0 reader, +4 status (2 = ready), +8 selection (available
 * roles in bits 4..6), +12 levels (7 bits per role, 0 = off), +20 armed. The symbol
 * must be 2 x 0x4e0 bytes, the layout this was written against, or nothing is read. */
#define STEMS_STATE_FILE   "/tmp/rx3-stems-pad-state.bin"
#define STEMS_STATE_MAGIC  0x53544d53u
#define STEMS_DECKS_SIZE   0x9c0u

struct stems_state_pkt { uint32_t magic, sequence; uint8_t active[2], armed[2]; };

static const unsigned char *stems_decks_addr;
static int stems_state_fd = -1;

/* KEY's LEDs, the same way: the toolkit's `keyshift_decks[2]` (rx3_keyshift_decl.h,
 * 0x50 bytes each, `request` at +0: semitones = (request & 0xff) - 12). The file
 * /tmp/rx3-key-pad-state.bin is { "KEYS", sequence, leds[2] }, leds bit n = KEY
 * SHIFT pad n+1: pad 1 lit below the track's key, pad 2 lit AT it, pad 3 above. */
#define KEY_STATE_FILE     "/tmp/rx3-key-pad-state.bin"
#define KEY_STATE_MAGIC    0x5359454bu
#define KEY_DECKS_SIZE     0xa0u

struct key_state_pkt { uint32_t magic, sequence; uint8_t leds[2], reserved[2]; };

static const unsigned char *key_decks_addr;
static void (*key_fire_fn)(unsigned int, unsigned int, unsigned int);
/* The toolkit's `performance_refresh_pending`: its touch path sets it after every
 * change (ui/rx3_pad_widgets.h) and its renderer repaints the open panel. Its
 * watcher sets it for other changes only once `performance_window_ready`, which on
 * this unit never comes up (measured 2026-10-14: 0, the "native performance
 * window" line never logged), so a pad change played but the panel kept the old
 * state. Raised here whenever the shim sees the stems or key state change. */
static unsigned int *panel_refresh_flag;

static void panel_refresh(void)
{
    if (panel_refresh_flag)
        __atomic_store_n(panel_refresh_flag, 1u, __ATOMIC_SEQ_CST);
}
static int key_state_fd = -1;

struct stems_find { const char *path; unsigned long base; };

static int stems_find_core(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct stems_find *f = arg;

    (void)size;
    if (info->dlpi_name && strstr(info->dlpi_name, "librx3_core.so")) {
        f->path = info->dlpi_name;
        f->base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static unsigned long stems_symbol(const char *path, const char *name, unsigned long *size)
{
    Elf32_Ehdr eh;
    Elf32_Shdr sh, link;
    unsigned long found = 0;
    int fd = real_open(path, O_RDONLY, 0), i;

    if (fd < 0)
        return 0;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS32)
        goto out;
    for (i = 0; i < eh.e_shnum && !found; i++) {
        Elf32_Sym sym;
        unsigned int k;
        char nm[32];
        size_t len = strlen(name) + 1;

        if (pread(fd, &sh, sizeof sh, eh.e_shoff + (off_t)i * eh.e_shentsize) != (ssize_t)sizeof sh ||
            sh.sh_type != SHT_SYMTAB ||
            pread(fd, &link, sizeof link, eh.e_shoff + (off_t)sh.sh_link * eh.e_shentsize) != (ssize_t)sizeof link)
            continue;
        for (k = 0; k < sh.sh_size / sizeof sym; k++) {
            if (pread(fd, &sym, sizeof sym, sh.sh_offset + (off_t)k * sizeof sym) != (ssize_t)sizeof sym)
                break;
            if (len > sizeof nm ||
                pread(fd, nm, len, link.sh_offset + sym.st_name) != (ssize_t)len ||
                memcmp(nm, name, len) != 0)
                continue;
            found = sym.st_value;
            *size = sym.st_size;
            break;
        }
    }
out:
    real_close(fd);
    return found;
}

static void stems_state_init(void)
{
    struct stems_find f = { NULL, 0 };
    unsigned long off, size = 0;

    dl_iterate_phdr(stems_find_core, &f);
    if (!f.path) {
        pointsrc_log("stems: LEDs off: librx3_core.so not loaded\n");
        return;
    }
    size = 0;
    {
        unsigned long koff = stems_symbol(f.path, "keyshift_decks", &size);

        if (koff && size == KEY_DECKS_SIZE) {
            key_decks_addr = (const unsigned char *)(f.base + koff);
            key_state_fd = real_open(KEY_STATE_FILE, O_RDWR | O_CREAT, 0644);
            pointsrc_log("key: LEDs from keyshift_decks @%p -> %s\n",
                         (const void *)key_decks_addr, KEY_STATE_FILE);
        } else {
            pointsrc_log("key: LEDs off: keyshift_decks %s (size %lu)\n",
                         koff ? "has an unknown layout" : "not found", size);
        }
    }
    {
        unsigned long fsz = 0, foff = stems_symbol(f.path, "keyshift_fire", &fsz);

        if (foff && !(foff & 1u)) {
            key_fire_fn = (void (*)(unsigned int, unsigned int, unsigned int))(f.base + foff);
            pointsrc_log("key: keyshift_fire @%p\n", (void *)key_fire_fn);
        }
    }
    {
        unsigned long rsz = 0, roff = stems_symbol(f.path, "performance_refresh_pending", &rsz);

        if (roff && rsz == 4) {
            panel_refresh_flag = (unsigned int *)(f.base + roff);
            pointsrc_log("stems: panel refresh flag @%p\n", (void *)panel_refresh_flag);
        }
    }
    off = stems_symbol(f.path, "stems_decks", &size);
    if (!off || size != STEMS_DECKS_SIZE) {
        pointsrc_log("stems: LEDs off: stems_decks %s in %s (size %lu)\n",
                     off ? "has an unknown layout" : "not found", f.path, size);
        return;
    }
    stems_decks_addr = (const unsigned char *)(f.base + off);

    /* SLIP LOOP PADS 5-7 STAY NATIVE (operator, 2026-10-14). The toolkit also maps
     * SLIP LOOP pads 5/6/7 to INST/VOCAL/DRUMS (its stems_pad on the onKey_Pad hook,
     * the hook's only client) and recolours their LEDs (slip_light); on the DDJ-400
     * SAMPLER is SLIP LOOP, whose pads are this project's SLIP LOOP / RELEASE FX. Stems
     * live on SHIFT+BEAT JUMP here, so that client is retired: pad_client_count 0 is
     * the hook's own "no client" path (rx3_input.c pad_hooked -> the native onKey_Pad)
     * and slip_light 0 its "native LEDs" path. Only when the one client is stems'. */
    {
        unsigned long csz = 0, coff = stems_symbol(f.path, "pad_client_count", &csz);
        unsigned long lsz = 0, loff = stems_symbol(f.path, "slip_light", &lsz);
        unsigned long tsz = 0, toff = stems_symbol(f.path, "pad_clients", &tsz);

        if (coff && csz == 4 && toff && tsz >= 12 && loff && lsz == 4 &&
            __atomic_load_n((uint32_t *)(f.base + coff), __ATOMIC_SEQ_CST) == 1u &&
            *(uint32_t *)(f.base + toff) == (uint32_t)(f.base + off)) {
            __atomic_store_n((uint32_t *)(f.base + coff), 0u, __ATOMIC_SEQ_CST);
            __atomic_store_n((uint32_t *)(f.base + loff), 0u, __ATOMIC_SEQ_CST);
            pointsrc_log("stems: SLIP LOOP pads 5-7 back to native (toolkit pad client retired)\n");
        } else {
            pointsrc_log("stems: SLIP LOOP pad client left as is (not the expected single stems client)\n");
        }
    }
    /* the existing file, written in place: the bridge holds it open */
    stems_state_fd = real_open(STEMS_STATE_FILE, O_RDWR | O_CREAT, 0644);
    pointsrc_log("stems: LEDs from %s stems_decks @%p -> %s\n", f.path,
                 (const void *)stems_decks_addr, STEMS_STATE_FILE);
}

static void key_state_tick(void)
{
    /* Gate on the actual semitone, not just its 3-state LED bucket (below /
     * at / above the track's key): repeated presses in the same direction
     * (-1, -2, -3, ...) stay in the same bucket, so comparing buckets alone
     * dropped every repaint and LED refresh past the first step even though
     * the toolkit's real pitch (and the audio) kept changing. Confirmed live
     * 2026-10-14 by reading keyshift_decks' raw request next to this file. */
    static int last_semi[2] = { 999, 999 };
    static uint32_t seq;
    struct key_state_pkt now;
    int d, changed = 0;

    if (!key_decks_addr || key_state_fd < 0)
        return;
    memset(&now, 0, sizeof now);
    for (d = 0; d < 2; d++) {
        uint32_t req = __atomic_load_n((const uint32_t *)(key_decks_addr + d * (KEY_DECKS_SIZE / 2)),
                                       __ATOMIC_SEQ_CST);
        int semi = (int)(req & 0xffu) - 12;

        now.leds[d] = semi < 0 ? 1 : semi == 0 ? 2 : 4;
        if (semi != last_semi[d])
            changed = 1;
        last_semi[d] = semi;
    }
    if (seq && !changed)
        return;
    panel_refresh();
    now.magic = KEY_STATE_MAGIC;
    now.sequence = ++seq;
    (void)pwrite(key_state_fd, &now, sizeof now, 0);
}

/* BEAT FX ON/OFF, the real state (2026-10-15): the DDJ-400's own ON/OFF button
 * LED, driven by ddj400-bridge.c, used to just flip a local shadow on every
 * press, so a touchscreen toggle of the same control (fx_zone.c's FX_ACT_POWER,
 * which sends K_BFX straight to rbp and keeps no state of its own) left the
 * physical LED wrong and the bridge's next press toggling the wrong way. This
 * is independent of RX3_STEMS_PADS: Beat FX is global, not a STEMS/KEY touch
 * concern, so it gets its own small always-on thread rather than riding
 * stems_thread's conditional one. */
#define BFX_STATE_FILE   "/tmp/rx3-bfx-state.bin"
#define BFX_STATE_MAGIC  0x53584642u   /* "BFXS" */

struct bfx_state_pkt { uint32_t magic, sequence; uint8_t on, reserved[3]; };

static int bfx_state_fd = -1;

static void bfx_state_tick(void)
{
    static int last_on = -1;
    static uint32_t seq;
    struct bfx_state_pkt now;
    int on;

    if (bfx_state_fd < 0)
        return;
    on = ((int (*)(void))UI_GET_MIX_BFX_ON)() ? 1 : 0;
    if (seq && on == last_on)
        return;
    last_on = on;
    memset(&now, 0, sizeof now);
    now.magic = BFX_STATE_MAGIC;
    now.sequence = ++seq;
    now.on = (uint8_t)on;
    (void)pwrite(bfx_state_fd, &now, sizeof now, 0);
}

/* MASTER REC (item 7, 2026-10-15): djengine::DjEngineIF::getRecordingStatus()/
 * startRecording()/stopRecording(), all `this`-only member functions on the
 * same DJ_ENGINE_GLOBAL singleton two other native calls already use (see
 * rbp_abi.h). Two halves, same split as BFX above and deliberately so:
 *
 *   - master_rec_state_tick() only ever READS getRecordingStatus() and
 *     mirrors it to a state file, every bfx_thread tick (50ms), same shape
 *     as bfx_state_tick(). Zero risk, and it is what got exercised live
 *     FIRST, before either native call that writes a file to the USB stick
 *     was ever touched, specifically to confirm the DJ_ENGINE_GLOBAL pointer
 *     and the calling convention off a read before trusting them for a
 *     write. It keeps running regardless of the toggle below.
 *   - master_rec_toggle_tick() polls MASTER_REC_TOGGLE_REQUEST and, on a
 *     hit, reads the status and calls start or stop accordingly -- the only
 *     place this file calls startRecording()/stopRecording(). A toggle
 *     rather than separate start/stop requests because every caller (the
 *     DDJ-400's held-SHIFT gesture, a touchscreen button) wants "flip it"
 *     and none has an independent reason to know which state it's already
 *     in. */
#define MASTER_REC_STATE_FILE  "/tmp/rx3-master-rec-state.bin"
#define MASTER_REC_STATE_MAGIC 0x43455252u   /* "RREC" */
#define MASTER_REC_TOGGLE_REQUEST "/tmp/rx3-master-rec-toggle.request"

struct master_rec_state_pkt { uint32_t magic, sequence; uint8_t recording, reserved[3]; };

static int master_rec_state_fd = -1;

static int master_rec_status(void)
{
    void *engine = *(void *volatile *)DJ_ENGINE_GLOBAL;

    if (!engine)
        return 0;
    return ((int (*)(void *))DJ_ENGINE_GET_REC_STATUS)(engine) ? 1 : 0;
}

static void master_rec_state_tick(void)
{
    static int last_recording = -1;
    static uint32_t seq;
    struct master_rec_state_pkt now;
    int recording;

    if (master_rec_state_fd < 0)
        return;
    recording = master_rec_status();
    if (seq && recording == last_recording)
        return;
    last_recording = recording;
    memset(&now, 0, sizeof now);
    now.magic = MASTER_REC_STATE_MAGIC;
    now.sequence = ++seq;
    now.recording = (uint8_t)recording;
    (void)pwrite(master_rec_state_fd, &now, sizeof now, 0);
    pointsrc_log("master rec: status -> %s\n", recording ? "recording" : "idle");
}

/* First USB slot's mount point as seen from inside rbp's own chroot (matches
 * the host's work/runtime/rx3/media/usb1/sda1). rbp normally learns this
 * itself from ui::UsbStorageManager::notify_db_attach() when a drive mounts
 * and calls prepareRecording() with it automatically; that path apparently
 * never fires here (see DJ_ENGINE_PREPARE_RECORDING in rbp_abi.h), so we
 * supply it ourselves. Single slot only -- not a general USB-selection
 * feature. */
#define MASTER_REC_USB_PATH "/media/usb1/sda1"

static void master_rec_toggle_tick(void)
{
    void *engine;
    int probe, recording;

    probe = real_open(MASTER_REC_TOGGLE_REQUEST, O_RDONLY, 0);
    if (probe < 0)
        return;
    real_close(probe);
    unlink(MASTER_REC_TOGGLE_REQUEST);
    engine = *(void *volatile *)DJ_ENGINE_GLOBAL;
    if (!engine) {
        pointsrc_log("master rec: toggle requested but engine not up yet\n");
        return;
    }
    recording = master_rec_status();
    if (recording) {
        ((void (*)(void *))DJ_ENGINE_STOP_RECORDING)(engine);
        pointsrc_log("master rec: stopRecording() called\n");
    } else {
        ((void (*)(void *, const char *, int))DJ_ENGINE_PREPARE_RECORDING)(
            engine, MASTER_REC_USB_PATH, 0);
        pointsrc_log("master rec: prepareRecording(\"%s\", false) called\n",
                      MASTER_REC_USB_PATH);
        ((void (*)(void *))DJ_ENGINE_START_RECORDING)(engine);
        pointsrc_log("master rec: startRecording() called\n");
    }
}

/* One-off screenshot, for measuring where to draw a new control against a
 * live screen (2026-10-15, the CROSSFADER CURVE row): touch
 * /tmp/rx3-screenshot.request and the next tick dumps the live scanout
 * (rx3_kms_peek(), the same buffer strip_page() reads) to a 24-bit BMP,
 * top-down so no row-flip is needed, and removes the request file. Not a
 * permanent feature -- a debug aid, left in rather than stripped out so the
 * next measurement does not need this plumbing rebuilt. */
#define SCREENSHOT_REQUEST "/tmp/rx3-screenshot.request"
#define SCREENSHOT_FILE    "/tmp/rx3-screenshot.bmp"

static void screenshot_tick(void)
{
    const void *base;
    int w, h, pitch, fd, y, probe;
    unsigned char filehdr[14], infohdr[40];
    uint32_t rowbytes, filesize, dataoff;

    /* real_open()+close() rather than stat(): this chroot's libc has no
     * plain stat (glibc213-compat.h's fstat note applies here too). */
    probe = real_open(SCREENSHOT_REQUEST, O_RDONLY, 0);
    if (probe < 0)
        return;
    real_close(probe);
    if (rx3_kms_peek(&base, &w, &h, &pitch) < 0 || w <= 0 || h <= 0) {
        pointsrc_log("screenshot: no live scanout\n");
        unlink(SCREENSHOT_REQUEST);
        return;
    }
    rowbytes = ((unsigned int)w * 3u + 3u) & ~3u;
    dataoff = 14u + 40u;
    filesize = dataoff + rowbytes * (unsigned int)h;
    memset(filehdr, 0, sizeof filehdr);
    filehdr[0] = 'B'; filehdr[1] = 'M';
    memcpy(filehdr + 2, &filesize, 4);
    memcpy(filehdr + 10, &dataoff, 4);
    memset(infohdr, 0, sizeof infohdr);
    {
        uint32_t v;
        v = 40u; memcpy(infohdr + 0, &v, 4);
        v = (uint32_t)w; memcpy(infohdr + 4, &v, 4);
        v = (uint32_t)(-h); memcpy(infohdr + 8, &v, 4);   /* negative: top-down */
        infohdr[12] = 1; infohdr[13] = 0;                 /* planes = 1 */
        infohdr[14] = 24; infohdr[15] = 0;                /* bitcount = 24 */
    }
    fd = real_open(SCREENSHOT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pointsrc_log("screenshot: open %s: %s\n", SCREENSHOT_FILE, strerror(errno));
        unlink(SCREENSHOT_REQUEST);
        return;
    }
    (void)write(fd, filehdr, sizeof filehdr);
    (void)write(fd, infohdr, sizeof infohdr);
    {
        unsigned char *row = malloc(rowbytes);

        if (row) {
            for (y = 0; y < h; y++) {
                const unsigned char *src = (const unsigned char *)base + (size_t)y * (size_t)pitch;
                int x;

                memset(row, 0, rowbytes);
                for (x = 0; x < w; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];   /* B */
                    row[x * 3 + 1] = src[x * 4 + 1];   /* G */
                    row[x * 3 + 2] = src[x * 4 + 2];   /* R */
                }
                (void)write(fd, row, rowbytes);
            }
            free(row);
        }
    }
    real_close(fd);
    unlink(SCREENSHOT_REQUEST);
    pointsrc_log("screenshot: %dx%d -> %s\n", w, h, SCREENSHOT_FILE);
}

/* Debug/test tap injection: write "<down> <x> <y>\n" (logical 1280x800, same
 * space as stems_tap()'s panel taps above) to TOUCH_INJECT_REQUEST and it is
 * fed into rbp's own touch stream via tscfake_emit() -- the same call this
 * file already uses for the STEMS/KEY panel's own synthetic taps, just driven
 * from outside the process instead of from a zone handler. Used to script a
 * tour of touch-only screens (UTILITY, BROWSE's sort tabs, ...) that have no
 * DDJ-400 key equivalent. One request = one down or up edge; a caller sends
 * a press as two requests ("1 x y" then "0 x y") a beat apart, exactly like a
 * real touch. */
#define TOUCH_INJECT_REQUEST "/tmp/rx3-touch.request"

static void touch_inject_tick(void)
{
    int fd, down, x, y;
    char buf[64];
    ssize_t n;

    fd = real_open(TOUCH_INJECT_REQUEST, O_RDONLY, 0);
    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    real_close(fd);
    unlink(TOUCH_INJECT_REQUEST);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (sscanf(buf, "%d %d %d", &down, &x, &y) != 3)
        return;
    if (x < 0) x = 0;
    if (x >= POINT_LOGICAL_W) x = POINT_LOGICAL_W - 1;
    if (y < 0) y = 0;
    if (y >= POINT_LOGICAL_H) y = POINT_LOGICAL_H - 1;
    tscfake_emit(down ? 1 : 0, x, y);
    pointsrc_log("touch inject: down=%d x=%d y=%d\n", down, x, y);
}

static void *bfx_thread(void *arg)
{
    (void)arg;
    bfx_state_fd = real_open(BFX_STATE_FILE, O_RDWR | O_CREAT, 0644);
    if (bfx_state_fd < 0) {
        pointsrc_log("bfx: open %s: %s\n", BFX_STATE_FILE, strerror(errno));
        return NULL;
    }
    pointsrc_log("bfx: on/off state -> %s\n", BFX_STATE_FILE);
    master_rec_state_fd = real_open(MASTER_REC_STATE_FILE, O_RDWR | O_CREAT, 0644);
    if (master_rec_state_fd < 0)
        pointsrc_log("master rec: open %s: %s\n", MASTER_REC_STATE_FILE, strerror(errno));
    else
        pointsrc_log("master rec: status -> %s, toggle <- %s\n",
                      MASTER_REC_STATE_FILE, MASTER_REC_TOGGLE_REQUEST);
    for (;;) {
        bfx_state_tick();
        rx3_brightness_tick();
        screenshot_tick();
        touch_inject_tick();
        master_rec_state_tick();
        master_rec_toggle_tick();
        usleep(50000);
    }
    return NULL;
}

static void stems_state_tick(void)
{
    key_state_tick();
    static struct stems_state_pkt last;
    static uint32_t seq;
    struct stems_state_pkt now;
    int d;

    if (!stems_decks_addr || stems_state_fd < 0)
        return;
    memset(&now, 0, sizeof now);
    for (d = 0; d < 2; d++) {
        const unsigned char *c = stems_decks_addr + d * (STEMS_DECKS_SIZE / 2);
        uint32_t reader = __atomic_load_n((const uint32_t *)(c + 0), __ATOMIC_SEQ_CST);
        uint32_t status = __atomic_load_n((const uint32_t *)(c + 4), __ATOMIC_SEQ_CST);
        uint32_t sel    = __atomic_load_n((const uint32_t *)(c + 8), __ATOMIC_SEQ_CST);
        uint32_t levels = __atomic_load_n((const uint32_t *)(c + 12), __ATOMIC_SEQ_CST);
        int32_t  armed  = __atomic_load_n((const int32_t *)(c + 20), __ATOMIC_SEQ_CST);
        unsigned int avail = (sel >> 4) & 7u, on = 0, i;

        for (i = 0; i < 3; i++)
            if ((levels >> (i * 7u)) & 127u)
                on |= 1u << i;
        now.armed[d] = status == 2u && reader && armed;
        now.active[d] = (uint8_t)(on & avail);
    }
    if (seq && !memcmp(now.active, last.active, 2) && !memcmp(now.armed, last.armed, 2))
        return;
    panel_refresh();
    last = now;
    now.magic = STEMS_STATE_MAGIC;
    now.sequence = ++seq;
    (void)pwrite(stems_state_fd, &now, sizeof now, 0);
}

/* Pads act on the toolkit's state directly (2026-10-14): taps on the panel were late
 * under load, needed the panel open and the glass free, and the DRUMS-cell test read
 * a switched-off VOCAL as the gap, dropping DRUMS. A stem toggle is what the
 * toolkit's own stems_toggle() does, an atomic flip of the role's 7-bit level
 * between 0 and 100 in `levels` (rx3_stems_audio.h stems_change_levels), under the
 * gate its pad handler uses (status 2, reader, armed, role available). KEY calls the
 * toolkit's own keyshift_fire(deck, 0, part), the stepper's touch action, found by
 * name like stems_decks. The tabs are still tapped, only on the page buttons. */
static int stems_toggle_direct(int deck, unsigned int bit)
{
    unsigned char *c;
    uint32_t *levels, old, next;
    unsigned int i;

    if (!stems_decks_addr)
        return 0;
    c = (unsigned char *)stems_decks_addr + deck * (STEMS_DECKS_SIZE / 2);
    if (__atomic_load_n((uint32_t *)(c + 4), __ATOMIC_SEQ_CST) != 2u ||
        !__atomic_load_n((uint32_t *)(c + 0), __ATOMIC_SEQ_CST) ||
        !__atomic_load_n((int32_t *)(c + 20), __ATOMIC_SEQ_CST) ||
        !(((__atomic_load_n((uint32_t *)(c + 8), __ATOMIC_SEQ_CST) >> 4) & 7u) & bit))
        return 0;
    levels = (uint32_t *)(c + 12);
    old = __atomic_load_n(levels, __ATOMIC_SEQ_CST);
    for (;;) {
        next = old;
        for (i = 0; i < 3; i++)
            if (bit & (1u << i)) {
                unsigned int sh = i * 7u;
                next = (next & ~(127u << sh)) | ((((old >> sh) & 127u) ? 0u : 100u) << sh);
            }
        if (__atomic_compare_exchange_n(levels, &old, next, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 1;
    }
}

/* SHIFT+HOTCUE pad delete (2026-10-15): ported from this tree's own
 * rbp_bridge.c (knobshim.so, the FLX4-era controller, not loaded in the
 * DDJ-400 stack) where it already shipped and was verified live on the unit
 * 2026-10-01 (docs/13-raspberrypi4.md S9.11) -- "PASS... the cue is gone and
 * the cell stopped drawing". ddj400-bridge.c's own SHIFT+HOTCUE path used to
 * reach this through the legacy command-file protocol (stems_pad_command_path)
 * that only the legacy librx3_core.so ever read; since RX3_CORE=toolkit
 * (2026-10-14) nothing reads that file, so the gesture went nowhere. This is
 * native rbp functionality -- two direct calls into rbp's own binary, found by
 * the previous session and recorded in rbp_abi.h (ADDR_HOTCUE_DELETE,
 * ADDR_ENGINE_CLEAR_HOTCUE, ADDR_ENGINE_IS_REGHOTCUE) -- not toolkit code, so
 * nothing here needs the external-code permission gate. plinn_scan.c (the
 * PlayerInnards-candidate validator, already in this tree, unused by
 * fbshim.so until now) is shared with the beat-loop path's own use of plinn()
 * in rbp_bridge.c; this file gets its own copy of the scan and g_plinn because
 * the two shims are never loaded together. */
static void *g_plinn[2];

#define PVTABLE_PLAYERINNARDS 0x004d1960UL

static void scan_plinn(void)
{
    FILE *f;
    char line[256];
    char tally[256];
    struct plinn_tally t;
    static unsigned long long last;
    unsigned long long now;

    if (g_plinn[0] && g_plinn[1])
        return;
    now = shim_now_ms();
    if (now - last < 1000)
        return;
    last = now;
    memset(&t, 0, sizeof t);
    f = fopen("/proc/self/maps", "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s = 0, e = 0, a;
        char perms[8];

        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3)
            continue;
        if (perms[0] != 'r' || perms[1] != 'w')
            continue;
        if (e <= s || (e - s) > (512UL << 20))
            continue;
        t.regions++;
        for (a = (s + 3) & ~3UL; a + 0x90 <= e; a += 4) {
            struct plinn_cand c;
            enum plinn_refuse why;
            int d;

            t.words++;
            if (*(volatile unsigned int *)a != PVTABLE_PLAYERINNARDS)
                continue;
            t.hits++;
            c.chan = *(volatile unsigned char *)(a + PLAYERINNARDS_CHAN_OFF);
            c.mode = *(volatile unsigned char *)(a + PLAYERINNARDS_MODE_OFF);
            c.eng = *(volatile unsigned int *)(a + PLAYERINNARDS_ENGINE_OFF);
            d = plinn_classify(&c, &why);
            if (d < 0) {
                t.refused[why]++;
                continue;
            }
            t.accepted[d]++;
            if (!g_plinn[d]) {
                g_plinn[d] = (void *)a;
                pointsrc_log("hotcue: PlayerInnards deck%d @%p (+0x74=%d)\n",
                             d + 1, (void *)a, c.mode);
            }
        }
    }
    fclose(f);
    if (!g_plinn[0] || !g_plinn[1])
        pointsrc_log("hotcue: PlayerInnards scan: deck1=%p deck2=%p; %s\n",
                     g_plinn[0], g_plinn[1],
                     plinn_tally_line(tally, sizeof tally, &t));
}

static void *plinn(int deck)
{
    if (!g_plinn[deck])
        scan_plinn();
    return g_plinn[deck];
}

/* rbp's own player array, walked exactly as IUiObjManager::getPlayer() does
 * (rbp_abi.h's UIOBJ_* constants). UiObject::Channel is 1-based, so the deck
 * index is the array index. */
static void *ui_player(int deck)
{
    void *holder, *players;
    int n;

    holder = *(void **)UIOBJ_HOLDER_GLOBAL;
    if (!holder)
        return NULL;
    players = *(void **)((char *)holder + UIOBJ_PLAYERS_OFF);
    n = *(int *)((char *)holder + UIOBJ_NPLAYERS_OFF);
    if (!players || n <= 0 || n > 8 || deck < 0 || deck >= n)
        return NULL;
    return *(void **)((char *)players + 4 * deck);
}

static void hotcue_delete(int deck, int pad)
{
    void *p, *inn, *eng = NULL;

    if (pad < 1 || pad > 8)
        return;
    p = ui_player(deck);
    if (!p) {
        pointsrc_log("hotcue: delete deck%d pad%d: no ui::Player yet\n",
                     deck + 1, pad);
        return;
    }
    ((void (*)(void *, int))ADDR_HOTCUE_DELETE)(p, pad);
    inn = plinn(deck);
    if (inn)
        eng = *(void **)((char *)inn + PLAYERINNARDS_ENGINE_OFF);
    if (eng) {
        int reg = ((int (*)(void *, int, int))ADDR_ENGINE_IS_REGHOTCUE)
                       (eng, deck, pad);
        ((void (*)(void *, int, int))ADDR_ENGINE_CLEAR_HOTCUE)(eng, deck, pad);
        pointsrc_log("hotcue: delete deck%d pad%d @%p inn=%p eng=%p reg=%d\n",
                     deck + 1, pad, p, inn, eng, reg);
    } else {
        pointsrc_log("hotcue: delete deck%d pad%d @%p: no engine yet -- the"
                     " cue is gone from rbp's data but the pad grid will keep"
                     " drawing it\n", deck + 1, pad, p);
    }
}

static void stems_command(const struct stems_cmd *c)
{
    int deck = c->deck;

    if (c->control == STEMS_CMD_OPEN) {
        pointsrc_log("stems: open panel -> %s\n", stems_open_panel() ? "open" : "not open");
        return;
    }
    if (c->control == KEY_CMD_OPEN) {
        pointsrc_log("key: open panel -> %s\n",
                     open_panel(KEY_TAB_X, KEY_TAB_Y, key_tab_probe) ? "open" : "not open");
        return;
    }
    if (deck < 0 || deck > 1)
        return;
    if (c->control >= HOTCUE_DELETE_BASE && c->control <= HOTCUE_DELETE_BASE + 7) {
        hotcue_delete(deck, (int)(c->control - HOTCUE_DELETE_BASE) + 1);
        return;
    }
    if (c->control >= KEY_CMD_PART && c->control <= KEY_CMD_PART + 2) {
        if (!key_fire_fn) {
            pointsrc_log("key: deck%d: keyshift_fire not found, dropped\n", deck + 1);
            return;
        }
        key_fire_fn((unsigned int)deck, 0u, (unsigned int)(c->control - KEY_CMD_PART));
        return;
    }
    /* controls in screen order: 0 INST (bit 1), 1 VOCAL (2), 2 DRUMS (4) */
    if (c->control <= 2 && !stems_toggle_direct(deck, 1u << c->control))
        pointsrc_log("stems: deck%d control %d: no such ready stem, ignored\n",
                     deck + 1, c->control);
}

static void *stems_thread(void *arg)
{
    struct stems_cmd c;
    int fd;

    (void)arg;
    unlink(STEMS_FIFO);
    if (mkfifo(STEMS_FIFO, 0600) < 0 && errno != EEXIST) {
        pointsrc_log("stems: mkfifo %s: %s\n", STEMS_FIFO, strerror(errno));
        return NULL;
    }
    /* O_RDWR: a reader that is also a writer never sees EOF between bridge writes */
    fd = real_open(STEMS_FIFO, O_RDWR, 0);
    if (fd < 0) {
        pointsrc_log("stems: open %s: %s\n", STEMS_FIFO, strerror(errno));
        return NULL;
    }
    pointsrc_log("stems: controller commands on %s\n", STEMS_FIFO);
    stems_state_init();
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t n;

        stems_state_tick();
        if (real_poll(&p, 1, 50) <= 0)
            continue;
        n = real_read(fd, &c, sizeof c);
        if (n == (ssize_t)sizeof c && c.magic == STEMS_CMD_MAGIC) {
            stems_command(&c);
            stems_state_tick();
        }
    }
    return NULL;
}

/* RBP'S STATUS CONTROLS STAY LIVE UNDER THE BEAT FX PAGE. Drawing the BEAT FX BANK +
 * X-PAD over the strip does not take rbp's own STATUS hit rects away: measured
 * 2026-10-13, on the BEAT FX page a tap at y 508/517 changed a deck's pad mode (the
 * caption) and taps on FLANGER/ECHO/VINYL BRAKE/DELAY at y ~526 also fired that
 * deck's hot cue (the waveform jumped; no shim line -- rbp's own pad cell). So every
 * report on the strip, on that page only, goes to y STRIP_BUTTON_ROW_Y: the gutter
 * between the two pad rows, inside the same bank/X-PAD button the finger is on, so
 * the button still fires and no hidden STATUS control does. Pads page or unknown
 * page: nothing is moved. */
static int strip_bfx_y(int y)
{
    if (y >= STRIP_Y0 && y <= STRIP_Y1 && rbp_perf_screen() &&
        strip_page() == 1)
        return STRIP_BUTTON_ROW_Y;
    return y;
}

/* ------------------------------------------------ the HOT CUE pad row (hc_zone.h)
 *
 * The operator's ask of 2026-10-06 -- a touch on a HOT CUE pad fires that pad's cue,
 * and on a pad with no cue does NOTHING and creates nothing -- and the three reads it
 * needs. hc_zone.c owns the geometry, the anchor and the rule; this owns rbp.
 *
 * THE MIDDLE READ IS THE WHOLE FEATURE. On real Pioneer gear an unlit HOT CUE pad
 * STORES a cue at the playhead, so "fires when it should not" does not merely
 * disappoint -- it writes a cue onto the operator's own track, mid-set. Every read
 * below therefore answers its FAILURE as "no": a pad mode that cannot be read is -1, a
 * registration that cannot be asked is 0, and hc_may_fire() refuses on both. */

/* rbp's per-deck pad mode, straight out of the walk UiGetPadMode @0xfd3cc performs --
 * rbp_abi.h's UI_PADMODE_* block carries the derivation and the measurement behind it
 * (0 HOT CUE, 1 AUTO BEAT LOOP, 2 SLIP BEAT LOOP, 3 BEAT JUMP). -1 when any link of the
 * chain is missing, which hc_may_fire() reads as "not HOT CUE".
 *
 * IT IS READ RATHER THAN ASSUMED because rbp's eight pad keycodes mean whatever the
 * CURRENT mode makes them mean: K_PAD1+p triggers a cue in HOT CUE and engages a beat
 * loop in AUTO BEAT LOOP, so a tap on a drawn pad in the wrong mode is a beat loop the
 * operator never asked for. */
static int hc_pad_mode(int deck)
{
    long h  = (long)rbp_i32(UI_PADMODE_HOLDER_GLOBAL);
    long sw, pd;

    if (!h)
        return -1;
    sw = (long)rbp_i32((unsigned long)h + PADMODE_STATWATCHER_OFF);
    if (!sw)
        return -1;
    pd = (long)rbp_i32((unsigned long)sw + (deck ? PADMODE_DECK2_OFF
                                                 : PADMODE_DECK1_OFF));
    if (!pd)
        return -1;
    return rbp_i32((unsigned long)pd + PADMODE_BYTE_OFF) & 0xff;
}

/* Is a hot cue registered on this pad? rbp's own question, asked of rbp's own engine.
 *
 * THE CALL IS DIRECT BECAUSE THERE IS NOTHING TO CHASE: djengine::DjEngineIF::
 * isRegisteredHotCue @0x48b00 saves r1 and r2 and RELOADS r0 from its own singleton
 * (`ldr r6,[pc,#152]` then `ldr r0,[r6]`, at 48b08/48b10), so the `this` argument is
 * dead and NULL is passed because nothing is ever read from it. What it forwards to
 * playengine::PlayEngine::isRegisteredHotCue is the channel -- the 0-BASED deck index,
 * which is what rbp passes -- and the PAD NUMBER 1..8, not an index (rbp_abi.h's
 * ADDR_ENGINE_IS_REGHOTCUE carries the derivation). rbp_bridge.c's hotcue_delete() has
 * been making this same call on this same address since 2026-10-01.
 *
 * 0 IS THE ANSWER THAT DOES NOTHING, and every failure answers it: not rbp, no track
 * loaded, no engine built yet. hc_may_fire() reads it as "empty pad". */
static int hc_registered(int deck, int pad)
{
    static int am_rbp = -1;

    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return 0;
    return ((int (*)(void *, int, int))ADDR_ENGINE_IS_REGHOTCUE)(NULL, deck, pad);
}

/* A qualifying release on a pad cell: the rule, and the line that says which way it
 * went. NEVER SILENT -- a tap that did nothing because the pad is empty and a tap that
 * did nothing because a read failed are the same pixels, and telling them apart is the
 * whole of what the operator would want to know.
 *
 * THE SEND IS map_flx4.c's AND aloop_apply()'s: one press edge and one release edge,
 * back to back, on rbp's own channel for the deck -- which is what keeps deck 1's tap
 * from being deck 2's cue. */
static void hc_fire(int deck, int pad)
{
    static int am_rbp = -1;
    int mode, pm, reg;

    /* The guard is here as well as in hc_registered() because this reads rbp's browse
     * mode and its pad mode too, and it is reached from a LATCHED press -- which is
     * deliberately not gated on the performance screen, so the release still lands
     * after rbp has left it. Outside rbp nothing is read and the gates see -1. */
    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    mode = am_rbp ? rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE) : -1;
    pm = am_rbp ? hc_pad_mode(deck) : -1;
    reg = (pm == HC_PAD_MODE_HOT) ? hc_registered(deck, pad) : 0;

    if (!hc_may_fire(mode, pm, reg)) {
        pointsrc_log("pointsrc: hot cue deck%d pad%d -> nothing sent "
                     "(mode %d pad mode %d registered %d)", deck + 1, pad, mode, pm, reg);
        return;
    }
    pointsrc_log("pointsrc: hot cue deck%d pad%d -> key %#x ch%d", deck + 1, pad,
                 hc_pad_keycode(pad), deck + 1);
    send_rx_key(hc_pad_keycode(pad), OP_PRESS, deck + 1, 0);
    send_rx_key(hc_pad_keycode(pad), OP_RELEASE, deck + 1, 0);
}

/* The drawers' keys and fader, on their way to rbp.
 *
 * THE TWO TRANSPORT KEYS ARE PLAIN PRESS/RELEASE, and that is the difference from
 * menu_fire() above: rbp's CUE and PLAY handlers act on the press alone (the same
 * shape map_flx4.c uses -- one press edge, one release edge, back to back), so there
 * is no synthesized hold here and no usleep. A transport button has no second meaning
 * and nothing to wait for; the band's MENU key has both, which is why it does.
 *
 * The channel is side_channel(): 1 for the left drawer (deck 1) and 2 for the right
 * (deck 2) -- rbp's send-channel argument, which is what keeps one on-screen CUE from
 * being both decks' CUE.
 *
 * THE FADER IS AN ABSOLUTE VALUE, not a move. K_FADER takes the 10-bit position and
 * the normalised float -- exactly what map_flx4.c's CC handler sends for the hardware
 * fader, and the call the operator's own measurement used to take a silent channel to
 * audible (docs/09-audio.md). There is no MIDI and no device in this path at all.
 *
 * The bookkeeping line is not optional. rbp_vu.c seeds BOTH channel faders to unity
 * for ~30 s after rbp starts its mixer, gated on g_fader_seen -- so a fader the
 * operator has moved stops being overridden only if the same array the meter reads
 * says a surface reported one. That is the whole reason fader_state.o exists; writing
 * these two globals directly from this file would have created a second copy
 * invisible to knobshim, and the seed would have won for the first 30 s of every run.
 *
 * SYNC IS PRESS AND RELEASE, like CUE and PLAY, and it is one key on one channel:
 * K_SYNC addressed to this side's deck. It is the same key the FLX4's own SYNC button
 * sends (map_flx4.c), so the drawer and the hardware cannot disagree about what
 * syncing is.
 */

/* ---------------------------------------------------------------------------
 * THE NUDGE, which is rbp's JOG and nothing else.
 *
 * There is NO bend, pitch-bend or nudge keycode in rbp's table -- the only mechanism
 * it has for "push the track a little" is the jog wheel's rotate message, which is the
 * whole reason this borrows map_flx4.c's flx4_jog() rather than inventing anything:
 *
 *   send_rx_key_fl(K_JOG_ROT 0x4305, OP_ROTATE, ch, 0, speed, vpos)
 *                                                            (rev/s)  (16-bit)
 *
 * TWO THINGS ABOUT THAT CONTRACT DRIVE THE CODE BELOW.
 *
 * 1. rbp KEEPS BENDING UNTIL IT IS TOLD SPEED 0. A jog message starts a bend at a
 *    signed rate and nothing in rbp ends it -- flx4_jog_idle() exists for exactly this
 *    reason on the hardware path (it fires a speed-0 message once the wheel has been
 *    still for JOG_IDLE_MS). So the drawer's nudge is a START on the press and a STOP
 *    on the release, and the stop is emitted whether or not the finger is still on the
 *    cell it began on. A bend left running is a track that slides out from under the
 *    operator with nothing on the glass able to stop it.
 *
 * 2. THE SPEED'S MAGNITUDE IS A CALIBRATION, NOT A DERIVATION. The FLX4 computes it
 *    from how fast the platter turns; a button has no speed, so one has to be chosen,
 *    and rbp's own mapping from rev/s to tempo bend is not known here. The default is
 *    deliberately small and the knob is SIDE_NUDGE_SPEED so it can be settled on the
 *    glass in one restart rather than in a rebuild -- which is the honest state of
 *    this number, and the drill on the unit is what will fix it.
 *
 * `nudge_running[]` is the bend each side is making RIGHT NOW (-1 back, +1 forward, 0
 * none) and `nudge_pos[]` is the 16-bit position the last message carried, which the
 * contract wants alongside the speed. The running state is what makes this idempotent:
 * the edges come from side_zone.c's acts, and side_bend_sync() reconciles them against
 * side_nudging() on the paths where no report will arrive to deliver an edge.
 * ------------------------------------------------------------------------- */
#define SZ_NUDGE_SPEED_DEFAULT  0.35

static int          nudge_running[2];
static unsigned int nudge_pos[2];

static void side_bend(int side, int dir)
{
    int ch = side_channel(side);
    double speed = env_double("SIDE_NUDGE_SPEED", SZ_NUDGE_SPEED_DEFAULT);

    if (side != SZ_LEFT && side != SZ_RIGHT)
        return;
    if (dir == nudge_running[side])
        return;                      /* already bending that way: one message is the whole protocol */
    nudge_running[side] = dir;

    if (dir != 0)
        nudge_pos[side] = (unsigned int)(nudge_pos[side] +
                                         (dir > 0 ? 128u : (unsigned int)-128)) & 0xFFFFu;

    pointsrc_log("pointsrc: side %s nudge %s -> key %#x rotate speed %+.2f ch%d pos %u",
                 side ? "right" : "left",
                 dir > 0 ? "forward" : (dir < 0 ? "back" : "stop"),
                 K_JOG_ROT, dir * speed, ch, nudge_pos[side]);
    send_rx_key_fl(K_JOG_ROT, OP_ROTATE, ch, 0, (float)(dir * speed),
                   (long)nudge_pos[side]);
}

/* The reconciler, for the paths where no report will arrive. Called after
 * side_reset_all(), which is where a finger can stop existing without a release edge:
 * the touch device going away, and the menu being switched off under a held press. In
 * both, side_zone.c has already forgotten the press, so side_nudging() reads 0 and
 * this is what actually stops the track. It is idempotent, so calling it on a quiet
 * system sends nothing. */
static void side_bend_sync(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        int want = side_nudging(i);

        if (want != nudge_running[i])
            side_bend(i, want);
    }
}

static void side_act(int side, int act, int value)
{
    int ch = side_channel(side);

    switch (act) {
    case SZ_ACT_SYNC:
        pointsrc_log("pointsrc: side %s SYNC -> key %#x ch%d", side ? "right" : "left",
                     K_SYNC, ch);
        send_rx_key(K_SYNC, OP_PRESS, ch, 0);
        send_rx_key(K_SYNC, OP_RELEASE, ch, 0);
        break;
    case SZ_ACT_CUE:
        pointsrc_log("pointsrc: side %s CUE -> key %#x ch%d", side ? "right" : "left",
                     K_CUE, ch);
        send_rx_key(K_CUE, OP_PRESS, ch, 0);
        send_rx_key(K_CUE, OP_RELEASE, ch, 0);
        break;
    case SZ_ACT_PLAY:
        pointsrc_log("pointsrc: side %s PLAY -> key %#x ch%d", side ? "right" : "left",
                     K_PLAY, ch);
        send_rx_key(K_PLAY, OP_PRESS, ch, 0);
        send_rx_key(K_PLAY, OP_RELEASE, ch, 0);
        break;
    case SZ_ACT_NUDGE_REV:
        side_bend(side, -1);
        break;
    case SZ_ACT_NUDGE_FWD:
        side_bend(side, +1);
        break;
    case SZ_ACT_NUDGE_STOP:
        side_bend(side, 0);
        break;
    case SZ_ACT_FADER:
        /* The value is logged for every change -- a full-height drag is a few hundred
         * lines and not a few thousand, because side_zone.c only reports a change --
         * and it is the only line that says what the drawer actually sent. */
        pointsrc_log("pointsrc: side %s fader ch%d -> %d", side ? "right" : "left",
                     ch, value);
        send_rx_key_f(K_FADER, OP_VALUE, ch, value, (float)value / 1023.0f);
        fader_state_set(ch, value);
        break;
    default:
        break;
    }
}

/* A swallowed drawer tap, given back to rbp -- menu_replay_tap()'s twin, and the
 * same three reasons apply verbatim: the drawer cannot hand rbp the original report
 * (it is gone, and the drawer had to decide the gesture), a bare release would be a
 * release for a press rbp never had, and it is replayed at the point the finger
 * LANDED because that is what the operator aimed at.
 *
 * The one difference is the coordinate: side_tap_point() hands back ABSOLUTE logical
 * px, because the drawer's own frame is panel-local and mirror-reversed and tscfake
 * emits in the pointer's space. Doing the un-mirroring anywhere else would put it in
 * two places. */
static void side_replay_tap(int side)
{
    int tx = 0, ty = 0;
    int dwell_ms = env_int("POINT_MENU_TAP_MS", POINT_MENU_TAP_DEFAULT_MS);

    side_tap_point(side, &tx, &ty);
    /* The entry columns overlap the pad strip's ends, and a replay is a press rbp
     * sees, so it obeys the strip's rules like a forwarded report: dropped on the
     * STATUS page while that page is dead to touch, moved into the button row on the
     * BEAT FX page. Without this the corners of the strip reached rbp's hidden
     * pad-mode caption on both pages (2026-10-13: deck 2 BEAT JUMP <-> 3 from the
     * right column, deck 1 0 <-> 4 from the left). */
    if (ty >= STRIP_Y0 && ty <= STRIP_Y1 && rbp_perf_screen()) {
        int pg = strip_page();

        if (pg == 0 && !status_touch) {
            pointsrc_log("pointsrc: side %s edge tap at (%d,%d) is on the STATUS strip"
                         " -> not replayed (POINT_STATUS_TOUCH=0)",
                         side ? "right" : "left", tx, ty);
            return;
        }
        ty = strip_bfx_y(ty);
    }
    pointsrc_log("pointsrc: side %s edge tap at (%d,%d) is not a swipe or a button"
                 " -> replayed to rbp as a press (held %dms)",
                 side ? "right" : "left", tx, ty, dwell_ms);
    tscfake_emit(1, tx, ty);
    if (dwell_ms > 0)
        usleep((useconds_t)dwell_ms * 1000);
    tscfake_emit(0, tx, ty);
}

/* A swallowed strip tap, given back to rbp.
 *
 * The operator's fourth finding on the glass was that the strip's cost was more
 * the info and timer buttons specifically". The replay was the first answer to it
 * and the entry zone is the second and better one: rbp's INFO control sits in the
 * right third of the band, which MZ_ENTRY_X0/X1 no longer swallows at all, so it
 * is answered natively now and this function is never called for it. What is left
 * for the replay is whatever rbp draws in the middle third of its own top bar,
 * which is chrome on every screen it has. The menu cannot hand rbp the original
 * report -- it is gone, and the panel must still decide the gesture -- so it
 * replays the press instead, and this is the whole of it.
 *
 * BOTH EDGES, at the point the finger landed: rbp never saw the down, so a bare
 * release would be a release for a press it does not have, and tscfake_emit()'s
 * dedup would in any case drop a repeated release. At the *start* point because
 * that is what the operator aimed at -- the press is a tap by definition here (it
 * never travelled MZ_SWIPE_PX), and a finger that drifted a few px on the way up
 * still meant the button it landed on.
 *
 * THE DWELL IS NOT OPTIONAL -- see POINT_MENU_TAP_DEFAULT_MS. It blocks this
 * reader thread for its duration, which is the same trade read_loop_rel() makes
 * for a fast physical click: evdev buffers, so a touch arriving during the replay
 * is delayed and not lost.
 *
 * quantize_tap() is deliberately NOT called for the replay, even though the normal
 * path calls it for every report it forwards: it would advance touch_zone.c's edge
 * state for a press that can never be on one of its boxes (they are at y 732..780,
 * the strip is y <= 55), and a swallowed press must not touch that state. */
static void menu_replay_tap(void)
{
    int tx = 0, ty = 0;
    int dwell_ms = env_int("POINT_MENU_TAP_MS", POINT_MENU_TAP_DEFAULT_MS);

    menu_tap_point(&tx, &ty);
    pointsrc_log("pointsrc: strip tap at (%d,%d) is not a swipe or a button ->"
                 " replayed to rbp as a press (held %dms)", tx, ty, dwell_ms);
    tscfake_emit(1, tx, ty);
    if (dwell_ms > 0)
        usleep((useconds_t)dwell_ms * 1000);
    tscfake_emit(0, tx, ty);
}

/* ---------------------------------------------------------------------------
 * THE UTILITY SCREEN'S TOUCH GESTURE (util_zone.c), and the two halves that keep
 * that module pure: reading rbp's state into a struct, and sending what it answers.
 *
 * rbp binds NO touch to its own UTILITY screen -- docs/07-touch.md calls it "not a
 * touch screen" -- and the whole screen is driven by K_SELECTOR: OP_ROTATE moves
 * the highlight, OP_PRESS/OP_RELEASE is Enter. This is the operator's *"give the
 * ability to scroll up and down items and tap to click enter on an item (only do
 * this in this menu)"*, and "only in this menu" is the gate below: the browse mode
 * rbp's own dispatcher compares against (rbp_abi.h's BROWSE_MODE_UTILITY).
 *
 * EVERY READ HERE IS ONE WORD OF uiBrowse AT A FIXED ADDRESS, which is the only
 * reason it is safe from this thread: uiBrowse is a plain .bss singleton, so there
 * is no pointer chase and no structure built during rbp's start-up to walk. (The
 * guarded walks in rbp_led.c are guarded for exactly the opposite reason.) The
 * process test is the one guard that matters -- these addresses are only mapped
 * inside rbp, so a shim loaded anywhere else must not dereference them at all.
 *
 * The cursor is read at the ACTIVE list, the way rbp's own getActiveCursorNo
 * @0x112790 does -- the list a rotation will move. While navigating UTILITY that is
 * index 0; the edit-mode index is clamped in rather than assumed, so a value rbp
 * never sets cannot walk off the field.
 * ------------------------------------------------------------------------- */
static int rbp_i32(unsigned long addr)
{
    return *(volatile int *)(uintptr_t)addr;
}

static void util_read_state(struct util_state *st)
{
    static int am_rbp = -1;
    int list;

    st->mode = 0;
    st->editing = 0;
    st->calibrating = 0;
    st->cursor = 0;

    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return;

    st->mode = rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE);
    st->editing = rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_ACTIVE_LIST);

    list = st->editing ? 1 : 0;
    st->cursor = rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_CURSOR + 4 * list);

    if (rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_CALIB_A))
        st->calibrating =
            rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_CALIB_B) != 0;
}

/* What util_zone.c's answer turns into. Rotations first, then, for a tap, the Enter
 * -- the same order the module's own header states, and the order matters: the
 * travel is what puts the highlight on the row the operator touched, and only then
 * does Enter open it.
 *
 * The burst is bounded by the module (UTIL_ROT_MAX) and is sent with NO SLEEP
 * between the steps: rbp's rotate is a posted message, and a wait here would starve
 * the one input loop the band, the window, the drawers and this gesture all share
 * (docs/07-touch.md's TRAP 2). map_kbd.c's kbd_rot does the same and for the same
 * reason -- this is that sender's second caller, with the count coming from a
 * finger instead of a wheel.
 *
 * A press with no release never follows, because a tap is complete at its release
 * and this is called from there; the module's own answer is what decides. */
static void util_send(int act, int value)
{
    int i, n = value;

    if (act == UTIL_ACT_NONE)
        return;

    for (i = 0; i < (n < 0 ? -n : n); i++)
        send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, n < 0 ? -1 : 1);

    if (act == UTIL_ACT_TAP) {
        send_rx_key(K_SELECTOR, OP_PRESS, CH_GLOBAL, 0);
        send_rx_key(K_SELECTOR, OP_RELEASE, CH_GLOBAL, 0);
    }
}

/* --- the waveform swipe ------------------------------------------------------
 *
 * wave_zone.c owns the gesture; these two functions are the only part of it that
 * touches rbp, and they are here for the same reason util_send() is: the module is
 * pure and this file is where the reads and the sends live.
 *
 * WHY IT IS A SELECTOR ROTATION AND NOT A TOUCH. rbp has no waveform touch: the zoom
 * is reached only from BrowseEncoderRotate @0x1212a4 (CursorWaveZoom @0x102720 is the
 * binary's only caller of setPlayModeWaveScale), and the on-screen "− ZOOM GRID" is an
 * indicator whose panel class is referenced nowhere in executable code. So a swipe is
 * spent as K_SELECTOR rotations -- the same key the browse encoder produces and the
 * same one the UTILITY gesture above already drives. rbp then zooms through its own
 * code path, with its own clamping (0..4) and its own indicator.
 *
 * THE GATE IS rbp's OWN DISPATCH DECISION, not a rule invented here. On the
 * performance screen ComputeCursorMode @0x113084 routes a rotation to CursorWaveZoom
 * only when two conditions hold, and RB_WAVE_ZOOM_OK() in rbp_abi.h is that branch
 * spelled out -- so a step is sent exactly when rbp itself would have sent the
 * rotation to the waveform zoom. The other branch of the same test moves the BEAT
 * GRID, which rewrites the analysis of the operator's track; that is why this is a
 * refusal and not a nicety, and why the flag it reads is the LIVE one rather than the
 * mirror rbp caches between rotations (rbp_abi.h names both addresses).
 *
 * ...and one condition rbp knows nothing about: nothing of the shim's may be in front
 * of that screen. The band's panel and the drawers are drawn OVER rbp's own, and a
 * gesture that zooms a waveform the operator cannot see is the same defect the UTILITY
 * gesture's "nothing is up" terms exist to prevent. It is also the operator's own
 * answer to the band: swipe the drawer away first.
 *
 * IT IS RE-READ ON EVERY REPORT, and that is not a nicety either: the gate is what
 * tells the module that the screen went away under a press, and a gate cached at the
 * down edge would let one rotation land on whatever screen replaced it. */
static int wave_zoom_ok(int allow_menu)
{
    static int am_rbp = -1;

    if (!allow_menu)
        return 0;
    if (am_rbp < 0)
        am_rbp = is_rbp_process();
    if (!am_rbp)
        return 0;
    if (menu_window_is_open() || menu_is_open() || side_any_open())
        return 0;

    return RB_WAVE_ZOOM_OK(rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE),
                           rbp_i32(UIBROWSE_GLOBAL + UIBROWSE_OFF_PLAY_SUBGATE),
                           rbp_i32(GRID_ADJUST_FLG_GLOBAL));
}

/* rbp's state for one report: whether a rotation sent now would zoom, and -- only
 * when it would -- where the scale stands. The scale read is behind the gate because
 * it is used at a down edge to keep the ratchet inside the ladder, so it is worth
 * exactly one load on the one screen where the gesture exists and is not worth one on
 * any other. */
static void wave_read_state(struct wave_state *st, int allow_menu)
{
    st->zoom_ok = wave_zoom_ok(allow_menu);
    st->scale = st->zoom_ok ? rbp_i32(WAVE_SCALE_GLOBAL) : 0;
}

/* The steps, and only the steps -- util_send()'s rotation half with no Enter. Sent
 * with NO SLEEP between them, for the reason util_send() states: rbp's rotate is a
 * posted message, and a wait here would starve the one input loop the band, the
 * window, the drawers and both gestures share. The count is bounded by the module --
 * by rbp's own 0..4 ladder, which is the widest it can ever be -- so it does not have
 * to be re-bounded here. */
static void wave_send(int steps)
{
    int i, n = steps;

    for (i = 0; i < (n < 0 ? -n : n); i++)
        send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, n < 0 ? -1 : 1);
}

/* The gesture, asked before the ladder below and answered out loud. Three things
 * about that placement:
 *
 * IT IS ASKED FIRST BECAUSE IT TAKES NOTHING (wave_zone.h's "additive"). Every feeder
 * in the ladder answers MZ_FEED_* and a TAKEN report leaves this function early, so a
 * module asked later would not see the reports a surface swallowed -- and a module that
 * does not see every report of a press cannot hold an anchor for it. Asked here, its
 * view of the press is the panel's own view, exactly. That is not a tidiness
 * preference: the pinch this replaces was asked late and swallowed reports, and a
 * gesture left holding an anchor across a swallowed press would spend it on the next
 * one.
 *
 * IT COSTS NOTHING ON ANY OTHER SCREEN. The gate is three loads of uiBrowse plus three
 * "is a shim surface open" compares, and the module answers 0 the instant it is false --
 * which is every report of every touch anywhere but the performance screen.
 *
 * AND THE SEND MUST PRECEDE THE TOUCH, which is why the key goes out here rather than
 * after the emit below. IKeyManager::sendKey @0x37ad64 fills ONE IKeyInput -- the
 * pointer at KeyManager+132 -- and only then walks its listeners, so a key sent after
 * the touch emit can have its keycode overwritten by rbp's own TouchPanel thread
 * before it is read. Keys first, touch second: quantize_tap() below has done it in
 * that order since it was written. */
static void wave_swipe(int allow_menu, int down, int x, int y)
{
    struct wave_state st;
    int steps;

    wave_read_state(&st, allow_menu);
    steps = wave_feed(&st, down, x, y);
    if (steps) {
        pointsrc_log("pointsrc: wave %s %d step(s) at (%d,%d)",
                     steps > 0 ? "zoom IN" : "zoom OUT",
                     steps > 0 ? steps : -steps, x, y);
        wave_send(steps);
    }
}

/* The drawers' open/close, said out loud. side_zone.c is pure and has no log of its
 * own (menu_zone.c's rule), so the transition is noticed here, where the log lives.
 * Two integer compares per report, and one line per transition -- the line the drill
 * reads to know the swipe arrived at all, as opposed to the key it may or may not
 * have produced. */
static void side_log_transitions(void)
{
    static int was[2] = { -1, -1 };
    int i;

    for (i = 0; i < 2; i++) {
        int now = side_is_open(i);

        if (now != was[i]) {
            pointsrc_log("pointsrc: side %s drawer %s", i ? "right" : "left",
                         now ? "open" : "closed");
            was[i] = now;
        }
    }
}

/* One report, in the one order that works: the browser WINDOW decides first if it
 * is open, then the menu decides whether rbp sees this press at all, then a drawer,
 * then rbp's own UTILITY screen if it is the one in front, then the QUANTIZE box
 * interprets it, then it goes to rbp.
 *
 * THE WINDOW IS FIRST because it is opaque and drawn OVER the menu's own rectangle:
 * a press that landed on the window must not also be judged by the swipe rules
 * underneath it. It answers only for points on itself, so nothing else in this
 * chain changes.
 *
 * THE MENU IS NEXT because it owns the whole report when it takes it. rbp gets
 * nothing -- not the touch, and not the QUANTIZE box either: a press that
 * dismisses the panel must not also press whatever is drawn under it, and
 * touch_zone_feed() is reached only through quantize_tap() below, so a swallowed
 * press never advances the box's edge state. That is what leaves the next real
 * press on a QUANTIZE box still a fresh press (test_point.c pins it).
 *
 * THE DRAWERS ARE LAST OF THE THREE, and they are asked only while the window is
 * shut: the window owns the plane and a drawer must not be opened under it. The
 * band's own precedence is inside side_zone.c instead (its closed-entry arm refuses
 * while the band is open) -- which is why an open band, which swallows at full
 * width, still answers first and a drawer cannot steal the tap that dismisses it.
 * side_feed_any() is the one call and it returns which drawer answered, because only
 * that drawer knows the channel the key or the fader value goes out on.
 *
 * MZ_FEED_TAP is the one swallowed answer rbp does hear, and it hears a *different*
 * press: the swallowing surface's own copy, emitted here (menu_replay_tap /
 * side_replay_tap) rather than forwarded. So even that report takes neither the
 * QUANTIZE box nor the original coordinates.
 *
 * `allow_menu` is the caller's answer to "may the menu have this one": the
 * absolute loop passes menu_enabled, the relative loop passes menu_mouse, and a
 * report that is not offered is never swallowed and never changes the menu's
 * state -- so POINT_MENU=0 really is rbp's untouched stream.
 *
 * `held_ms` is how long the press behind this report has been down, and it is the
 * caller's because this file is the one with the clock: 0 on a report that is not
 * a release (nothing can fire from those), and the measured duration at the
 * release, which is where a menu button fires and where the tap/hold split is
 * decided. The drawers have no hold of their own: a transport key has no second
 * meaning, so `held_ms` is not passed on. */
static int status_latched;   /* a STATUS-strip gesture is being swallowed (below) */

static void pointer_report(int down, int x, int y, int allow_menu, int held_ms)
{
    static int last_down;
    int was_down = last_down;
    int button = 0;
    int verdict = MZ_FEED_NONE;
    int side = -1, sact = SZ_ACT_NONE, svalue = 0;

    /* A gesture the STATUS-strip rung (further down) took on its press is swallowed
     * here, first, until its release -- before any other surface can see a move or
     * an up of it. was_down is every report's predecessor, whoever took it. */
    last_down = down;
    if (status_latched) {
        if (!down)
            status_latched = 0;
        return;
    }

    /* THE USB STOP CHOOSER FIRST, before everything else including the waveform
     * swipe, and unconditionally -- not behind `allow_menu`. While the box is up it
     * owns every report (prompt_zone.h), and a report it did not take would reach rbp
     * as a press or a release of rbp's own: a finger that dismisses the box would
     * also press whatever the performance screen has under it. The box can only be up
     * if the menu was enabled when it was raised, and the POINT_MENU=0 path calls
     * prompt_reset() beside menu_reset(), so ungated is safe as well as simpler.
     *
     * The device liveness is re-read on EVERY report, not cached at the open: rbp's
     * answer is the whole point of the two device buttons and a stop the operator
     * starts elsewhere is exactly the change the box should show.
     *
     * EVERY WAY A HOLD CAN END IS LOGGED, which is what makes the drill readable. The
     * eject is logged with the milliseconds it was actually held, because "held 3020 ms"
     * is the line that says a finger crossed the threshold rather than the tick's own
     * slice; an early release is logged with its (shorter) duration, because the absence
     * of an eject line is otherwise the same evidence as "the box never opened"; and a
     * finger that leaves the button it was holding on is logged for the same reason,
     * since that rule -- the anchor does not resume -- is invisible on the glass.
     *
     * THE CLOSE LAST, and it is the one line that cannot say WHY. By the time the release
     * arrives the box has no memory of the press either way: the release that dismisses a
     * box nobody pressed into and the release that puts away a box whose hold just fired
     * are the same report through the same path (prompt_zone.c's fire branch disowns the
     * press precisely so that they can be). It is logged as the fact and not as a verdict,
     * and the line above it says which of the two it was. */
    if (prompt_is_open()) {
        struct prompt_state ps;
        int pact = PR_ACT_NONE;
        int held_cell = prompt_hold_cell();
        unsigned long long now = shim_now_ms();
        unsigned long long held = prompt_hold_ms(now);

        pointsrc_usb_state(&ps);
        if (prompt_feed(&ps, down, x, y, now, &pact) != MZ_FEED_NONE) {
            if (pact != PR_ACT_NONE) {
                int ch = prompt_act_channel(pact);

                pointsrc_log("pointsrc: usb stop chooser -> usb %d held %llu ms"
                             " (hold is %d ms) -> eject (channel %d)",
                             ch, held, PR_HOLD_MS, ch);
                usb_stop_send(ch);
            } else if (!down && held_cell) {
                pointsrc_log("pointsrc: usb stop chooser -> usb %d released after"
                             " %llu ms (hold is %d ms) -> nothing",
                             held_cell, held, PR_HOLD_MS);
            } else if (down && held_cell && !prompt_hold_cell()) {
                pointsrc_log("pointsrc: usb stop chooser -> usb %d left the button"
                             " after %llu ms -> the hold is off",
                             held_cell, held);
            } else if (!prompt_is_open()) {
                pointsrc_log("pointsrc: usb stop chooser closed");
            }
            return;
        }
    }

    /* THE EFFECT PICKER, beside the USB chooser and for the same reason: while the box is
     * up it owns every report (fx_zone.h), so a finger that dismisses it must not also
     * press whatever the performance screen has underneath it. Ungated on `allow_menu`,
     * like the chooser: the box can only be up if its knob was on when it was raised, and
     * the POINT_FX_TOUCH=0 path calls fxlist_reset() beside menu_reset().
     *
     * A ROW IS SENT AS THE SWITCH POSITION rbp's own effect selector takes -- the same
     * call the FLX4's FX SELECT button makes (map_flx4.c:520) -- and the word the operator
     * saw on the row is fx_zone.c's measured table. A release that answered nothing (a
     * miss, a slide, a tap on the title) is logged as a dismissal, because "the box closed
     * and nothing happened" is otherwise indistinguishable from "the tap never landed". */
    if (fxlist_is_open()) {
        int pos = -1;

        if (fxlist_feed(down, x, y, &pos) != MZ_FEED_NONE) {
            if (pos >= 0) {
                fx_type_send(pos);
                pointsrc_log("pointsrc: beat fx picker -> type position %d (%s)",
                             pos, fxlist_row_label(pos + 1));
            } else {
                pointsrc_log("pointsrc: beat fx picker dismissed");
            }
            return;
        }
    }

    /* THE WAVEFORM SWIPE, which takes nothing and therefore goes first -- wave_swipe()
     * says why that is a rule and not a preference. */
    wave_swipe(allow_menu, down, x, y);

    /* THE WINDOW FIRST, and it is asked before the menu rather than after because
     * the two overlap on the glass: the window is opaque, so a press inside its
     * rectangle must never reach the menu's gesture rules underneath it. It only
     * answers for points on itself (menu_window.h), so a press anywhere else still
     * gets both the menu and rbp exactly as before -- which is what keeps the
     * swipe-down gesture and test_menu.c's 6,373 checks untouched.
     *
     * A window report is always TAKEN, never TAP: the caller must not replay it to
     * rbp, and `button` is left alone because the window acts on its own state
     * machine inside menu_window_feed() rather than handing a button back. */
    if (allow_menu && menu_window_is_open())
        verdict = menu_window_feed(down, x, y);

    if (verdict == MZ_FEED_NONE && allow_menu)
        verdict = menu_feed(down, x, y, &button);

    /* THE BEAT FX BANK IS SHIM-DRIVEN. rbp's STATUS pads stay live under the BEAT FX
     * page (strip_bfx_y()'s comment): a tap rbp sees on FLANGER also fires deck 1's hot
     * cue A, and no y inside that box misses the hidden pad -- FLANGER lies wholly over
     * pad A, and moving the tap to the gutter between the pad rows still fired it
     * (measured with both decks paused, 2026-10-13). So on that page a press on any of
     * the four slots is withheld from rbp, gesture and all (status_latched), and the
     * shim selects the slot's effect itself: rbp's own UiGetMixBeatFXBank(slot) gives
     * the switch position and K_BFXTYPE carries it -- the effect picker's proven path.
     * Asked BEFORE the drawers' entry column, which used to take the left of FLANGER
     * and replay it to rbp unmoved. An open drawer, the band or the window still come
     * first; the trash icon's delete mode, an empty or unreadable slot, or any other
     * page leave the tap to the rungs below, as before. */
    if (verdict == MZ_FEED_NONE && down && !was_down && rbp_perf_screen() &&
        y >= STRIP_Y0 && y <= STRIP_Y1 && x >= BFX_BANK_X0 && x <= BFX_BANK_X1 &&
        !menu_window_is_open() && !menu_is_open() && !side_any_open() &&
        strip_page() == 1 &&
        ((int (*)(void))UI_GET_BFX_BANK_DELMODE)() == 0) {
        int slot = (x - BFX_BANK_X0) / BFX_BANK_PITCH;
        int pos;

        if (slot > 3)
            slot = 3;
        pos = ((int (*)(int))UI_GET_MIX_BFX_BANK)(slot);
        if (pos >= 0 && pos < BFX_TYPE_POSITIONS) {
            status_latched = 1;
            pointsrc_log("pointsrc: beat fx bank slot %d at (%d,%d) -> type position %d"
                         " (rbp did not see the tap)", slot + 1, x, y, pos);
            fx_type_send(pos);
            return;
        }
        pointsrc_log("pointsrc: beat fx bank slot %d at (%d,%d): no position (%d),"
                     " left to rbp", slot + 1, x, y, pos);
    }

    if (verdict == MZ_FEED_NONE && allow_menu && !menu_window_is_open())
        verdict = side_feed_any(SZ_PTR_MAIN, down, x, y, &side, &sact, &svalue);

    /* After the one call that can change it and before any of the three ways out, so
     * an open and the key it produced land on consecutive lines. */
    side_log_transitions();

    /* THE BEAT FX PANEL'S THREE TAPS, after every shim surface and before rbp's own screen
     * -- the same rung the UTILITY gesture sits on and for the same reason: these are
     * controls rbp PAINTS, so every surface drawn over them must get first refusal. The
     * three rects are the header bar (x 1090..1269, y 57..85) and the two inset boxes
     * (x 1100..1259, at y 98..137 and y 168..203), which the right-hand drawer
     * (x 1100..1279, full height) covers completely while it is out, and which the band's
     * panel is drawn across -- so an open surface of any kind wins, by the rectangle it
     * already owns through side_feed_any() and by the explicit gates here.
     *
     * NOT GATED ON `allow_menu` and not gated on the drawer being shut for its own sake:
     * POINT_FX_TOUCH is these three controls' own switch, and a press that misses all three
     * rects is handed straight back, so rbp's stream is untouched everywhere else.
     *
     * THE MODE IS RE-READ HERE rather than cached at the start of the gesture: rbp can
     * leave the performance screen under a press that is already down, and a swallowed
     * press on a screen that no longer has the panel is a control firing on nothing. */
    if (verdict == MZ_FEED_NONE && fx_touch && rbp_perf_screen() &&
        !menu_window_is_open() && !menu_is_open() && !side_any_open()) {
        int fact = FX_ACT_NONE;

        if (fx_feed(down, x, y, &fact) != MZ_FEED_NONE) {
            if (fact == FX_ACT_CH) {
                fx_ch_cycle();
            } else if (fact == FX_ACT_PICK) {
                fxlist_open(shim_now_ms());
                pointsrc_log("pointsrc: beat fx picker raised (%d rows)",
                             FXLIST_ROWS);
            } else if (fact == FX_ACT_POWER) {
                /* Fires on the PRESS (fx_zone.h), so this line appears as the finger
                 * goes down. It names the SEND and not the resulting state: the shim
                 * does not read rbp's enable word -- there is no measured one, the type
                 * word +0x50 was already measured dead -- and it does not need to, since
                 * the toggle is rbp's own. */
                fx_power_send();
                pointsrc_log("pointsrc: beat fx power toggle sent");
            }
            return;
        }
    }

    /* THE BPM CELL AS A MOMENTARY X/Y PAD, on the same rung as the three taps above and
     * for the same reasons: the cell is rbp's own drawing, so every shim surface gets
     * first refusal (the explicit gates), the mode is re-read here rather than cached
     * (rbp can leave the performance screen under a press that is already down), and it
     * is NOT gated on `allow_menu` -- POINT_FX_TOUCH is this panel's own switch.
     *
     * THE READ IS THE GATE, AND ONLY ON A PRESS. A pad that cannot read the level and
     * the rung it would have to put back must not engage at all, or it restores the
     * effect to a number it invented -- on the operator's own track, mid-set. A pad that
     * is ALREADY engaged keeps owning its gesture wherever the finger goes, read or no
     * read, because a press this module swallowed has been withheld from rbp for its
     * whole life and must see its own release (declined-press-must-still-see-release).
     *
     * THE PICKER AND THE CHOSER ARE GATED OUT TOO (`!fxlist_is_open() && !prompt_is_open()`),
     * which the three taps above do not need: those are one-shot, while this pad owns a
     * gesture for as long as a finger is down, so a box raised by a second finger, or by
     * the band, must never have a pad engaged underneath it. */
    if (verdict == MZ_FEED_NONE && fx_touch &&
        (fxpad_busy() || (rbp_perf_screen() && !menu_window_is_open() &&
                          !menu_is_open() && !side_any_open() &&
                          !fxlist_is_open() && !prompt_is_open()))) {
        struct fxpad_live fl;
        const char *why = 0;
        int may = 1;

        if (!fxpad_engaged())
            may = fx_state_read_why(&fl, &why);
        if (!may && down && !fxpad_engaged() && fxpad_hit(x, y)) {
            /* THE REFUSAL, NAMED. There was no line here at all until 2026-10-07, and
             * that absence is what made the operator's "it doesn't engage" take a whole
             * session to place: a pad that never engaged, a pad that engaged and sent
             * nothing, and a shim that was not running were all the same silence. This
             * fires only on a press that WOULD have engaged -- a press inside the cell,
             * with nothing already down -- so a release, a miss and a drag across the
             * panel cost nothing. */
            pointsrc_log("pointsrc: beat fx pad refused at (%d,%d) -- %s", x, y, why);
        }
        if (may || fxpad_engaged()) {
            int was = fxpad_engaged();

            if (fxpad_feed(down, x, y) != MZ_FEED_NONE) {
                if (!was && fxpad_engaged()) {
                    /* The snapshot is taken on the first TICK, not here, so this line
                     * names what the pad is about to take away -- which is the thing a
                     * drill needs to compare the restore against.
                     *
                     * THE TYPE IS ON THE LINE BECAUSE IT DECIDES WHAT `effect` MEANS.
                     * `+0x3c` reads 1 on a switched-off effect (rbp_abi.h, nine states),
                     * so `type 0` here is the fingerprint of the state that used to
                     * leave the pad sending nothing at all: the flag says on, the class
                     * is `BeatEffectOff`, and every control the pad drives is a no-op. */
                    pointsrc_log("pointsrc: beat fx pad engaged at (%d,%d) -- type %d,"
                                 " effect %s, level %d/%d, rung %d of %d..%d",
                                 x, y, fl.type, fxpad_live_on(&fl) ? "on" : "off",
                                 fl.depth, BFX_DEPTH_FULL,
                                 fl.beat, fl.beat_min, fl.beat_max);
                }
                return;
            }
        }
    }

    /* THE STATUS PAGE OF THE PAD STRIP IS DEAD TO TOUCH (status_touch off). Asked
     * after the band, the drawers and the BEAT FX panel, which are drawn over or beside
     * the strip and keep first refusal; before the HOT CUE row, which it replaces.
     * The decision is made once, on the press, and the whole gesture follows it: a
     * press withheld from rbp must have its moves and its release withheld too, or rbp
     * is handed an up for a down it never saw. A press only latches from the up state,
     * so a finger that slides onto the strip from elsewhere stays rbp's. An unknown
     * page (-1) is not the pads, so the strip is left alone then. */
    {
        if (verdict == MZ_FEED_NONE && !status_touch && down && !was_down &&
            y >= STRIP_Y0 && y <= STRIP_Y1 && rbp_perf_screen() &&
            !menu_window_is_open() && !menu_is_open() && !side_any_open() &&
            strip_page() == 0) {
            status_latched = 1;
            pointsrc_log("pointsrc: STATUS strip tap at (%d,%d) ignored"
                         " (POINT_STATUS_TOUCH=0)", x, y);
            return;
        }
    }

    /* THE HOT CUE PAD ROW, on the same rung as the BEAT FX panel's three controls and for     * the same reason: these sixteen cells are pads rbp PAINTS, so every surface drawn
     * over them gets first refusal -- and the right-hand drawer (x 1100..1279, full
     * height) lies across deck 2's last two cells while it is out.
     *
     * NOT GATED ON `allow_menu`, because POINT_HOTCUE_TOUCH is this row's own switch
     * (the FX rung makes the same choice): these are rbp's own pads, not a shim surface,
     * so an operator who turns the band off still gets them. A report that misses every
     * cell comes back MZ_FEED_NONE and reaches rbp untouched, byte for byte.
     *
     * THE PRESS IS GATED ON THE PERFORMANCE SCREEN; THE RELEASE IS NOT. rbp can leave
     * that screen under a finger that is already down -- the FX rung's comment says the
     * same of its own rects -- and a press this module took has been withheld from rbp
     * for its whole life, so its release must reach the latch wherever it lands or rbp is
     * handed an up for a down it never saw. hc_latched() is that term, and hc_feed()
     * refuses the release of a gesture that began off the grid on its own.
     *
     * AND ONLY WHILE THE GRID IS WHAT THE STRIP SHOWS (strip_shows_pads()): with the
     * STATUS | BEAT FX toggle on BEAT FX the same rows hold the BEAT FX BANK, and this
     * rung used to take those taps -- the bank never saw them and a registered cue
     * under the finger fired (2026-10-13, `hot cue deck1 pad4 -> key 0x411a` on a tap
     * of DELAY). */
    if (verdict == MZ_FEED_NONE && hc_touch &&
        (hc_latched() || (rbp_perf_screen() && strip_shows_pads() &&
                          !menu_window_is_open() &&
                          !menu_is_open() && !side_any_open()))) {
        int hdeck = -1, hpad = 0;

        if (hc_feed(down, x, y, &hdeck, &hpad) != MZ_FEED_NONE) {
            if (hdeck >= 0)
                hc_fire(hdeck, hpad);
            return;
        }
    }

    /* AND LAST, rbp's OWN UTILITY SCREEN -- the operator's *"only do this in this
     * menu"*. It is asked after the three shipped surfaces rather than before them,
     * because this gesture sits on a band of glass rbp draws for itself and every
     * one of those surfaces is drawn over that screen: the band's strip is the first
     * 56 rows and the list starts at row 50, and a drawer's entry column is the
     * outer 56 px of a screen whose list is the full width. Giving them first
     * refusal is what makes "which surface is in front" a decision made once, in the
     * order below, instead of by a rectangle test here.
     *
     * The three "nothing else is up" terms are therefore belt and braces, and they
     * are deliberate: the feeders above already took everything on their own
     * rectangles, so what is left is the glass BETWEEN them -- and a gesture that
     * scrolls a list the operator cannot see, behind an open drawer or under the
     * band's panel, is a gesture that looks like it worked but cannot be aimed. With
     * them, "nothing is in front of rbp's screen" is exactly when this answers, which
     * is also the operator's own answer to the band: swipe the drawer away first.
     *
     * util_zone.c has already refused every mode but UTILITY and the calibration
     * sub-screen, so a press on rbp's browse screen still reaches rbp untouched --
     * which is what keeps the six columns, the keyboard and the mouse working over
     * it. */
    if (verdict == MZ_FEED_NONE && allow_menu &&
        !menu_window_is_open() && !menu_is_open() && !side_any_open()) {
        struct util_state ust;
        int uact = UTIL_ACT_NONE, uvalue = 0;

        util_read_state(&ust);
        verdict = util_feed(&ust, down, x, y, &uact, &uvalue);
        if (verdict == MZ_FEED_TAKEN)
            util_send(uact, uvalue);
    }

    if (verdict == MZ_FEED_TAP) {
        if (side >= 0)
            side_replay_tap(side);
        else
            menu_replay_tap();
        /* Both replays emit a press AND its release, so rbp ends this report UP
         * whatever it was before. */
        return;
    }
    if (verdict) {
        if (side >= 0)
            side_act(side, sact, svalue);
        else if (button)
            menu_fire(button, held_ms);
        return;
    }
    y = strip_bfx_y(y);
    quantize_tap(down, x, y);
    tscfake_emit(down, x, y);
    /* ...and this is the only line that publishes a finger of rbp's own. A TAKEN report
     * above leaves rbp's belief alone on purpose: the shim swallowed that report, rbp
     * never saw it, and its belief is still whatever the last forwarded report said. */
}

/* THE SECOND CONTACT, AND THE ONLY THING IN THIS SHIM THAT IS TWO-HANDED.
 *
 * WHY IT EXISTS. The operator, on the two edge drawers: *"if i try to drag both
 * volume meters it gets confused and only one of them changes"* (2026-10-06). The
 * drawers carry the two CHANNEL FADERS, and those are the one control pair a DJ rides
 * with both hands -- everything else on the glass is a momentary press that two
 * fingers gain nothing on. Measured on the unit before this path existed: slot 0 down
 * on the left lane, slot 1 dragged alone on the right, and the only line the log
 * produced was `side left fader ch1 -> 198`.
 *
 * IT GOES TO THE DRAWERS AND NOWHERE ELSE, and that is the whole of its scope. rbp
 * has ONE pointer -- the whole rest of this file, the band, the browser window, the
 * USB STOP chooser and the UTILITY gesture are built around it -- so a second contact
 * that lands anywhere but a drawer is DROPPED, never forwarded. Forwarding it would
 * hand rbp a press it cannot interpret.
 *
 * THE TWO GATES ARE THE SURFACES THAT OWN THE GLASS. The USB STOP chooser takes every
 * report by design while it is up (prompt_zone.h), and the browser window is opaque
 * and drawn over everything; both would otherwise be reachable by a hand the operator
 * did not aim with. The band needs no gate here: a drawer cannot arm while the band is
 * open, which side_zone.h's closed-entry arm already enforces.
 *
 * A TAP IS DROPPED, not replayed. `side_feed_any()` can answer MZ_FEED_TAP for a
 * second finger that tapped a shut drawer's entry column, and the primary path
 * answers that by replaying the press to rbp from side_tap_point(). There is no
 * second pointer to replay -- one would be a press rbp never had a finger for -- so
 * the answer is swallowed and forgotten, and the drawer is simply left as it is.
 *
 * `x` is the raw->logical transform this reader was opened with, and the raw pair is
 * passed rather than the transform's output because the two contacts are transformed
 * independently. */
static void pointer_report_alt(int down, const struct point_xform *x, int rx, int ry)
{
    int lx, ly;
    int side = -1, act = SZ_ACT_NONE, value = 0;

    if (!menu_enabled)
        return;
    /* The picker joins the chooser here: both own every report while they are up, so a
     * second contact must not reach a drawer underneath one -- and a second finger on a
     * drawer while a box is over the glass is a hand the operator did not aim with.
     *
     * THE BPM CELL'S PAD JOINS THEM for the same reason, and it is the one surface here
     * that owns the glass for as long as a finger is DOWN rather than while a box is up:
     * the right-hand drawer's entry column runs the full height of the cell's own panel,
     * so a second finger landing there while the pad is engaged would open a drawer drawn
     * straight over the pad the operator is still dragging. */
    if (prompt_is_open() || fxlist_is_open() || menu_window_is_open() || fxpad_busy())
        return;

    point_xform_abs(x, rx, ry, &lx, &ly);
    if (side_feed_any(SZ_PTR_ALT, down, lx, ly, &side, &act, &value) == MZ_FEED_NONE)
        return;

    side_log_transitions();
    if (act != SZ_ACT_NONE)
        side_act(side, act, value);
}
/* Monotonic milliseconds, 64 bits wide on purpose -- which is the whole reason
 * this is a helper and not a line of arithmetic at the call site.
 *
 * The obvious spelling is `(long)ts.tv_sec * 1000000000L + ts.tv_nsec`, and on
 * this shim's target `long` is FOUR bytes (ARM32; docs/05-chroot.md). A
 * nanosecond clock in a `long` therefore wraps every 4.29 s of uptime and reads
 * NEGATIVE for about half of it, and a press stamped in the negative half failed
 * the `down_ns > 0` test at its own release: a real 450 ms finger measured 0 ms.
 * Measured on the unit 2026-09-29 with work/m23clock.py against the kernel's own
 * delivery times, 3 presses in 5 read 0 ms -- one of them an 1100 ms hold -- so
 * the long press reached UTILITY only when the phase happened to favour it.
 *
 * The controls shim keeps the same helper in shimutil.h. The fb shim has no
 * shared clock header and the two families' headers are included together in
 * seven translation units, where two definitions of one static inline collide,
 * so this file carries its own copy -- the same trade syscalls.h makes, and for
 * the same reason.
 *
 * The host suite cannot catch a regression here: the host's `long` is 8 bytes,
 * so both spellings behave. work/m23clock.py, which runs on the target and
 * compares the shim's number with the kernel's, is the test that can. */
static unsigned long long shim_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void read_loop_abs(int fd, const struct point_xform *x)
{
    struct ev_input_event ev;
    /* ONE POINTER PER SLOT. Index 0 is the primary contact -- the one rbp, the band,
     * the browser window, the USB STOP chooser and the UTILITY gesture all read, and
     * the only one they are told about. Index 1 exists solely for the edge drawers,
     * whose two channel faders are the one control pair a DJ rides with both hands;
     * pointer_report_alt() below is where it goes, and a report it does not take is
     * dropped rather than forwarded. */
    int rx[POINT_SLOTS]   = { 0 };
    int ry[POINT_SLOTS]   = { 0 };
    int down[POINT_SLOTS] = { 0 };
    /* HAS THIS SLOT SAID WHERE IT IS YET? Cleared when the slot goes down and set by
     * its first position report. See the note at pointer_report_alt()'s call below:
     * the input core de-duplicates an ABS value that has not changed, so the FIRST
     * position of a contact can be dropped -- and a contact whose position is unknown
     * must not be taken for one at (0,0), because (0,0) is the left drawer's own
     * corner. */
    int seen[POINT_SLOTS] = { 0 };
    int slot = 0;                    /* the slot the MT_* axes below belong to */
    unsigned long long down_ms = 0;  /* when the press now down began, monotonic */
    int have_down = 0;               /* and that the stamp above is THIS press's */
    int hold_ms = env_int("POINT_MENU_HOLD_MS", MZ_HOLD_FINGER_MS);

    seed_abs_position(fd, &rx[0], &ry[0]);

    for (;;) {
        ssize_t n;

        /* THE ONE PRESS THIS LOOP MUST NOT BLOCK THROUGH.
         *
         * Everywhere else here the blocking read below *is* the loop: nothing can
         * happen until the panel sends an event. A finger held on a menu button is
         * the exception, and it is the operator's own ask (2026-10-04): *"for
         * holding the MENU to get utility, after two seconds the menu should
         * disappear and it should just go to utility by itself"*. rbp's hold timer
         * runs on the key, and the key cannot go down until the shim knows the
         * finger has been there long enough -- which, if the only wake-up is an
         * event from a finger that is deliberately holding still, is never.
         *
         * So while such a press is outstanding this waits in POINT_MENU_HOLD_TICK_MS
         * slices instead, and asks menu_hold_pending() at each one. menu_pressed()
         * is the gate that keeps this off every other press in the file: it is
         * non-zero only while the panel is open with a button under the finger, so
         * a press that cannot become a hold -- anywhere on the glass, with the panel
         * shut -- is answered by the blocking read exactly as before. */
        /* THE CHOOSER'S OWN CLOCK, and the reason it is here rather than in a thread
         * of its own: the box has to be able to go away with nobody touching anything,
         * AND a hold has to be able to fire with nobody touching anything -- the whole
         * point of a three-second hold is that the finger does not move, so there is no
         * further report to wake this loop on. Waiting in slices keeps the touch thread
         * the ONE writer of the prompt's state, exactly as the hold block below does --
         * menu_draw.c only ever reads it.
         *
         * prompt_tick() refuses the timeout while a finger is down, so this cannot hand
         * rbp a release with no press behind it (prompt_zone.c). It is also the only
         * caller of it: the tick is the module's single time entry point precisely so
         * that the eject and the blink cannot come to disagree about the clock. */
        if (prompt_is_open()) {
            struct prompt_state ps;
            int pact = PR_ACT_NONE;
            unsigned long long held = prompt_hold_ms(shim_now_ms());
            int ev;

            pointsrc_usb_state(&ps);
            ev = prompt_tick(&ps, shim_now_ms(), &pact);
            if (ev == PR_TICK_EJECT) {
                int ch = prompt_act_channel(pact);

                pointsrc_log("pointsrc: usb stop chooser -> usb %d held %llu ms"
                             " (hold is %d ms) -> eject (channel %d)",
                             ch, held, PR_HOLD_MS, ch);
                usb_stop_send(ch);
            } else if (ev == PR_TICK_TIMEOUT) {
                pointsrc_log("pointsrc: usb stop chooser timed out after %d ms",
                             PR_TIMEOUT_MS);
            } else {
                struct pollfd p;

                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (real_poll(&p, 1, POINT_MENU_HOLD_TICK_MS) == 0)
                    continue;        /* still up: look at the clock again */
            }
        }

        /* THE PICKER'S OWN CLOCK, the same shape one block up and for the same reasons:
         * a box that covers rbp's pad rows has to be able to go away with nobody touching
         * anything, and this loop otherwise wakes only on an event. fxlist_expire() refuses
         * while a finger is down, so this cannot hand rbp a release with no press behind
         * it (fx_zone.h). */
        if (fxlist_is_open()) {
            if (fxlist_expire(shim_now_ms())) {
                pointsrc_log("pointsrc: beat fx picker timed out after %d ms",
                             FX_TIMEOUT_MS);
            } else {
                struct pollfd p;

                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (real_poll(&p, 1, POINT_MENU_HOLD_TICK_MS) == 0)
                    continue;        /* still up: look at the clock again */
            }
        }

        /* THE PAD'S OWN TICK, the fourth of its kind and the same shape as the two above.
         * It is here, and not in a thread, for two reasons that are the whole design of
         * the momentary pad:
         *
         *   - A FINGER HELD STILL has no report to wake this loop on, and the beat ladder
         *     is climbed one rung per tick ON RBP'S OWN ANSWER -- so without this the
         *     pad would move the level and then sit there.
         *   - THE RELEASE IS THE LAST REPORT THERE IS, and rbp applies keys
         *     asynchronously, so the unwind that puts the effect back can only be driven
         *     by a loop that keeps running after the finger is gone.
         *
         * fxpad_tick() is the module's single time entry point, exactly as prompt_tick()
         * and fxlist_expire() are for the two boxes, and it is the only writer of the
         * pad's state -- menu_draw.c only ever reads it.
         *
         * A READ THAT FAILS MID-GESTURE ticks nothing and is NOT logged here: rbp can be
         * mid-teardown for a tick or two, and the state machine is holding the targets
         * already. The pad stays busy, so the moment the chase answers again it carries
         * on to where it was going. */
        if (fxpad_busy()) {
            struct fxpad_live fl;

            if (fx_state_read(&fl)) {
                struct fxpad_out fo;

                fxpad_tick(&fl, &fo);
                if (fo.want_on >= 0) {
                    fx_power_send();
                    pointsrc_log("pointsrc: beat fx pad -> effect %s",
                                 fo.want_on ? "on" : "off");
                }
                if (fo.depth >= 0) {
                    fx_depth_send(fo.depth);
                    pointsrc_log("pointsrc: beat fx pad -> level %d/%d",
                                 fo.depth, BFX_DEPTH_FULL);
                }
                if (fo.beat_step) {
                    fx_beat_step_send(fo.beat_step);
                    pointsrc_log("pointsrc: beat fx pad -> rung %s from %d",
                                 fo.beat_step > 0 ? "up" : "down", fl.beat);
                }
                if (!fxpad_busy()) {
                    /* THE RESTORE'S WITNESS, read fresh rather than reusing the values
                     * the tick above worked from: this is the line a drill compares with
                     * the "engaged at" line, so it has to be rbp's answer and not the
                     * shim's intent. */
                    struct fxpad_live fin;

                    if (fx_state_read(&fin))
                        pointsrc_log("pointsrc: beat fx pad unwound -- effect %s,"
                                     " level %d/%d, rung %d",
                                     fin.on ? "on" : "off", fin.depth,
                                     BFX_DEPTH_FULL, fin.beat);
                }
            }
            {
                struct pollfd p;

                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (real_poll(&p, 1, POINT_MENU_HOLD_TICK_MS) == 0)
                    continue;        /* still engaged or still unwinding */
            }
        }

        if (menu_enabled && hold_ms > 0 && down[0] && have_down && menu_pressed()) {
            int held = (int)(shim_now_ms() - down_ms);
            int button = menu_hold_pending(held, hold_ms);

            if (button) {
                /* Says which of the two paths answered this hold. menu_fire() below
                 * logs the key it sends, but its line looks the same either way, and
                 * "did the panel dismiss itself or did the finger have to lift" is
                 * the whole of what this block changed. */
                pointsrc_log("pointsrc: menu button %d held %dms and the finger is"
                             " still down -> the panel dismissed itself", button, held);
                menu_fire(button, held);
                /* It does not `continue`: the press is not over, and the read below
                 * is what will see the finger leave. menu_pressed() is 0 now, so
                 * this block is behind us for the rest of the press. */
            } else {
                struct pollfd p;

                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (real_poll(&p, 1, POINT_MENU_HOLD_TICK_MS) == 0)
                    continue;        /* still holding: look at the clock again */
            }
        }

        n = real_read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return;                     /* device gone: rediscover */
        }
        switch (ev.type) {
        case EV_ABS:
            /* ONE POINTER PER SLOT, and the slot is whatever ABS_MT_SLOT last said.
             *
             * The panel is an MT-B device -- its HID descriptor declares ten Finger
             * collections -- so it announces each contact with ABS_MT_SLOT before
             * that contact's own axes. Reading the slot is the whole repair for the
             * operator's *"if i try to drag both volume meters it gets confused and
             * only one of them changes"*: without it the two contacts' positions
             * landed in one pair of variables, so the pointer followed whichever
             * finger moved last and the drawer that had latched the press swallowed
             * both.
             *
             * ABS_X/ABS_Y still go to slot 0 and only slot 0, because that is exactly
             * what the kernel's pointer emulation puts there (MT-B mirrors the
             * PRIMARY contact into the legacy axes) -- and they are what a device
             * with no MT axis at all uses, so the SC Live 4's digitiser and an HDMI
             * mouse behave here precisely as they always have.
             *
             * `slot` is left at -1 for a finger beyond POINT_SLOTS, so a third
             * contact's axes go nowhere rather than into the second pointer's slot.
             * ABS_MT_SLOT persists until it is changed, which is the protocol: a
             * POSITION with no SLOT before it belongs to the slot still selected. */
            if (ev.code == ABS_MT_SLOT) {
                slot = (ev.value >= 0 && ev.value < POINT_SLOTS) ? (int)ev.value : -1;
            } else if (ev.code == ABS_X) {
                rx[0] = ev.value;
            } else if (ev.code == ABS_Y) {
                ry[0] = ev.value;
            } else if (ev.code == ABS_MT_POSITION_X) {
                if (slot >= 0) {
                    rx[slot] = ev.value;
                    seen[slot] = 1;
                }
            } else if (ev.code == ABS_MT_POSITION_Y) {
                if (slot >= 0) {
                    ry[slot] = ev.value;
                    seen[slot] = 1;
                }
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                /* >= 0 is down, -1 is up, and the DOWN is what allocates the slot. A
                 * new press has not said where it is yet; the release has no position
                 * to keep. */
                if (slot >= 0) {
                    down[slot] = (ev.value >= 0) ? 1 : 0;
                    seen[slot] = 0;
                }
            }
            break;
        case EV_KEY:
            /* BTN_TOUCH is the primary contact's presence, and on an MT device it
             * reads "at least one finger" -- which for MT-B implies slot 0, since
             * slots are allocated in order. A device with no MT axes drives slot 0
             * with it and nothing else, which is the same code path. */
            if (ev.code == BTN_TOUCH)
                down[0] = ev.value ? 1 : 0;
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT) {
                int lx, ly;
                int was = st_down;
                long held_ms = 0;

                point_xform_abs(x, rx[0], ry[0], &lx, &ly);

                /* The press's duration is stamped at its two edges and nowhere
                 * else. Only a release can fire a menu button, so that is the only
                 * report the number is ever used on -- and reading the clock on
                 * every report would put a clock read in the path this file exists
                 * to keep cheap. `was` is read before st_down is published below,
                 * because that is the previous report's state.
                 *
                 * `have_down`, not `down_ms > 0`, is what says a press is
                 * outstanding: a stamp is a clock reading, and a clock reading is
                 * allowed to be small. (That test is also what the 32-bit wrap
                 * above used to defeat.) */
                if (down[0] != was) {
                    if (down[0]) {
                        down_ms = shim_now_ms();
                        have_down = 1;
                    } else {
                        if (have_down)
                            held_ms = (long)(shim_now_ms() - down_ms);
                        have_down = 0;
                    }
                }
                st_down = down[0];
                st_cursor_x = lx;
                st_cursor_y = ly;
                pointer_report(down[0], lx, ly, menu_enabled, (int)held_ms);
                /* ...AND THE SECOND CONTACT, to the drawers and to nothing else.
                 * Reported after the primary, on the same SYN, because the two are
                 * independent presses and neither may be told about the other. It is
                 * called unconditionally rather than only while slot 1 is down: the
                 * RELEASE is the report a latched press cannot go without, and a
                 * report this path does not want costs one panel test and no state.
                 *
                 * AND ONLY ONCE IT HAS SAID WHERE IT IS. The input core drops an
                 * ABS value that has not changed (input_get_disposition's
                 * `absinfo[code].value == value` test), and it keeps ONE such value
                 * per axis -- so a second contact that lands on a row the first
                 * contact already reported can have that first position dropped
                 * entirely. Measured on the unit 2026-10-06 with work/poke.py's
                 * `bothfaders`: both fingers down at the same height delivered
                 * `MT_SLOT 1, MT_ID 2, SYN` and nothing else, leaving the second
                 * contact at (0,0) -- which is the LEFT drawer's own corner, so the
                 * operator's right hand moved the left fader. A contact whose
                 * position has never been reported is therefore not a contact: it is
                 * reported as up, and the drawers see a finger that simply starts
                 * moving from wherever the panel next says it is. */
                pointer_report_alt(down[1] && seen[1], x, rx[1], ry[1]);
                if (log_debug)                    /* Both ends of the transform, because they are not the same
                     * value: tscfake_emit reflects x (see tscfake.h's third
                     * quirk), so `wire` is what rbp consumes and `logical` is
                     * where the finger is. The raw->logical fit is checked
                     * against the middle pair; a tap landing on the wrong side
                     * of the screen is read from the last. */
                    pointsrc_log("pointsrc: abs raw=(%d,%d) logical=(%d,%d) wire=(%d,%d) down=%d",
                         rx[0], ry[0], lx, ly, tscfake_wire_x(lx), ly, down[0]);
            }
            break;
        default:
            break;
        }
    }
}

static void read_loop_rel(int fd, const struct point_xform *x)
{
    struct ev_input_event ev;
    int dx = 0, dy = 0, down = 0;
    int cx = POINT_LOGICAL_W / 2, cy = POINT_LOGICAL_H / 2;
    int dwell_ms = env_int("POINT_MIN_DWELL_MS", POINT_DWELL_DEFAULT_MS);
    unsigned long long down_ms = 0;

    for (;;) {
        ssize_t n = real_read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return;
        }
        switch (ev.type) {
        case EV_REL:
            if (ev.code == REL_X) dx += ev.value;
            else if (ev.code == REL_Y) dy += ev.value;
            break;
        case EV_KEY:
            if (ev.code == BTN_LEFT) {
                int nd = ev.value ? 1 : 0;
                if (nd && !down) {
                    /* Press: publish the press at the current position, which is
                     * where the operator is looking. */
                    down_ms = shim_now_ms();
                    down = 1;
                    st_down = 1;
                    pointer_report(1, cx, cy, menu_mouse, 0);
                    if (log_debug)
                        pointsrc_log("pointsrc: rel press at (%d,%d)", cx, cy);
                } else if (!nd && down) {
                    /* Release. A click shorter than the dwell is held open: rbp's
                     * TouchAdValueHysteresis needs a few frames of "down" to
                     * register a tap, and a fast physical click can occupy fewer
                     * than that, so without this the click is simply swallowed.
                     *
                     * `down` is 1 exactly when the press above stamped `down_ms`,
                     * so it is the flag here. The 32-bit wrap this loop shared with
                     * the abs reader made `elapsed_ms` fall back to `dwell_ms` about
                     * half the time -- harmless-looking, but it meant a genuinely
                     * fast click was never recognised as short and so was never
                     * extended, which is the one case this block exists for. */
                    long elapsed_ms = down ? (long)(shim_now_ms() - down_ms) : dwell_ms;
                    if (dwell_ms > 0 && elapsed_ms < dwell_ms) {
                        usleep((useconds_t)(dwell_ms - elapsed_ms) * 1000);
                        if (log_debug)
                            pointsrc_log("pointsrc: rel release held %ldms (dwell %d)",
                                 (long)dwell_ms - elapsed_ms, dwell_ms);
                    }
                    down = 0;
                    st_down = 0;
                    pointer_report(0, cx, cy, menu_mouse, (int)elapsed_ms);
                }
            }
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT && (dx != 0 || dy != 0)) {
                point_xform_rel(x, dx, dy, &cx, &cy);
                dx = dy = 0;
                st_cursor_x = cx;
                st_cursor_y = cy;
                /* Motion is published only while pressed. Emitting the released
                 * position on every movement would flood rbp with up-state
                 * frames — it needs the coordinate, not the traffic — and the
                 * press that follows carries the position anyway. */
                if (down) {
                    pointer_report(1, cx, cy, menu_mouse, 0);
                    if (log_debug)
                        pointsrc_log("pointsrc: rel drag to (%d,%d)", cx, cy);
                }
            }
            break;
        default:
            break;
        }
    }
}

static int open_pointer(int *kind_out, char *path_out, unsigned long path_len,
                        char *name_out, unsigned long name_len)
{
    int cands[2];
    int n, i;
    const char *match;

    n = kind_candidates(cands, want_kind());
    match = env_str("POINT_DEV", NULL);

    for (i = 0; i < n; i++) {
        int fd = open_matching_device(cands[i], match, path_out, path_len,
                                      name_out, name_len);
        if (fd >= 0) {
            *kind_out = cands[i];
            return fd;
        }
    }
    return -1;
}

static void *reader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int kind = POINT_KIND_NONE;
        char path[POINT_PATH_MAX];
        char name[POINT_NAME_MAX];
        int fd;

        path[0] = '\0';
        name[0] = '\0';
        fd = open_pointer(&kind, path, sizeof path, name, sizeof name);
        if (fd < 0) {
            /* Nothing attached, or what is attached is not a pointer. Retry
             * forever rather than exiting: plugging a mouse in later has to
             * work, and rbp is already running by then. */
            usleep(POINT_RESCAN_MS * 1000);
            continue;
        }

        st_kind = kind;
        snprintf(st_path, sizeof st_path, "%s", path);
        snprintf(st_name, sizeof st_name, "%s", name);
        log_attach(kind, path, name);

        {
            struct point_xform x;
            absorb_into_xform(&x, fd, kind);
            if (kind == POINT_KIND_ABS)
                read_loop_abs(fd, &x);
            else
                read_loop_rel(fd, &x);
        }

        real_close(fd);
        st_kind = POINT_KIND_NONE;
        st_down = 0;
        /* The device that was down is gone, so the next one's first press is a
         * first press. (Nothing to release: a QUANTIZE gesture is complete at
         * the press.) */
        touch_zone_reset();
        /* The same for the menu, and it matters more here: a panel left open by a
         * finger that went away with the device would be a panel nothing can
         * dismiss -- the gesture that closes it needs a press, and every press is
         * swallowed while it is open. */
        menu_reset();
        /* The same for the drawers, and for the same reason one level down: a
         * drawer left open by a finger that went away with the device is a drawer
         * nothing can dismiss and, worse, one that holds the overlay plane the band
         * needs. Both edges, because only one drawer can be open and a reset must
         * not depend on knowing which. */
        side_reset_all();
        side_log_transitions();
        /* ...and the UTILITY gesture, for the same reason as the drawers and with
         * the same failure behind it: a latched press whose finger went away with
         * the device would make the next press look like a continuation of it --
         * carrying the old anchor, and with it the wrong row for a tap. */
        util_reset();
        /* ...and the waveform swipe, whose failure is the ANCHOR: a press left
         * anchored by a finger that went away with the device would make the next
         * press look like a continuation of it, and the first move it reported would
         * be measured from a point on a screen that is gone. It is the mildest of the
         * five -- it can only ever cost an unwanted zoom -- and it costs nothing to
         * say, so it is said. */
        wave_reset();
        /* ...and the USB STOP chooser. Its failure is the one with a real
         * consequence: a box left up by a finger that went away with the device
         * cannot be dismissed at all -- every press is swallowed while it is up --
         * and its deadline would close it with a press that never came. */
        prompt_reset();
        /* ...and the BEAT FX panel's three controls. Both halves, because their failures
         * are the chooser's: a picker left up by a finger that went away with the device
         * cannot be dismissed at all -- every press is swallowed while it is up -- and its
         * deadline would close it with a press that never came; and a zone latch left
         * holding a press would make the next press look like a continuation of it. */
        fx_reset();
        fxlist_reset();
        /* ...and the BPM cell's momentary pad, whose failure is the zone half twice over:
         * a pad left engaged by a finger that went away with the device would keep this
         * loop ticking forever, and -- worse -- the restore it is holding is the only
         * thing that knows where the effect's level and rung BELONGED, so dropping the
         * gesture without dropping it would leave rbp's effect wherever the finger last
         * put it. */
        fxpad_reset();
        /* ...and the HOT CUE pad row's latch, which has the zone half of that failure and
         * not the picker half: a latched press would make the next press look like a
         * continuation of a gesture whose beginning rbp never saw, so the release that
         * follows would fire a pad the operator never aimed at. */
        hc_reset();
        /* ...and the bend, which is the one piece of drawer state rbp holds rather
         * than this process: the finger went away without a release, so no STOP edge
         * will ever be delivered and the track would keep sliding. */
        side_bend_sync();
        if (log_debug)
            pointsrc_log("pointsrc: %s went away; rescanning", path);
    }
    return NULL;
}

int pointsrc_start(void)
{
    log_debug = env_flag("POINT_DEBUG", 0);
    quantize_enabled = env_flag("POINT_QUANTIZE_TAP", 1);
    menu_enabled = env_flag("POINT_MENU", 1);
    menu_mouse = env_flag("POINT_MENU_MOUSE", 0);
    fx_touch = env_flag("POINT_FX_TOUCH", 1);
    /* The HOT CUE pad row's own switch, for fx_touch's reason: it is rbp's own drawing
     * rather than a shim surface. Off means rbp's stream is untouched at those sixteen
     * cells, byte for byte. */
    hc_touch = env_flag("POINT_HOTCUE_TOUCH", 1);
    status_touch = env_flag("POINT_STATUS_TOUCH", 0);
    /* menu_key_eff is built even when the menu is off, so the log states the
     * table it would use rather than staying silent about a knob that was set. */
    menu_keys_init();
    if (!menu_enabled)
        menu_reset();      /* off means closed, whatever the state was */
    if (!menu_enabled)
        side_reset_all();  /* and the same for the drawers: POINT_MENU=0 is rbp's
                            * untouched stream, and a drawer cannot be opened while
                            * it is set -- the funnel never offers it a report */
    if (!menu_enabled)
        side_bend_sync();  /* and stop any bend a held nudge cell left running:
                            * this runs at start-up, where "the drawer was reset"
                            * has to mean "so was the track" */
    if (!menu_enabled)
        util_reset();      /* and the UTILITY gesture: it is offered the same
                            * reports as the rest of the menu, so POINT_MENU=0
                            * turns it off too and must not leave a press latched
                            * for whenever it is turned back on */
    if (!menu_enabled)
        wave_reset();      /* and the waveform swipe: it is offered the same reports
                            * and gated on the same switch, so POINT_MENU=0 must leave
                            * nothing anchored behind it either */
    if (!menu_enabled)
        prompt_reset();    /* and the USB STOP chooser, which is raised from the
                            * band's own seventh column: with the band off there is
                            * no way to raise it, and a box left up from a state
                            * before would swallow every report in the file */
    if (!fx_touch) {
        /* And the Beat FX panel's three controls, which are rbp's drawing rather than a
         * shim surface and so have their OWN switch: with POINT_FX_TOUCH=0 the panel
         * belongs to rbp entirely, and a picker left up from a state before would
         * swallow every report in the file -- the same failure the chooser's line above
         * names, one surface over. */
        fx_reset();
        fxlist_reset();
        fxpad_reset();     /* ...and the BPM cell's pad, which is rbp's drawing too and so
                            * shares this switch rather than POINT_MENU's: an engagement
                            * left from a run where the panel was live would keep the loop
                            * ticking and hold the only copy of where the effect belonged */
    }
    if (!hc_touch)
        hc_reset();        /* and the HOT CUE pad row, which is also rbp's own drawing and
                            * so shares this shape rather than POINT_MENU's: a latch left
                            * holding a press from a run where the row was live would make
                            * the next press look like a continuation, and fire a pad the
                            * operator never aimed at */

    if (reader_started)
        return 0;
    {
        static int stems_started;
        const char *sp = getenv("RX3_STEMS_PADS");
        pthread_t stems_tid;

        if (!stems_started && sp && strcmp(sp, "touch") == 0 && is_rbp_process() &&
            (stems_started = 1) &&
            pthread_create(&stems_tid, NULL, stems_thread, NULL) == 0)
            pthread_detach(stems_tid);
    }
    {
        static int bfx_started;
        pthread_t bfx_tid;

        if (!bfx_started && is_rbp_process() && (bfx_started = 1) &&
            pthread_create(&bfx_tid, NULL, bfx_thread, NULL) == 0)
            pthread_detach(bfx_tid);
    }
    /* POINT_KIND=none means "no pointer at all" — the display-only bring-up
     * setting. Returning before creating the thread keeps that case free of a
     * thread that would only ever sleep. */
    if (want_kind() == POINT_KIND_NONE)
        return 0;
    if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0)
        return -1;
    reader_started = 1;
    return 0;
}

/* The menu's state for the status line. "off" reports the *gate*, so it reads the
 * same whether the panel happens to be closed or was left open by an earlier
 * run; whether an open panel is really on the glass is menu_draw.c's question and
 * not one a text field can answer.
 *
 * The drawers are folded into the same word because they are the same gate
 * (POINT_MENU) and because the two are mutually exclusive on the glass -- the band
 * and a drawer cannot both hold the one overlay plane. `l`/`r` name which edge is
 * out, so a status line taken during a drill says which drawer opened and not just
 * that one did. */
static const char *menu_status_str(char *buf, unsigned long buflen)
{
    if (!menu_enabled)
        return "off";
    if (side_any_open())
        snprintf(buf, buflen, "drawer=%s", side_is_open(SZ_LEFT) ? "l" : "r");
    else if (!menu_is_open())
        return "closed";
    else
        snprintf(buf, buflen, "open,pressed=%d", menu_pressed());
    return buf;
}

void pointsrc_status(char *buf, unsigned long buflen)
{
    const char *kindstr;
    char ms[24];

    switch (st_kind) {
    case POINT_KIND_ABS: kindstr = "abs"; break;
    case POINT_KIND_REL: kindstr = "rel"; break;
    default:             kindstr = "none"; break;
    }

    if (st_kind == POINT_KIND_NONE) {
        snprintf(buf, buflen, "kind=%s (no pointer attached) menu=%s",
                 env_str("POINT_KIND", "auto"), menu_status_str(ms, sizeof ms));
        return;
    }
    snprintf(buf, buflen, "kind=%s dev=%s name='%s' at=(%d,%d) down=%d menu=%s",
             kindstr, st_path, st_name, st_cursor_x, st_cursor_y, st_down,
             menu_status_str(ms, sizeof ms));
}

int pointsrc_cursor(int *logical_x, int *logical_y, int *down)
{
    /* A relative device is the only case that needs an arrow: an absolute one
     * reports where it is being touched, so a cursor would be a second, lagging
     * mark on the screen saying the same thing. Returning 0 here is also how the
     * compositor learns that the mouse was unplugged and the arrow it drew has
     * to come back off. */
    if (st_kind != POINT_KIND_REL)
        return 0;
    if (logical_x)
        *logical_x = st_cursor_x;
    if (logical_y)
        *logical_y = st_cursor_y;
    if (down)
        *down = st_down;
    return 1;
}
