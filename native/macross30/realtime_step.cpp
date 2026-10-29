// Original producer: 0002AAE4, TOC=00AC1BA0. Its lfs at 2AC1C/2AC8C/2AD00
// reads 00ABAFFC=3D088889 (float 1/30), regardless of incoming FPR1.
// Keep the shared constant and animation's separate 1/30 quantization intact.
// Neither experimental mode establishes correct physics/camera/input pacing.
#include "realtime_step.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint64_t ppu_timebase_now(void);
extern "C" uint32_t vm_read32(uint64_t);

namespace {
constexpr double frequency = 79800000.0;
constexpr float initial_step = 1.0f / 30.0f;
constexpr double maximum_step = 0.050;
enum Mode { Original, Caller, Wall };
struct Settings { Mode mode = Original; bool probe = false, variable = false; bool active() const { return mode != Original || probe; } };
const Settings& settings() {
    static const Settings result = [] {
        Settings s;
        const char* e = std::getenv("MACROSS_REALTIME_STEP");
        if (e && !std::strcmp(e, "caller")) s.mode = Caller;
        else if (e && !std::strcmp(e, "wall")) s.mode = Wall;
        e = std::getenv("MACROSS_REALTIME_STEP_PROBE");
        s.probe = e && !std::strcmp(e, "1");
        e = std::getenv("MACROSS_VARIABLE_STEP");
        s.variable = e && !std::strcmp(e, "1");
        if (s.active()) std::fprintf(stderr, "[realtime-step] mode=%s probe=%u scope=3_fixed_lfs_only initial=1/30 max=0.050 game_pace_verified=0 clamp_discards_excess_time=1\n",
            s.mode == Caller ? "caller" : s.mode == Wall ? "wall" : "original", unsigned(s.probe));
        if (s.variable) std::fprintf(stderr, "[variable-step] requested=1 enabled=%u requires=caller scope=guarded_main_app_floor_and_3_consumers upper_1/15_and_global_override_preserved=1 extra_caller_50ms_clamp=0 game_pace_verified=0\n", unsigned(s.mode == Caller));
        return s;
    }();
    return result;
}
struct Site {
    uint64_t calls = 0, frames = 0, fallback = 0, clamps = 0;
    uint64_t caller_clamp_bypassed = 0;
    double original_sum = 0, effective_sum = 0, unique_effective_sum = 0;
};
struct Window {
    uint64_t frames = 0, consumers = 0, missing_consumers = 0, duplicate_consumers = 0;
    uint64_t wall_clamps = 0, initial_frames = 0, incoming_invalid = 0;
    uint64_t variable_frames = 0, variable_guard_failed = 0;
    uint64_t floor_calls = 0, floor_preserved = 0, floor_fallback = 0;
    double floor_removed_excess = 0;
    double raw_sum = 0, clamped_sum = 0, incoming_sum = 0;
    double incoming_min = 1e300, incoming_max = -1e300;
    Site site[3];
};
struct Frame {
    uint64_t owner = 0, generation = 0, previous = 0, sampled = 0, report_at = 0;
    unsigned depth = 0, consumers = 0, reports = 0;
    bool seen[3] = {};
    bool variable_valid = false;
    float wall = initial_step;
    Window window;
};
thread_local Frame frame;
std::atomic<unsigned> fallback_lines{0};
int site_index(uint32_t pc) {
    return pc == 0x0002AC1C ? 0 : pc == 0x0002AC8C ? 1 : pc == 0x0002AD00 ? 2 : -1;
}
bool guard(uint64_t ea, double original) {
    return ea == 0x00ABAFFC && original == double(initial_step) &&
        vm_read32(0x0002AC1C) == 0xC022945C &&
        vm_read32(0x0002AC8C) == 0xC022945C &&
        vm_read32(0x0002AD00) == 0xC3E2945C &&
        vm_read32(0x00ABAFFC) == 0x3D088889;
}
bool variable_guard() {
    // Both signed tick conversion paths join this original fmr. Preserve
    // their comparisons, the later upper 1/15 clamp and optional override.
    return vm_read32(0x000AA274) == 0xFF8D6000 &&
        vm_read32(0x000AA278) == 0x419C0248 &&
        vm_read32(0x000AA4B8) == 0xFF8D6000 &&
        vm_read32(0x000AA4BC) == 0x409CFDC0 &&
        vm_read32(0x000AA4C0) == 0xFDA06090 &&
        vm_read32(0x00ABDC54) == 0x3D088889 &&
        vm_read32(0x00ABDC60) == 0x3D888889 &&
        guard(0x00ABAFFC, double(initial_step));
}
void fallback(uint64_t thread, uint32_t pc, const char* reason) {
    if (fallback_lines.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr, "[realtime-step-fallback] thread=%llu pc=%08X reason=%s original_preserved=1\n",
            (unsigned long long)thread, pc, reason);
}
void report() {
    auto& f = frame;
    if (!f.report_at || f.sampled - f.report_at < uint64_t(frequency)) return;
    if (f.reports++ < 120 || (f.reports % 10) == 0) {
        const auto& w = f.window;
        std::fprintf(stderr, "[realtime-step-window] thread=%llu generation=%llu elapsed=%.9f producer_frames=%llu consumer_calls=%llu frames_without_consumer=%llu duplicate_consumer_calls=%llu raw_dt_sum=%.9f clamped_dt_sum=%.9f wall_clamps=%llu initial_frames=%llu incoming_sum=%.9f incoming_min=%.9f incoming_max=%.9f incoming_invalid=%llu",
            (unsigned long long)f.owner, (unsigned long long)f.generation, double(f.sampled-f.report_at)/frequency,
            (unsigned long long)w.frames, (unsigned long long)w.consumers,
            (unsigned long long)w.missing_consumers, (unsigned long long)w.duplicate_consumers,
            w.raw_sum, w.clamped_sum, (unsigned long long)w.wall_clamps,
            (unsigned long long)w.initial_frames, w.incoming_sum,
            w.consumers > w.incoming_invalid ? w.incoming_min : 0.,
            w.consumers > w.incoming_invalid ? w.incoming_max : 0., (unsigned long long)w.incoming_invalid);
        std::fprintf(stderr, " variable_frames=%llu variable_guard_failed=%llu floor_calls=%llu floor_preserved=%llu floor_fallback=%llu floor_removed_excess=%.9f",
            (unsigned long long)w.variable_frames, (unsigned long long)w.variable_guard_failed,
            (unsigned long long)w.floor_calls, (unsigned long long)w.floor_preserved,
            (unsigned long long)w.floor_fallback, w.floor_removed_excess);
        for (unsigned i=0; i<3; ++i) {
            const auto& s=w.site[i];
            std::fprintf(stderr, " site%u_calls=%llu site%u_frames=%llu site%u_original_sum=%.9f site%u_effective_sum=%.9f site%u_unique_effective_sum=%.9f site%u_fallback=%llu site%u_clamps=%llu",
                i, (unsigned long long)s.calls, i, (unsigned long long)s.frames,
                i, s.original_sum, i, s.effective_sum, i, s.unique_effective_sum,
                i, (unsigned long long)s.fallback, i, (unsigned long long)s.clamps);
            std::fprintf(stderr, " site%u_caller_clamp_bypassed=%llu", i, (unsigned long long)s.caller_clamp_bypassed);
        }
        std::fputc('\n', stderr);
    }
    f.window = Window{};
    f.report_at = f.sampled;
}
}

