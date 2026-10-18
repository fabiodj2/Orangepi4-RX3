/* keyshim.c - LD_PRELOAD shim: inject XDJ-RX3 button presses into rbp from a
 * FIFO, so the host-side viewer can map keyboard keys to rb buttons.
 *
 * Mechanism (from Prime GO knobshim2.c, live-verified):
 *   IUiObjManager singleton @ 0x2685f2c; KeyManager = *(obj+100);
 *   vtable word 2 = IKeyManager::sendKey(keycode, op, ch, param, f, l).
 *   op 0 = press, 2 = release, 4 = rotate, 5 = absolute value.
 *
 * Inputs (both live in /tmp, which the chroot bind-mounts to the host /tmp):
 *   /tmp/rb-keys.fifo  12-byte records { int32 key, ch, down }  (buttons)
 *   /tmp/rb-ctrl.fifo  24-byte records { key, ch, op, param, float f, l }
 *                      (controller events: faders, EQ, jog, relative knobs)
 *
 * The control FIFO is what a real MIDI controller bridge (ddj400-bridge.c)
 * feeds.  While such a bridge is active the periodic re-application of the
 * synthetic mixer defaults is disabled, so physical faders are not fought over.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/mman.h>

static int real_open(const char *p, int flags)
{
    return syscall(SYS_openat, AT_FDCWD, p, flags, 0);
}
static ssize_t real_read(int fd, void *buf, size_t n)
{
    return syscall(SYS_read, fd, buf, n);
}
static int real_close(int fd)
{
    return syscall(SYS_close, fd);
}

/* ---- tiny logger (raw syscalls, no printf) ---- */
static void klog_str(const char *s)
{
    int fd = syscall(SYS_openat, AT_FDCWD, "/tmp/keyshim.log",
                     O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)syscall(SYS_write, fd, s, strlen(s));
        (void)syscall(SYS_close, fd);
    }
}
static void klog_hex(const char *label, unsigned int v)
{
    char buf[64];
    int n = 0;
    static const char hex[] = "0123456789abcdef";
    while (*label && n < 32) buf[n++] = *label++;
    for (int i = 7; i >= 0; i--) buf[n++] = hex[(v >> (i * 4)) & 0xf];
    buf[n++] = '\n';
    buf[n] = 0;
    klog_str(buf);
}
static void klog_dec(const char *label, int v)
{
    char buf[64];
    int n = 0, i, neg = 0;
    char tmp[16];
    int t = 0;
    while (*label && n < 32) buf[n++] = *label++;
    if (v < 0) { neg = 1; v = -v; }
    do { tmp[t++] = "0123456789"[v % 10]; v /= 10; } while (v);
    if (neg) tmp[t++] = '-';
    for (i = t - 1; i >= 0; i--) buf[n++] = tmp[i];
    buf[n++] = '\n';
    buf[n] = 0;
    klog_str(buf);
}

/* ---- rbp process check ---- */
static int is_rbp_process(void)
{
    char cmd[128];
    int fd, n;
    fd = real_open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    n = (int)real_read(fd, cmd, sizeof(cmd) - 1);
    real_close(fd);
    if (n <= 0) return 0;
    cmd[n] = '\0';
    for (int i = 0; i < n; i++)
        if (cmd[i] == '\0') cmd[i] = ' ';
    return strstr(cmd, "rbp") != NULL;
}

/* ---- KeyManager injection ---- */
#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#define KEY_MANAGER_OFF   100
#define SENDKEY_VTABLE_WORD 2

#define OP_PRESS   0
#define OP_RELEASE 2
#define OP_ROTATE  4
#define OP_VALUE   5

static int rbp_checked = -1;

static void *get_key_manager(void)
{
    void **p;
    void *mgr;
    if (rbp_checked < 0)
        rbp_checked = is_rbp_process();
    if (!rbp_checked)
        return NULL;
    p = (void **)UI_OBJ_MGR_GLOBAL;
    if (!p) return NULL;
    mgr = *p;
    if (!mgr) return NULL;
    return *(void **)((char *)mgr + KEY_MANAGER_OFF);
}

typedef void (*sendkey_fn)(void *km, int keycode, int op, int ch,
                           long param, float f, long l);

