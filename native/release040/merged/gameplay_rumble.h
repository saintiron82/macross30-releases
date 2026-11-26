#ifndef MACROSS_GAMEPLAY_RUMBLE_H
#define MACROSS_GAMEPLAY_RUMBLE_H

enum { MACROSS_RUMBLE_WEAK = 1, MACROSS_RUMBLE_STRONG = 2 };

/* Starting values for live impact tuning, in guest velocity units and host ms. */
enum { MACROSS_IMPACT_MERGE_MS = 300 };

static inline float macross_impact_intensity(float closing_speed)
{
    const float minimum = 1.5f, maximum = 8.0f;
    if (!(closing_speed >= minimum)) return -1.0f;
    if (closing_speed >= maximum) return 1.0f;
    return (closing_speed - minimum) / (maximum - minimum);
}

#ifdef __cplusplus
extern "C" {
#endif

void cellPad_gameplay_rumble(unsigned port, unsigned kind);
void cellPad_gameplay_impact(unsigned port, float closing_speed);
void cellPad_set_rumble_enabled(int enabled);
void cellPad_set_rumble_strength(unsigned percent);

#ifdef __cplusplus
}
#endif

#endif
