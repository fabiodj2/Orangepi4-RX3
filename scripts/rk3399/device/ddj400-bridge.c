/* ddj400-bridge.c - Pioneer DDJ-400 (USB MIDI) -> XDJ-RX3 rekordbox engine (rbp)
 *
 * `rbp` (the standalone rekordbox player) has no MIDI input: it reads its front
 * panel from Pioneer microcontrollers and exposes an internal key manager.
 * `keyshim.so` (LD_PRELOADed into rbp) already turns 12-byte key records from
 * /tmp/rb-keys.fifo into IKeyManager::sendKey() calls; this bridge adds the
 * value/relative half of the API through /tmp/rb-ctrl.fifo (24-byte records).
 *
 *   DDJ-400 --USB MIDI--> this bridge --24B records--> /tmp/rb-ctrl.fifo
 *                          (host, hard-float musl)        |
 *                                                         v
 *                       keyshim.so -> IKeyManager::sendKey(key, op, ch, param, f, l)
 *
 * Runs on the Chromebit *host* rootfs (postmarketOS armv7, musl), NOT in the
 * soft-float chroot: it only needs /dev/snd/midiC*D* and the shared /tmp.
 * No libasound dependency - the ALSA rawmidi device is read directly, so it
 * builds natively with the device's own gcc.
 *
 * MIDI map source: Pioneer/AlphaTheta "DDJ-400 List of MIDI messages v1.00"
 * (DDJ-400_MIDI_Message_List_E1.pdf), cross-checked against the Mixxx
 * Pioneer-DDJ-400 mapping.  Keycodes and op semantics come from the
 * live-verified PrimeBox knobshim2.c (docs/09-input.md):
 *
 *   op 0 PRESS / op 2 RELEASE : param/f/l unused
 *   op 4 ROTATE: param = 10-bit absolute (faders, EQ, crossfader, cue knobs)
 *                or relative delta (browse knob); f = normalised / speed
 *   op 5 VALUE : param = 10-bit, f = normalised (colour FX, Beat FX depth,
 *                tempo slider [-1..+1])
 *
 * Usage:
 *   ddj400-bridge [-v] [-s] [-d /dev/snd/midiC1D0] [-f /tmp/rb-ctrl.fifo]
 *                 [-J pulses_per_rev] [-F]
 *     -v  verbose (log every mapped event)
 *     -s  sniff mode: log every MIDI message with its decoded name and send
 *         nothing (use this to verify/repair a mapping against new hardware)
 *     -l  list the mapping table and exit
 *     -J  jog pulses per revolution (default 720)
 *     -F  at startup, select FILTER as the Sound Color FX type on both chans
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* rbp / XDJ-RX3 keys (docs/09-input.md, PrimeBox MAPPING.md)          */
/* ------------------------------------------------------------------ */
#define OP_PRESS      0
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5

#define K_SELECTOR    0x420c
#define K_SOURCE      0x0201
#define K_MENU        0x0206
#define K_SHORTCUT    0x0210  /* UiKey_Shortcut, browse mode 10 -- opens on a
                                * plain press+release, same shape as K_MENU
                                * above (see rbpi4b's rbp_abi.h for the full
                                * derivation off rbp's live dispatch table). */
#define K_BACK        0x420d
#define K_LOAD        0x4311   /* + deck channel */
#define K_PLAY        0x4101
#define K_QUANTIZE   0x410b
#define K_CUE         0x4102
#define K_AUTOCUE     0x410a   /* keycode id from github.com/mutlisensor/Rx3-flx4
                                 * (v1.19 firmware, dumped as "TimeMode/ACue").
                                 * Genuinely dual-purpose, confirmed by disassembling
                                 * ui::PlayerInnards::onKey_TimeAutoCue() in our own
                                 * (unstripped) v1.20 rbp: it only calls
                                 * DjEngineIF::setAutoCueOn() when an internal
                                 * press-duration flag == 1, else no-ops and a separate
                                 * handler flips TimeMode (REMAIN/TIME display) instead.
                                 * Live-confirmed 2026-09-30: a quick tap toggles
                                 * TimeMode, holding it toggles AUTO CUE -- same as the
                                 * real XDJ-RX3 button's own behavior, not a bridge
                                 * artifact. */
#define K_VINYL      0x4104
#define K_SYNC        0x4112
#define K_JOG_TOUCH   0x4306
#define K_JOG_ROT     0x4305
#define K_SEARCHFWD   0x411f   /* found via onPhysicalKey() dispatch in rbp
                                 * (not stripped): press/release, hold to
                                 * scan forward -- adjacent to PAD1..PAD8
                                 * (0x4117-0x411e) in the keycode space. */
#define K_SEARCHREV   0x4120
#define K_TEMPO_RANGE 0x4107
#define K_MASTER_TEMPO 0x4108
#define K_TEMPO_SLIDER 0x4109
#define K_ALOOP       0x4114
#define K_HOTCUE      0x4113
#define K_CUEDELETE   0x4124
#define K_SLIPLOOP    0x4115
#define K_BEATJUMP    0x4116
#define K_PAD1        0x4117   /* .. K_PAD1+7 */
#define K_LOOPIN      0x410c
#define K_LOOPOUT     0x410d
#define K_RELOOP      0x410e
#define K_REV         0x410f
#define K_MASTER      0x4111
#define K_TRIM        0x5019
#define K_EQH         0x501a
#define K_EQM         0x501b
#define K_EQL         0x501c
#define K_FADER       0x501e
#define K_XFADER      0x6017
#define K_FADERCURVE  0x6018   /* crossfader curve select, op5 param: confirmed
                                 * live 0=THRU, 1=normal, 2=sharp/cut, 3=another
                                 * distinct curve ("wide open") */
#define K_HPMIX       0x4405
#define K_HPLEVEL     0x4406
#define K_COLOR       0x509d
#define K_FILTER      0x50a6
#define K_COLOR_TYPE  0x50a7   /* not a real rbp keycode -- keyshim-only,
                                 * selects the Sound Color FX type (1-6
                                 * confirmed live 2026-09-27; 0 and 7+ are
                                 * no-ops). param = type, ch = deck. */
#define K_BFXTYPE     0x448b
#define K_BFXCH       0x448c
#define K_BFX         0x448d
#define K_DEPTH       0x448f
#define K_BEATPREV    0x4490
#define K_BEATNEXT    0x4491
#define K_TAP         0x4492
#define K_CALLNEXT    0x4322
#define K_CALLPREV    0x4323
#define K_HPCUECH     0x5020   /* per-channel headphone cue toggle; param ch=1/2 */
#define K_MASTERCUE   0x4407   /* master headphone cue toggle */
#define K_SEARCH      0x0205   /* on-screen keyboard / text search */

#define CH_GLOBAL     1

/* DDJ-400 MIDI channels (0-based nibble of the status byte) */
#define MC_DECK1      0        /* "DECK 1"  */
#define MC_DECK2      1        /* "DECK 2"  */
#define MC_FX         4        /* "EFFECT" ch 5 */
#define MC_MIXER      6        /* ch 7: browser, crossfader, master, phones, filter */
#define MC_PAD1       7        /* pads deck 1, no SHIFT   */
#define MC_PAD1_SH    8        /* pads deck 1, with SHIFT */
#define MC_PAD2       9        /* pads deck 2, no SHIFT   */
#define MC_PAD2_SH   10        /* pads deck 2, with SHIFT */

/* ------------------------------------------------------------------ */
static int   opt_verbose = 0;
static int   opt_sniff   = 0;
static int   opt_filter_init = 0;
static const char *fifo_path = "/tmp/rb-ctrl.fifo";
static const char *midi_dev  = NULL;
static char midi_node_path[128];
static float jog_ppr = 720.0f;    /* DDJ-400 jog pulses per revolution */
static int   jog_idle_ms = 60;     /* emit speed 0 after this idle time  */

static int fifo_fd = -1;

/* Keepalive: with no physical input, exercise the USB-MIDI endpoint so
 * nothing upstream (the kernel's own USB autosuspend, or the DDJ-400
 * itself) can decide the link is idle and let it go to standby. MIDI
 * Active Sensing (0xFE) is the standard, content-free byte for exactly
 * this -- single byte, no channel, no LED or UI effect, and already
 * silently dropped by our own read loop's `b >= 0xF8` filter if the
 * DDJ-400 ever echoes one back. Off with DDJ400_KEEPALIVE=0; the FLX4
 * row in the older controllers.c framework did the same job with its own
 * CTRL_KEEPALIVE (docs/08-controls.md in RX3-RBPI4B-RK3399), unrelated
 * code but the same idea. */
static long long now_ms(void);
static void logmsg(const char *fmt, ...);
static int opt_keepalive = 1;
#define KEEPALIVE_IDLE_MS   8000LL   /* only once nothing physical for this long */
#define KEEPALIVE_PERIOD_MS 5000LL   /* ...then resend this often while idle */
static long long g_last_input_ms;
static long long g_last_keepalive_ms;

static void keepalive_note_input(void)
{
    g_last_input_ms = now_ms();
}

static void keepalive_tick(int fd)
{
    long long now;
    unsigned char active_sensing = 0xFE;

    if (!opt_keepalive)
        return;
    now = now_ms();
    if (now - g_last_input_ms < KEEPALIVE_IDLE_MS)
        return;
    if (now - g_last_keepalive_ms < KEEPALIVE_PERIOD_MS)
        return;
    g_last_keepalive_ms = now;
    if (write(fd, &active_sensing, 1) != 1 && opt_verbose)
        logmsg("ddj400: keepalive write: %s\n", strerror(errno));
    else if (opt_verbose)
        logmsg("ddj400: keepalive (idle %lldms)\n", now - g_last_input_ms);
}

#define INTERFACE_TOGGLE_REQUEST \
    "/run/rx3-interface-toggle.request"
#define INTERFACE_TOGGLE_HOLD_MS 5000LL

static long long interface_toggle_pressed_ms = -1;

/* BROWSE push is overloaded: 1st press opens the library, while the browser
 * is open it selects/enters (K_SELECTOR) instead. LOAD-ing a track or
 * backing out (SHIFT+push) returns to the deck view, so they reset this
 * back to "closed". SHIFT+push (handle_interface_toggle_note()) also reads
 * this: SOURCE (device picker) from the deck view, BACK once browsing. */
static int browser_open;
static int browser_press_key = K_BACK;
static int interface_toggle_fired;

/* SHIFT (note 0x3f) held state per deck, tracked so other channels' note
 * handlers (e.g. MASTER CUE on MC_MIXER) can tell whether SHIFT is held --
 * the hardware only auto-renumbers notes *within the same MIDI channel*, so
 * a deck's SHIFT does not by itself change what MC_MIXER sends. */
static int shift_held[2];

/* MASTER REC (item 7, 2026-10-15): hold SHIFT alone for 3s to toggle the
 * native recording on/off, by user request ("aperte e pressione apenas
 * SHIFT por 3 segundos"). The native call itself lives in rbpi4b's own
 * fbshim.so (pointsrc.c master_rec_toggle_tick()), reached the same way the
 * screenshot tour's touch injection is -- a request file in the same /tmp
 * directory as rb-ctrl.fifo (rx3-master-rec-toggle.request, derived from
 * fifo_path the same way vu_path is, see master_rec_init_path()). "Alone"
 * is enforced by canceling the hold the moment any other note/CC arrives
 * while SHIFT is down (main loop, right after handle_note()/handle_cc()) --
 * without that, any long combo held with SHIFT (e.g. SHIFT+FX ch select)
 * would also toggle recording after 3s. */
#define MASTER_REC_HOLD_MS 3000LL
static long long master_rec_hold_started_ms = -1;
static int master_rec_hold_fired;
static char master_rec_toggle_path[512];

/* SEARCH (SHIFT+MASTER CUE): toggles the on-screen keyboard. While active,
 * the MASTER CUE LED blinks at 1Hz so it's obvious search is open. */
static int search_active;
static long long search_blink_ms;
static int search_led_on;


struct ctrl_ev {
    int32_t key;
    int32_t ch;
    int32_t op;
    int32_t param;
    float   f;
    int32_t l;
};

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---------------- FIFO output ---------------- */
static void send_ctrl(int key, int op, int ch, int param, float f, int l)
{
    struct ctrl_ev ev;
    size_t off = 0;

    if (opt_sniff)
        return;

    ev.key = key; ev.op = op; ev.ch = ch; ev.param = param; ev.f = f; ev.l = l;

    if (fifo_fd < 0)
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0)
        return;

    while (off < sizeof(ev)) {
        ssize_t n = write(fifo_fd, (char *)&ev + off, sizeof(ev) - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            static int warned = 0;
            if (!warned) {
                logmsg("ddj400: no reader on %s (rbp/keyshim not running?)\n", fifo_path);
                warned = 1;
            }
            return;
        } else {
            close(fifo_fd);
            fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
            return;
        }
    }
    if (opt_verbose)
        logmsg("  -> key=0x%04x op=%d ch=%d param=%d f=%.3f l=%d\n",
               key, op, ch, param, (double)f, l);
}