static void send_rx_key(int keycode, int op, int ch)
{
    void *km = get_key_manager();
    if (!km) {
        klog_str("send_rx_key: no key manager\n");
        return;
    }
    void **vt = *(void ***)km;
    sendkey_fn fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD];
    if (!fn) {
        klog_str("send_rx_key: vtable fn null\n");
        return;
    }
    klog_hex("sendkey key=", (unsigned)keycode);
    fn(km, keycode, op, ch, 0, 0.0f, 0);
}

static void send_rx_key_fl(int keycode, int op, int ch, long param, float f, long l)
{
    void *km = get_key_manager();
    if (!km)
        return;
    void **vt = *(void ***)km;
    sendkey_fn fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD];
    if (fn)
        fn(km, keycode, op, ch, param, f, l);
}

/* Mixer input routing (djengine::MixerRouteMngr) @ 0x01149f50/0x01149f54.
 * On the RX3 the physical DECK/LINE switches assign each mixer channel to a
 * player; with no sub-microcontroller both channels come up pointing at
 * Player 0, so the *channel-2 fader controls deck 1* (both faders move the
 * same sound).  Pin them like PrimeBox's knobshim2 (docs/05 §5):
 *   ch1 -> route object 0x01149f08 (Player 0 / deck 1)
 *   ch2 -> route object 0x01149f10 (Player 1 / deck 2)   */
#define MIX_ROUTE_CH1   0x01149f50UL
#define MIX_ROUTE_CH2   0x01149f54UL
#define MIX_ROUTE_PLAYER0 0x01149f08UL
#define MIX_ROUTE_PLAYER1 0x01149f10UL

static void mixer_route(void)
{
    *(volatile unsigned int *)MIX_ROUTE_CH1 = MIX_ROUTE_PLAYER0;
    *(volatile unsigned int *)MIX_ROUTE_CH2 = MIX_ROUTE_PLAYER1;
}

/* Native DjEngineIF instance and crossfader assignment.
 * Assignment 0 bypasses the crossfader.  The RX3 panel normally initializes
 * input 0 as A and input 1 as B; without the panel both remain bypassed. */
#define DJ_ENGINE_GLOBAL            0x0268617cUL
#define FN_SET_CROSSFADER_ASSIGN    0x004cc0cUL

static int mixer_crossfader_assign(void)
{
    void *engine = *(void * volatile *)DJ_ENGINE_GLOBAL;
    void (*set_assign)(void *, int, int);

    if (!engine)
        return 0;

    set_assign = (void (*)(void *, int, int))
                 FN_SET_CROSSFADER_ASSIGN;

    set_assign(engine, 0, 1); /* mixer input 0 / deck 1 -> A */
    set_assign(engine, 1, 2); /* mixer input 1 / deck 2 -> B */

    klog_str("keyshim: crossfader assigned deck1=A deck2=B\n");
    return 1;
}

/* Without the RX3 sub-microcontroller the mixer controls have no physical
 * position, so the engine keeps its defaults — channel faders 0.0 (= silence)
 * and the crossfader hard left.  Push sane defaults once the KeyManager is up
 * (same idea as the Prime GO knobshim2 mixer init, SESSION_STATE_12).
 * keycodes: FADER 0x501e, TRIM 0x5019, EQH/M/L 0x501a/b/c, XFADER 0x6017. */

/* Seleciona o tipo de Sound Color FX nativo num canal (0=deck1, 1=deck2).
 * O painel original do RX3 teria um seletor de 3 posições ao lado do knob
 * de Color FX para isso; a DDJ-400 não tem esse seletor físico, só o knob
 * de valor (ver ddj400-bridge.c, SHIFT+BEAT LOOP). `type` ainda não tem
 * significado confirmado além de 1=FILTER (única chamada até agora,
 * sempre nos dois canais, no startup). */
static int rx3_native_color_fx_select(int deck, int type)
{
    void *engine = *(void *volatile *)0x0268617cUL;

    if (!engine || deck < 0 || deck > 1)
        return 0;

    ((void (*)(void *, int, int))0x004e2c4UL)(engine, deck, type);

    klog_dec("keyshim: native color fx deck=", deck);
    klog_dec("         type=", type);
    return 1;
}

