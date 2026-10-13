/* Temporary diagnostic: log DirectFB gfxcard operations whose destination
 * rectangle has its right edge (or a 1-px width) on the columns where a stray
 * pixel column shows up next to the selected-deck frame (x=183, x=631, ...).
 * LD_PRELOAD'ed into rbp; interposes the PLT calls libdirectfb makes into its
 * own exported dfb_gfxcard_* functions. Not part of the shipped stack. */
#include <config.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <directfb.h>
#include <core/state.h>
#include <core/surface.h>

/* vendor glibc 2.13: dlsym lives in libdl at GLIBC_2.4 (see crashcatch.c) */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");

static FILE *lf; static int lines; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int hot(int x, int w) {
     int r = x + w - 1;
     return r == 182 || r == 183 || r == 184 || r == 630 || r == 631 || r == 632 ||
            r == 1270 || r == 1271 || r == 1272 || x == 183 || x == 631 || x == 1271;
}
static void lg(const char *op, CardState *s, const DFBRectangle *sr, int dx, int dy, int dw, int dh, void *ra) {
     if (!hot(dx, dw)) return;
     pthread_mutex_lock(&mu);
     if (!lf) { lf = fopen("/tmp/blittrace.log", "a"); if (lf) setvbuf(lf, NULL, _IOLBF, 0); }
     if (lf && lines < 20000) {
          CoreSurface *d = s->destination, *src = s->source;
          fprintf(lf, "%s dst=%p(%dx%d) d=%d,%d %dx%d src=%p(%dx%d)", op, d,
                  d ? d->config.size.w : -1, d ? d->config.size.h : -1, dx, dy, dw, dh, src,
                  src ? src->config.size.w : -1, src ? src->config.size.h : -1);
          if (sr) fprintf(lf, " s=%d,%d %dx%d", sr->x, sr->y, sr->w, sr->h);
          fprintf(lf, " bf=%x df=%x clip=%d,%d-%d,%d col=%02x%02x%02x%02x ra=%p\n", s->blittingflags,
                  s->drawingflags, s->clip.x1, s->clip.y1, s->clip.x2, s->clip.y2,
                  s->color.a, s->color.r, s->color.g, s->color.b, ra);
          lines++;
     }
     pthread_mutex_unlock(&mu);
}
#define REAL(n) static __typeof__(n) *real; if (!real) real = dlsym(RTLD_NEXT, #n)

void dfb_gfxcard_blit(DFBRectangle *rect, int dx, int dy, CardState *state) {
     REAL(dfb_gfxcard_blit);
     lg("blit", state, rect, dx, dy, rect->w, rect->h, __builtin_return_address(0));
     real(rect, dx, dy, state);
}
void dfb_gfxcard_stretchblit(DFBRectangle *srect, DFBRectangle *drect, CardState *state) {
     REAL(dfb_gfxcard_stretchblit);
     lg("stretch", state, srect, drect->x, drect->y, drect->w, drect->h, __builtin_return_address(0));
     real(srect, drect, state);
}
void dfb_gfxcard_batchblit(DFBRectangle *rects, DFBPoint *points, int num, CardState *state) {
     REAL(dfb_gfxcard_batchblit);
     for (int i = 0; i < num; i++)
          lg("batch", state, &rects[i], points[i].x, points[i].y, rects[i].w, rects[i].h, __builtin_return_address(0));
     real(rects, points, num, state);
}
void dfb_gfxcard_tileblit(DFBRectangle *rect, int dx1, int dy1, int dx2, int dy2, CardState *state) {
     REAL(dfb_gfxcard_tileblit);
     lg("tile", state, rect, dx1, dy1, dx2 - dx1, dy2 - dy1, __builtin_return_address(0));
     real(rect, dx1, dy1, dx2, dy2, state);
}
void dfb_gfxcard_fillrectangles(const DFBRectangle *rects, int num, CardState *state) {
     REAL(dfb_gfxcard_fillrectangles);
     for (int i = 0; i < num; i++)
          lg("fill", state, NULL, rects[i].x, rects[i].y, rects[i].w, rects[i].h, __builtin_return_address(0));
     real(rects, num, state);
}
void dfb_gfxcard_drawrectangle(DFBRectangle *rect, CardState *state) {
     REAL(dfb_gfxcard_drawrectangle);
     lg("drawrect", state, NULL, rect->x, rect->y, rect->w, rect->h, __builtin_return_address(0));
     real(rect, state);
}
void dfb_gfxcard_drawlines(DFBRegion *l, int num, CardState *state) {
     REAL(dfb_gfxcard_drawlines);
     for (int i = 0; i < num; i++) {
          int x = l[i].x1 < l[i].x2 ? l[i].x1 : l[i].x2, w = abs(l[i].x2 - l[i].x1) + 1;
          lg("line", state, NULL, x, l[i].y1, w, abs(l[i].y2 - l[i].y1) + 1, __builtin_return_address(0));
     }
     real(l, num, state);
}
