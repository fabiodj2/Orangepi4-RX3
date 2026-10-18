/*
 * test_xfader.c -- the CROSSFADER CURVE row and its native label patch, with no
 * Pi, no panel and no rbp anywhere in this file. test_fxpad.c's own reasoning: it
 * links the PRODUCTION xfader_zone.c and xfader_paint.c, because a rectangle
 * boundary and "which pixel is ink" are exactly the kind of thing a copy would
 * quietly get wrong.
 *
 * xfader_zone.c is pure in the stricter sense fxpad_zone.c is not: it never reads
 * rbp at all, not even through a caller-filled struct -- rbp's curve value and
 * whether SHORTCUT is on screen arrive as two small pushed-in words
 * (xfader_set_selected()/xfader_set_showing()), the same split menu_zone.h's
 * menu_set_blink() uses. So there is no simulated rbp here, only the geometry, the
 * hit test, the push/read round trip and the two pictures (the icon row and the
 * native label patch).
 */

#include "xfader_zone.h"
#include "xfader_paint.h"
#include "menu_zone.h"      /* MZ_LOGICAL_W/H: the space the row must fit in */
#include "rbp_abi.h"        /* BROWSE_MODE_SHORTCUT: pinned equal to the module's
                             * own copy, test_util.c's own convention for
                             * BROWSE_MODE_UTILITY/UTIL_MODE_UTILITY */

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                       \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

/* ------------------------------------------------------------- the geometry */

static void test_mode_pin(void)
{
    CHECK(XFADER_MODE_SHORTCUT == BROWSE_MODE_SHORTCUT,
          "xfader_zone.h's XFADER_MODE_SHORTCUT is %d but rbp_abi.h's is %d",
          XFADER_MODE_SHORTCUT, BROWSE_MODE_SHORTCUT);
}

static void test_param_mapping(void)
{
    CHECK(XFADER_PARAM(0) == 1, "icon 0 -> K_FADERCURVE param 1 (normal)");
    CHECK(XFADER_PARAM(1) == 2, "icon 1 -> K_FADERCURVE param 2 (sharp/cut)");
    CHECK(XFADER_PARAM(2) == 3, "icon 2 -> K_FADERCURVE param 3 (wide open)");
}

static void test_hit_span_fits_logical_space(void)
{
    CHECK(XFADER_HIT_X0 >= 0 && XFADER_HIT_X1 < MZ_LOGICAL_W,
          "the hit-test span is inside the panel");
    CHECK(XFADER_ROW_Y0 >= 0 && XFADER_ROW_Y1 < MZ_LOGICAL_H,
          "the row's y span is inside the panel");
    CHECK(XFADER_ROW_Y0 < XFADER_ROW_Y1, "the row has real height");
}

static void test_icons_fit_and_dont_overlap(void)
{
    int i;
    int last_x1 = -1;

    for (i = 0; i < XFADER_ICONS; i++) {
        int x0 = xfader_icon_x0(i);
        int x1 = x0 + XFADER_ICON_W - 1;

        CHECK(x0 > last_x1, "icon %d starts (%d) after the previous icon ended (%d)",
              i, x0, last_x1);
        CHECK(x1 <= XFADER_PAINT_X1, "icon %d (right edge %d) fits inside the paint"
              " lane (x1 %d)", i, x1, XFADER_PAINT_X1);
        last_x1 = x1;
    }
    CHECK(xfader_icon_x0(0) == XFADER_PAINT_X0,
          "the first icon starts exactly at the paint lane's own left edge");
    CHECK(last_x1 == XFADER_PAINT_X1,
          "the last icon ends exactly at the paint lane's own right edge");
}

/* ------------------------------------------------------------- the hit test */