static void send_tap(int key, int ch)
{
    send_ctrl(key, OP_PRESS, ch, 0, 0.0f, 0);
    send_ctrl(key, OP_RELEASE, ch, 0, 0.0f, 0);
}

/* 7-bit CC (0..127) -> RX3 10-bit value (0..1023), as in knobshim2 */
static int cc_to_10bit(int v)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    return (v << 3) | (v >> 4);
}

/* ---------------- 14-bit CC pairs ---------------- */
#define N14 16
struct cc14 {
    int ch;
    int msb_cc;      /* LSB is always msb_cc + 0x20 on the DDJ-400 */
    int key;
    int op;          /* OP_ROTATE for faders/knobs, OP_VALUE for colour/depth */
    int send_ch;
    int signed_norm; /* tempo slider: normalise to -1..+1 instead of 0..1 */
    int msb;
    int have_msb;
    int last10;      /* dedupe / last value actually forwarded */

    /* Reference value for mixer_state_save(): the loaded-or-default value
     * to persist if this control is never touched this run, so an
     * untouched control doesn't lose its last known position across a
     * restart. Applied immediately on physical input like any other
     * control -- no pickup/soft-takeover gating (removed by user request:
     * felt unresponsive waiting for the physical position to cross it). */
    int target10;
};
static struct cc14 cc14[N14];
static int cc14_n = 0;

static void add_cc14(int ch, int msb_cc, int key, int op, int send_ch,
                      int signed_norm, int default10)
{
    if (cc14_n >= N14)
        return;
    cc14[cc14_n].ch = ch;
    cc14[cc14_n].msb_cc = msb_cc;
    cc14[cc14_n].key = key;
    cc14[cc14_n].op = op;
    cc14[cc14_n].send_ch = send_ch;
    cc14[cc14_n].signed_norm = signed_norm;
    cc14[cc14_n].last10 = -1;
    cc14[cc14_n].target10 = default10;
    cc14_n++;
}

/* ---------------- mixer state persistence ----------------
 * The DDJ-400's pots/faders are plain absolute MIDI controls: they only
 * transmit on physical movement, so neither the bridge nor the engine has
 * any way to query a control's current position on startup (this is a
 * hardware/firmware limitation of the controller itself, not something we
 * can read around -- confirmed against the official MIDI message list).
 *
 * keyshim's mixer_defaults() primes the engine with fixed values (fader
 * up, trim/EQ/crossfader centred) since the engine's own native defaults
 * are worse (silence, hard-left). Those fixed values are also this file's
 * fallback "first ever boot" target below.
 *
 * To do better across a normal restart/reboot (controller untouched the
 * whole time), we persist the last value actually forwarded for each
 * control to disk (sibling of the control FIFO, so same host-side real
 * storage keyshim's other telemetry files already use -- NOT /tmp
 * tmpfs). On the next boot that persisted value replaces the hardcoded
 * default as the pickup target. The soft-takeover logic in handle_cc14()
 * (and the FILTER handling in handle_cc()) is kept as a safety net
 * regardless of where the target came from, in case the controller was
 * moved while the service/system was down (or this is genuinely the
 * first boot and no file exists yet). */
#define MIXER_STATE_MAGIC   0x52334d58UL /* "R3MX" */
#define MIXER_STATE_VERSION 1
#define MIXER_STATE_MAX     (N14 + 2)    /* cc14[] entries + FILTER ch1/ch2 */

struct mixer_state_entry {
    int32_t key;
    int32_t send_ch;
    int32_t v10;
};

struct mixer_state_file {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    struct mixer_state_entry entries[MIXER_STATE_MAX];
};

static void stems_pad_sibling_path(char *output, size_t capacity,
                                   const char *name);

static char mixer_state_path[512];

/* FILTER (Sound Color FX) is a plain 7-bit CC handled directly in
 * handle_cc(), not part of cc14[] -- tracked here so it gets the same
 * persistence treatment. Index 0 = deck 1, 1 = deck 2. target10 is only
 * the reference value saved if this control is never touched this run. */
static int filter_target10[2]   = { 512, 512 };
static int filter_last10[2]     = { -1, -1 };

static void mixer_state_init_path(void)
{
    stems_pad_sibling_path(mixer_state_path, sizeof(mixer_state_path),
                           "rx3-mixer-state.bin");
    if (opt_verbose)
        logmsg("ddj400: mixer state %s\n", mixer_state_path);
}

static long long now_ms(void);

/* Debounced: a fast knob/fader sweep can change a value dozens of times a
 * second, and this used to write the whole struct to disk inline on every
 * single change -- confirmed live (2026-09-27) as a likely contributor to
 * audible master-output clicks (real disk I/O competing with the audio
 * thread on this SBC), most noticeable sweeping the FILTER knob. Callers
 * just mark state dirty now; mixer_state_tick() flushes at most once per
 * MIXER_STATE_FLUSH_MS from the main poll loop. */
#define MIXER_STATE_FLUSH_MS 300
static int mixer_state_dirty;
static long long mixer_state_dirty_since;

static void mixer_state_flush(void)
{
    struct mixer_state_file file;
    int n = 0;

    memset(&file, 0, sizeof(file));
    file.magic = MIXER_STATE_MAGIC;
    file.version = MIXER_STATE_VERSION;

    for (int i = 0; i < cc14_n && n < MIXER_STATE_MAX; i++) {
        struct cc14 *s = &cc14[i];
        file.entries[n].key = s->key;
        file.entries[n].send_ch = s->send_ch;
        file.entries[n].v10 = (s->last10 >= 0) ? s->last10 : s->target10;
        n++;
    }
    for (int ch = 0; ch < 2 && n < MIXER_STATE_MAX; ch++) {
        file.entries[n].key = K_COLOR;
        file.entries[n].send_ch = ch + 1;
        file.entries[n].v10 =
            (filter_last10[ch] >= 0) ? filter_last10[ch] : filter_target10[ch];
        n++;
    }
    file.count = (uint16_t)n;

    int fd = open(mixer_state_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)!write(fd, &file, sizeof(file));
        close(fd);
    }
    mixer_state_dirty = 0;
}

static void mixer_state_save(void)
{
    if (!mixer_state_dirty) {
        mixer_state_dirty = 1;
        mixer_state_dirty_since = now_ms();
    }
}

static void mixer_state_tick(void)
{
    if (mixer_state_dirty &&
        now_ms() - mixer_state_dirty_since >= MIXER_STATE_FLUSH_MS)
        mixer_state_flush();
}

static void mixer_state_load(void)
{
    struct mixer_state_file file;
    int fd = open(mixer_state_path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, &file, sizeof(file));
    close(fd);
    if (n != (ssize_t)sizeof(file) ||
        file.magic != MIXER_STATE_MAGIC ||
        file.version != MIXER_STATE_VERSION)
        return;

    int count = file.count;
    if (count > MIXER_STATE_MAX)
        count = MIXER_STATE_MAX;

    for (int e = 0; e < count; e++) {
        struct mixer_state_entry *ent = &file.entries[e];

        if (ent->key == K_COLOR) {
            if (ent->send_ch == 1)
                filter_target10[0] = ent->v10;
            else if (ent->send_ch == 2)
                filter_target10[1] = ent->v10;
            continue;
        }
        for (int i = 0; i < cc14_n; i++) {
            if (cc14[i].key == ent->key && cc14[i].send_ch == ent->send_ch) {
                cc14[i].target10 = ent->v10;
                break;
            }
        }
    }

    if (opt_verbose)
        logmsg("ddj400: loaded %d persisted mixer control(s) from %s\n",
               count, mixer_state_path);
}

static int handle_cc14(int ch, int cc, int val)
{
    for (int i = 0; i < cc14_n; i++) {
        struct cc14 *s = &cc14[i];
        if (s->ch != ch)
            continue;
        if (cc == s->msb_cc) {
            s->msb = val;
            s->have_msb = 1;
            return 1;
        }
        if (cc == s->msb_cc + 0x20) {
            if (!s->have_msb)
                return 1;                    /* MSB not seen yet */
            int pos = (s->msb << 7) | val;   /* 0..16383 */
            float norm;
            int v10;
            if (s->signed_norm) {
                norm = ((float)pos / 16383.0f) * 2.0f - 1.0f;
                v10 = (int)((norm + 1.0f) * 511.5f + 0.5f);
            } else {
                norm = (float)pos / 16383.0f;
                v10 = (int)(norm * 1023.0f + 0.5f);
            }
            if (v10 < 0) v10 = 0;
            if (v10 > 1023) v10 = 1023;

            if (v10 != s->last10) {
                s->last10 = v10;
                send_ctrl(s->key, s->op, s->send_ch, v10, norm, pos);
                mixer_state_save();
                if (opt_verbose)
                    logmsg("  key=0x%04x ch%d pos=%d norm=%.3f v10=%d\n",
                           s->key, s->send_ch, pos, (double)norm, v10);
            }
            return 1;
        }
    }
    return 0;
}

static void build_cc14(void)
{
    /* mixer channel strips (deck channel == send channel).
     * Live-verified on this engine: faders/EQ/trim/crossfader/cue knobs are
     * driven with OP_ROTATE + 10-bit param (+ normalised float).
     *
     * The trailing arg is only the fallback value mixer_state_save()
     * persists if this control is never touched this run (overwritten by
     * mixer_state_load() right after this, if a persisted value exists):
     * it matches keyshim's mixer_defaults() (fader up, trim/EQ/crossfader
     * centred) for the controls that function actually primes, and a
     * best-guess centre for the ones it doesn't touch (HP mix/level, beat
     * FX depth). Physical input is applied immediately either way -- it's
     * not a pickup/soft-takeover target, so exactness here isn't
     * load-bearing. */
    add_cc14(MC_DECK1, 0x13, K_FADER,  OP_ROTATE, 1, 0, 1023);
    add_cc14(MC_DECK2, 0x13, K_FADER,  OP_ROTATE, 2, 0, 1023);
    add_cc14(MC_DECK1, 0x04, K_TRIM,   OP_ROTATE, 1, 0, 512);
    add_cc14(MC_DECK2, 0x04, K_TRIM,   OP_ROTATE, 2, 0, 512);
    add_cc14(MC_DECK1, 0x07, K_EQH,    OP_ROTATE, 1, 0, 512);
    add_cc14(MC_DECK2, 0x07, K_EQH,    OP_ROTATE, 2, 0, 512);
    add_cc14(MC_DECK1, 0x0B, K_EQM,    OP_ROTATE, 1, 0, 512);
    add_cc14(MC_DECK2, 0x0B, K_EQM,    OP_ROTATE, 2, 0, 512);
    add_cc14(MC_DECK1, 0x0F, K_EQL,    OP_ROTATE, 1, 0, 512);
    add_cc14(MC_DECK2, 0x0F, K_EQL,    OP_ROTATE, 2, 0, 512);
    /* tempo slider: OP_VALUE, signed normalised [-1 = slow .. +1 = fast] */
    add_cc14(MC_DECK1, 0x00, K_TEMPO_SLIDER, OP_VALUE, 1, 1, 512);
    add_cc14(MC_DECK2, 0x00, K_TEMPO_SLIDER, OP_VALUE, 2, 1, 512);
    /* master section (ch 7) */
    add_cc14(MC_MIXER, 0x1F, K_XFADER,  OP_ROTATE, CH_GLOBAL, 0, 512);
    add_cc14(MC_MIXER, 0x0C, K_HPMIX,   OP_ROTATE, CH_GLOBAL, 0, 512);
    add_cc14(MC_MIXER, 0x0D, K_HPLEVEL, OP_ROTATE, CH_GLOBAL, 0, 512);
    /* LEVEL/DEPTH: CC14 0x02/0x22, resolução 0..16383. */
    add_cc14(MC_FX, 0x02, K_DEPTH, OP_VALUE, CH_GLOBAL, 0, 512);
}

/* ---------------- jog wheels ---------------- */
struct jog {
    int midi_ch;
    int send_ch;
    unsigned int vpos;
    int moving;
    long long last_ms;
    long long last_emit_ms;
};
static struct jog jogs[2];

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void jog_delta(int midi_ch, int delta)
{
    struct jog *s = NULL;
    for (int i = 0; i < 2; i++)
        if (jogs[i].midi_ch == midi_ch)
            s = &jogs[i];
    if (!s || delta == 0)
        return;

    long long t = now_ms();
    float dt = (float)(t - s->last_ms) / 1000.0f;
    s->last_ms = t;
    if (dt < 0.0005f)
        dt = 0.0005f;

    s->vpos = (s->vpos + (unsigned int)delta) & 0xFFFFu;
    float speed = (float)delta / jog_ppr / dt;
    if (speed > 8.0f) speed = 8.0f;
    if (speed < -8.0f) speed = -8.0f;
    s->moving = 1;
    s->last_emit_ms = t;

    send_ctrl(K_JOG_ROT, OP_ROTATE, s->send_ch, 0, speed, (int)s->vpos);
    if (opt_verbose)
        logmsg("  jog ch%d delta=%d speed=%.2f rev/s pos=%u\n",
               midi_ch, delta, (double)speed, s->vpos);
}

