/*
 * xfader_zone.h -- a CROSSFADER CURVE picker drawn into the blank band of rbp's own
 * SHORTCUT screen. PURE: no rbp address, no env, no I/O, no clock, same discipline as
 * util_zone.h and fxpad_zone.h.
 *
 * WHY IT EXISTS. rbp's SHORTCUT screen (K_SHORTCUT 0x0210, browse mode 10, rbp_abi.h)
 * already carries a CHANNEL FADER CURVE picker, natively drawn and natively handled.
 * There is no crossfader-curve equivalent anywhere in the binary -- confirmed by
 * symbol search (`nm -C`): no `ui::touch_panel::Shortcut_CrossFaderCurve_*` class, no
 * `Obj_CTRL_SHORTCUT_GRP_CROSSFADERCURVE_*` resource, nothing. The XDJ-RX3 has no
 * physical crossfader, so Pioneer never built one. The engine-side support is real and
 * already live, though: `djengine::DjEngineIF::setCrossFaderCurve()` is reachable
 * today through the DDJ-400's SHIFT+FX CH1/CH2/MASTER binding
 * (RX3-RK3399-DDJ400/scripts/rb/device/ddj400-bridge.c, K_FADERCURVE 0x6018). This
 * module is the touchscreen half: a 3-icon picker drawn by the shim itself, in the one
 * band of blank glass this screen has, driving the SAME real engine call.
 *
 * WHY A SHIM OVERLAY AND NOT A PATCH TO rbp's OWN SCREEN. Investigated and rejected
 * (2026-10-17): rbp is a non-PIE EXEC with a ~90MB .bss; its process heap starts right
 * after the last mapped segment, which is exactly where a new PT_LOAD for injected
 * code/data would have to land, and ShortcutMode's own constructor already leans on
 * operator new() continuously. Real heap-collision risk for no need, when the whole
 * rest of this shim already proves the"call rbp's own compiled function from here"
 * pattern works (DJ_ENGINE_GLOBAL, rbp_abi.h).
 *
 * THE GEOMETRY, measured off a live screenshot taken 2026-10-17 (work console,
 * SHORTCUT screen open): both of the screen's two columns end their own content at
 * y=667, and rbp's own deck strip begins at y=711 (the same deck-strip floor
 * util_zone.h already documents for its own screen). y=670..708 is the only blank
 * band on the whole screen, full width, and is where this picker lives. It is shown
 * and tappable only while rbp is actually on SHORTCUT (its own BROWSE_MODE_SHORTCUT,
 * kept here as this module's own literal 10 -- test_xfader.c pins the two together,
 * the same convention util_zone.h uses for BROWSE_MODE_UTILITY).
 *
 * THE WIRE FORM is not this module's concern -- it hands back WHICH icon (0..2) was
 * tapped, in logical screen points, same shape as fxpad_zone.c's fxpad_hit(). The
 * caller (pointsrc.c) turns that into
 * `send_rx_key_f(K_FADERCURVE, OP_VALUE, CH_GLOBAL, XFADER_PARAM(i), (float)XFADER_PARAM(i))`,
 * mirroring the DDJ-400 bridge's own already-proven-live call byte for byte.
 */
#ifndef RBPI4B_XFADER_ZONE_H
#define RBPI4B_XFADER_ZONE_H

/* rbp's browse mode for the SHORTCUT screen -- see the header comment and
 * rbp_abi.h's K_SHORTCUT comment ("opens the SHORTCUT view, browse mode 10").
 * Kept as this module's own literal rather than including rbp_abi.h, the same
 * reason util_zone.h gives for UTIL_MODE_UTILITY; test_xfader.c pins the two. */
#define XFADER_MODE_SHORTCUT 10

/* The row, in the shim's 1280x800 logical space -- see the header comment for the
 * measurement. Inclusive at both ends, same convention as fxpad_zone.h's rect.
 * Y0/Y1 are the one blank full-width strip on the whole screen (measured
 * 2026-10-17) and are shared by painting and hit-testing alike -- there is
 * nothing else on this row for either to collide with. */