static void test_hit_outside_row(void)
{
    CHECK(xfader_hit(XFADER_HIT_X0, XFADER_ROW_Y0 - 1) == -1,
          "one row above the strip is not the strip");
    CHECK(xfader_hit(XFADER_HIT_X0, XFADER_ROW_Y1 + 1) == -1,
          "one row below the strip is not the strip");
    CHECK(xfader_hit(XFADER_HIT_X0 - 1, (XFADER_ROW_Y0 + XFADER_ROW_Y1) / 2) == -1,
          "one column left of the hit span is not the strip");
    CHECK(xfader_hit(XFADER_HIT_X1 + 1, (XFADER_ROW_Y0 + XFADER_ROW_Y1) / 2) == -1,
          "one column right of the hit span is not the strip");
    CHECK(!xfader_in_band(XFADER_HIT_X0, XFADER_ROW_Y0 - 1),
          "xfader_in_band agrees: one row above is out");
}

static void test_hit_each_icon(void)
{
    int i;

    for (i = 0; i < XFADER_ICONS; i++) {
        int x0 = xfader_icon_x0(i);
        int x1 = x0 + XFADER_ICON_W - 1;
        int y = (XFADER_ROW_Y0 + XFADER_ROW_Y1) / 2;

        CHECK(xfader_hit(x0, y) == i, "icon %d's own left edge hits icon %d", i, i);
        CHECK(xfader_hit(x1, y) == i, "icon %d's own right edge hits icon %d", i, i);
        CHECK(xfader_hit((x0 + x1) / 2, y) == i, "icon %d's own middle hits icon %d",
              i, i);
    }
}

/* Added 2026-10-17, 2nd pass: the row used to have a dead gap between icons
 * (and a label cell to their left) that a tap could land in and hit nothing.
 * The operator reported real taps missing on the glass, and the fix was to stop
 * leaving any such gap at all (xfader_zone.h's "WHERE IT ANSWERS TOUCH") --
 * these checks are the ones that would have caught the old behaviour. */
static void test_no_dead_gap_anywhere_in_the_strip(void)
{
    int x, y = (XFADER_ROW_Y0 + XFADER_ROW_Y1) / 2;

    for (x = XFADER_HIT_X0; x <= XFADER_HIT_X1; x += 7) {
        int hit = xfader_hit(x, y);

        CHECK(hit >= 0 && hit < XFADER_ICONS,
              "every x in the hit span lands on some icon (x=%d got %d)", x, hit);
    }
    /* The far ends of the hit span -- well outside the painted icons -- still
     * resolve to the nearest one rather than nothing. */
    CHECK(xfader_hit(XFADER_HIT_X0, y) == 0,
          "the hit span's own far-left edge still claims icon 0");
    CHECK(xfader_hit(XFADER_HIT_X1, y) == XFADER_ICONS - 1,
          "the hit span's own far-right edge still claims the last icon");
}

/* -------------------------------------------------------- the pushed state */

static void test_selected_round_trip(void)
{
    xfader_set_selected(-1);
    CHECK(xfader_selected() == -1, "default/explicit -1 reads back as -1");
    xfader_set_selected(1);
    CHECK(xfader_selected() == 1, "a pushed value reads back unchanged");
    xfader_set_selected(2);
    CHECK(xfader_selected() == 2, "...and again, independently");
    xfader_set_selected(-1);
}

static void test_showing_round_trip(void)
{
    xfader_set_showing(0);
    CHECK(xfader_showing() == 0, "pushed 0 reads back as 0");
    xfader_set_showing(1);
    CHECK(xfader_showing() == 1, "pushed 1 reads back as 1");
    xfader_set_showing(0);
}

/* ------------------------------------------------------- the icon row's picture */

#define XFW (XFADER_PAINT_X1 - XFADER_PAINT_X0 + 1)   /* 301: no scaling, 1:1 */
#define XFH (XFADER_ROW_H)                              /* 39 */

static unsigned short xf_dst[XFW * XFH];