static void jog_tick(void)
{
    long long t = now_ms();
    for (int i = 0; i < 2; i++) {
        struct jog *s = &jogs[i];
        if (!s->moving)
            continue;
        if (t - s->last_emit_ms < jog_idle_ms)
            continue;
        s->moving = 0;
        send_ctrl(K_JOG_ROT, OP_ROTATE, s->send_ch, 0, 0.0f, (int)s->vpos);
    }
}

/* SHIFT+platter (CC 0x29): unlike the plain jog CCs, this is the DDJ-400's
 * "search" gesture and maps to K_SEARCHFWD/K_SEARCHREV -- discrete
 * press/release "hold to scan" keys (found live in rbp's onPhysicalKey()
 * dispatch, not stripped), not a continuous rotate like K_JOG_ROT. A
 * direction change releases the other key first; releasing happens after
 * SEARCH_IDLE_MS with no further delta, since the DDJ-400 never sends a
 * discrete "stopped turning" event of its own. */
#define SEARCH_IDLE_MS 150
struct search_state {
    int midi_ch;
    int send_ch;
    int dir;            /* 0 = released, 1 = fwd, -1 = rev */
    long long last_ms;
};
static struct search_state searches[2];

static void search_release(struct search_state *s)
{
    send_ctrl(s->dir > 0 ? K_SEARCHFWD : K_SEARCHREV, OP_RELEASE,
              s->send_ch, 0, 0.0f, 0);
    s->dir = 0;
}

static void search_delta(int midi_ch, int delta)
{
    struct search_state *s = NULL;
    for (int i = 0; i < 2; i++)
        if (searches[i].midi_ch == midi_ch)
            s = &searches[i];
    if (!s || delta == 0)
        return;

    int dir = (delta > 0) ? 1 : -1;
    s->last_ms = now_ms();

    if (s->dir == dir)
        return;
    if (s->dir != 0)
        search_release(s);

    send_ctrl(dir > 0 ? K_SEARCHFWD : K_SEARCHREV, OP_PRESS,
              s->send_ch, 0, 0.0f, 0);
    s->dir = dir;
    if (opt_verbose)
        logmsg("  search ch%d dir=%s\n", midi_ch, dir > 0 ? "fwd" : "rev");
}

static void search_tick(void)
{
    long long t = now_ms();
    for (int i = 0; i < 2; i++) {
        struct search_state *s = &searches[i];
        if (s->dir != 0 && t - s->last_ms >= SEARCH_IDLE_MS)
            search_release(s);
    }
}

/* ---------------- pads ---------------- */
/*
 * Formato real confirmado na DDJ-400:
 *
 *   0x00..0x07 = HOT CUE
 *   0x10..0x17 = PAD FX 1
 *   0x20..0x27 = BEAT JUMP
 *   0x30..0x37 = SAMPLER
 *   0x40..0x47 = KEYBOARD
 *   0x50..0x57 = PAD FX 2
 *   0x60..0x67 = BEAT LOOP
 *   0x70..0x77 = KEY SHIFT
 *
 * O engine RX3 possui quatro bancos nativos. Os modos sem equivalente
 * exato são encaminhados ao banco nativo mais próximo.
 */
static int pad_bank[2] = { -1, -1 };

static int pad_rx3_bank(int ddj_mode)
{
    switch (ddj_mode) {
    case 0: return 0; /* HOT CUE   -> HOT CUE (direct) */
    case 1: return -1; /* PAD FX 1  -> Sound Color FX type select, handled
                           entirely in handle_pad()/led_handle_input(); never
                           reaches here (was wrongly falling back to SLIP
                           LOOP, dead/redundant with SAMPLER below). */
    case 2: return 3; /* BEAT JUMP -> BEAT JUMP (direct) */
    case 3: return 2; /* SAMPLER   -> SLIP LOOP (fallback, no equivalent) */
    case 4: return 0; /* KEYBOARD  -> HOT CUE (fallback, no equivalent) */
    case 5: return 2; /* PAD FX 2  -> SLIP LOOP (fallback; unused: STEMS
                          page is handled separately in handle_pad()) */
    case 6: return 1; /* BEAT LOOP -> BEAT LOOP (direct -- was wrongly 2/SLIP
                          LOOP, swapped with SAMPLER above; confirmed live
                          via raw MIDI capture that note 0x60-0x67 really is
                          BEAT LOOP, matching the RX3's own native bank) */
    case 7: return 3; /* KEY SHIFT -> BEAT JUMP (fallback, no equivalent) */
    default:
        return -1;
    }
}

static const char *pad_mode_name(int mode)
{
    static const char *names[8] = {
        "HOT CUE",
        "PAD FX 1",
        "BEAT JUMP",
        "SAMPLER",
        "KEYBOARD",
        "PAD FX 2",
        "BEAT LOOP",
        "KEY SHIFT"
    };

    if (mode < 0 || mode > 7)
        return "UNKNOWN";

    return names[mode];
}

static void pad_select_bank(int deck, int ddj_mode)
{
    int rx3_bank = pad_rx3_bank(ddj_mode);
    int key;

    switch (rx3_bank) {
    case 0: key = K_HOTCUE;   break;
    case 1: key = K_ALOOP;    break;
    case 2: key = K_SLIPLOOP; break;
    case 3: key = K_BEATJUMP; break;
    default:
        return;
    }

    if (pad_bank[deck] == rx3_bank)
        return;

    pad_bank[deck] = rx3_bank;

    /*
     * A seleção real do banco é feita pelo keyshim, que consulta o
     * estado nativo e aguarda a confirmação da troca.
     */

    if (opt_verbose)
        logmsg("  pad bank deck%d: DDJ %s (%d) -> RX3 bank %d key=0x%04x\n",
               deck + 1, pad_mode_name(ddj_mode), ddj_mode,
               rx3_bank, key);
}

#define STEMS_PAD_HOTCUE_DELETE_BASE 0x10

/* Set from RX3_STEMS_PADS=touch in main(); see handle_pad(). */
static int stems_pads_touch;
#define STEMS_TOUCH_OPEN 0x20
#define STEMS_TOUCH_KEY_OPEN 0x21
#define STEMS_TOUCH_KEY_PART 0x30   /* +0 -1, +1 own key, +2 +1 */
/* SHIFT+HOTCUE delete, toolkit-core route (2026-10-15): the rbpi4b shim's
 * pointsrc.c reads this off the same touch FIFO and calls rbp's native
 * hotcue_delete() directly -- see that file. STEMS_PAD_HOTCUE_DELETE_BASE
 * above is the legacy-core route (a command file only librx3_core.so ever
 * read); RX3_CORE=toolkit (the default since 2026-10-14) reads no such file,
 * which is why this gesture went silent until this route was added. */
#define STEMS_TOUCH_HOTCUE_DELETE_BASE 0x40 /* +0..+7 = pad 1..8 */
static void stems_touch_send(int deck, int control);
static uint32_t stems_pad_state_sequence;
static uint32_t key_pad_state_sequence;

static void stems_pad_send_command(int deck, int control);

/* Sound Color FX type select (PAD FX1 page, mode 1, reached via SHIFT+BEAT
 * LOOP -- note 0x1E, confirmed live 2026-09-27). Types 1-6 are the only
 * ones confirmed to do anything (0 and 7+ are silent no-ops); pad 0 selects
 * type 1, ..., pad 5 selects type 6. Per deck, independent of the other. */
#define COLOR_FX_TYPE_COUNT 6
static int color_fx_pad_idx[2] = { 0, 0 }; /* type 1 (FILTER), matches
                                             * keyshim's mixer_defaults() */

static void handle_pad(int deck, int note, int on, int shifted)
{
    int mode = (note >> 4) & 0x0f;
    int idx = note & 0x0f;

    if (opt_verbose)
        logmsg("  pad deck%d note 0x%02x (%s mode=%d pad=%d) %s\n",
               deck + 1, note, pad_mode_name(mode), mode, idx + 1,
               on ? "press" : "release");

    if (idx > 7 || mode > 7)
        return;

    /* PAD FX 2 (mode 5) is the dedicated DDJ STEMS page. Mode 7 (KEY SHIFT,
     * confirmed live: reached via SHIFT+SAMPLER, note 0x6F) has no RX3
     * equivalent either, and is repurposed as a second entry point into the
     * same STEMS page, by user request. Pads 1-3 mirror the touchscreen
     * DRUMS/VOCAL/INST controls; pads 4-8 remain reserved. */
    if (mode == 5 || mode == 7) {
        /* RX3_STEMS_PADS=touch (rbpi4b launcher, RX3_CORE=toolkit): the
         * rx3-toolkit core reads no command file, so the command goes to the
         * rbpi4b touch shim, which taps the toolkit's STEMS panel (pointsrc.c,
         * "THE DDJ-400'S STEMS PAGE"). No pad-mode key is sent: the deck
         * stays in its pad mode and the STEMS panel stays open. Pads 1-3 are
         * the panel's order, INST/VOCAL/DRUMS; their LEDs come back through
         * rx3-stems-pad-state.bin, written by the shim (stems_pad_state_tick). */
        if (stems_pads_touch) {
            /* Mode 7 (KEY SHIFT, SHIFT+SAMPLER) is the toolkit's KEY panel:
             * pads 1-3 = -1 / the track's own key / +1; LEDs back through
             * rx3-key-pad-state.bin (key_pad_state_tick). */
            if (on && idx < 3)
                stems_touch_send(deck, mode == 7 ? STEMS_TOUCH_KEY_PART + idx : idx);
            /* The DDJ drives a pressed pad's LED itself (off on release), so
             * a press that changes nothing -- pad 2 at the track's own key --
             * would leave it dark: resend this page's LEDs from the state on
             * the next tick, after every edge. */
            if (mode == 7)
                key_pad_state_sequence = 0;
            else
                stems_pad_state_sequence = 0;
            return;
        }
        if (on && idx < 3)
            stems_pad_send_command(deck, idx);
        return;
    }

    /* PAD FX 1 (mode 1, SHIFT+BEAT LOOP): Sound Color FX type select,
     * per deck. Pad idx 0..5 -> type 1..6; pads 6/7 unused (types 7+ do
     * nothing on this engine). LED feedback in led_handle_input(). */
    if (mode == 1) {
        if (on && idx < COLOR_FX_TYPE_COUNT) {
            int type = idx + 1;
            color_fx_pad_idx[deck] = idx;
            send_ctrl(K_COLOR_TYPE, OP_VALUE, deck + 1, type, (float)type, 0);
            if (opt_verbose)
                logmsg("  Sound Color FX deck%d -> type %d (pad %d)\n",
                       deck + 1, type, idx + 1);
        }
        return;
    }

    pad_select_bank(deck, mode);

    /*
     * SHIFT + HOT CUE usa a função nativa de exclusão dentro do RBP.
     * Somente a borda de pressão gera o comando; a liberação é consumida.
     * Toolkit core: a legacy command-file route below reaches nothing (see
     * STEMS_TOUCH_HOTCUE_DELETE_BASE's comment), so this goes out the same
     * touch FIFO KEY/STEMS already use whenever RX3_STEMS_PADS=touch.
     */
    if (shifted && mode == 0) {
        if (on) {
            if (stems_pads_touch)
                stems_touch_send(deck, STEMS_TOUCH_HOTCUE_DELETE_BASE + idx);
            else
                stems_pad_send_command(
                    deck, STEMS_PAD_HOTCUE_DELETE_BASE + idx);
        }

        if (opt_verbose)
            logmsg("  HOT CUE NATIVE DELETE deck%d pad%d %s\n",
                   deck + 1, idx + 1,
                   on ? "request" : "release");
        return;
    }

    {
        int rx3_bank = pad_rx3_bank(mode);

        if (rx3_bank >= 0)
            send_ctrl(K_PAD1 + idx,
                      on ? OP_PRESS : OP_RELEASE,
                      deck + 1, 0, 0.0f,
                      0x5040 | rx3_bank);
    }
}


static int request_interface_toggle(void)
{
    static const char message[] = "toggle\n";
    int fd = open(
        INTERFACE_TOGGLE_REQUEST,
        O_WRONLY | O_CREAT | O_TRUNC,
        0644
    );

    if (fd < 0) {
        logmsg("  interface toggle request failed: %s\n",
               strerror(errno));
        return 0;
    }

    ssize_t written = write(fd, message, sizeof(message) - 1u);
    close(fd);

    if (written != (ssize_t)(sizeof(message) - 1u)) {
        logmsg("  interface toggle request incomplete\n");
        return 0;
    }

    logmsg("  SHIFT+BACK 5s -> interface toggle requested\n");
    return 1;
}