static void mixer_defaults(void)
{
    rx3_native_color_fx_select(0, 1);
    rx3_native_color_fx_select(1, 1);
    int ch;
    mixer_route();  /* re-assert Input 0->deck1, Input 1->deck2 */
    mixer_crossfader_assign();
    for (ch = 1; ch <= 2; ch++) {
        send_rx_key_fl(0x501e, OP_ROTATE, ch, 1023, 1.0f, 0); /* channel fader up */
        send_rx_key_fl(0x5019, OP_ROTATE, ch, 512, 0.5f, 0);  /* trim center */
        send_rx_key_fl(0x501a, OP_ROTATE, ch, 512, 0.5f, 0);  /* HI */
        send_rx_key_fl(0x501b, OP_ROTATE, ch, 512, 0.5f, 0);  /* MID */
        send_rx_key_fl(0x501c, OP_ROTATE, ch, 512, 0.5f, 0);  /* LOW */
    }
    send_rx_key_fl(0x6017, OP_ROTATE, 0, 512, 0.5f, 0);       /* crossfader center */
}

/* ---- PanelComPeer "UiMain" message-pump thread ----
 * The uif::MsgManager that IKeyManager::sendKey posts to is the MsgManager
 * sub-object of the PanelComPeerLinux singleton (mainMsgManager @ 0x268612c).
 * Its queue is drained ONLY by PanelComPeerLinux::run() (thread name "UiMain"):
 * it loops takeOutMessage() -> MsgManager::handleMessage().  That thread is
 * started by PanelComPeerLinux::openDevice(); on the phone the subucom open
 * path never runs, so no UiMain thread exists and every injected key sits in
 * the queue forever (IKeyManager::onKey is never reached).  Start it here.
 */
#define PANEL_PEER_INSTANCE_GLOBAL 0x26870b0UL  /* ui::PanelComPeerLinux::instance */
#define PANEL_PEER_NUM_DEVICES_OFF 0x94         /* +0x94 = nDevices */
#define JUCE_THREAD_OFF            4            /* juce::Thread subobject at +4 */
#define FN_THREAD_IS_RUNNING       0x3b6360UL   /* juce::Thread::isThreadRunning() const */
#define FN_THREAD_START            0x3b6674UL   /* juce::Thread::startThread(int) */
#define FN_OPEN_DEVICE             0x366804UL   /* ui::PanelComPeerLinux::openDevice(int) */

static int ui_pump_running(void)
{
    void *inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst)
        return 0;
    return ((int (*)(void *))FN_THREAD_IS_RUNNING)((char *)inst + JUCE_THREAD_OFF);
}

static int ui_pump_ensure(void)
{
    void *inst;
    int n;
    if (ui_pump_running())
        return 1;
    inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst)
        return 0;
    n = *(int *)((char *)inst + PANEL_PEER_NUM_DEVICES_OFF);
    if (n <= 0)
        return 0;   /* run() returns immediately while nDevices <= 0 */

    klog_str("keyshim: UiMain pump not running -> openDevice(0)\n");
    ((int (*)(void *, int))FN_OPEN_DEVICE)(inst, 0);

    if (!ui_pump_running()) {
        klog_str("keyshim: openDevice did not start it -> startThread\n");
        ((void (*)(void *, int))FN_THREAD_START)((char *)inst + JUCE_THREAD_OFF, 5);
    }
    if (ui_pump_running()) {
        klog_str("keyshim: UiMain pump started\n");
        return 1;
    }
    klog_str("keyshim: UiMain pump FAILED to start\n");
    return 0;
}

/* ---- clear the stale "USB Error / Remove Device" browse caution ----
 * TotalCnt_CautionTASK sets [0x05a191fc]=0x12d (caution 0xc021) from early USB
 * initialisation.  While non-zero the UI disables browse/touch handling.
 * On the phone no USB mount ever runs the
 * code that clears it, so clear it ourselves (same as knobshim2's usb_auto). */
#define BROWSE_CAUTION_ID 0x05a191fcUL

static void clear_browse_caution(void)
{
    volatile unsigned int *p = (volatile unsigned int *)BROWSE_CAUTION_ID;
    if (*p != 0)
        *p = 0;
}

/* ---- FIFO input ---- */
#define KEY_FIFO  "/tmp/rb-keys.fifo"
#define CTRL_FIFO "/tmp/rb-ctrl.fifo"

struct key_ev {
    int32_t key;
    int32_t ch;
    int32_t down;
};

