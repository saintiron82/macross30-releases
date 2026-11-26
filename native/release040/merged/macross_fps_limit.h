#ifndef MACROSS_FPS_LIMIT_H
#define MACROSS_FPS_LIMIT_H
#include <string.h>

/* The existing main callback forwards this integer to the guest gate and
 * treats any nonzero mode as an uncapped DXGI present. Keep its 0/1 ABI and
 * add mode 2 for the one-vblank (60 Hz) guest gate. */
enum { MACROSS_FPS_30 = 0, MACROSS_FPS_UNLIMITED = 1, MACROSS_FPS_60 = 2 };
static inline int macross_fps_limit_parse(const char* value, int* out)
{
    if (!value || !out) return 0;
    if (!strcmp(value, "30")) *out = 30;
    else if (!strcmp(value, "60")) *out = 60;
    else if (!strcmp(value, "0")) *out = 0;
    else return 0;
    return 1;
}
static inline int macross_fps_limit_to_mode(int limit)
{
    return limit == 0 ? MACROSS_FPS_UNLIMITED : limit == 60 ? MACROSS_FPS_60 : MACROSS_FPS_30;
}
static inline int macross_fps_mode_to_limit(int mode)
{
    return mode == MACROSS_FPS_UNLIMITED ? 0 : mode == MACROSS_FPS_60 ? 60 : 30;
}
static inline unsigned macross_fps_mode_vblanks(int mode)
{
    return mode == MACROSS_FPS_UNLIMITED ? 0u : mode == MACROSS_FPS_60 ? 1u : 2u;
}
#endif