static int handle_interface_toggle_note(int ch, int note, int on)
{
    if (ch != MC_MIXER || note != 0x42)
        return 0;

    if (on) {
        if (interface_toggle_pressed_ms < 0) {
            interface_toggle_pressed_ms = now_ms();
            interface_toggle_fired = 0;
            logmsg("  SHIFT+BACK hold started\n");
        }
        return 1;
    }

    if (interface_toggle_pressed_ms >= 0 &&
        !interface_toggle_fired) {
        long long held =
            now_ms() - interface_toggle_pressed_ms;

        if (held < INTERFACE_TOGGLE_HOLD_MS) {
            /* From the waveform/deck view (browser closed): jump straight
             * to SOURCE (device picker: USB1/USB2/PC), by user request.
             * Once actually browsing, keep the normal BACK behaviour. */
            int key = browser_open ? K_BACK : K_SOURCE;

            send_ctrl(key, OP_PRESS,   CH_GLOBAL, 0, 0.0f, 0);
            send_ctrl(key, OP_RELEASE, CH_GLOBAL, 0, 0.0f, 0);

            /* SOURCE puts a device (USB1/USB2/PC) on screen, highlighted --
             * the very next plain push must select it (K_SELECTOR), not
             * try to "open" again (K_BACK). Without this, picking a device
             * from SOURCE needed two plain pushes instead of one. */
            browser_open = 1;

            if (opt_verbose)
                logmsg("  SHIFT+BACK short -> %s\n",
                       key == K_SOURCE ? "SOURCE" : "BACK");
        }
    }

    interface_toggle_pressed_ms = -1;
    interface_toggle_fired = 0;
    return 1;
}

static void interface_toggle_tick(void)
{
    if (interface_toggle_pressed_ms < 0 ||
        interface_toggle_fired)
        return;

    if (now_ms() - interface_toggle_pressed_ms <
        INTERFACE_TOGGLE_HOLD_MS)
        return;

    interface_toggle_fired = 1;
    (void)request_interface_toggle();
}

/* ---------------- note mapping table ---------------- */
struct notemap {
    int ch;
    int note;
    int key;
    int send_ch;   /* 0 = derive from the MIDI channel (deck 1/2) */
    const char *name;
};
static const struct notemap notemap[] = {
    /* Note 0x3f on a deck's own channel is NOT load -- it's that deck's
     * physical SHIFT key (confirmed live via raw MIDI capture: holding it
     * changes other buttons' note numbers). See led_handle_input() for the
     * (no-op) handling. LOAD is note 0x46/0x47 on MC_MIXER below. */

    /* BROWSER (ch 7) */
    /* note 0x41 (plain push) is handled in led_handle_input(): it needs
     * state (1st press opens, later presses select) that this static
     * table can't express. */
    { MC_MIXER, 0x42, K_BACK,     CH_GLOBAL, "SHIFT+BROWSE push (back)" },
    { MC_MIXER, 0x46, K_LOAD,     1,         "LOAD deck 1" },
    { MC_MIXER, 0x47, K_LOAD,     2,         "LOAD deck 2" },
    { MC_MIXER, 0x68, K_SOURCE,   CH_GLOBAL, "SHIFT+LOAD 1 (source/usb)" },
    { MC_MIXER, 0x7A, K_SHORTCUT, CH_GLOBAL, "SHIFT+LOAD 2 (shortcut)" },
    /* DECK transport / loops (deck channel) */
    { MC_DECK1, 0x68, K_QUANTIZE, 0,
      "SHIFT+CHANNEL CUE 1 -> QUANTIZE deck 1" },
    { MC_DECK2, 0x68, K_QUANTIZE, 0,
      "SHIFT+CHANNEL CUE 2 -> QUANTIZE deck 2" },
    { MC_DECK1, 0x0B, K_PLAY, 0, "PLAY/PAUSE" },
    { MC_DECK2, 0x0B, K_PLAY, 0, "PLAY/PAUSE" },
    { MC_DECK1, 0x47, K_REV,  0, "SHIFT+PLAY (censor)" },
    { MC_DECK2, 0x47, K_REV,  0, "SHIFT+PLAY (censor)" },
    { MC_DECK1, 0x48, K_VINYL, 0, "SHIFT+CUE -> VINYL" },
    { MC_DECK1, 0x0C, K_CUE,  0, "CUE" },
    { MC_DECK2, 0x48, K_VINYL, 0, "SHIFT+CUE -> VINYL" },
    { MC_DECK2, 0x0C, K_CUE,  0, "CUE" },
    { MC_DECK1, 0x58, K_MASTER_TEMPO, 0, "BEAT SYNC -> MASTER TEMPO" },
    { MC_DECK2, 0x58, K_MASTER_TEMPO, 0, "BEAT SYNC -> MASTER TEMPO" },
    { MC_DECK1, 0x5C, K_MASTER, 0, "BEAT SYNC long (master)" },
    { MC_DECK2, 0x5C, K_MASTER, 0, "BEAT SYNC long (master)" },
    { MC_DECK1, 0x60, K_TEMPO_RANGE, 0, "SHIFT+SYNC (tempo range)" },
    { MC_DECK2, 0x60, K_TEMPO_RANGE, 0, "SHIFT+SYNC (tempo range)" },
    { MC_DECK1, 0x10, K_LOOPIN,  0, "LOOP IN / 4 BEAT" },
    { MC_DECK2, 0x10, K_LOOPIN,  0, "LOOP IN / 4 BEAT" },
    { MC_DECK1, 0x11, K_LOOPOUT, 0, "LOOP OUT" },
    { MC_DECK2, 0x11, K_LOOPOUT, 0, "LOOP OUT" },
    { MC_DECK1, 0x4D, K_RELOOP,  0, "RELOOP/EXIT" },
    { MC_DECK2, 0x4D, K_RELOOP,  0, "RELOOP/EXIT" },
    /* SHIFT+RELOOP/EXIT (note 0x50, deck channel): unused by this bridge and
     * by the official notemap (the hardware doesn't renumber RELOOP/EXIT's
     * own note under SHIFT the way PLAY/CUE/LOAD do -- confirmed against the
     * Mixxx Pioneer-DDJ-400 mapping, which independently lists 0x50 as this
     * button's own separate SHIFT note, "Reloop and stop"). Repurposed here
     * for AUTO CUE, which has no dedicated DDJ-400 button.
     * Live-confirmed 2026-09-30: forwarding raw press/release lets the
     * engine's own short/long-press timing decide, same as real hardware --
     * a quick tap flips TimeMode (REMAIN/TIME) instead of AUTO CUE (see
     * K_AUTOCUE above); holding it briefly (~1s) toggles AUTO CUE. Not a
     * bridge bug -- both reactions are the real button's own behavior. */
    { MC_DECK1, 0x50, K_AUTOCUE, 0, "SHIFT+RELOOP/EXIT hold -> AUTO CUE (tap -> TimeMode)" },
    { MC_DECK2, 0x50, K_AUTOCUE, 0, "SHIFT+RELOOP/EXIT hold -> AUTO CUE (tap -> TimeMode)" },
    { MC_DECK1, 0x51, K_CALLPREV, 0, "CUE/LOOP CALL 1/2X" },
    { MC_DECK1, 0x53, K_CALLNEXT, 0, "CUE/LOOP CALL 2X" },
    { MC_DECK2, 0x51, K_CALLPREV, 0, "CUE/LOOP CALL 1/2X" },
    { MC_DECK2, 0x53, K_CALLNEXT, 0, "CUE/LOOP CALL 2X" },
    /* jog plate touch */
    { MC_DECK1, 0x36, K_JOG_TOUCH, 0, "jog plate touch" },
    { MC_DECK2, 0x36, K_JOG_TOUCH, 0, "jog plate touch" },
    { MC_DECK1, 0x67, K_JOG_TOUCH, 0, "SHIFT+jog plate touch" },
    { MC_DECK2, 0x67, K_JOG_TOUCH, 0, "SHIFT+jog plate touch" },
    /* pad mode buttons. Notes confirmed live via raw MIDI capture (2026-09-27):
     * HOT CUE=0x1B, BEAT LOOP=0x6D, BEAT JUMP=0x20, SAMPLER=0x22. The
     * previous table had 0x1E for "beat loop" (never actually sent by the
     * hardware -- dead entry) and sent K_ALOOP (beat loop's own reaction)
     * for SAMPLER's real note (0x22) instead, so pressing SAMPLER wrongly
     * triggered a beat-loop reaction while BEAT LOOP's real button (0x6D)
     * triggered nothing. Fixed to match pad_rx3_bank()'s fallback choice
     * (SAMPLER -> SLIP LOOP, no direct RX3 equivalent). */
    { MC_DECK1, 0x1B, K_HOTCUE,   0, "PAD MODE hot cue" },
    { MC_DECK2, 0x1B, K_HOTCUE,   0, "PAD MODE hot cue" },
    { MC_DECK1, 0x6D, K_ALOOP,    0, "PAD MODE beat loop" },
    { MC_DECK2, 0x6D, K_ALOOP,    0, "PAD MODE beat loop" },
    { MC_DECK1, 0x20, K_BEATJUMP, 0, "PAD MODE beat jump" },
    { MC_DECK2, 0x20, K_BEATJUMP, 0, "PAD MODE beat jump" },
    { MC_DECK1, 0x22, K_SLIPLOOP, 0, "PAD MODE sampler -> slip loop" },
    { MC_DECK2, 0x22, K_SLIPLOOP, 0, "PAD MODE sampler -> slip loop" },
    /* BEAT FX (canal MIDI 5; verificado por captura física) */
    { MC_FX, 0x63, K_BFXTYPE,  CH_GLOBAL, "BEAT FX select" },
    { MC_FX, 0x4A, K_BEATPREV, CH_GLOBAL, "BEAT FX beat <" },
    { MC_FX, 0x4B, K_BEATNEXT, CH_GLOBAL, "BEAT FX beat >" },
    { MC_FX, 0x47, K_BFX,      CH_GLOBAL, "BEAT FX on/off" },
    /* SHIFT+BEAT ▶ = TAP (confirmed live 2026-09-27: user-labelled "TAP",
     * manually tap the Beat FX tempo). Note 0x6B is BEAT ▶'s own SHIFT
     * variant per the official MIDI list, not a separate physical button. */
    { MC_FX, 0x6B, K_TAP,      CH_GLOBAL, "SHIFT+BEAT FX beat > (TAP)" },
};
#define NNMAP ((int)(sizeof(notemap) / sizeof(notemap[0])))

/* BEAT FX channel-select switch: notes 16/17/20 on ch 5 -> ch 1 / 2 / MASTER */

#define BFX_TYPE_COUNT 14

/* O botão SELECT da DDJ-400 é momentâneo, enquanto o RX3 espera
 * o índice absoluto do efeito via OP_VALUE. O estado começa em DELAY=0. */
static int beat_fx_type = 0;

static int handle_beat_fx_select(int note, int on)
{
    if (note != 0x63)
        return 0;

    /* Consumir também a nota OFF, sem avançar novamente. */
    if (!on)
        return 1;

    beat_fx_type++;
    if (beat_fx_type >= BFX_TYPE_COUNT)
        beat_fx_type = 0;

    if (opt_verbose)
        logmsg("  beat fx type select -> %d\n", beat_fx_type);

    send_ctrl(
        K_BFXTYPE,
        OP_VALUE,
        CH_GLOBAL,
        beat_fx_type,
        (float)beat_fx_type,
        beat_fx_type
    );

    return 1;
}

static int handle_fxch(int note, int on)
{
    int value;

    /* Índices nativos do RX3:
     *   0 = deck 1
     *   1 = deck 2
     *   5 = master
     *
     * A chave da DDJ envia também notas OFF das posições anteriores.
     * Elas devem ser consumidas, mas não enviadas ao player.
     */
    switch (note) {
    case 0x10:
        value = 0;
        break;
    case 0x11:
        value = 1;
        break;
    case 0x14:
        value = 5;
        break;
    default:
        return 0;
    }

    if (!on)
        return 1;

    /* SHIFT+FX channel select: crossfader curve, by user request. Confirmed
     * live (raw MIDI capture) that SHIFT does not renumber these notes --
     * unlike MASTER CUE/PLAY/LOAD, so shift_held[] is the only way to tell.
     * CH1/CH2/MASTER -> curve param 1/2/3 (0=THRU intentionally unused). */
    if (shift_held[0] || shift_held[1]) {
        static const int curve_param[3] = { 1, 2, 3 };
        int idx = (note == 0x10) ? 0 : (note == 0x11) ? 1 : 2;

        send_ctrl(K_FADERCURVE, OP_VALUE, CH_GLOBAL,
                  curve_param[idx], (float)curve_param[idx], curve_param[idx]);

        if (opt_verbose)
            logmsg("  SHIFT+FX ch select -> crossfader curve %d\n",
                   curve_param[idx]);

        return 1;
    }

    if (opt_verbose)
        logmsg("  beat fx channel select -> %d\n", value);

    send_ctrl(
        K_BFXCH,
        OP_VALUE,
        CH_GLOBAL,
        value,
        (float)value,
        value
    );

    return 1;
}

static const char *note_name(int ch, int note)
{
    static char buf[64];
    for (int i = 0; i < NNMAP; i++)
        if (notemap[i].ch == ch && notemap[i].note == note)
            return notemap[i].name;
    snprintf(buf, sizeof(buf), "%s", "");
    return buf;
}

