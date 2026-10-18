/*
 * xfader_paint.c -- the CROSSFADER CURVE icon row's pixels, and the native label
 * patch. See xfader_paint.h for what each half is and why.
 *
 * Pixel accessor and the font-coverage walk are duplicated from fxpad_paint.c and
 * menu_paint.c respectively, the same small-deliberate-duplication fxpad_paint.c's own
 * header comment explains: exporting either would mean an accessor that has to know
 * module-internal notions, to save under twenty lines.
 */
#include "xfader_paint.h"

#include "menu_font.h"
#include "menu_zone.h"       /* MZ_LOGICAL_W/H -- the space xfader_paint_w/h scale out of */
#include <math.h>            /* sqrt() -- the rounded-rect corner test */
#include <stddef.h>          /* size_t */

/* --- pixels, in the view's own (framebuffer-local) coordinates -------------- */

static void xf_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
{
    unsigned char *row;

    if (fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)fx * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)fx * 2) = (unsigned short)pv;
}

/* --- logical (1280x800) to this view's own pixels ---------------------------
 *
 * Same shape as fxpad_paint.c's fxp_fx/fxp_fy: the icon lane's own origin is
 * subtracted first, then scaled by the view's picture size over the lane's
 * logical size, then v->bx/by (0 for every caller this module has today) is
 * added back. Scaled off XFADER_PAINT_X0/X1 -- the icon lane, not the wider
 * hit-test span xfader_hit() answers on. */
static int xf_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - XFADER_PAINT_X0) * v->dw / (XFADER_PAINT_X1 - XFADER_PAINT_X0 + 1);
}

static int xf_fy(const struct menu_view *v, int ly)
{
    return v->by + (ly - XFADER_ROW_Y0) * v->dh / XFADER_ROW_H;
}

/* The same mapping for the label patch's own (separate) view and span. */
static int xfl_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - XFADER_LABEL_X0) * v->dw / (XFADER_LABEL_X1 - XFADER_LABEL_X0 + 1);
}

/* --- the blend, duplicated from menu_paint.c's menu_trip_of()/menu_blend() (both
 * file-local there) -- same formula, so an antialiased edge here looks exactly
 * like one on the row itself. ------------------------------------------------- */

static void xf_trip_of(int bpp, unsigned int v, int *r, int *g, int *b)
{
    if (bpp == 32) {
        *r = (int)((v >> 16) & 0xffu);
        *g = (int)((v >> 8) & 0xffu);
        *b = (int)(v & 0xffu);
    } else {
        *r = (int)((v >> 11) & 0x1fu) * 255 / 31;
        *g = (int)((v >> 5) & 0x3fu) * 255 / 63;
        *b = (int)(v & 0x1fu) * 255 / 31;
    }
}

/* menu_paint.c's MENU_TRIP is file-local; this is the same two-line expansion
 * (RGB565 for bpp 16, 0xRRGGBB for bpp 32), named distinctly so a reader never
 * mistakes the two for sharing storage. */
#define MENU_TRIP_COMPAT(bpp, r, g, b) \
    ((bpp) == 32 ? (unsigned int)(((r) << 16) | ((g) << 8) | (b)) \
                 : (unsigned int)(((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3))))

static unsigned int xf_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;

    if (cov <= 0)
        return base;
    if (cov >= 255)
        return ink;
    xf_trip_of(bpp, base, &br, &bg, &bb);
    xf_trip_of(bpp, ink, &ir, &ig, &ib);
    return MENU_TRIP_COMPAT(bpp,
                            (br * (255 - cov) + ir * cov + 127) / 255,
                            (bg * (255 - cov) + ig * cov + 127) / 255,
                            (bb * (255 - cov) + ib * cov + 127) / 255);
}

/* --- text: menu_paint.c's glyph walk (lines ~328-363), inlined for a single
 * fixed string instead of a column's label table. Used only by the label patch
 * now -- the icon row itself no longer draws any text (xfader_paint.h). ------- */