#define XFADER_ROW_Y0  670
#define XFADER_ROW_Y1  708
#define XFADER_ROW_H   (XFADER_ROW_Y1 - XFADER_ROW_Y0 + 1)   /* 39 */

/* WHERE IT IS PAINTED. Revised 2026-10-17 after the first live deploy: the
 * operator reported the band landing far from the block it is meant to sit
 * under ("abaixo dos deck 1 e 2") -- a full-width strip reads as its own
 * unrelated row when the native CHANNEL FADER CURVE block it extends is one
 * narrow column on the right (measured off a scanout screenshot: its label+3
 * icons span x790..1094, y595..667). The three icons here are measured off
 * that same block's own three icons (794/895/996), not chosen independently,
 * so this row's icons sit in the exact same lanes as the real ones above --
 * no label is painted in this row any more (the operator asked for it
 * removed, 2026-10-17 2nd pass); the combined heading now lives in the
 * native label's own spot instead, patched in place (see the "native label
 * patch" section below). XFADER_PAINT_X0/X1 is the plane's own rect
 * (menu_draw.c sizes and positions the plane from these, not from the wider
 * hit-test span below). */
#define XFADER_ICONS     3
#define XFADER_ICON_X0   794
#define XFADER_ICON_W    99
#define XFADER_ICON_GAP  2
#define XFADER_ICON_MARGIN_Y 2   /* top/bottom inset from the row into each icon */
#define XFADER_PAINT_X0  XFADER_ICON_X0
#define XFADER_PAINT_X1  (XFADER_ICON_X0 + XFADER_ICONS * (XFADER_ICON_W + XFADER_ICON_GAP) \
                           - XFADER_ICON_GAP - 1)   /* 1094: the last icon's own right edge */

/* WHERE IT ANSWERS TOUCH. Widened 2026-10-17, 2nd pass, after the operator
 * reported taps missing: narrowing the PAINTED row to the native column's own
 * width (above) made the hit-test span narrow too, since it used to share
 * XFADER_BAND_X0/X1 with painting -- any small, pre-existing calibration slop
 * in this touch panel (every report already passes through one fit, `pointsrc:
 * abs fit: panel 1920x1080 -> picture 1728x1080 at 96,0`) that used to be
 * absorbed by a 1280px-wide target became material against a 301px one. The
 * fix is not to guess at the panel's own error and compensate for it -- it is
 * to stop needing to: the ENTIRE row is blank glass (XFADER_ROW_Y0/Y1's own
 * comment), so hit-testing can safely claim the whole width again while the
 * paint stays narrow and aligned. xfader_hit() below picks the NEAREST of the
 * three icons for any x in this span -- there is no dead gap to miss into any
 * more, only a boundary. */
#define XFADER_HIT_X0  0
#define XFADER_HIT_X1  1279

/* icon index 0..2 -> rbp's K_FADERCURVE OP_VALUE parameter, 1..3. 0 (THRU) has no
 * icon here, the same choice the DDJ-400's own SHIFT+FX binding already made
 * (docs/12-ddj400.md: "0=THRU exists but unused here"). */
#define XFADER_PARAM(i)  ((i) + 1)

/* Left edge of icon i (0..XFADER_ICONS-1), in logical px -- where it is PAINTED,
 * not the (wider) span it answers touch on. */
int xfader_icon_x0(int i);

/* Is this logical point inside the row at all (XFADER_ROW_Y0..Y1, the full
 * XFADER_HIT_X0..X1 span)? Exposed so the caller can withhold the whole row from
 * rbp before testing which icon, same two-step fxpad_zone.h's hit test is not (fxpad
 * is one rect; this is one row containing three). */
int xfader_in_band(int x, int y);