/* ---------------- MIDI dispatch ---------------- */
static void handle_note(int ch, int note, int on)
{
    if (opt_sniff) {
        logmsg("MIDI ch%-2d NOTE %3d (0x%02x) %-3s %s\n",
               ch + 1, note, note, on ? "on" : "off", note_name(ch, note));
        return;
    }
    if (handle_interface_toggle_note(ch, note, on))
        return;
    if (ch == MC_FX && handle_beat_fx_select(note, on))
        return;
    if (ch == MC_FX && handle_fxch(note, on))
        return;
    if (ch == MC_PAD1 || ch == MC_PAD1_SH) {
        handle_pad(0, note, on, ch == MC_PAD1_SH);
        return;
    }
    if (ch == MC_PAD2 || ch == MC_PAD2_SH) {
        handle_pad(1, note, on, ch == MC_PAD2_SH);
        return;
    }

    /* SHIFT+BEAT JUMP (the PAD FX 2 mode button, deck channel note 0x6B,
     * unmapped otherwise): open the toolkit's STEMS panel on the screen. */
    if (stems_pads_touch && (ch == MC_DECK1 || ch == MC_DECK2) && note == 0x6B) {
        if (on)
            stems_touch_send(ch == MC_DECK1 ? 0 : 1, STEMS_TOUCH_OPEN);
        return;
    }
    /* SHIFT+SAMPLER (the KEY SHIFT mode button, note 0x6F): the KEY panel. */
    if (stems_pads_touch && (ch == MC_DECK1 || ch == MC_DECK2) && note == 0x6F) {
        if (on)
            stems_touch_send(ch == MC_DECK1 ? 0 : 1, STEMS_TOUCH_KEY_OPEN);
        return;
    }

    for (int i = 0; i < NNMAP; i++) {
        int sch;
        if (notemap[i].ch != ch || notemap[i].note != note)
            continue;
        sch = notemap[i].send_ch;
        if (sch == 0)
            sch = (ch == MC_DECK1) ? 1 : 2;
        send_ctrl(notemap[i].key, on ? OP_PRESS : OP_RELEASE, sch, 0, 0.0f, 0);
        if (opt_verbose)
            logmsg("  %s -> 0x%04x %s ch%d\n", notemap[i].name,
                   notemap[i].key, on ? "press" : "release", sch);
        return;
    }
    if (opt_verbose)
        logmsg("  unmapped ch%d note 0x%02x %s\n", ch, note, on ? "on" : "off");
}

static void handle_cc(int ch, int cc, int val)
{
    if (opt_sniff) {
        logmsg("MIDI ch%-2d CC  %3d (0x%02x) val=%3d\n", ch + 1, cc, cc, val);
        return;
    }

    /* browse encoder: relative two's complement */
    if (ch == MC_MIXER && cc == 0x40) {
        int delta = (val >= 64) ? val - 128 : val;
        if (delta > 16) delta = 16;
        if (delta < -16) delta = -16;
        for (int i = 0; i < (delta < 0 ? -delta : delta); i++)
            send_ctrl(K_SELECTOR, OP_ROTATE, CH_GLOBAL, (delta < 0) ? -1 : 1, 0.0f, 0);
        if (opt_verbose)
            logmsg("  browse delta=%d\n", delta);
        return;
    }

    /* jog wheels: CC 0x21 side, 0x22 platter vinyl, 0x23 platter non-vinyl.
     * All relative, offset-binary around 0x40 (0x3f=-1, 0x40=0, 0x41=+1;
     * not two's complement). */
    if ((ch == MC_DECK1 || ch == MC_DECK2) &&
        (cc == 0x21 || cc == 0x22 || cc == 0x23)) {
        jog_delta(ch, val - 64);
        return;
    }

    /* SHIFT+platter (search) -- see search_delta()/search_tick() above. */
    if ((ch == MC_DECK1 || ch == MC_DECK2) && cc == 0x29) {
        search_delta(ch, val - 64);
        return;
    }

    if (handle_cc14(ch, cc, val))
        return;

    /* 7-bit absolute controls */
    if (ch == MC_MIXER && (cc == 0x17 || cc == 0x18)) {   /* FILTER ch1 / ch2 */
        int sch = (cc == 0x17) ? 1 : 2;
        int idx = sch - 1;
        int v10 = cc_to_10bit(val);

        /* Dedupe like every cc14[] entry already does -- this was missing
         * (pre-existing, not just from today's changes) and let a fast
         * knob sweep flood the control FIFO and (since mixer state
         * persistence was added) trigger a disk write per MIDI message
         * instead of per actual value change. */
        if (v10 == filter_last10[idx])
            return;
        filter_last10[idx] = v10;
        send_ctrl(K_COLOR, OP_VALUE, sch, v10, (float)val / 127.0f, val);
        mixer_state_save();
        if (opt_verbose)
            logmsg("  filter ch%d val=%d\n", sch, val);
        return;
    }
    if (ch == MC_FX && cc == 0x02) {                      /* BEAT FX depth */
        send_ctrl(K_DEPTH, OP_VALUE, CH_GLOBAL, cc_to_10bit(val), (float)val / 127.0f, val);
        if (opt_verbose)
            logmsg("  beat fx depth val=%d\n", val);
        return;
    }

    if (opt_verbose)
        logmsg("  unmapped ch%d cc 0x%02x val=%d\n", ch, cc, val);
}


/* ---------------- DDJ-400 Master VU output ---------------- */
struct rx3_vu_packet {
    uint16_t sequence;
    uint8_t left;
    uint8_t right;
};

static char vu_path[512];
static int vu_fd = -1;
static uint16_t vu_last_sequence;
static int vu_left;
static int vu_right;
static int vu_idle_ticks;

static void master_rec_init_path(void)
{
    const char *slash = strrchr(fifo_path, '/');
    static const char name[] = "/rx3-master-rec-toggle.request";

    if (slash) {
        size_t length = (size_t)(slash - fifo_path);

        if (length > sizeof(master_rec_toggle_path) - sizeof(name))
            length = sizeof(master_rec_toggle_path) - sizeof(name);

        memcpy(master_rec_toggle_path, fifo_path, length);
        master_rec_toggle_path[length] = '\0';
        strcat(master_rec_toggle_path, name);
    } else {
        snprintf(master_rec_toggle_path, sizeof(master_rec_toggle_path),
                  "rx3-master-rec-toggle.request");
    }

    if (opt_verbose)
        logmsg("ddj400: MASTER REC toggle request %s\n",
               master_rec_toggle_path);
}

static int request_master_rec_toggle(void)
{
    int fd = open(master_rec_toggle_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        logmsg("  MASTER REC toggle request failed: %s\n", strerror(errno));
        return 0;
    }
    close(fd);
    return 1;
}

static void master_rec_cancel_hold(void)
{
    if (master_rec_hold_started_ms >= 0 && !master_rec_hold_fired) {
        master_rec_hold_started_ms = -1;
        if (opt_verbose)
            logmsg("  SHIFT hold cancelled (other control used)\n");
    }
}

static void master_rec_hold_tick(void)
{
    if (master_rec_hold_started_ms < 0 || master_rec_hold_fired)
        return;
    if (now_ms() - master_rec_hold_started_ms < MASTER_REC_HOLD_MS)
        return;
    master_rec_hold_fired = 1;
    if (request_master_rec_toggle())
        logmsg("  SHIFT held 3s -> MASTER REC toggle requested\n");
}

static void vu_init_path(void)
{
    const char *slash = strrchr(fifo_path, '/');

    if (slash) {
        size_t length = (size_t)(slash - fifo_path);

        if (length > sizeof(vu_path) - 13)
            length = sizeof(vu_path) - 13;

        memcpy(vu_path, fifo_path, length);
        vu_path[length] = '\0';
        strcat(vu_path, "/rx3-vu.bin");
    } else {
        snprintf(vu_path, sizeof(vu_path), "rx3-vu.bin");
    }

    if (opt_verbose)
        logmsg("ddj400: VU telemetry %s\n", vu_path);
}

static void midi_write3(int fd, int status, int data1, int data2)
{
    unsigned char message[3];
    int tries;

    message[0] = (unsigned char)status;
    message[1] = (unsigned char)data1;
    message[2] = (unsigned char)data2;

    /* fd is O_NONBLOCK: a momentary full buffer returns EAGAIN, which used
     * to be silently swallowed (and never logged, even with -v, since the
     * old check explicitly excluded EAGAIN/EINTR from the log condition) --
     * one-shot LED writes like the Sound Color FX pad selection had no
     * retry, so a single dropped write meant that LED just never lit, with
     * no trace anywhere. Retry a few times with a short backoff instead. */
    for (tries = 0; tries < 5; tries++) {
        ssize_t n = write(fd, message, sizeof(message));
        if (n == (ssize_t)sizeof(message))
            return;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(2000);
            continue;
        }
        if (opt_verbose)
            logmsg("ddj400: MIDI output failed: %s\n", strerror(errno));
        return;
    }
    if (opt_verbose)
        logmsg("ddj400: MIDI output dropped after retries (status=0x%02x "
               "d1=%d d2=%d)\n", status, data1, data2);
}

static void vu_midi_init(int midi_fd)
{
    /* Use both physical meters as stereo Master L/R. */
    midi_write3(midi_fd, 0x9f, 0x4b, 0x7f);
    midi_write3(midi_fd, 0xb0, 0x02, 0);
    midi_write3(midi_fd, 0xb1, 0x02, 0);

    vu_left = 0;
    vu_right = 0;
    vu_idle_ticks = 0;
    vu_last_sequence = 0;
}

static int vu_decay(int current, int target)
{
    if (target >= current)
        return target;

    current -= 5;
    if (current < target)
        current = target;
    if (current < 0)
        current = 0;

    return current;
}

static void vu_tick(int midi_fd)
{
    struct rx3_vu_packet packet;
    int target_left = 0;
    int target_right = 0;
    int previous_left = vu_left;
    int previous_right = vu_right;

    if (vu_fd < 0)
        vu_fd = open(vu_path, O_RDONLY | O_NONBLOCK);

    if (vu_fd >= 0) {
        ssize_t count = pread(vu_fd, &packet, sizeof(packet), 0);

        if (count == (ssize_t)sizeof(packet) &&
            packet.sequence != vu_last_sequence) {
            vu_last_sequence = packet.sequence;
            target_left = packet.left;
            target_right = packet.right;
            vu_idle_ticks = 0;
        } else {
            vu_idle_ticks++;
            if (vu_idle_ticks < 25) {
                target_left = vu_left;
                target_right = vu_right;
            }
        }
    }

    vu_left = vu_decay(vu_left, target_left);
    vu_right = vu_decay(vu_right, target_right);

    if (vu_left != previous_left)
        midi_write3(midi_fd, 0xb0, 0x02, vu_left);
    if (vu_right != previous_right)
        midi_write3(midi_fd, 0xb1, 0x02, vu_right);
}


/* ---------------- DDJ-400 LED feedback ---------------- */
static unsigned char led_play[2];
static unsigned char led_sync[2];
static unsigned char led_master[2];
static unsigned char led_channel_cue[2];
static unsigned char led_loop_in[2];
static unsigned char led_loop_out[2];
static unsigned char led_reloop[2];
static unsigned char led_master_cue;
static unsigned char led_beat_fx;

/* Estado local dos pads até termos telemetria nativa do player. */
static signed char led_beat_loop_pad[2] = { -1, -1 };
static long long led_beat_jump_deadline[2][8];

static void led_send(int fd, int channel, int note, int enabled)
{
    midi_write3(fd, 0x90 | channel, note, enabled ? 0x7f : 0x00);
}


/* Estado nativo publicado pelo keyshim dentro do processo RBP. */
#define RX3_LED_MAGIC   0x524c4544UL
#define RX3_LED_VERSION 1

/* Bits of rx3_led_state_packet.flags[deck], written by keyshim.c from real
 * native engine state (rx3_native_call1 against onPhysicalKey's own query
 * functions). Only MASTER_TEMPO is consumed here so far. */
#define RX3_LED_CUE          0x01
#define RX3_LED_LOOP         0x02
#define RX3_LED_RELOOP       0x04
#define RX3_LED_SYNC_MASTER  0x08
#define RX3_LED_SYNC_SLAVE   0x10
#define RX3_LED_MASTER_TEMPO 0x20

struct rx3_led_state_packet {
    uint32_t magic;
    uint16_t version;
    uint16_t sequence;

    uint8_t hotcue[2];
    uint8_t play_mode[2];
    uint8_t flags[2];
    uint8_t pad_mode[2];

    uint32_t play_time[2];
    uint32_t total_time[2];
} __attribute__((packed));

static char native_led_state_path[512];
static int native_led_state_fd = -1;
static uint16_t native_led_last_sequence;
static unsigned char native_led_hotcue[2] = { 0xff, 0xff };
static unsigned char native_led_cue[2] = { 0xff, 0xff };
static long long native_led_transport_refresh = -1;
static int native_led_force_sync = 1;