extern "C" void macross_realtime_step_begin(uint64_t thread_id) {
    if (!settings().active()) return; // OFF: no clock, VM reads or TLS writes.
    auto& f=frame;
    if (f.depth) { if (f.owner == thread_id) ++f.depth; return; }
    if (f.owner != thread_id) { f=Frame{}; f.owner=thread_id; }
    f.depth=1; f.consumers=0; f.seen[0]=f.seen[1]=f.seen[2]=false;
    f.variable_valid=settings().variable && settings().mode == Caller && variable_guard();
    f.sampled=ppu_timebase_now();
    if (!f.report_at) f.report_at=f.sampled;
    const bool first=!f.previous || f.sampled < f.previous;
    const double raw=first ? 0. : double(f.sampled-f.previous)/frequency;
    f.wall=first ? initial_step : float(raw > maximum_step ? maximum_step : raw);
    f.previous=f.sampled; ++f.generation; ++f.window.frames;
    f.window.raw_sum += raw; f.window.clamped_sum += double(f.wall);
    f.window.wall_clamps += raw > maximum_step; f.window.initial_frames += first;
    f.window.variable_frames += settings().variable && settings().mode == Caller;
    f.window.variable_guard_failed += settings().variable && settings().mode == Caller && !f.variable_valid;
}
extern "C" void macross_realtime_step_end(uint64_t thread_id) {
    if (!settings().active()) return;
    auto& f=frame;
    if (!f.depth || f.owner != thread_id) return;
    if (--f.depth) return;
    f.window.missing_consumers += f.consumers == 0;
    report();
}
extern "C" void macross_realtime_step_consumer(uint64_t thread_id, double incoming) {
    if (!settings().active()) return;
    auto& f=frame;
    if (!f.depth || f.owner != thread_id || !f.generation) {
        fallback(thread_id, 0x0002AAE4, "no_active_producer"); return;
    }
    f.window.duplicate_consumers += f.consumers++ != 0;
    ++f.window.consumers;
    if (std::isfinite(incoming)) {
        f.window.incoming_sum += incoming;
        if (incoming < f.window.incoming_min) f.window.incoming_min=incoming;
        if (incoming > f.window.incoming_max) f.window.incoming_max=incoming;
    } else ++f.window.incoming_invalid;
}
extern "C" double macross_realtime_step_value(uint64_t thread_id, uint32_t pc,
    uint64_t constant_ea, double original, double incoming) {
    const auto& cfg=settings();
    if (!cfg.active()) return original;
    auto& f=frame; const int i=site_index(pc);
    if (i < 0 || !f.depth || f.owner != thread_id || !f.generation) {
        fallback(thread_id, pc, "no_active_producer_or_site"); return original;
    }
    auto& s=f.window.site[i]; ++s.calls; s.original_sum+=original;
    double effective=original;
    if (cfg.variable && (cfg.mode != Caller || !f.variable_valid || !variable_guard())) {
        f.variable_valid=false; ++s.fallback; fallback(thread_id, pc, "paired_variable_guard");
    }
    else if (!guard(constant_ea, original)) { f.variable_valid=false; ++s.fallback; fallback(thread_id, pc, "source_guard"); }
    else if (cfg.mode == Wall) effective=double(f.wall);
    else if (cfg.mode == Caller) {
        if (!std::isfinite(incoming) || incoming < 0) { ++s.fallback; fallback(thread_id, pc, "invalid_incoming"); }
        else if (cfg.variable) { ++s.caller_clamp_bypassed; effective=double(float(incoming)); }
        else { s.clamps += incoming > maximum_step; effective=double(float(incoming > maximum_step ? maximum_step : incoming)); }
    }
    s.effective_sum+=effective;
    if (!f.seen[i]) { f.seen[i]=true; ++s.frames; s.unique_effective_sum+=effective; }
    return effective;
}