/* Which icon (0..XFADER_ICONS-1) this logical point is CLOSEST to, or -1 if it is
 * outside the row entirely (xfader_in_band() false). Every point inside the row
 * maps to one of the three -- see XFADER_HIT_X0/X1's comment for why there is no
 * middle answer any more. */
int xfader_hit(int x, int y);

/* WHICH ICON IS CURRENTLY SELECTED, as far as rbp's own engine state says -- pushed
 * in by pointsrc.c's xfader_curve_tick() (it reads DJ_ENGINE_GET_XFADER_CURVE, which
 * this pure module must not touch) and read back by menu_draw.c's menu_xfader_live()
 * to know which icon to paint highlighted. The same split menu_zone.h's
 * menu_set_blink()/menu_blink() already uses for MASTER REC's indicator, and for the
 * same reason: this is live engine state, not touch-gesture state, so it is NOT
 * touched by any reset function in this file. -1 means none of the three (rbp is on
 * THRU, or the engine is not up yet). */
void xfader_set_selected(int i);
int xfader_selected(void);

/* WHETHER THE BAND SHOULD BE SHOWN AT ALL -- pushed in by the same tick as the
 * selection, above, for the same reason: menu_draw.c's compositor never reads rbp
 * directly (see menu_fxpad_plane_sync()'s own gate, fxpad_busy(), a pure predicate
 * for the same purpose), it only ever reads a pure module's own state. 1 while rbp
 * is on the SHORTCUT screen with nothing else covering it, 0 otherwise. */
void xfader_set_showing(int on);
int xfader_showing(void);

/* ---------------------------------------------------------------------------
 * THE NATIVE LABEL PATCH (2026-10-17, 2nd pass). The operator asked for the
 * icon row's own "CROSSFADER CURVE" label removed (the icons read clearly
 * enough on their own, aligned under the real ones) and for rbp's own
 * "CHANNEL FADER CURVE" heading changed to "CHANNEL/CROSS FADER CURVE" instead,
 * so the one heading now describes both rows. That heading is NOT blank glass
 * -- it is rbp's own drawn text -- so this half is a PAGE-ROUTE overwrite
 * (menu_fxp_page_view()'s own technique: write straight into rbp's live
 * framebuffer, not a plane of our own) rather than a new overlay. Pioneer's
 * own font is not reachable from here (it is compiled into rbp, not a shared
 * asset), so the patch uses this shim's own menu_font.h, the same font every
 * other label in this shim's family already uses -- consistent with the
 * shim's own look even though it cannot byte-for-byte match rbp's.
 *
 * THE RECT was measured off the same 2026-10-17 screenshot the icon lanes
 * were: the native label's own ink spans x790..979, y595..605 (menu_text_width()
 * confirms "CHANNEL FADER CURVE" is comfortably narrower than the lane it
 * sits in -- the native font is smaller than this shim's). XFADER_LABEL_X0/X1
 * reuse the icon row's own span (790..1094) so the patch never reaches into
 * the unrelated knob control to its left or the icons it sits above; Y0/Y1
 * give this shim's taller font (MENU_FONT_LINE) room while stopping clear of
 * the real icons at y617. Because this is live rbp pixels and not a plane, it
 * is repainted every tick while showing (same reasoning fxpad's page-route
 * fallback gives: there is no way to know rbp will not redraw under it, so
 * the only safe answer is to keep re-asserting it) and simply stops when
 * xfader_showing() goes to 0 -- leaving the SCREEN, not this file, to put
 * rbp's own original heading back the next time it draws this screen fresh. */
#define XFADER_LABEL_X0  787                /* 3px clear of the measured ink (790) */
#define XFADER_LABEL_X1  XFADER_PAINT_X1    /* 1094: the icon lane's own right edge */
#define XFADER_LABEL_Y0  590
#define XFADER_LABEL_Y1  616                /* clear of the real icons at y617 */

#endif /* RBPI4B_XFADER_ZONE_H */