static int xf_text_cov(const char *s, int lx, int ly, int fx, int fy)
{
    int dx = fx - lx, dy = fy - ly, k;

    if (dx < 0 || dy < 0 || dy >= MENU_FONT_LINE)
        return 0;
    for (k = 0; s[k]; k++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)s[k]);
        int adv = menu_font_adv((unsigned char)s[k], (unsigned char)s[k + 1]);

        if (dx < adv)
            return menu_font_cov(g, dx - g->left, dy - g->top);
        dx -= adv;
    }
    return 0;
}

/* --- the icons: three filled curves -------------------------------------
 *
 * Each column fx has a curve height h(t) in [0,1] (t = how far across the icon's
 * own inset rect, 0 = left, 1 = right); the icon is filled from the row's own
 * bottom up to that height, the same solid-silhouette shape the screenshot shows
 * the native CHANNEL FADER CURVE icons using (not the thin diagonal LINE the
 * first version of this row drew -- replaced 2026-10-17 2nd pass after the
 * operator called it low-resolution and low-contrast next to the real ones).
 *
 *   0  slow-then-steep   h(t) = t^3           -- a late, sharp rise (a "cut")
 *   1  linear            h(t) = t             -- the straight diagonal
 *   2  steep-then-flat   h(t) = 1-(1-t)^3     -- an early rise that flattens out
 *
 * "top"/"bottom" are SCREEN sense: smaller y is higher on the glass.
 */
static double xf_curve_h(int i, double t)
{
    double u;

    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    switch (i) {
    case 0:
        return t * t * t;
    case 1:
        return t;
    default:
        u = 1.0 - t;
        return 1.0 - u * u * u;
    }
}

/* Coverage (0..255) of the fill at this view pixel: how far the pixel's own
 * centre (fy+0.5) sits past the curve's boundary row at this column, softened
 * over one pixel of edge so the boundary is antialiased rather than stair-stepped. */
static int xf_icon_cov(const struct menu_view *v, int i, int fx, int fy)
{
    int lx0 = xfader_icon_x0(i);
    int ly0 = XFADER_ROW_Y0 + XFADER_ICON_MARGIN_Y;
    int ly1 = XFADER_ROW_Y1 - XFADER_ICON_MARGIN_Y;
    int inset_x = XFADER_ICON_W / 6;
    int ix0 = xf_fx(v, lx0 + inset_x);
    int ix1 = xf_fx(v, lx0 + XFADER_ICON_W - inset_x);
    int iy0 = xf_fy(v, ly0);   /* top (small y) */
    int iy1 = xf_fy(v, ly1);   /* bottom (large y) */
    double t, ycurve, d;
    int cov;

    if (ix1 <= ix0 || iy1 <= iy0)
        return 0;
    t = (double)(fx - ix0) / (double)(ix1 - ix0);
    ycurve = (double)iy1 - xf_curve_h(i, t) * (double)(iy1 - iy0);
    d = ((double)fy + 0.5) - ycurve;
    if (d >= 0.5)
        return 255;
    if (d <= -0.5)
        return 0;
    cov = (int)((d + 0.5) * 255.0 + 0.5);
    if (cov < 0) cov = 0;
    if (cov > 255) cov = 255;
    return cov;
}

/* --- rounded rects: the "segmented control" look --------------------------
 *
 * Added 2026-10-17, 3rd pass: the operator shared a reference design (a web
 * mockup of a segmented fader-curve picker -- rounded buttons on a darker
 * "well", the selected one filled in an accent blue with a lighter inset
 * ring) and asked for that look, icons only (no per-button caption -- the row
 * is 39px tall end to end, nowhere near the ~50px a stacked icon+caption
 * needs, and that trade-off was put to the operator directly and this was
 * the choice). The palette below is lifted from that mockup's own CSS custom
 * properties (--well, --btn, --btn-border, --accent) so the row reads as the
 * same design, not an approximation of it; --accent-ring (an inset white ring
 * at 35% opacity over the mockup's accent fill) is pre-blended here since
 * this plane has no alpha channel to do it live.
 */