static void xf_view(struct menu_view *v, int w, int h)
{
    memset(v, 0, sizeof *v);
    memset(xf_dst, 0, sizeof xf_dst);
    v->pix   = xf_dst;
    v->pitch = w;
    v->fb_w  = w;
    v->fb_h  = h;
    v->dw    = w;
    v->dh    = h;
    v->bpp   = 16;
}

/* `x`/`y` in LOGICAL (1280x800) coordinates, same as every other function in this
 * file -- xf_view()'s picture IS the icon row (identity-scaled: XFW/XFH equal the
 * row's own logical span), so only the row's own origin needs subtracting. */
static unsigned short xf_px(int w, int x, int y)
{
    int lx = x - XFADER_PAINT_X0;
    int ly = y - XFADER_ROW_Y0;

    return xf_dst[ly * w + lx];
}

static void test_paint_refusals(void)
{
    struct menu_view v;

    xf_view(&v, XFW, XFH);
    CHECK(xfader_paint_ok(&v) == 1, "the real row is a view the picker can draw into");

    CHECK(xfader_paint_ok(NULL) == 0, "no view, no paint");

    xf_view(&v, XFW, XFH);
    v.pix = NULL;
    CHECK(xfader_paint_ok(&v) == 0, "no pixels, no paint");

    xf_view(&v, XFW, XFH);
    v.bpp = 8;
    CHECK(xfader_paint_ok(&v) == 0, "an unknown depth is refused");

    xf_view(&v, XFW, XFH);
    v.pitch = XFW - 1;
    CHECK(xfader_paint_ok(&v) == 0, "a stride narrower than the row is refused");

    xf_view(&v, XFW, 1);
    CHECK(xfader_paint_ok(&v) == 0, "a picture one pixel tall is refused");
}

static void test_paint_w_h_never_zero(void)
{
    CHECK(xfader_paint_w(1) >= 1 && xfader_paint_h(1) >= 1,
          "a tiny page never yields a 0-sized plane");
    CHECK(xfader_paint_w(1280) == XFW, "a full-size page scales to the row's own width");
}

/* The segmented-control palette (xfader_paint.c's "rounded rects" section),
 * duplicated here the same way the module's own blend math is -- a second,
 * independent computation of what the production code SHOULD draw, not a
 * call into it. RGB565, matching xf_view()'s bpp 16. */
static unsigned short xf_rgb565(int r, int g, int b)
{
    return (unsigned short)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3));
}

#define XF_BTN_R 37
#define XF_BTN_G 43
#define XF_BTN_B 53
#define XF_ACCENT_R 31
#define XF_ACCENT_G 111
#define XF_ACCENT_B 214

/* Every icon's own fill colour matches the segmented control's --btn
 * (unselected) or --accent (selected), sampled at a corner each icon's own
 * curve fill clears -- every curve shape (xfader_paint.c's xf_curve_h()) is 0
 * at t=0, so the top-left corner, two pixels in from the edge (clear of both
 * the border and the rounded-corner cutout at this icon size), is never
 * inside the fill. */
static void test_paint_selection_highlight(void)
{
    struct menu_view v;
    unsigned int btn = xf_rgb565(XF_BTN_R, XF_BTN_G, XF_BTN_B);
    unsigned int sel = xf_rgb565(XF_ACCENT_R, XF_ACCENT_G, XF_ACCENT_B);
    int i;

    xfader_set_selected(1);
    xf_view(&v, XFW, XFH);
    xfader_paint(&v, xfader_selected());

    for (i = 0; i < XFADER_ICONS; i++) {
        int fx = xfader_icon_x0(i) + 2;
        int fy = XFADER_ROW_Y0 + XFADER_ICON_MARGIN_Y + 2;
        unsigned short got = xf_px(XFW, fx, fy);
        unsigned int want = (i == 1) ? sel : btn;

        CHECK(got == (unsigned short)want, "icon %d's own fill near its corner is"
              " %s (got %#x, want %#x)", i, (i == 1) ? "MENU_FX_SEL" : "MENU_BTN",
              got, want);
    }
    xfader_set_selected(-1);
}