/* controller event: op is one of OP_PRESS/OP_RELEASE/OP_ROTATE/OP_VALUE.
 *   op 4 (ROTATE): param = relative delta, f = speed, l = position/counter
 *   op 5 (VALUE) : param = 10-bit absolute, f = normalised value
 * Same payload layout as the sendKey vtable call, so it can be forwarded
 * verbatim.  Must stay 24 bytes and fixed-size (a FIFO has no framing). */
struct ctrl_ev {
    int32_t key;
    int32_t ch;
    int32_t op;
    int32_t param;
    float   f;
    int32_t l;
};

/* ---- native pad-bank adapter ---- */
#define RX3_NATIVE_PAD_MODE_GETTER 0x000fd3ccUL
#define RX3_NATIVE_PAD_SELECTOR    0x4113

static unsigned char rx3_pad_held[2][8];

static int rx3_native_pad_mode(int deck)
{
    int (*get_mode)(int) =
        (int (*)(int))RX3_NATIVE_PAD_MODE_GETTER;

    if (deck < 0 || deck > 1)
        return -1;

    return get_mode(deck);
}

static int rx3_select_pad_bank(int deck, int target)
{
    int current;
    int expected;
    int tries;

    if (deck < 0 || deck > 1 || target < 0 || target > 7)
        return 0;

    current = rx3_native_pad_mode(deck);

    if (current == target)
        return 1;

    if (current < 0 || current > 7)
        return 0;

    expected = (current % 4 == target % 4)
        ? (current ^ 4)
        : (target % 4);

    send_rx_key_fl(
        RX3_NATIVE_PAD_SELECTOR + target % 4,
        OP_PRESS,
        deck + 1,
        0, 0.0f, 0
    );

    send_rx_key_fl(
        RX3_NATIVE_PAD_SELECTOR + target % 4,
        OP_RELEASE,
        deck + 1,
        0, 0.0f, 0
    );

    for (tries = 0; tries < 25; tries++) {
        usleep(10000);

        if (rx3_native_pad_mode(deck) == expected)
            break;
    }

    if (rx3_native_pad_mode(deck) != expected)
        return 0;

    if (expected != target) {
        send_rx_key_fl(
            RX3_NATIVE_PAD_SELECTOR + target % 4,
            OP_PRESS,
            deck + 1,
            0, 0.0f, 0
        );

        send_rx_key_fl(
            RX3_NATIVE_PAD_SELECTOR + target % 4,
            OP_RELEASE,
            deck + 1,
            0, 0.0f, 0
        );

        for (tries = 0; tries < 25; tries++) {
            usleep(10000);

            if (rx3_native_pad_mode(deck) == target)
                break;
        }
    }

    return rx3_native_pad_mode(deck) == target;
}

static int rx3_handle_tagged_pad(const struct ctrl_ev *ev)
{
    int deck;
    int pad;
    int bank;
    int actual;
    unsigned char *held;

    if ((ev->l & ~7L) != 0x5040L)
        return 0;

    if (ev->key < 0x4117 || ev->key > 0x411e)
        return 0;

    if (ev->ch < 1 || ev->ch > 2)
        return 0;

    if (ev->op != OP_PRESS && ev->op != OP_RELEASE)
        return 0;

    deck = ev->ch - 1;
    pad = ev->key - 0x4117;
    bank = (int)(ev->l & 7L);
    held = &rx3_pad_held[deck][pad];

    if (ev->op == OP_PRESS) {
        actual = rx3_native_pad_mode(deck);

        /* Same *family* as what the bridge asked for (0..3 vs. the
         * double-click "layer 2" variant 4..7 -- e.g. HOT CUE/GATE CUE,
         * BEAT LOOP/BEAT LOOP 2 share a family): trust whichever sub-layer
         * is actually active instead of forcing back to the layer-1 bank.
         * Forcing unconditionally here used to silently undo the native
         * double-click toggle (GATE CUE, BEAT LOOP 2, RELEASE FX, ...)
         * every time a pad was touched. Only force a switch on a genuine
         * cross-family mismatch (e.g. pads sent while on the wrong mode). */
        if (actual < 0 || actual % 4 != bank % 4) {
            if (!rx3_select_pad_bank(deck, bank))
                return 1;
            actual = bank;
        }

        if (*held == (unsigned char)(actual + 1))
            return 1;

        *held = (unsigned char)(actual + 1);

        send_rx_key_fl(
            ev->key, ev->op, ev->ch,
            ev->param, ev->f, 0
        );

        return 1;
    }

    if (*held == 0)
        return 1;

    actual = (int)(*held) - 1;
    *held = 0;

    if (rx3_native_pad_mode(deck) != actual)
        return 1;

    send_rx_key_fl(
        ev->key, ev->op, ev->ch,
        ev->param, ev->f, 0
    );

    return 1;
}