/* Bidirectional state transport shared with librx3_core.so inside the chroot. */
#define STEMS_PAD_COMMAND_MAGIC 0x53434d44u
#define STEMS_PAD_STATE_MAGIC   0x53544d53u

struct stems_pad_command_packet {
    uint32_t magic;
    uint32_t sequence;
    uint8_t deck;
    uint8_t control;
    uint16_t reserved;
};

struct stems_pad_state_packet {
    uint32_t magic;
    uint32_t sequence;
    uint8_t active[2];
    uint8_t armed[2];
};

static char stems_pad_command_path[512];
static char stems_pad_state_path[512];
static int stems_pad_command_fd = -1;
static int stems_pad_state_fd = -1;
static uint32_t stems_pad_command_sequence;

static void stems_pad_sibling_path(char *output, size_t capacity,
                                   const char *name)
{
    const char *slash = strrchr(fifo_path, '/');
    if (slash) {
        size_t length = (size_t)(slash - fifo_path);
        if (length > capacity - strlen(name) - 2u)
            length = capacity - strlen(name) - 2u;
        snprintf(output, capacity, "%.*s/%s",
                 (int)length, fifo_path, name);
    } else {
        snprintf(output, capacity, "%s", name);
    }
}

static void stems_pad_init_paths(void)
{
    stems_pad_sibling_path(stems_pad_command_path,
                           sizeof(stems_pad_command_path),
                           "rx3-stems-pad-command.bin");
    stems_pad_sibling_path(stems_pad_state_path,
                           sizeof(stems_pad_state_path),
                           "rx3-stems-pad-state.bin");
    if (opt_verbose) {
        logmsg("ddj400: STEMS command %s\n", stems_pad_command_path);
        logmsg("ddj400: STEMS state %s\n", stems_pad_state_path);
    }
}

/* The toolkit route: same packet, into the FIFO the rbpi4b touch shim reads
 * (created by rbp at start; no reader means the write is dropped). */
static void stems_touch_send(int deck, int control)
{
    static char path[512];
    struct stems_pad_command_packet packet;
    int fd;

    if (!path[0])
        stems_pad_sibling_path(path, sizeof(path), "rx3-stems-touch.fifo");
    fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        logmsg("  STEMS touch: %s: %s\n", path, strerror(errno));
        return;
    }
    memset(&packet, 0, sizeof(packet));
    packet.magic = STEMS_PAD_COMMAND_MAGIC;
    packet.sequence = ++stems_pad_command_sequence;
    packet.deck = (uint8_t)deck;
    packet.control = (uint8_t)control;
    (void)write(fd, &packet, sizeof(packet));
    close(fd);
    if (opt_verbose)
        logmsg("  STEMS touch deck%d %s\n", deck + 1,
               control == STEMS_TOUCH_OPEN ? "open STEMS panel" :
               control == STEMS_TOUCH_KEY_OPEN ? "open KEY panel" :
               control == STEMS_TOUCH_KEY_PART ? "KEY -1" :
               control == STEMS_TOUCH_KEY_PART + 1 ? "KEY own key" :
               control == STEMS_TOUCH_KEY_PART + 2 ? "KEY +1" :
               control == 0 ? "INST" : control == 1 ? "VOCAL" : "DRUMS");
}

static void stems_pad_send_command(int deck, int control)
{
    struct stems_pad_command_packet packet;
    if (stems_pad_command_fd < 0)
        stems_pad_command_fd = open(stems_pad_command_path,
                                    O_RDWR | O_CREAT, 0644);
    if (stems_pad_command_fd < 0)
        return;
    memset(&packet, 0, sizeof(packet));
    packet.magic = STEMS_PAD_COMMAND_MAGIC;
    packet.sequence = ++stems_pad_command_sequence;
    packet.deck = (uint8_t)deck;
    packet.control = (uint8_t)control;
    (void)pwrite(stems_pad_command_fd, &packet, sizeof(packet), 0);
    if (opt_verbose)
        logmsg("  STEMS deck%d %s toggle\n", deck + 1,
               control == 0 ? "DRUMS" :
               control == 1 ? "VOCAL" : "INST");
}

static void stems_pad_state_tick(int midi_fd)
{
    struct stems_pad_state_packet packet;
    if (stems_pad_state_fd < 0)
        stems_pad_state_fd = open(stems_pad_state_path,
                                  O_RDONLY | O_NONBLOCK);
    if (stems_pad_state_fd < 0)
        return;
    if (pread(stems_pad_state_fd, &packet, sizeof(packet), 0) !=
            (ssize_t)sizeof(packet) ||
        packet.magic != STEMS_PAD_STATE_MAGIC ||
        packet.sequence == stems_pad_state_sequence)
        return;
    stems_pad_state_sequence = packet.sequence;
    for (int deck = 0; deck < 2; deck++) {
        int channel = deck == 0 ? MC_PAD1 : MC_PAD2;

        for (int control = 0; control < 3; control++)
            led_send(midi_fd, channel, 0x50 + control,
                     packet.armed[deck] &&
                     (packet.active[deck] & (1u << control)) ? 0x7f : 0);
    }
    if (opt_verbose)
        logmsg("  STEMS LEDs deck1=0x%02x deck2=0x%02x\n",
               packet.active[0], packet.active[1]);
}

/* KEY SHIFT page LEDs (toolkit core): written by the rbpi4b touch shim from the
 * toolkit's key state -- bit n lights pad n+1: 1 below the track's key, 2 at it,
 * 3 above. Reopened until the shim has created it. */
#define KEY_PAD_STATE_MAGIC 0x5359454bu
struct key_pad_state_packet {
    uint32_t magic;
    uint32_t sequence;
    uint8_t leds[2];
    uint8_t reserved[2];
};
static int key_pad_state_fd = -1;

static void key_pad_state_tick(int midi_fd)
{
    static char path[512];
    struct key_pad_state_packet packet;

    if (!stems_pads_touch)
        return;
    if (!path[0])
        stems_pad_sibling_path(path, sizeof(path), "rx3-key-pad-state.bin");
    if (key_pad_state_fd < 0)
        key_pad_state_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (key_pad_state_fd < 0)
        return;
    if (pread(key_pad_state_fd, &packet, sizeof(packet), 0) !=
            (ssize_t)sizeof(packet) ||
        packet.magic != KEY_PAD_STATE_MAGIC ||
        packet.sequence == key_pad_state_sequence)
        return;
    key_pad_state_sequence = packet.sequence;
    for (int deck = 0; deck < 2; deck++) {
        int channel = deck == 0 ? MC_PAD1 : MC_PAD2;

        for (int part = 0; part < 3; part++)
            led_send(midi_fd, channel, 0x70 + part,
                     (packet.leds[deck] & (1u << part)) ? 0x7f : 0);
    }
    if (opt_verbose)
        logmsg("  KEY LEDs deck1=0x%02x deck2=0x%02x\n",
               packet.leds[0], packet.leds[1]);
}

/* BEAT FX ON/OFF LED (2026-10-15): driven from rbp's own real on/off state
 * (the rbpi4b shim's UiGetMixBeatFxOn poll, rbp_abi.h), not a locally-toggled
 * guess. A touchscreen tap of the same control (fx_zone.c's FX_ACT_POWER)
 * sends K_BFX straight to rbp and keeps no shadow state of its own, so the
 * old `led_beat_fx ^= 1` on every physical press (led_handle_input) went
 * wrong the moment a touch toggle and the next physical press disagreed:
 * the LED (and the next press's assumed starting point) tracked presses
 * counted here, not the engine's real state. */
#define BFX_STATE_MAGIC 0x53584642u /* "BFXS" */
struct bfx_state_packet { uint32_t magic, sequence; uint8_t on, reserved[3]; };
static uint32_t bfx_state_sequence;
static int bfx_state_fd = -1;

static void bfx_state_tick(int midi_fd)
{
    static char path[512];
    struct bfx_state_packet packet;

    if (!path[0])
        stems_pad_sibling_path(path, sizeof(path), "rx3-bfx-state.bin");
    if (bfx_state_fd < 0)
        bfx_state_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (bfx_state_fd < 0)
        return;
    if (pread(bfx_state_fd, &packet, sizeof(packet), 0) !=
            (ssize_t)sizeof(packet) ||
        packet.magic != BFX_STATE_MAGIC ||
        packet.sequence == bfx_state_sequence)
        return;
    bfx_state_sequence = packet.sequence;
    led_beat_fx = packet.on ? 1 : 0;
    led_send(midi_fd, 4, 0x47, led_beat_fx);
    if (opt_verbose)
        logmsg("  BEAT FX LED -> %s (rbp's own state)\n",
               led_beat_fx ? "on" : "off");
}

static void native_led_state_init_path(void)
{
    const char *slash = strrchr(fifo_path, '/');

    if (slash) {
        size_t length = (size_t)(slash - fifo_path);

        if (length > sizeof(native_led_state_path) - 24)
            length = sizeof(native_led_state_path) - 24;

        memcpy(native_led_state_path, fifo_path, length);
        native_led_state_path[length] = '\0';
        strcat(native_led_state_path, "/rx3-led-state.bin");
    } else {
        snprintf(native_led_state_path,
                 sizeof(native_led_state_path),
                 "rx3-led-state.bin");
    }

    if (opt_verbose)
        logmsg("ddj400: native LED telemetry %s\n",
               native_led_state_path);
}

static void native_led_state_reset(void)
{
    native_led_last_sequence = 0;
    native_led_hotcue[0] = 0xff;
    native_led_hotcue[1] = 0xff;
    native_led_force_sync = 1;
}

static void native_led_send_hotcues(int midi_fd, int deck,
                                    unsigned char bitmap)
{
    int normal_channel =
        deck == 0 ? MC_PAD1 : MC_PAD2;
    int shifted_channel =
        deck == 0 ? MC_PAD1_SH : MC_PAD2_SH;

    /*
     * A DDJ-400 mantém páginas separadas de LED para o estado normal
     * e para SHIFT. O mapeamento oficial envia cada Hot Cue para ambas.
     */
    for (int pad = 0; pad < 8; pad++) {
        int enabled = (bitmap & (1U << pad)) != 0;

        led_send(midi_fd, normal_channel, pad, enabled);
        led_send(midi_fd, shifted_channel, pad, enabled);
    }
}

static void native_led_state_tick(int midi_fd)
{
    struct rx3_led_state_packet packet;

    if (native_led_state_fd < 0) {
        native_led_state_fd =
            open(native_led_state_path, O_RDONLY | O_NONBLOCK);

        if (native_led_state_fd < 0)
            return;
    }

    ssize_t count = pread(native_led_state_fd,
                          &packet, sizeof(packet), 0);

    if (count != (ssize_t)sizeof(packet))
        return;

    if (packet.magic != RX3_LED_MAGIC ||
        packet.version != RX3_LED_VERSION)
        return;

    if (!native_led_force_sync &&
        packet.sequence == native_led_last_sequence)
        return;

    native_led_last_sequence = packet.sequence;

    for (int deck = 0; deck < 2; deck++) {
        if (native_led_force_sync ||
            packet.hotcue[deck] != native_led_hotcue[deck]) {
            native_led_hotcue[deck] = packet.hotcue[deck];

            native_led_send_hotcues(
                midi_fd, deck, packet.hotcue[deck]);

            if (opt_verbose)
                logmsg("ddj400: native hotcue deck%d bitmap=0x%02x\n",
                       deck + 1, packet.hotcue[deck]);
        }
    }

    /* Estados nativos observados no player:
       2/3 = tocando, 4 = pausa, 6 = parado no CUE, 7 = CUE preview.
       PLAY permanece aceso tocando e pisca em estados pausados. */
    long long current_ms = now_ms();
    int blink = (int)((current_ms / 400) & 1);
    long long refresh_slot = current_ms / 500;
    int force_transport =
        refresh_slot != native_led_transport_refresh;

    for (int deck = 0; deck < 2; deck++) {
        int mode = packet.play_mode[deck];
        int playing = mode == 2 || mode == 3;
        int paused = mode == 4 || mode == 6;
        int play_led = playing ? 1 : (paused ? blink : 0);
        int loaded =
            packet.total_time[deck] != 0xffffffffu &&
            packet.total_time[deck] != 0u;
        /* Reverted 2026-09-30 by user request, back to this original
         * lit-once-loaded behavior. Two mode-driven alternatives were tried
         * and worked in principle but were rolled back without a recorded
         * reason -- if revisiting, the useful finding to keep is that the
         * native RX3_LED_CUE flag (packet.flags[deck] & RX3_LED_CUE) is
         * NOT a live "should the button be lit now" signal: live telemetry
         * capture that day showed it stays 1 across mode 2 (playing), 4
         * (paused off-cue), 6 (stopped at cue) and 7 (preview) alike -- it
         * only means "this deck has a cue point memorized at all". Any
         * future attempt needs to key off `mode` (2/3 playing, 4 paused
         * off-cue, 6 stopped-at-cue, 7 CUE-held preview) instead. */
        int cue_led = loaded ? 1 : 0;

        if (native_led_force_sync || force_transport ||
            led_play[deck] != play_led) {
            led_play[deck] = (unsigned char)play_led;
            led_send(midi_fd, deck, 0x0b, play_led);
        }

        if (native_led_force_sync || force_transport ||
            native_led_cue[deck] != (unsigned char)cue_led) {
            native_led_cue[deck] = (unsigned char)cue_led;
            led_send(midi_fd, deck, 0x0c, cue_led);
        }

        int master_tempo_led =
            (packet.flags[deck] & RX3_LED_MASTER_TEMPO) ? 1 : 0;

        if (native_led_force_sync ||
            led_sync[deck] != (unsigned char)master_tempo_led) {
            led_sync[deck] = (unsigned char)master_tempo_led;
            led_send(midi_fd, deck, 0x58, master_tempo_led);
        }
    }

    native_led_transport_refresh = refresh_slot;
    native_led_force_sync = 0;
}