static void test_paint_no_selection_is_all_unselected(void)
{
    struct menu_view v;
    unsigned int btn = xf_rgb565(XF_BTN_R, XF_BTN_G, XF_BTN_B);
    unsigned int sel = xf_rgb565(XF_ACCENT_R, XF_ACCENT_G, XF_ACCENT_B);
    int i, any_sel = 0;

    xfader_set_selected(-1);
    xf_view(&v, XFW, XFH);
    xfader_paint(&v, xfader_selected());

    for (i = 0; i < XFADER_ICONS; i++) {
        int fx = xfader_icon_x0(i) + 2;
        int fy = XFADER_ROW_Y0 + XFADER_ICON_MARGIN_Y + 2;

        if (xf_px(XFW, fx, fy) == (unsigned short)sel)
            any_sel = 1;
        CHECK(xf_px(XFW, fx, fy) == (unsigned short)btn,
              "icon %d is MENU_BTN when nothing is selected", i);
    }
    CHECK(!any_sel, "no icon reads MENU_FX_SEL with selected == -1");
}

/* Each curve now fills an AREA (xfader_paint.h's "filled-curve art"), so a
 * near-white pixel should appear plentifully near the BOTTOM of every icon
 * (every curve is high there) and only icon 2's (the fast-early-rise one)
 * should be substantially filled near the TOP-middle, where icon 0's (the
 * slow-then-steep one) is still mostly background. This is a coarse shape
 * check, not a claim about exact curve math -- xfader_paint.h says these are
 * stylised, not extracted. */
static void test_paint_icons_look_different(void)
{
    struct menu_view v;
    unsigned int ink = menu_pixel(16, MENU_LABEL);
    int i, x, y;
    int ink_count[XFADER_ICONS] = { 0 };

    xfader_set_selected(-1);
    xf_view(&v, XFW, XFH);
    xfader_paint(&v, xfader_selected());

    for (i = 0; i < XFADER_ICONS; i++) {
        int x0 = xfader_icon_x0(i), x1 = x0 + XFADER_ICON_W - 1;

        for (y = XFADER_ROW_Y0 + XFADER_ICON_MARGIN_Y;
             y <= XFADER_ROW_Y1 - XFADER_ICON_MARGIN_Y; y++)
            for (x = x0; x <= x1; x++)
                if (xf_px(XFW, x, y) == (unsigned short)ink)
                    ink_count[i]++;
        CHECK(ink_count[i] > 0, "icon %d has some fill drawn", i);
    }
    CHECK(ink_count[0] != ink_count[1] || ink_count[1] != ink_count[2],
          "the three icons are not pixel-identical to each other");
    /* A filled curve covers a real fraction of its own icon -- a thin line
     * would not. Guards against a regression back to line art without pinning
     * the exact shapes. */
    for (i = 0; i < XFADER_ICONS; i++) {
        int area = XFADER_ICON_W * (XFADER_ROW_Y1 - XFADER_ROW_Y0 + 1 - 2 * XFADER_ICON_MARGIN_Y);

        CHECK(ink_count[i] > area / 10, "icon %d's fill covers a real fraction of"
              " its own icon (got %d of %d px)", i, ink_count[i], area);
    }
}

/* ------------------------------------------------------- the native label patch */

#define XLW (XFADER_LABEL_X1 - XFADER_LABEL_X0 + 1)
#define XLH (XFADER_LABEL_Y1 - XFADER_LABEL_Y0 + 1)

static unsigned short xfl_dst[XLW * XLH];

static void xfl_view(struct menu_view *v, int w, int h)
{
    memset(v, 0, sizeof *v);
    memset(xfl_dst, 0, sizeof xfl_dst);
    v->pix   = xfl_dst;
    v->pitch = w;
    v->fb_w  = w;
    v->fb_h  = h;
    v->dw    = w;
    v->dh    = h;
    v->bpp   = 16;
}