/* Sound Color FX type select: not a real rbp keycode, this value is only
 * ever meaningful to keyshim itself (see ddj400-bridge.c's PAD FX1 page,
 * reached via SHIFT+BEAT LOOP). ev->ch = 1/2 (deck), ev->param = type. */
#define RX3_COLOR_FX_TYPE_SELECT 0x50a7

static int rx3_handle_color_fx_type(const struct ctrl_ev *ev)
{
    if (ev->key != RX3_COLOR_FX_TYPE_SELECT || ev->op != OP_VALUE)
        return 0;

    if (ev->ch < 1 || ev->ch > 2)
        return 1;

    rx3_native_color_fx_select(ev->ch - 1, ev->param);
    return 1;
}



/* ---- Native touchscreen Performance Pads -----------------------------
 *
 * The stock RX3 draws pad labels but does not treat those labels as touch
 * buttons. Intercept only their logical 1280x800 rectangles and translate
 * them to the same native pad keys used by the physical panel.
 */
struct rx3_touch_event {
    uint8_t down;
    uint8_t padding[3];
    int x;
    int y;
};

typedef void (*rx3_touch_fn)(
    void *handler,
    const struct rx3_touch_event *touch,
    void *mode
);

static rx3_touch_fn rx3_original_touch;
static int rx3_touch_held_key;
static int rx3_touch_held_channel;

static int rx3_player_screen_active(void)
{
    void *active =
        *(void * volatile *)(0x024942bcUL + 0x20);

    return active &&
        *(volatile int16_t *)((char *)active + 6) == 0;
}

static int rx3_main_panel_visible(void)
{
    uintptr_t panel;

    if (!rx3_player_screen_active())
        return 0;

    panel = *(volatile uint32_t *)0x0245b880UL;

    for (int index = 0; panel && index < 256; index++) {
        if (*(volatile uint32_t *)(panel + 0x0c) == 136)
            return
                (*(volatile uint32_t *)(panel + 4) & 8) != 0;

        panel = *(volatile uint32_t *)(panel + 0x84);
    }

    return 0;
}

static void rx3_native_pad_touch(
    void *handler,
    const struct rx3_touch_event *touch,
    void *mode)
{
    int key = 0;
    int channel = 0;

    if (rx3_touch_held_key) {
        if (!touch->down) {
            send_rx_key(
                rx3_touch_held_key,
                OP_RELEASE,
                rx3_touch_held_channel
            );

            klog_hex(
                "keyshim: touch pad release=",
                (unsigned)rx3_touch_held_key
            );

            rx3_touch_held_key = 0;
            rx3_touch_held_channel = 0;
        }

        return;
    }

    if (touch->down &&
        !*((uint8_t *)handler + 4) &&
        rx3_main_panel_visible()) {
        /*
         * Native pad-mode selectors:
         * logical y 492..517, four columns per deck.
         */
        if (touch->y >= 492 && touch->y < 518 &&
            touch->x >= 0 && touch->x < 1280) {
            int deck = touch->x / 640;
            int local = touch->x % 640 - 10;
            int column = local >= 0 ? local / 158 : -1;

            if (column >= 0 && column < 4 &&
                local % 158 < 150) {
                key = 0x4113 + column;
                channel = deck + 1;
            }
        }

        /*
         * Native performance pads:
         * row 1: logical y 518..540, pads 1..4
         * row 2: logical y 548..570, pads 5..8
         */
        if (!key) {
            int row =
                touch->y >= 518 && touch->y <= 540 ? 0 :
                touch->y >= 548 && touch->y <= 570 ? 1 : -1;

            int deck =
                touch->x >= 0 && touch->x < 1280
                    ? touch->x / 640 : -1;

            int local =
                deck >= 0 ? touch->x % 640 - 10 : -1;

            int column =
                local >= 0 ? local / 158 : -1;

            if (row >= 0 &&
                deck >= 0 && deck < 2 &&
                column >= 0 && column < 4 &&
                local % 158 < 150) {
                key = 0x4117 + row * 4 + column;
                channel = deck + 1;
            }
        }

        if (key) {
            rx3_touch_held_key = key;
            rx3_touch_held_channel = channel;

            send_rx_key(key, OP_PRESS, channel);

            klog_hex(
                "keyshim: touch pad press=",
                (unsigned)key
            );

            return;
        }
    }

    rx3_original_touch(handler, touch, mode);
}

