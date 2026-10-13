/*
 * point_xform.c — see point_xform.h for why this is a configurable transform
 * rather than a derived one.
 *
 * The two callers are pointsrc.c's absolute path (a touchscreen: the device
 * reports where you touched) and its relative path (a mouse: the device reports
 * how far you moved). They share the flag semantics deliberately — SWAP_XY and
 * INVERT_* describe how the *device's axes* are laid out, and each path applies
 * that description in the way its own kind of coordinate allows. For an absolute
 * device that means reflecting or transposing the position; for a relative one
 * it means reflecting or transposing the motion, because reflecting a cursor's
 * position would teleport the pointer to the far edge.
 */
#include "point_xform.h"
#include "envutil.h"

#include <string.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/syscall.h>
#include <unistd.h>

#define POINT_SPEED_DEFAULT 1.0

static int clamp_int(long v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return (int)v;
}

void point_xform_init(struct point_xform *x)
{
    memset(x, 0, sizeof(*x));

    x->raw_min_x = POINT_RAW_MIN_DEFAULT;
    x->raw_max_x = POINT_RAW_MAX_DEFAULT;
    x->raw_min_y = POINT_RAW_MIN_DEFAULT;
    x->raw_max_y = POINT_RAW_MAX_DEFAULT;

    /* Defaults are "the device is not rotated, not mirrored" — the SC Live 4
     * needed swap=1 and got it from rb.conf, so this file holds no
     * target-specific knowledge. */
    x->swap_xy  = env_flag("POINT_SWAP_XY", 0);
    x->invert_x = env_flag("POINT_INVERT_X", 0);
    x->invert_y = env_flag("POINT_INVERT_Y", 0);

    /* The real fb geometry, by raw syscall: ioctl() in this process is
     * fb_shim.c's, which reports the logical 1280x800 on purpose. */
    if (env_flag("POINT_ABS_FIT", 1)) {
        int fd = (int)syscall(SYS_openat, AT_FDCWD, env_str("FB_DEV", "/dev/fb0"),
                              O_RDONLY | O_CLOEXEC);
        struct fb_var_screeninfo var;

        if (fd >= 0) {
            memset(&var, 0, sizeof(var));
            if (syscall(SYS_ioctl, fd, FBIOGET_VSCREENINFO, &var) == 0 &&
                var.xres > 0 && var.yres > 0 &&
                ((int)var.xres != POINT_LOGICAL_W || (int)var.yres != POINT_LOGICAL_H)) {
                int stretch = strcmp(env_str("DFB_PRESENT_FIT", "fit"), "stretch") == 0;

                x->fit_fb_w = (int)var.xres;
                x->fit_fb_h = (int)var.yres;
                point_fit(POINT_LOGICAL_W, POINT_LOGICAL_H, x->fit_fb_w, x->fit_fb_h,
                          stretch, &x->fit_dw, &x->fit_dh, &x->fit_bx, &x->fit_by);
                if (x->fit_dw < 1 || x->fit_dh < 1)
                    x->fit_fb_w = 0;
            }
            syscall(SYS_close, fd);
        }
    }
}