static void test_label_patch_geometry(void)
{
    CHECK(XFADER_LABEL_X0 >= 0 && XFADER_LABEL_X1 < MZ_LOGICAL_W,
          "the label patch's x span is inside the panel");
    CHECK(XFADER_LABEL_Y0 >= 0 && XFADER_LABEL_Y1 < MZ_LOGICAL_H,
          "the label patch's y span is inside the panel");
    CHECK(XFADER_LABEL_Y1 < XFADER_ROW_Y0,
          "the label patch sits clear above the icon row -- it must never"
          " overlap the icons it is patched over");
}

static void test_label_paint_refusals(void)
{
    struct menu_view v;

    xfl_view(&v, XLW, XLH);
    CHECK(xfader_label_paint_ok(&v) == 1, "the real patch rect can be drawn into");

    CHECK(xfader_label_paint_ok(NULL) == 0, "no view, no paint");

    xfl_view(&v, XLW, XLH);
    v.pix = NULL;
    CHECK(xfader_label_paint_ok(&v) == 0, "no pixels, no paint");

    xfl_view(&v, XLW, 2);
    CHECK(xfader_label_paint_ok(&v) == 0, "a picture too short for one line of"
          " text is refused");
}

static void test_label_paint_w_h_never_zero(void)
{
    CHECK(xfader_label_paint_w(1) >= 1 && xfader_label_paint_h(1) >= 1,
          "a tiny page never yields a 0-sized patch");
    CHECK(xfader_label_paint_w(1280) == XLW,
          "a full-size page scales to the patch's own width");
    CHECK(xfader_label_paint_h(800) == XLH,
          "a full-size page scales to the patch's own height");
}

/* The patch draws SOME ink (the heading is not blank) and leaves most of the
 * rect at the background fill (text is sparse next to a whole rect). Not a
 * claim about the exact glyph shapes -- that is menu_font.h's own contract,
 * already covered by test_menu.c. */
static void test_label_paint_draws_text(void)
{
    struct menu_view v;
    unsigned int bed = menu_pixel(16, MENU_FILL);
    int x, y, ink_px = 0, bed_px = 0;

    xfl_view(&v, XLW, XLH);
    xfader_label_paint(&v);

    for (y = 0; y < XLH; y++) {
        for (x = 0; x < XLW; x++) {
            unsigned short got = xfl_dst[y * XLW + x];

            if (got == (unsigned short)bed)
                bed_px++;
            else
                ink_px++;
        }
    }
    CHECK(ink_px > 0, "the patch draws some ink (the heading is not blank)");
    CHECK(bed_px > ink_px, "most of the patch rect is still the background fill");
}

static void test_label_paint_ok_false_is_a_noop(void)
{
    struct menu_view v;
    unsigned short before;

    xfl_view(&v, XLW, XLH);
    xfl_dst[0] = 0x1234;
    before = xfl_dst[0];
    v.pix = NULL;
    xfader_label_paint(&v);
    CHECK(xfl_dst[0] == before, "a refused view is left untouched");
}

int main(void)
{
    test_mode_pin();
    test_param_mapping();
    test_hit_span_fits_logical_space();
    test_icons_fit_and_dont_overlap();

    test_hit_outside_row();
    test_hit_each_icon();
    test_no_dead_gap_anywhere_in_the_strip();

    test_selected_round_trip();
    test_showing_round_trip();

    test_paint_refusals();
    test_paint_w_h_never_zero();
    test_paint_selection_highlight();
    test_paint_no_selection_is_all_unselected();
    test_paint_icons_look_different();

    test_label_patch_geometry();
    test_label_paint_refusals();
    test_label_paint_w_h_never_zero();
    test_label_paint_draws_text();
    test_label_paint_ok_false_is_a_noop();

    printf("test_xfader: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
