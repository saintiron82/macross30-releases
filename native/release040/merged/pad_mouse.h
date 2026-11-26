/* Mouse velocity sampled once per pad sweep.  No platform dependencies. */
#ifndef PS3RECOMP_PAD_MOUSE_H
#define PS3RECOMP_PAD_MOUSE_H
#include <math.h>
#include <stdlib.h>
typedef struct { unsigned long long last, poll; int x, y; } PadMouseState;
static int pad_mouse_axis(long delta, double scale)
{
    double v = (double)delta * scale;
    return v < -128 ? -128 : v > 127 ? 127 : (int)v;
}
static void pad_mouse_step(PadMouseState *s, unsigned long long now,
                           long dx, long dy, double sx, double sy,
                           int invert_y, int active, int reset)
{
    if (!active || reset) { s->x = s->y = 0; s->last = s->poll = 0; return; }
    s->poll = now;
    if (dx || dy) {
        /* Tuned native response: 1.00 means four stick units per raw
         * count. Combining events per sweep differs from MyInput, which
         * clamps each event independently. */
        s->x = pad_mouse_axis(dx, sx * 4.0);
        s->y = pad_mouse_axis(dy, (invert_y ? -sy : sy) * 4.0);
        s->last = now;
    } else if (!s->last || now - s->last > 35) s->x = s->y = 0;
}
static double pad_mouse_scale(const char *e)
{
    char *end = 0;
    double v = e ? strtod(e, &end) : 1.0;
    return !e || end == e || *end || !isfinite(v) || v < 0.01 || v > 8.0 ? 1.0 : v;
}
#endif