/* Sound Color FX LED (PAD FX1 page): sent from the next poll tick instead
 * of synchronously inline with the triggering note-on, in case writing an
 * LED command back-to-back with processing the incoming press (same read
 * burst) confuses this specific hardware page (every other software-level
 * explanation -- other LED writers, EAGAIN drops, redraw-on-entry -- has
 * been ruled out live; this is the one difference left vs. the isolated
 * `amidi` test, which never wrote anything synchronously with a read). */
static int color_fx_led_shown[2]   = { 0, 0 };
static int color_fx_led_pending[2] = { -1, -1 };
static long long color_fx_led_deadline[2];

static void color_fx_led_tick(int fd)
{
    long long current = now_ms();

    for (int deck = 0; deck < 2; deck++) {
        int idx = color_fx_led_pending[deck];
        int channel;
        int prev;

        if (idx < 0 || current < color_fx_led_deadline[deck])
            continue;

        channel = deck == 0 ? MC_PAD1 : MC_PAD2;
        prev = color_fx_led_shown[deck];
        color_fx_led_pending[deck] = -1;

        if (prev != idx)
            led_send(fd, channel, prev, 0);
        led_send(fd, channel, idx, 1);
        color_fx_led_shown[deck] = idx;
    }
}

/* Apaga Beat Jump um segundo depois sem bloquear o loop MIDI. */
static void pad_led_tick(int fd)
{
    long long current = now_ms();

    for (int deck = 0; deck < 2; deck++) {
        int channel = deck == 0 ? MC_PAD1 : MC_PAD2;

        for (int idx = 0; idx < 8; idx++) {
            long long deadline = led_beat_jump_deadline[deck][idx];

            if (deadline > 0 && current >= deadline) {
                led_beat_jump_deadline[deck][idx] = 0;
                led_send(fd, channel, 0x20 + idx, 0);
            }
        }
    }
}

/* Blinks the MASTER CUE LED at 1Hz while SEARCH (SHIFT+MASTER CUE) is
 * active, so it's obvious the on-screen keyboard is open. */
static void search_led_tick(int fd)
{
    if (!search_active)
        return;

    long long current = now_ms();

    if (current - search_blink_ms >= 1000) {
        search_blink_ms = current;
        search_led_on = !search_led_on;
        led_send(fd, 6, 0x63, search_led_on);
    }
}

/* Turns SEARCH off and restores the MASTER CUE LED. There is no real
 * feedback from the engine about the on-screen keyboard closing, so this is
 * called as a best-effort heuristic from any other navigation action
 * (browsing, backing out, loading) -- better to occasionally stop a blink
 * too early than to leave it stuck forever after the user left some other
 * way than pressing SHIFT+MASTER CUE again. */
static void search_cancel(int fd)
{
    if (!search_active)
        return;
    search_active = 0;
    search_led_on = 0;
    led_send(fd, 6, 0x63, led_master_cue);
}

static void led_clear_deck(int fd, int deck)
{
    int channel = deck;

    led_play[deck] = 0;
    led_sync[deck] = 0;
    led_master[deck] = 0;
    led_loop_in[deck] = 0;
    led_loop_out[deck] = 0;
    led_reloop[deck] = 0;

    led_send(fd, channel, 0x0b, 0);
    led_send(fd, channel, 0x0c, 0);
    led_send(fd, channel, 0x58, 0);
    led_send(fd, channel, 0x5c, 0);
    led_send(fd, channel, 0x10, 0);
    led_send(fd, channel, 0x11, 0);
    led_send(fd, channel, 0x4d, 0);
}

static void led_loaded(int fd, int deck)
{
    led_clear_deck(fd, deck);
    midi_write3(fd, 0x9f, deck, 0x7f);
}

static void led_midi_init(int fd)
{
    memset(led_play, 0, sizeof(led_play));
    memset(led_sync, 0, sizeof(led_sync));
    memset(led_master, 0, sizeof(led_master));
    memset(led_channel_cue, 0, sizeof(led_channel_cue));
    memset(led_loop_in, 0, sizeof(led_loop_in));
    memset(led_loop_out, 0, sizeof(led_loop_out));
    memset(led_reloop, 0, sizeof(led_reloop));

    led_master_cue = 0;
    led_beat_fx = 0;

    for (int deck = 0; deck < 2; deck++) {
        int pad_channel = deck == 0 ? MC_PAD1 : MC_PAD2;

        led_beat_loop_pad[deck] = -1;
        memset(led_beat_jump_deadline[deck], 0,
               sizeof(led_beat_jump_deadline[deck]));

        for (int mode = 0; mode < 8; mode++)
            for (int idx = 0; idx < 8; idx++)
                led_send(fd, pad_channel, mode * 16 + idx, 0);
    }

    led_clear_deck(fd, 0);
    led_clear_deck(fd, 1);

    /* Sound Color FX default is type 1 (FILTER, keyshim's mixer_defaults());
     * light pad 0 in the PAD FX1 page to match, so it's already correct the
     * first time SHIFT+BEAT LOOP is pressed after startup. */
    color_fx_pad_idx[0] = 0;
    color_fx_pad_idx[1] = 0;
    color_fx_led_shown[0] = 0;
    color_fx_led_shown[1] = 0;
    color_fx_led_pending[0] = -1;
    color_fx_led_pending[1] = -1;
    led_send(fd, MC_PAD1, 0x10, 1);
    led_send(fd, MC_PAD2, 0x10, 1);

    midi_write3(fd, 0x9f, 0x00, 0);
    midi_write3(fd, 0x9f, 0x01, 0);

    led_send(fd, 0, 0x54, 0);
    led_send(fd, 1, 0x54, 0);
    led_send(fd, 6, 0x63, 0);
    led_send(fd, 4, 0x47, 0);
}

static void led_handle_input(int fd, int channel, int note, int on)
{
    int deck;

    /*
     * Feedback dos performance pads.
     *
     * HOT CUE:
     *   permanece aceso; SHIFT+PAD apaga.
     *
     * BEAT LOOP:
     *   permanece aceso enquanto o loop local estiver ativo.
     *
     * BEAT JUMP:
     *   permanece aceso por um segundo após o acionamento.
     *
     * Outros modos:
     *   indicação momentânea.
     */
    if (channel == MC_PAD1 || channel == MC_PAD1_SH ||
        channel == MC_PAD2 || channel == MC_PAD2_SH) {
        int shifted = channel == MC_PAD1_SH || channel == MC_PAD2_SH;
        int normal_channel =
            (channel == MC_PAD1 || channel == MC_PAD1_SH)
                ? MC_PAD1 : MC_PAD2;
        int pad_deck =
            (channel == MC_PAD1 || channel == MC_PAD1_SH) ? 0 : 1;
        int mode = (note >> 4) & 0x0f;
        int idx = note & 0x0f;

        if (idx > 7 || mode > 7)
            return;

        /* O transporte de estado STEMS é o único proprietário destes LEDs.
           Não permita que o release físico sobrescreva o estado confirmado. */
        if (mode == 5)
            return;

        /* Sound Color FX type select (PAD FX1): "radio button" -- só o pad
         * do tipo ativo fica aceso por deck. Estado é local (nós decidimos o
         * tipo, não há confirmação nativa pra consultar), então não precisa
         * de telemetria de volta como o STEMS. */
        if (mode == 1) {
            if (on && idx < COLOR_FX_TYPE_COUNT) {
                if (opt_verbose)
                    logmsg("  color fx LED deck%d scheduled idx=%d "
                           "(shown=%d)\n", pad_deck + 1, idx,
                           color_fx_led_shown[pad_deck]);
                color_fx_led_pending[pad_deck] = idx;
                color_fx_led_deadline[pad_deck] = now_ms() + 15;
            }
            return;
        }

        /* SHIFT+PAD em Hot Cue: apagar o LED persistente. */
        if (shifted && mode == 0) {
            if (on)
                led_send(fd, normal_channel, idx, 0);
            return;
        }

        /* Hot Cue permanece aceso. */
        if (!shifted && mode == 0) {
            if (on)
                led_send(fd, normal_channel, idx, 1);
            return;
        }

        /* Beat Jump acende durante um segundo. */
        if (!shifted && mode == 2) {
            if (on) {
                led_send(fd, normal_channel, note, 1);
                led_beat_jump_deadline[pad_deck][idx] =
                    now_ms() + 1000;
            }
            return;
        }

        /* Beat Loop: um pad fica travado por deck. */
        if (!shifted && mode == 6) {
            if (on) {
                int previous = led_beat_loop_pad[pad_deck];

                if (previous == idx) {
                    led_send(fd, normal_channel, 0x60 + idx, 0);
                    led_beat_loop_pad[pad_deck] = -1;
                } else {
                    if (previous >= 0)
                        led_send(fd, normal_channel,
                                 0x60 + previous, 0);

                    led_send(fd, normal_channel, 0x60 + idx, 1);
                    led_beat_loop_pad[pad_deck] = idx;
                }
            }
            return;
        }

        led_send(fd, channel, note, on);
        return;
    }

    /* Note 0x3f on a deck's own channel is that deck's physical SHIFT key
     * (confirmed live: holding it changes other buttons' note numbers, e.g.
     * PLAY 0x0b -> 0x47 CENSOR/REVERSE). It carries no meaning by itself for
     * its own channel -- earlier code wrongly treated it as "LOAD", which
     * silently reloaded the deck every time SHIFT was held for any other
     * combo. The real LOAD button is note 0x46/0x47 on MC_MIXER (confirmed
     * live), handled below. We still track it in shift_held[] because
     * MC_MIXER controls (e.g. MASTER CUE) need to know SHIFT is held even
     * though it's on a different MIDI channel. */
    if ((channel == 0 || channel == 1) && note == 0x3f) {
        shift_held[channel] = on;
        if (on) {
            if (master_rec_hold_started_ms < 0) {
                master_rec_hold_started_ms = now_ms();
                master_rec_hold_fired = 0;
                if (opt_verbose)
                    logmsg("  SHIFT hold started (MASTER REC at 3s)\n");
            }
        } else if (!shift_held[0] && !shift_held[1]) {
            master_rec_hold_started_ms = -1;
            master_rec_hold_fired = 0;
        }
        return;
    }

    /* LOAD alternativo enviado pelo canal do browser. */
    if (channel == 6 && (note == 0x46 || note == 0x47)) {
        if (on) {
            led_loaded(fd, note == 0x46 ? 0 : 1);
            browser_open = 0;
            search_cancel(fd);
        }
        return;
    }

    /* BROWSE push: 1st press opens the library; while open, press
     * selects/enters instead. K_SOURCE was tried for the opening press but
     * it detours through the SOURCE/device-picker screen every time
     * (confirmed live: press -> SOURCE screen -> press again -> file list).
     * K_SELECTOR alone was also tried and does nothing from the closed/deck
     * view. K_BACK (the same key SHIFT+push already sends) goes straight to
     * the file list, confirmed live -- reused here for the plain push too. */
    if (channel == MC_MIXER && note == 0x41) {
        if (on) {
            browser_press_key = browser_open ? K_SELECTOR : K_BACK;
            browser_open = 1;
            search_cancel(fd);
        }
        send_ctrl(browser_press_key, on ? OP_PRESS : OP_RELEASE,
                  CH_GLOBAL, 0, 0.0f, 0);
        return;
    }

    /* SHIFT+push resolves to BACK once already browsing (see
     * handle_interface_toggle_note()) -- that's a reasonable "I'm leaving
     * search" signal too, so cancel it here where fd is available (that
     * function doesn't have it). Only on the BACK branch (browser_open==1);
     * the SOURCE branch is covered by the plain-push case above once the
     * user actually presses something there. */
    if (channel == MC_MIXER && note == 0x42 && on && browser_open)
        search_cancel(fd);

    /* Note 0x42 (SHIFT+push) itself is fully handled in
     * handle_interface_toggle_note() (called from handle_note(), after this
     * function): it reads browser_open to choose BACK vs SOURCE and must
     * see its *current* value, so this function must not touch it here.
     * (It used to reset browser_open=0 on press, which ran before that
     * decision and made it always see "closed" -> always send SOURCE,
     * silently breaking BACK while actually browsing.) */

    if (channel == 0 || channel == 1) {
        deck = channel;

        switch (note) {
        case 0x0b: /* PLAY/PAUSE */
        case 0x0c: /* CUE */
            /* O estado real do player é o único proprietário destes LEDs. */
            return;

        case 0x58: /* BEAT SYNC -> MASTER TEMPO. O estado real do engine
                    * (RX3_LED_MASTER_TEMPO em native_led_state_tick()) é o
                    * único dono deste LED; antes era um toggle local que
                    * divergia do estado real (ex.: no boot, quando o mixer
                    * state persistido já entra com master tempo ligado). */
            return;

        case 0x5c: /* MASTER */
            if (on) {
                int other = deck ^ 1;
                led_master[deck] ^= 1;

                if (led_master[deck]) {
                    led_master[other] = 0;
                    led_send(fd, other, 0x5c, 0);
                }

                led_send(fd, deck, note, led_master[deck]);
            }
            return;

        case 0x47: /* CENSOR/REVERSE */
        case 0x51: /* CUE/LOOP CALL 1/2X */
        case 0x53: /* CUE/LOOP CALL 2X */
            led_send(fd, deck, note, on);
            return;

        case 0x10: /* LOOP IN */
            if (on) {
                led_loop_in[deck] = 1;
                led_loop_out[deck] = 0;
                led_reloop[deck] = 0;

                led_send(fd, deck, 0x10, 1);
                led_send(fd, deck, 0x11, 0);
                led_send(fd, deck, 0x4d, 0);
            }
            return;

        case 0x11: /* LOOP OUT */
            if (on) {
                led_loop_out[deck] = 1;
                led_reloop[deck] = 1;

                led_send(fd, deck, 0x11, 1);
                led_send(fd, deck, 0x4d, 1);
            }
            return;

        case 0x4d: /* RELOOP/EXIT */
            if (on) {
                led_reloop[deck] ^= 1;
                led_send(fd, deck, 0x4d, led_reloop[deck]);

                if (!led_reloop[deck]) {
                    led_loop_in[deck] = 0;
                    led_loop_out[deck] = 0;
                    led_send(fd, deck, 0x10, 0);
                    led_send(fd, deck, 0x11, 0);

                    if (led_beat_loop_pad[deck] >= 0) {
                        int pad_channel =
                            deck == 0 ? MC_PAD1 : MC_PAD2;

                        led_send(fd, pad_channel,
                                 0x60 + led_beat_loop_pad[deck], 0);
                        led_beat_loop_pad[deck] = -1;
                    }
                }
            }
            return;

        case 0x54: /* CHANNEL CUE */
            if (on) {
                led_channel_cue[deck] ^= 1;
                led_send(fd, deck, 0x54, led_channel_cue[deck]);
                send_tap(K_HPCUECH, deck + 1);
            }
            return;
        }
    }

    if (channel == 6 && note == 0x63) { /* MASTER CUE */
        if (on) {
            led_master_cue ^= 1;
            led_send(fd, 6, 0x63, led_master_cue);
            send_tap(K_MASTERCUE, CH_GLOBAL);
        }
        return;
    }

    /* SHIFT+MASTER CUE: the hardware sends a *different* note (0x78) for
     * this, not 0x63 with a separate SHIFT flag -- confirmed live via raw
     * MIDI capture, same pattern as PLAY(0x0b)->CENSOR(0x47) and
     * LOAD(0x46)->SOURCE(0x68). SEARCH toggle. LED blink is driven by
     * search_led_tick(); this only sets the trigger. */
    if (channel == 6 && note == 0x78) {
        if (on) {
            search_active = !search_active;
            if (search_active) {
                send_tap(K_SEARCH, CH_GLOBAL);
                search_blink_ms = now_ms();
                search_led_on = 1;
                led_send(fd, 6, 0x63, 1);
            } else {
                send_tap(K_BACK, CH_GLOBAL);
                search_led_on = 0;
                led_send(fd, 6, 0x63, led_master_cue);
            }
        }
        return;
    }

    /* BEAT FX ON/OFF LED: driven from rbp's real state by bfx_state_tick(),
     * not a blind local toggle here any more (see its own comment) -- the
     * physical press still reaches rbp as K_BFX through the plain notemap
     * dispatch in handle_note(), unrelated to this LED-only function. */
}

