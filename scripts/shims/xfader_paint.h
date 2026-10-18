/*
 * xfader_paint.h -- the CROSSFADER CURVE row's pixels: three curve icons, one of
 * them highlighted. Pure painter, same family as fxpad_paint.c and side_paint.c --
 * a function of its arguments, every byte of `dst` written on every call (no
 * save-under, same reasoning menu_paint.h gives: this row is blank glass rbp never
 * draws into, so there is no "under" to preserve and nothing to copy first, unlike
 * fxpad_paint.c's HUD-over-rbp's-own-readout case).
 *
 * NO LABEL IS PAINTED HERE any more (removed 2026-10-17, 2nd pass, on request --
 * the icons read clearly enough aligned under the real ones). The combined
 * "CHANNEL/CROSS FADER CURVE" heading instead patches rbp's own native label in
 * place; see xfader_zone.h's "NATIVE LABEL PATCH" section and
 * xfader_label_paint() below.
 *
 * THE ICONS ARE STYLISED FILLED-CURVE ART, NOT EXTRACTED PIXELS -- but filled, not
 * line art, as of 2026-10-17 2nd pass: the operator reported the first (thin-line)
 * version reading as low-resolution and low-contrast next to the native block's
 * own solid-white curve silhouettes. xf_icon_cov() now fills the area UNDER each
 * curve, antialiased at the boundary, the same shape the real icons use (confirmed
 * against a screenshot: solid white fill on the button's own face colour). rbp's
 * real CHANNEL FADER CURVE icons live in an external, numerically-indexed archive
 * (root/gui/pset/imagedata/imagedata.dat) with no name-to-offset table -- decoding
 * it well enough to pull three specific icons out was judged its own open-ended
 * reverse-engineering task, separate from and riskier than filling three small,
 * clearly-distinct curve shapes by hand. The three button SLOTS, their selection
 * highlight and their palette all match the native block's own language
 * (menu_paint.h's MENU_FX_SEL is literally "the colour rbp fills a SELECTED box
 * with", confirmed pixel-for-pixel against the screenshot: (0,125,231) both
 * places); only the curve shapes themselves are this module's own.
 */
#ifndef RBPI4B_XFADER_PAINT_H
#define RBPI4B_XFADER_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "xfader_zone.h"     /* XFADER_* geometry */

/* The icon row's size in framebuffer pixels for a page of dw x dh -- what the
 * caller's plane has to be set up at (menu_draw.c's drm_band_setup()), the same
 * reason fxpad_paint_w/h exist: so the plane and the view drawn into it cannot
 * disagree about the row's size. Scaled off XFADER_PAINT_X0/X1 (the icon lane),
 * not the wider hit-test span. */
int xfader_paint_w(int page_dw);
int xfader_paint_h(int page_dh);

/* Can this view be drawn into? Needs room for three icons with a visible gap --
 * the same kind of guard every sibling painter has (fxpad_paint_ok(), menu_view_ok()). */
int xfader_paint_ok(const struct menu_view *v);

/* Draw the icon row into `dst`: three icons, icon `selected` (0..XFADER_ICONS-1)
 * drawn in MENU_FX_SEL and the other two in MENU_BTN. `selected` of -1 draws all
 * three unselected -- rbp's current curve value did not map to one of the three
 * params this picker offers (e.g. 0, THRU, which this picker does not expose --
 * see xfader_zone.h).
 *
 * `dst`'s picture rect IS the row already scaled and positioned by the caller
 * (menu_draw.c), the same contract every painter in this family takes. Does
 * nothing when xfader_paint_ok(dst) is 0. */
void xfader_paint(const struct menu_view *dst, int selected);

/* --- the native label patch (xfader_zone.h's "NATIVE LABEL PATCH" section) --- */

/* The patch rect's size in framebuffer pixels for a page of dw x dh, scaled off
 * XFADER_LABEL_X0/X1/Y0/Y1 -- the same w/h split every painter in this family uses. */
int xfader_label_paint_w(int page_dw);
int xfader_label_paint_h(int page_dh);

/* Can this view be drawn into? Needs room for one line of this shim's own font. */
int xfader_label_paint_ok(const struct menu_view *v);

/* Erase `dst` to the black bed (MENU_FILL, matching the screenshot-measured
 * background behind rbp's own label) and draw "CHANNEL/CROSS FADER CURVE" in a
 * muted grey matching the native label's own measured ink ((148,150,148), not
 * MENU_LABEL's brighter near-white -- the native heading is deliberately dimmer
 * than the icons below it). `dst` is rbp's own live page, scaled and positioned
 * the way menu_fxp_page_view() positions the pad's page-route fallback -- NOT a
 * plane of our own, so every call overwrites whatever rbp most recently drew
 * there, including its own original text. Does nothing when
 * xfader_label_paint_ok(dst) is 0. */
void xfader_label_paint(const struct menu_view *dst);

#endif /* RBPI4B_XFADER_PAINT_H */
