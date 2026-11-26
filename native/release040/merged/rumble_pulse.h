#ifndef MACROSS_RUMBLE_PULSE_H
#define MACROSS_RUMBLE_PULSE_H

#include <stdint.h>
#include "gameplay_rumble.h"

typedef struct {
    uint16_t lo, hi;
    unsigned remaining_ms;
} RumbleOutput;

typedef struct {
    uint64_t started_ms, until_ms;
    float intensity;
    uint16_t lo, hi;
} RumbleImpact;

typedef struct {
    uint64_t weak_until_ms;
    uint64_t strong_until_ms;
    RumbleImpact impact;
} RumblePulse;

static inline unsigned rumble_pulse_duration_ms(unsigned kind, unsigned strength_pct)
{
    const unsigned base_ms = kind == MACROSS_RUMBLE_WEAK ? 150 : 500;
    const unsigned scaled_pct = 2 * strength_pct; /* Slider 100% uses the original 200% pulse. */
    return scaled_pct > 100 ? base_ms * (200 + (scaled_pct - 100)) / 200 : base_ms;
}

static inline void rumble_start(RumblePulse* pulse, unsigned kind, uint64_t now, unsigned strength_pct)
{
    if (kind == MACROSS_RUMBLE_WEAK)
        pulse->weak_until_ms = now + rumble_pulse_duration_ms(kind, strength_pct);
    if (kind == MACROSS_RUMBLE_STRONG)
        pulse->strong_until_ms = now + rumble_pulse_duration_ms(kind, strength_pct);
}

static inline uint16_t rumble_motor_level(unsigned level)
{
    return (uint16_t)(level > UINT16_MAX ? UINT16_MAX : level);
}

static inline RumbleOutput rumble_pulse_output(unsigned kind, unsigned strength_pct)
{
    const unsigned scaled_pct = 2 * strength_pct;
    unsigned lo, hi;
    if (kind == MACROSS_RUMBLE_STRONG) {
        lo = 65535u * scaled_pct / 100;
        hi = 39321u * scaled_pct / 100;
    } else {
        lo = 22937u * scaled_pct / 100;
        hi = scaled_pct > 100 ? 19661u * (scaled_pct - 100) / 100 : 0;
    }
    return (RumbleOutput){rumble_motor_level(lo), rumble_motor_level(hi),
        rumble_pulse_duration_ms(kind, strength_pct)};
}

static inline unsigned rumble_interpolate(unsigned weak, unsigned strong, float intensity)
{
    return (unsigned)(weak + (strong - weak) * intensity + 0.5f);
}

static inline int rumble_start_impact(RumblePulse* pulse, float closing_speed, uint64_t now, unsigned strength_pct)
{
    RumbleImpact* impact = &pulse->impact;
    const float intensity = macross_impact_intensity(closing_speed);
    if (!strength_pct || intensity < 0) return 0;
    if (impact->until_ms && now - impact->started_ms < MACROSS_IMPACT_MERGE_MS
        && intensity <= impact->intensity) return 0;

    const RumbleOutput weak = rumble_pulse_output(MACROSS_RUMBLE_WEAK, strength_pct);
    const RumbleOutput strong = rumble_pulse_output(MACROSS_RUMBLE_STRONG, strength_pct);
    *impact = (RumbleImpact){
        now, now + rumble_interpolate(weak.remaining_ms, strong.remaining_ms, intensity), intensity,
        (uint16_t)rumble_interpolate(weak.lo, strong.lo, intensity),
        (uint16_t)rumble_interpolate(weak.hi, strong.hi, intensity),
    };
    return 1;
}

static inline void rumble_mix_channel(RumbleOutput* result, uint16_t lo, uint16_t hi, unsigned remaining_ms)
{
    if (lo > result->lo) result->lo = lo;
    if (hi > result->hi) result->hi = hi;
    /* Both backends reassess output at the first active channel's expiry. */
    if (!result->remaining_ms || remaining_ms < result->remaining_ms) result->remaining_ms = remaining_ms;
}

static inline RumbleOutput rumble_mix(RumblePulse pulse, uint64_t now, uint16_t guest_lo, uint16_t guest_hi,
                                      unsigned strength_pct)
{
    RumbleOutput result = {guest_lo, guest_hi, 0};
    const int strong = now < pulse.strong_until_ms;
    const uint64_t until_ms = strong ? pulse.strong_until_ms : pulse.weak_until_ms;
    if (now < until_ms) {
        const RumbleOutput hit = rumble_pulse_output(strong ? MACROSS_RUMBLE_STRONG : MACROSS_RUMBLE_WEAK,
            strength_pct);
        rumble_mix_channel(&result, hit.lo, hit.hi, (unsigned)(until_ms - now));
    }
    if (strength_pct && now < pulse.impact.until_ms) {
        rumble_mix_channel(&result, pulse.impact.lo, pulse.impact.hi, (unsigned)(pulse.impact.until_ms - now));
    }
    return result;
}

#endif
