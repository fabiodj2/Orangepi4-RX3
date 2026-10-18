/*
 * xfader_zone.c -- the CROSSFADER CURVE band's geometry and hit test. PURE: see
 * xfader_zone.h for what this is and why.
 */
#include "xfader_zone.h"

int xfader_icon_x0(int i)
{
    return XFADER_ICON_X0 + i * (XFADER_ICON_W + XFADER_ICON_GAP);
}

int xfader_in_band(int x, int y)
{
    return x >= XFADER_HIT_X0 && x <= XFADER_HIT_X1 &&
           y >= XFADER_ROW_Y0 && y <= XFADER_ROW_Y1;
}

/* NEAREST icon, not "inside this icon's own painted rect" -- see
 * XFADER_HIT_X0/X1's comment in xfader_zone.h for why: the row this picker
 * sits on is blank glass end to end, so there is no gap left to lose a tap
 * into. Boundaries are the midpoints between each pair of neighbouring icons'
 * own centres, which also correctly extends icon 0's and icon 2's own claim
 * out to XFADER_HIT_X0/X1. */
int xfader_hit(int x, int y)
{
    int i;

    if (!xfader_in_band(x, y))
        return -1;
    for (i = 0; i < XFADER_ICONS - 1; i++) {
        int c0 = xfader_icon_x0(i) + XFADER_ICON_W / 2;
        int c1 = xfader_icon_x0(i + 1) + XFADER_ICON_W / 2;
        int boundary = (c0 + c1) / 2;

        if (x <= boundary)
            return i;
    }
    return XFADER_ICONS - 1;
}

/* See xfader_zone.h: live engine state, pushed in by pointsrc.c's
 * xfader_curve_tick(), not gesture state -- no reset function in this file
 * touches it. */
static int selected_icon = -1;

void xfader_set_selected(int i)
{
    selected_icon = i;
}

int xfader_selected(void)
{
    return selected_icon;
}

static int showing;

void xfader_set_showing(int on)
{
    showing = on;
}

int xfader_showing(void)
{
    return showing;
}