/* ---------------- device discovery ---------------- */
static int find_ddj_card(void)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256];
    int card = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        int idx;
        if (sscanf(line, " %d [", &idx) == 1)
            card = idx;
        else if (card >= 0 && strstr(line, "DDJ")) {
            fclose(f);
            return card;
        }
    }
    fclose(f);
    return -1;
}

static char *find_midi_node(int card)
{
    static char path[128];
    DIR *d = opendir("/dev/snd");
    struct dirent *e;
    if (!d)
        return NULL;
    while ((e = readdir(d))) {
        int c, dev;
        if (sscanf(e->d_name, "midiC%dD%d", &c, &dev) == 2 && c == card) {
            snprintf(path, sizeof(path), "/dev/snd/%.32s", e->d_name);
            closedir(d);
            return path;
        }
    }
    closedir(d);
    return NULL;
}

static int open_midi(void)
{
    if (midi_dev) {
        snprintf(midi_node_path, sizeof(midi_node_path), "%s", midi_dev);
        return open(midi_dev, O_RDWR | O_NONBLOCK);
    }

    int card = find_ddj_card();
    if (card < 0) {
        logmsg("ddj400: no DDJ-400 card in /proc/asound/cards\n");
        return -1;
    }
    char *node = find_midi_node(card);
    if (!node) {
        logmsg("ddj400: card %d has no rawmidi node in /dev/snd\n", card);
        return -1;
    }
    snprintf(midi_node_path, sizeof(midi_node_path), "%s", node);
    logmsg("ddj400: using %s (ALSA card %d)\n", node, card);
    return open(node, O_RDWR | O_NONBLOCK);
}

static void list_map(void)
{
    printf("14-bit controls:\n");
    for (int i = 0; i < cc14_n; i++)
        printf("  ch%-2d CC 0x%02x + 0x%02x -> key 0x%04x op %s send-ch %d%s\n",
               cc14[i].ch + 1, cc14[i].msb_cc, cc14[i].msb_cc + 0x20,
               cc14[i].key, cc14[i].op == OP_VALUE ? "VALUE " : "ROTATE",
               cc14[i].send_ch, cc14[i].signed_norm ? " (signed)" : "");
    printf("notes:\n");
    for (int i = 0; i < NNMAP; i++)
        printf("  ch%-2d note 0x%02x -> key 0x%04x send-ch %d  %s\n",
               notemap[i].ch + 1, notemap[i].note, notemap[i].key,
               notemap[i].send_ch, notemap[i].name);
    printf("pads: note = mode*16 + (pad-1) -> keys 0x%04x..0x%04x "
           "(modes 0-7 = hot cue/pad fx/beat jump/sampler/keyboard/beat loop/key shift)\n", K_PAD1, K_PAD1 + 7);
}

static void run_device(int fd);

/* Set on SIGTERM/SIGINT (systemd's ExecStop, or an interactive Ctrl-C) so
 * the poll loops can flush mixer_state_save() and exit cleanly instead of
 * either losing the last values or being mistaken for "controller
 * unplugged" and looping into a reconnect wait. */
static volatile sig_atomic_t g_shutdown;

static void handle_shutdown_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

int main(int argc, char **argv)
{
    int opt, fd;

    while ((opt = getopt(argc, argv, "vsld:f:J:F")) != -1) {
        switch (opt) {
        case 'v': opt_verbose = 1; break;
        case 's': opt_sniff = 1; opt_verbose = 1; break;
        case 'd': midi_dev = optarg; break;
        case 'f': fifo_path = optarg; break;
        case 'J': jog_ppr = (float)atof(optarg); break;
        case 'F': opt_filter_init = 1; break;
        case 'l': build_cc14(); list_map(); return 0;
        default:
            fprintf(stderr, "usage: %s [-v] [-s] [-l] [-d dev] [-f fifo] "
                            "[-J jog_ppr] [-F]\n", argv[0]);
            return 2;
        }
    }

    {
        const char *sp = getenv("RX3_STEMS_PADS");
        stems_pads_touch = sp && strcmp(sp, "touch") == 0;
        logmsg("ddj400: STEMS pads (PAD FX 2) -> %s\n",
               stems_pads_touch ? "touch shim FIFO (toolkit core)"
                                : "command file (legacy core)");
    }

    {
        const char *ka = getenv("DDJ400_KEEPALIVE");
        opt_keepalive = !(ka && strcmp(ka, "0") == 0);
        g_last_input_ms = g_last_keepalive_ms = now_ms();
        logmsg("ddj400: keepalive (Active Sensing after %llds idle) -> %s\n",
               KEEPALIVE_IDLE_MS / 1000, opt_keepalive ? "on" : "off");
    }

    build_cc14();
    jogs[0].midi_ch = MC_DECK1; jogs[0].send_ch = 1;
    jogs[1].midi_ch = MC_DECK2; jogs[1].send_ch = 2;
    searches[0].midi_ch = MC_DECK1; searches[0].send_ch = 1;
    searches[1].midi_ch = MC_DECK2; searches[1].send_ch = 2;

    logmsg("ddj400-bridge: %s -> %s (jog %g pulses/rev)\n",
           opt_sniff ? "SNIFF (no output)" : "bridge", fifo_path, (double)jog_ppr);

    if (!opt_sniff) {
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (fifo_fd < 0)
            logmsg("ddj400: cannot open %s: %s (will retry per event)\n",
                   fifo_path, strerror(errno));
        if (opt_filter_init)
            for (int ch = 1; ch <= 2; ch++)
                send_tap(K_FILTER, ch);
    }

    vu_init_path();
    master_rec_init_path();
    native_led_state_init_path();
    stems_pad_init_paths();
    mixer_state_init_path();
    mixer_state_load();

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handle_shutdown_signal;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
    }

    /* Hot-plug friendly: keep waiting for the controller instead of dying,
     * so the bridge can be started before the DDJ-400 is plugged in. */
    while (!g_shutdown) {
        fd = open_midi();
        if (fd < 0) {
            if (midi_dev)
                return 1;              /* explicit -d device: fail fast */
            logmsg("ddj400: waiting for a DDJ-400 to be plugged in...\n");
            sleep(2);
            continue;
        }
        vu_midi_init(fd);
        led_midi_init(fd);
        native_led_state_reset();
        run_device(fd);
        midi_write3(fd, 0xb0, 0x02, 0);
        midi_write3(fd, 0xb1, 0x02, 0);
        close(fd);
        if (g_shutdown)
            break;
        logmsg("ddj400: device closed, waiting for reconnect...\n");
        sleep(2);
    }

    if (!opt_sniff) {
        mixer_state_flush();
        logmsg("ddj400: shutting down, mixer state saved\n");
    } else {
        logmsg("ddj400: shutting down (sniff mode, nothing to save)\n");
    }
    return 0;
}

static void run_device(int fd)
{
    unsigned char buf[512];
    int status = 0, d1 = 0, need = 0;

    while (!g_shutdown) {
        struct pollfd pfd;
        int pr;
        ssize_t n;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 20);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            logmsg("ddj400: poll: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) {
            interface_toggle_tick();
            master_rec_hold_tick();
            jog_tick();
            search_tick();
            mixer_state_tick();
            vu_tick(fd);
            native_led_state_tick(fd);
            stems_pad_state_tick(fd);
            key_pad_state_tick(fd);
            bfx_state_tick(fd);
            pad_led_tick(fd);
            color_fx_led_tick(fd);
            search_led_tick(fd);
            keepalive_tick(fd);
            continue;
        }
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            logmsg("ddj400: read: %s (unplugged?)\n", strerror(errno));
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            unsigned char b = buf[i];
            if (b >= 0xF8)
                continue;
            if (b & 0x80) {
                status = b;
                need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
                d1 = 0;
                continue;
            }
            if (!status)
                continue;
            if (need == 2) {
                d1 = b;
                need = 1;
                continue;
            }
            need = 2;
            int type = status & 0xF0;
            int ch = status & 0x0F;
            if (type == 0x90 || type == 0x80) {
                int on = (type == 0x90 && b > 0);
                keepalive_note_input();
                led_handle_input(fd, ch, d1, on);
                handle_note(ch, d1, on);
                if (on && d1 != 0x3f)
                    master_rec_cancel_hold();
            }
            else if (type == 0xB0) {
                keepalive_note_input();
                handle_cc(ch, d1, b);
                master_rec_cancel_hold();
            }
            else if (opt_sniff)
                logmsg("MIDI ch%d status=0x%02x d1=%d d2=%d\n", ch + 1, status, d1, b);
        }
        interface_toggle_tick();
        master_rec_hold_tick();
        jog_tick();
        search_tick();
        mixer_state_tick();
        vu_tick(fd);
            native_led_state_tick(fd);
            stems_pad_state_tick(fd);
            key_pad_state_tick(fd);
            bfx_state_tick(fd);
            pad_led_tick(fd);
            color_fx_led_tick(fd);
            search_led_tick(fd);
            keepalive_tick(fd);
    }
    return;
}