/* Coverage (0..255) of a rounded rect at this view pixel -- the same
 * boundary-softening shape xf_icon_cov() uses, just for a corner-radius test
 * instead of a curve. 255 well inside, 0 well outside, a soft ramp within
 * half a pixel of the true radius. r<=0 degrades to a plain (square) rect. */
static int xf_round_cov(int fx, int fy, int x0, int y0, int x1, int y1, int r)
{
    int cx, cy;
    double dx, dy, dist, d;

    if (fx < x0 || fx > x1 || fy < y0 || fy > y1)
        return 0;
    if (r <= 0)
        return 255;
    if (fx < x0 + r && fy < y0 + r)      { cx = x0 + r; cy = y0 + r; }
    else if (fx > x1 - r && fy < y0 + r) { cx = x1 - r; cy = y0 + r; }
    else if (fx < x0 + r && fy > y1 - r) { cx = x0 + r; cy = y1 - r; }
    else if (fx > x1 - r && fy > y1 - r) { cx = x1 - r; cy = y1 - r; }
    else
        return 255;
    dx = (double)(fx - cx);
    dy = (double)(fy - cy);
    dist = sqrt(dx * dx + dy * dy);
    d = (double)r - dist;
    if (d >= 0.5)
        return 255;
    if (d <= -0.5)
        return 0;
    return (int)((d + 0.5) * 255.0 + 0.5);
}

/* --- the public surface: the icon row ---------------------------------------- */