static void rx3_install_native_pad_touch(void)
{
    uint32_t *entry;
    uint32_t *trampoline;
    void *code_page;
    long page_size;

    if (!is_rbp_process())
        return;

    entry = (uint32_t *)0x002dc104UL;

    if (entry[0] != 0xe92d45f0UL ||
        entry[1] != 0xe1a06002UL) {
        klog_hex("keyshim: touch hook word0=", entry[0]);
        klog_hex("keyshim: touch hook word1=", entry[1]);
        klog_str("keyshim: native pad touch hook rejected\n");
        return;
    }

    page_size = getpagesize();

    trampoline = mmap(
        NULL,
        (size_t)page_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (trampoline == MAP_FAILED) {
        klog_str("keyshim: touch trampoline mmap failed\n");
        return;
    }

    trampoline[0] = entry[0];
    trampoline[1] = entry[1];
    trampoline[2] = 0xe51ff004UL;
    trampoline[3] = 0x002dc10cUL;

    if (mprotect(
            trampoline,
            (size_t)page_size,
            PROT_READ | PROT_EXEC) != 0) {
        klog_str("keyshim: touch trampoline mprotect failed\n");
        return;
    }

    syscall(0xf0002, trampoline, trampoline + 4, 0);

    rx3_original_touch = (rx3_touch_fn)trampoline;

    code_page = (void *)(
        (uintptr_t)entry & ~((uintptr_t)page_size - 1)
    );

    if (mprotect(
            code_page,
            (size_t)page_size,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        klog_str("keyshim: touch code mprotect failed\n");
        return;
    }

    entry[0] = 0xe51ff004UL;
    entry[1] = (uint32_t)(uintptr_t)rx3_native_pad_touch;

    syscall(0xf0002, entry, entry + 2, 0);

    mprotect(
        code_page,
        (size_t)page_size,
        PROT_READ | PROT_EXEC
    );

    klog_str("keyshim: native pad touch hook installed\n");
}

/* Set once a real control surface starts feeding /tmp/rb-ctrl.fifo: from then
 * on the synthetic mixer defaults are no longer re-applied every 10 s (they
 * would snap the physical faders/EQ back). */
static volatile int ctrl_surface_active = 0;

static void *key_thread(void *arg)
{
    struct pollfd pfd;
    int fd = -1;
    (void)arg;

    klog_str("keyshim: thread started\n");

    for (int i = 0; i < 300; i++) {
        if (get_key_manager())
            break;
        usleep(100000);
    }
    if (!get_key_manager()) {
        klog_str("keyshim: key manager never ready\n");
        return NULL;
    }
    klog_hex("keyshim: key manager ready km=", (unsigned)(uintptr_t)get_key_manager());

    /* Dismiss the stale USB-error caution early so browse/touch isn't gated. */
    clear_browse_caution();

    /* Wait for the panel devices to be registered, then make sure the UiMain
     * message-pump thread exists (it is what dispatches posted keys). */
    for (int i = 0; i < 150 && !ui_pump_running(); i++)
        ui_pump_ensure();

    /* DjEngine is created after KeyManager. Wait until it exists so the
     * native A/B assignment cannot be lost during startup. */
    {
        int assigned = 0;

        for (int i = 0; i < 300; i++) {
            if (mixer_crossfader_assign()) {
                assigned = 1;
                break;
            }
            usleep(100000);
        }

        if (!assigned)
            klog_str("keyshim: ERROR crossfader engine not ready\n");
    }

    /* Unmute the mixer after the UI pump and DjEngine are ready. */
    mixer_defaults();
    klog_str("keyshim: mixer defaults sent (ch1->deck1, ch2->deck2, faders up, "
             "trims/EQ/crossfader center)\n");

    for (int i = 0; i < 100; i++) {
        fd = real_open(KEY_FIFO, O_RDWR | O_NONBLOCK);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0) {
        klog_str("keyshim: fifo open failed\n");
        return NULL;
    }
    klog_str("keyshim: fifo open ok\n");

    pfd.fd = fd;
    pfd.events = POLLIN;

    /* The FIFO record is 12 bytes, but a writer may deliver it in several
     * write()s (this shim's own /tmp injectors do).  Accumulate across reads
     * instead of discarding partial data. */
    unsigned char rec[sizeof(struct key_ev)];
    size_t rec_len = 0;

    time_t last_mix = time(0);
    for (;;) {
        int pr = poll(&pfd, 1, 500);
        clear_browse_caution();   /* keep the USB-error popup dismissed */
        /* The mixer objects are created well after startup, so the first send
         * is often lost; keep re-applying them (idempotent, nothing physical
         * can change them on the phone). */
        time_t now = time(0);
        if (!ctrl_surface_active && now - last_mix >= 10) {
            mixer_defaults();
            last_mix = now;
        }
        if (pr <= 0)
            continue;
        ssize_t n = real_read(fd, rec + rec_len, sizeof(rec) - rec_len);
        if (n > 0) {
            rec_len += (size_t)n;
            if (rec_len == sizeof(rec)) {
                struct key_ev ev;
                memcpy(&ev, rec, sizeof(ev));
                rec_len = 0;

                if (ev.key == 0x7f01) {   /* host trigger: re-send mixer defaults */
                    mixer_defaults();
                    continue;
                }
                if (ev.key == 0x7f02) {   /* host trigger: rotate browse knob */
                    send_rx_key_fl(0x420c, OP_ROTATE, 1, ev.down, 0.0f, 0);
                    continue;
                }
                ui_pump_ensure();   /* restart pump if it ever exits */
                klog_dec("keyshim: got key ch=", ev.ch);
                klog_dec("         down=", ev.down);
                send_rx_key(ev.key, ev.down ? OP_PRESS : OP_RELEASE, ev.ch);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

/* ---- controller FIFO (/tmp/rb-ctrl.fifo) ----
 * 24-byte records, forwarded straight to sendKey(key, op, ch, param, f, l).
 * This is what the DDJ-400 MIDI bridge writes, so a real controller can drive
 * the engine's faders, EQ, jog wheels and relative knobs. */
static void *ctrl_thread(void *arg)
{
    struct pollfd pfd;
    int fd = -1;
    (void)arg;

    klog_str("keyshim: ctrl thread started\n");

    for (int i = 0; i < 300; i++) {
        if (get_key_manager())
            break;
        usleep(100000);
    }
    if (!get_key_manager()) {
        klog_str("keyshim: ctrl thread: key manager never ready\n");
        return NULL;
    }

    for (int i = 0; i < 150 && !ui_pump_running(); i++)
        ui_pump_ensure();

    for (int i = 0; i < 100; i++) {
        fd = real_open(CTRL_FIFO, O_RDWR | O_NONBLOCK);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0) {
        klog_str("keyshim: ctrl fifo open failed\n");
        return NULL;
    }
    klog_str("keyshim: ctrl fifo open ok\n");

    pfd.fd = fd;
    pfd.events = POLLIN;

    unsigned char rec[sizeof(struct ctrl_ev)];
    size_t rec_len = 0;

    for (;;) {
        int pr = poll(&pfd, 1, 1000);
        clear_browse_caution();
        if (pr <= 0)
            continue;
        ssize_t n = real_read(fd, rec + rec_len, sizeof(rec) - rec_len);
        if (n > 0) {
            rec_len += (size_t)n;
            if (rec_len == sizeof(rec)) {
                struct ctrl_ev ev;
                memcpy(&ev, rec, sizeof(ev));
                rec_len = 0;
                ctrl_surface_active = 1;
                ui_pump_ensure();
                klog_hex("keyshim: ctrl key=", (unsigned)ev.key);
                klog_dec("         op=", ev.op);
                klog_dec("         ch=", ev.ch);
                klog_dec("         param=", ev.param);
                if (rx3_handle_tagged_pad(&ev))
                    continue;
                if (rx3_handle_color_fx_type(&ev))
                    continue;

                send_rx_key_fl(ev.key, ev.op,
                               ev.key == 0x6017 ? 0 : ev.ch,
                               ev.param, ev.f, ev.l);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    return NULL;
}


/* ---- RX3 native player-state telemetry -------------------------------
 *
 * Published inside the RBP process so the host DDJ-400 bridge can mirror
 * state changed by track loading, touchscreen or any other native input.
 */
#define RX3_LED_MAGIC   0x524c4544UL
#define RX3_LED_VERSION 1

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

#define RX3_LED_CUE          0x01
#define RX3_LED_LOOP         0x02
#define RX3_LED_RELOOP       0x04
#define RX3_LED_SYNC_MASTER  0x08
#define RX3_LED_SYNC_SLAVE   0x10
#define RX3_LED_MASTER_TEMPO 0x20

static int rx3_native_call1(unsigned long address, int deck)
{
    return ((int (*)(int))address)(deck);
}

static int rx3_hotcue_bitmap(int deck)
{
    volatile unsigned char *root;
    volatile unsigned char *watcher;
    volatile unsigned char *snapshot;
    unsigned bitmap = 0;

    root = *(volatile unsigned char * volatile *)0x02685f2cUL;
    if (!root)
        return 0;

    watcher = *(volatile unsigned char * volatile *)(root + 0x38);
    if (!watcher)
        return 0;

    snapshot = *(volatile unsigned char * volatile *)
        (watcher + 0x0c + deck * 4);
    if (!snapshot)
        return 0;

    for (unsigned pad = 0; pad < 8; pad++) {
        uint32_t intime = *(volatile uint32_t *)
            (snapshot + 0x7c + pad * 0x34);

        if (intime != 0xffffffffUL)
            bitmap |= 1U << pad;
    }

    return (int)bitmap;
}

static void *rx3_led_state_thread(void *unused)
{
    struct rx3_led_state_packet packet;
    uint16_t sequence = 0;
    int fd = -1;

    (void)unused;

    klog_str("keyshim: native LED telemetry thread started\n");

    /* Aguarda o StatWatcher e os snapshots dos decks. */
    sleep(5);

    for (;;) {
        if (fd < 0) {
            fd = real_open("/tmp/rx3-led-state.bin",
                           O_RDWR | O_CREAT);
            if (fd < 0) {
                usleep(500000);
                continue;
            }

            /* real_open uses mode zero with O_CREAT. Fix access explicitly. */
            syscall(SYS_fchmod, fd, 0644);
        }

        memset(&packet, 0, sizeof(packet));

        packet.magic = RX3_LED_MAGIC;
        packet.version = RX3_LED_VERSION;
        packet.sequence = ++sequence;

        for (int deck = 0; deck < 2; deck++) {
            unsigned flags = 0;

            packet.hotcue[deck] =
                (uint8_t)rx3_hotcue_bitmap(deck);

            packet.play_mode[deck] =
                (uint8_t)rx3_native_call1(0x000fd960UL, deck);

            packet.pad_mode[deck] =
                (uint8_t)rx3_native_call1(0x000fd3ccUL, deck);

            if (rx3_native_call1(0x000fd53cUL, deck))
                flags |= RX3_LED_CUE;

            if (rx3_native_call1(0x000fd48cUL, deck))
                flags |= RX3_LED_LOOP;

            if (rx3_native_call1(0x000fdae8UL, deck))
                flags |= RX3_LED_RELOOP;

            if (rx3_native_call1(0x000fde30UL, deck))
                flags |= RX3_LED_SYNC_MASTER;

            if (rx3_native_call1(0x000fde60UL, deck))
                flags |= RX3_LED_SYNC_SLAVE;

            if (rx3_native_call1(0x000fd30cUL, deck))
                flags |= RX3_LED_MASTER_TEMPO;

            packet.flags[deck] = (uint8_t)flags;

            packet.play_time[deck] =
                (uint32_t)rx3_native_call1(0x000fcf7cUL, deck);

            packet.total_time[deck] =
                (uint32_t)rx3_native_call1(0x000fcfa4UL, deck);
        }

        if (lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, &packet, sizeof(packet)) !=
                (ssize_t)sizeof(packet)) {
            close(fd);
            fd = -1;
        }

        usleep(50000);
    }

    return NULL;
}

__attribute__((constructor))
static void keyshim_init(void)
{
    pthread_t tid;

    rx3_install_native_pad_touch();
    if (pthread_create(&tid, NULL, key_thread, NULL) == 0)
        pthread_detach(tid);
    if (pthread_create(&tid, NULL, ctrl_thread, NULL) == 0)
        pthread_detach(tid);
    if (pthread_create(&tid, NULL, rx3_led_state_thread, NULL) == 0)
        pthread_detach(tid);
}