extern "C" double macross_variable_step_floor(uint64_t thread_id,
    double computed, double original_floor) {
    const auto& cfg=settings();
    if (!cfg.variable || cfg.mode != Caller) return original_floor;
    auto& f=frame;
    if (!f.depth || f.owner != thread_id || !f.generation) {
        fallback(thread_id, 0x000AA4C0, "no_active_variable_producer"); return original_floor;
    }
    ++f.window.floor_calls;
    if (!f.variable_valid || !variable_guard() || original_floor != double(initial_step)) {
        f.variable_valid=false; ++f.window.floor_fallback;
        fallback(thread_id, 0x000AA4C0, "paired_variable_guard"); return original_floor;
    }
    if (!std::isfinite(computed) || computed < 0 || computed >= original_floor) {
        f.variable_valid=false; ++f.window.floor_fallback;
        fallback(thread_id, 0x000AA4C0, "invalid_computed_step"); return original_floor;
    }
    ++f.window.floor_preserved;
    f.window.floor_removed_excess += original_floor-computed;
    // computed already passed the original frsp; do not round or rescale it.
    return computed;
}

// MACROSS_DT_STEP=1 (experimental, with the frame cap lifted): the fourteen
// float 1/30 frame-step constants in the TOC data (found by value) are
// rewritten every frame with the measured frame time, clamped to 1/60..1/20.
// Measured: the save-point barrier pulse is 4.00 s at 30 FPS, 3.65 s at an
// uncapped 33 FPS (it advances 1/30 per frame) and 7.70 s with the constants
// at 1/60 -- it is timed by these constants. Frame-counted logic is not.
extern "C" uint8_t* vm_base;
extern "C" void macross_dt_step_frame(void) {
    static int enabled = -1;
    static const uint32_t sites[] = { 0x00ABAFFC, 0x00ABB62C, 0x00ABDC54, 0x00AC222C, 0x00AC2370,
        0x00AC4620, 0x00AC4CCC, 0x00AC5488, 0x00AD20DC, 0x00AD21B4, 0x00AD2698, 0x00AD29FC,
        0x00AD45C8, 0x00AD55B4 };
    if (enabled < 0) {
        const char* e = std::getenv("MACROSS_DT_STEP");
        enabled = e && !std::strcmp(e, "1");
        if (enabled)
            for (uint32_t ea : sites)
                if (vm_read32(ea) != 0x3D088889u) {
                    std::fprintf(stderr, "[dt-step] %08X is %08X, not 1/30: disabled\n", ea, vm_read32(ea));
                    enabled = 0; break;
                }
        if (enabled) std::fprintf(stderr, "[dt-step] enabled: %zu frame-step constants follow the measured frame time\n",
                                  sizeof sites / sizeof sites[0]);
    }
    if (!enabled) return;
    static uint64_t previous;
    const uint64_t now = ppu_timebase_now();
    float dt = previous && now > previous ? float(double(now - previous) / frequency) : initial_step;
    previous = now;
    if (dt < 1.0f / 60.0f) dt = 1.0f / 60.0f;
    if (dt > 1.0f / 20.0f) dt = 1.0f / 20.0f;
    uint32_t bits; std::memcpy(&bits, &dt, 4);
    const uint8_t be[4] = { uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits) };
    for (uint32_t ea : sites) std::memcpy(vm_base + ea, be, 4);
}