int xfader_paint_w(int page_dw)
{
    int w = (XFADER_PAINT_X1 - XFADER_PAINT_X0 + 1) * page_dw / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int xfader_paint_h(int page_dh)
{
    int h = XFADER_ROW_H * page_dh / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int xfader_paint_ok(const struct menu_view *v)
{
    int last_x1;

    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    if (v->dh < 6)
        return 0;
    /* The last icon's right edge, mapped, has to still land inside the view -- a
     * picture too narrow for that is one small enough the row is better left
     * blank than drawn with its third icon clipped off. */
    last_x1 = xf_fx(v, xfader_icon_x0(XFADER_ICONS - 1) + XFADER_ICON_W);
    if (last_x1 > v->dw)
        return 0;
    return 1;
}

void xfader_paint(const struct menu_view *dst, int selected)
{
    unsigned int page_bg, well, btn, btn_border, accent, accent_ring, ink;
    int fx, fy, i, well_r, btn_r;

    if (!xfader_paint_ok(dst))
        return;

    /* The mockup's own palette (--well/--btn/--btn-border/--accent), lifted
     * verbatim; accent_ring is --accent-ring (white at 35% opacity) pre-blended
     * onto --accent since this plane has no alpha of its own. page_bg is the
     * native screen's own black, which is what shows through outside the
     * well's rounded corners (xfader_paint.h: no "under" to preserve, but
     * there IS a fixed colour already there to match). */
    page_bg    = menu_pixel(dst->bpp, MENU_FILL);
    well       = MENU_TRIP_COMPAT(dst->bpp, 12, 15, 20);
    btn        = MENU_TRIP_COMPAT(dst->bpp, 37, 43, 53);
    btn_border = MENU_TRIP_COMPAT(dst->bpp, 52, 59, 71);
    accent      = MENU_TRIP_COMPAT(dst->bpp, 31, 111, 214);
    accent_ring = MENU_TRIP_COMPAT(dst->bpp, 109, 161, 228);
    ink         = menu_pixel(dst->bpp, MENU_LABEL);

    well_r = dst->dh / 6;
    if (well_r < 2) well_r = 2;
    btn_r = dst->dh / 7;
    if (btn_r < 2) btn_r = 2;

    /* Pass 1: the well. Outside its rounded corners the plane stays page_bg;
     * inside, well -- antialiased at the boundary by xf_round_cov(). */
    for (fy = 0; fy < dst->dh; fy++) {
        for (fx = 0; fx < dst->dw; fx++) {
            int cov = xf_round_cov(fx, fy, 0, 0, dst->dw - 1, dst->dh - 1, well_r);

            xf_set(dst, fx, fy, cov > 0 ? xf_blend(dst->bpp, page_bg, well, cov) : page_bg);
        }
    }

    /* Pass 2: the three segments, each its own rounded rect sitting on the
     * well -- XFADER_ICON_GAP already leaves the well showing between them,
     * the same role the mockup's own flex `gap` plays. A pixel outside a
     * button's own rounding is left exactly as pass 1 drew it (the well),
     * which is also why button rects must stay inside the well's bounds and
     * clear of ITS corner radius -- true here since XFADER_ICON_MARGIN_Y
     * already insets every button from the view's own top/bottom edge. */
    for (i = 0; i < XFADER_ICONS; i++) {
        int is_sel = (i == selected);
        unsigned int face = is_sel ? accent : btn;
        unsigned int border = is_sel ? accent_ring : btn_border;
        int border_w = is_sel ? 2 : 1;
        int bx0 = xf_fx(dst, xfader_icon_x0(i));
        int bx1 = xf_fx(dst, xfader_icon_x0(i) + XFADER_ICON_W) - 1;
        int by0 = xf_fy(dst, XFADER_ROW_Y0 + XFADER_ICON_MARGIN_Y);
        int by1 = xf_fy(dst, XFADER_ROW_Y1 - XFADER_ICON_MARGIN_Y);

        for (fy = by0; fy <= by1; fy++) {
            for (fx = bx0; fx <= bx1; fx++) {
                int cov = xf_round_cov(fx, fy, bx0, by0, bx1, by1, btn_r);
                unsigned int pv;

                if (cov <= 0)
                    continue;
                if (fx < bx0 + border_w || fx > bx1 - border_w ||
                    fy < by0 + border_w || fy > by1 - border_w) {
                    pv = border;
                } else {
                    int ink_cov = xf_icon_cov(dst, i, fx, fy);

                    pv = ink_cov > 0 ? xf_blend(dst->bpp, face, ink, ink_cov) : face;
                }
                xf_set(dst, fx, fy, cov >= 255 ? pv : xf_blend(dst->bpp, well, pv, cov));
            }
        }
    }
}

/* --- the public surface: the native label patch ------------------------------ */

int xfader_label_paint_w(int page_dw)
{
    int w = (XFADER_LABEL_X1 - XFADER_LABEL_X0 + 1) * page_dw / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int xfader_label_paint_h(int page_dh)
{
    int h = (XFADER_LABEL_Y1 - XFADER_LABEL_Y0 + 1) * page_dh / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int xfader_label_paint_ok(const struct menu_view *v)
{
    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    if (v->dh < MENU_FONT_LINE)
        return 0;
    return 1;
}

void xfader_label_paint(const struct menu_view *dst)
{
    /* Measured off the native label, 2026-10-17 (xfader_zone.h's "NATIVE LABEL
     * PATCH" section): peak ink (148,150,148), background pure black -- both
     * sampled from the same screenshot the rect itself was measured off. */
    unsigned int bed, ink;
    int fx, fy, label_ly;

    if (!xfader_label_paint_ok(dst))
        return;

    bed = menu_pixel(dst->bpp, MENU_FILL);
    ink = MENU_TRIP_COMPAT(dst->bpp, 148, 150, 148);
    for (fy = 0; fy < dst->dh; fy++)
        for (fx = 0; fx < dst->dw; fx++)
            xf_set(dst, fx, fy, bed);

    label_ly = dst->by + (dst->dh - MENU_FONT_LINE) / 2;
    if (label_ly < dst->by)
        label_ly = dst->by;
    for (fy = 0; fy < dst->dh; fy++) {
        for (fx = 0; fx < dst->dw; fx++) {
            int cov = xf_text_cov("CHANNEL/CROSS FADER CURVE",
                                   xfl_fx(dst, XFADER_LABEL_X0), label_ly, fx, fy);

            if (cov > 0)
                xf_set(dst, fx, fy, xf_blend(dst->bpp, bed, ink, cov));
        }
    }
}