void point_xform_abs(const struct point_xform *x, int raw_x, int raw_y,
                     int *logical_x, int *logical_y)
{
    /* Normalize to a zero base first, so a device whose range does not start at
     * zero (some digitizers report min=-100 or a signed range) still scales
     * across the full logical dimension instead of losing the negative half. */
    long rx = (long)raw_x - x->raw_min_x;
    long ry = (long)raw_y - x->raw_min_y;
    long range_x = (long)x->raw_max_x - x->raw_min_x + 1;
    long range_y = (long)x->raw_max_y - x->raw_min_y + 1;
    long lx, ly;

    /* A degenerate range cannot happen from EVIOCGABS (pointsrc refuses those
     * devices) but can from a hand-typed POINT_* override, and dividing by zero
     * inside rbp's input path is not a failure mode worth leaving available. */
    if (range_x < 1)
        range_x = 1;
    if (range_y < 1)
        range_y = 1;

    /* Long arithmetic, not int: a 16-bit device (max 32767) times 1280 is 42 M,
     * which fits in an int on paper and is exactly the kind of thing that stops
     * fitting when someone adds a 24-bit digitizer. The multiply is done before
     * the divide in both branches because lx/ly are only 1280/800 wide. */
    if (x->fit_fb_w > 0) {
        /* Panel position in fb pixels first, then into the picture rectangle:
         * a touch on a bar clamps to the nearest UI edge. */
        long px = (x->swap_xy ? ry * x->fit_fb_w / range_y : rx * x->fit_fb_w / range_x);
        long py = (x->swap_xy ? rx * x->fit_fb_h / range_x : ry * x->fit_fb_h / range_y);

        lx = (px - x->fit_bx) * POINT_LOGICAL_W / x->fit_dw;
        ly = (py - x->fit_by) * POINT_LOGICAL_H / x->fit_dh;
    } else if (x->swap_xy) {
        lx = ry * POINT_LOGICAL_W / range_y;
        ly = rx * POINT_LOGICAL_H / range_x;
    } else {
        lx = rx * POINT_LOGICAL_W / range_x;
        ly = ry * POINT_LOGICAL_H / range_y;
    }

    /* Reflecting the scaled position, not the raw one: `LOGICAL_W - 1 - lx` is
     * the same operation as `(range - 1 - rx) * W / range` only when the range
     * divides the dimension, and this form is exact for every range. */
    if (x->invert_x)
        lx = (POINT_LOGICAL_W - 1) - lx;
    if (x->invert_y)
        ly = (POINT_LOGICAL_H - 1) - ly;

    *logical_x = clamp_int(lx, 0, POINT_LOGICAL_W - 1);
    *logical_y = clamp_int(ly, 0, POINT_LOGICAL_H - 1);
}

void point_xform_rel(const struct point_xform *x, int dx, int dy,
                     int *cursor_x, int *cursor_y)
{
    double speed = env_double("POINT_MOUSE_SPEED", POINT_SPEED_DEFAULT);
    long mx, my;

    if (x->swap_xy) {
        long t = dx;
        dx = dy;
        dy = (int)t;
    }
    if (x->invert_x)
        dx = -dx;
    if (x->invert_y)
        dy = -dy;

    mx = (long)(dx * speed);
    my = (long)(dy * speed);

    *cursor_x = clamp_int((long)*cursor_x + mx, 0, POINT_LOGICAL_W - 1);
    *cursor_y = clamp_int((long)*cursor_y + my, 0, POINT_LOGICAL_H - 1);
}

void point_fit(int logical_w, int logical_h, int fb_w, int fb_h, int stretch,
               int *dw, int *dh, int *bx, int *by)
{
    /* The header says why this duplicates fbdev_present_fit(); this is a
     * deliberate transcription of it, down to the order of the operations, so
     * that a reader diffing the two sees structure rather than arithmetic. */
    if (logical_w <= 0 || logical_h <= 0 || fb_w <= 0 || fb_h <= 0) {
        *dw = *dh = *bx = *by = 0;
        return;
    }

    if (stretch) {
        *dw = fb_w;
        *dh = fb_h;
    } else {
        unsigned long long s = ((unsigned long long)fb_w << 16) / (unsigned)logical_w;
        unsigned long long t = ((unsigned long long)fb_h << 16) / (unsigned)logical_h;

        if (t < s)
            s = t;                                 /* whichever axis binds */

        *dw = (int)(((unsigned long long)logical_w * s + 32768) >> 16);
        *dh = (int)(((unsigned long long)logical_h * s + 32768) >> 16);
        if (*dw < 1)
            *dw = 1;
        if (*dh < 1)
            *dh = 1;
    }

    *bx = (fb_w - *dw) / 2;
    *by = (fb_h - *dh) / 2;
}
