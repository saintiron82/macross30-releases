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
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <string>
#include <chrono>
#include <new>
#include <thread>
#include <vector>

extern "C" uint64_t ppu_timebase_now(void);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint8_t* vm_base;

namespace {
constexpr double frequency = 79800000.0;
constexpr float initial_step = 1.0f / 30.0f;
constexpr double maximum_step = 0.050;
// The producer stores bounded elapsed through frsp/lfs. Its 50ms float rounds
// just above the host-double bound; consumer guards must accept that exact
// guest representation. Wall-time sampling keeps the strict bound above.
constexpr double maximum_guest_step = double(float(maximum_step));
enum Mode { Original, Caller, Wall };
struct Settings { Mode mode = Original; bool probe = false, variable = false, dt = false; bool active() const { return mode != Original || probe || dt; } };
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
        e = std::getenv("MACROSS_DT_STEP");
        s.dt = !e || std::strcmp(e, "0");
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
    uint64_t main_calls = 0, animation_calls = 0, timer_calls = 0, timer_ticks = 0;
    double main_sum = 0, animation_sum = 0;
};
struct FrameTime {
    uint64_t owner = 0, generation = 0, previous = 0, sampled = 0, report_at = 0;
    unsigned depth = 0, consumers = 0, reports = 0;
    bool seen[3] = {};
    bool variable_valid = false;
    bool dt_valid = false;
    uint64_t timer_remainder = 0;
    unsigned timer_ticks = 1;
    float wall = initial_step;
    double presentation = initial_step;
    double game = initial_step;
    Window window;
};
thread_local FrameTime frame;
std::atomic<unsigned> fallback_lines{0};
// Time units, clamps and cached animation defaults remain immutable.
constexpr uint32_t elapsed_sites[] = {
    0x00ABAFFC, 0x00AC222C, 0x00AC2370, 0x00AC4620, 0x00AD20DC,
    0x00AD21B4, 0x00AD2698, 0x00AD29FC, 0x00AD45C8
};
bool dt_enabled() {
    static const bool enabled = [] {
        if (!settings().dt) return false;
        constexpr uint32_t units[] = {0x00ABB62C,0x00ABDC54,0x00AC4CCC,0x00AC5488,0x00AD55B4};
        auto reject=[]() { std::fprintf(stderr, "[dt-step] disabled: incompatible guest code/constants; elapsed correction not applied\n"); return false; };
        for (uint32_t ea : elapsed_sites) if (vm_read32(ea) != 0x3D088889u) return reject();
        for (uint32_t ea : units) if (vm_read32(ea) != 0x3D088889u) return reject();
        if (vm_read32(0x000AA4C4) != 0x8122C0C4u || vm_read32(0x000AAAC8) != 0x8122C0C4u ||
            vm_read32(0x00031D28) != 0x8102973Cu || vm_read32(0x00233A70) != 0xF821FF71u ||
            vm_read32(0x00246450) != 0xF821FF71u) return reject();
        std::fprintf(stderr, "[dt-step] enabled: frame-owned elapsed clock; 9 elapsed constants; 5 time units preserved; fixed_timer_hz=30 game_pace_verified=0\n");
        return true;
    }();
    return enabled;
}
void publish_elapsed(float dt) {
    uint32_t bits; std::memcpy(&bits, &dt, 4);
    const uint8_t be[] = {uint8_t(bits>>24),uint8_t(bits>>16),uint8_t(bits>>8),uint8_t(bits)};
    for (uint32_t ea : elapsed_sites) std::memcpy(vm_base + ea, be, 4);
}
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
        char buffer[8192];
        size_t used=0;
        auto append=[&](const char* format, auto... args) {
            const size_t available=sizeof(buffer)-used;
            const int count=std::snprintf(buffer+used, available, format, args...);
            if (count>0) used += size_t(count)<available ? size_t(count) : available-1;
        };
        const auto& w = f.window;
        append("[realtime-step-window] thread=%llu generation=%llu elapsed=%.9f producer_frames=%llu consumer_calls=%llu frames_without_consumer=%llu duplicate_consumer_calls=%llu raw_dt_sum=%.9f clamped_dt_sum=%.9f wall_clamps=%llu initial_frames=%llu incoming_sum=%.9f incoming_min=%.9f incoming_max=%.9f incoming_invalid=%llu",
            (unsigned long long)f.owner, (unsigned long long)f.generation, double(f.sampled-f.report_at)/frequency,
            (unsigned long long)w.frames, (unsigned long long)w.consumers,
            (unsigned long long)w.missing_consumers, (unsigned long long)w.duplicate_consumers,
            w.raw_sum, w.clamped_sum, (unsigned long long)w.wall_clamps,
            (unsigned long long)w.initial_frames, w.incoming_sum,
            w.consumers > w.incoming_invalid ? w.incoming_min : 0.,
            w.consumers > w.incoming_invalid ? w.incoming_max : 0., (unsigned long long)w.incoming_invalid);
        append(" variable_frames=%llu variable_guard_failed=%llu floor_calls=%llu floor_preserved=%llu floor_fallback=%llu floor_removed_excess=%.9f",
            (unsigned long long)w.variable_frames, (unsigned long long)w.variable_guard_failed,
            (unsigned long long)w.floor_calls, (unsigned long long)w.floor_preserved,
            (unsigned long long)w.floor_fallback, w.floor_removed_excess);
        for (unsigned i=0; i<3; ++i) {
            const auto& s=w.site[i];
            append(" site%u_calls=%llu site%u_frames=%llu site%u_original_sum=%.9f site%u_effective_sum=%.9f site%u_unique_effective_sum=%.9f site%u_fallback=%llu site%u_clamps=%llu",
                i, (unsigned long long)s.calls, i, (unsigned long long)s.frames,
                i, s.original_sum, i, s.effective_sum, i, s.unique_effective_sum,
                i, (unsigned long long)s.fallback, i, (unsigned long long)s.clamps);
            append(" site%u_caller_clamp_bypassed=%llu", i, (unsigned long long)s.caller_clamp_bypassed);
        }
        append(" main_elapsed_calls=%llu main_elapsed_sum=%.9f animation_elapsed_calls=%llu animation_elapsed_sum=%.9f fixed_timer_calls=%llu fixed_timer_ticks=%llu\n",
            (unsigned long long)w.main_calls, w.main_sum, (unsigned long long)w.animation_calls,
            w.animation_sum, (unsigned long long)w.timer_calls, (unsigned long long)w.timer_ticks);
        std::fputs(buffer, stderr);
    }
    f.window = Window{};
    f.report_at = f.sampled;
}
}

namespace {
std::atomic<uint64_t> census_frame{0};
// Frame interval histogram (MACROSS_FRAME_HIST=<prefix>, 0.25 ms buckets up
// to 250 ms) to compare frame time with census on/off. Frame owner writes.
constexpr unsigned frame_hist_buckets = 1000;
std::atomic<uint64_t> frame_hist[frame_hist_buckets + 1];
uint64_t frame_hist_last = 0;
const char* frame_hist_path() {
    static const char* p = [] { const char* e = std::getenv("MACROSS_FRAME_HIST"); return (e && *e) ? e : nullptr; }();
    return p;
}
void frame_hist_sample(uint64_t sampled) {
    const uint64_t previous = frame_hist_last;
    frame_hist_last = sampled;
    if (!previous || sampled <= previous) return;
    const double ms = double(sampled - previous) * 1000.0 / 79800000.0;
    const unsigned b = ms >= 250.0 ? frame_hist_buckets : unsigned(ms * 4.0);
    frame_hist[b].store(frame_hist[b].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
void diagnostics_writer_start();
void motion_trace_write();
}

// Fixed step (user decision 2026-11-14): MACROSS_FIXED_STEP_HZ=60|90|120 (integer multiples of the
// authored 30 Hz). Every presented frame advances the frame clock by exactly 1/hz regardless of the
// measured wall time: an authored tick is exactly hz/30 frames, tick boundaries fall on frame
// boundaries, and frame-time jitter cannot reach the game. When the host cannot hold hz the game
// slows down (like the original below 30 fps). Requires the presentation to be paced at hz.
static unsigned fixed_step_env_hz() {
    static const unsigned hz = [] {
        const char* e = std::getenv("MACROSS_FIXED_STEP_HZ");
        const int v = e ? std::atoi(e) : 0;
        const unsigned r = (v == 60 || v == 90 || v == 120) ? unsigned(v) : 0u;
        if (e) std::fprintf(stderr, "[fixed-step] requested=%s effective_hz=%u (0 = measured wall time) slows_down_below_hz=1\n", e, r);
        return r;
    }();
    return hz;
}

// Automatic 60 -> 30 fallback (user decision 2026-11-14, MACROSS_FIXED_STEP_AUTO30=1 with
// MACROSS_FIXED_STEP_HZ=60): a scene the host cannot run at 60 would slow the game down under the fixed
// 60 Hz step. Instead the frame switches to the authored pace - one 1/30 step per frame, two vblanks -
// and returns to 60 when the frame fits one vblank again with margin.
//  * vblank handler (lifted 00559094): macross_fixed_step_auto_tick() every vblank,
//    macross_fixed_step_auto_signal() when it wakes the frame wait.
//  * frame wait entry (macross_dt_step_frame, game thread): one decision per frame.
//  60 mode: a frame is late when two or more vblanks passed since the previous wait entry; 12 late frames
//  out of the last 30 switch to 30, only after an even number of 1/60 steps (authored tick boundary).
//  30 mode: a frame is light when exactly two vblanks passed and the wait was entered less than 14 ms
//  after the wake signal; after the hold time, 57 light frames out of the last 60 switch back to 60.
//  A fallback within 3 s of a return doubles the hold time (5 s .. 60 s).
namespace {
std::atomic<unsigned> auto_hz{60};
std::atomic<uint64_t> auto_ticks{0}, auto_signal_time{0};
bool fixed_step_auto() {
    static const bool on = [] {
        const char* e = std::getenv("MACROSS_FIXED_STEP_AUTO30");
        const bool r = e && !std::strcmp(e, "1") && fixed_step_env_hz() == 60;
        if (e) std::fprintf(stderr, "[fixed-step-auto] requested=%s enabled=%d late=12/30 light_ms=14 light=57/60 hold_s=5..60\n", e, int(r));
        return r;
    }();
    return on;
}
struct AutoState {
    uint64_t previous_ticks = 0, frames = 0, entered = 0, last_up = 0, switches = 0;
    unsigned steps60 = 0, hold_frames = 150, at = 0;
    bool primed = false, want_down = false;
    unsigned char late[30] = {}, heavy[60] = {};
} auto_state;
void fixed_step_auto_frame() {
    if (!fixed_step_auto()) return;
    auto& a = auto_state;
    const uint64_t ticks = auto_ticks.load(std::memory_order_relaxed), now = ppu_timebase_now();
    const uint64_t passed = ticks - a.previous_ticks; a.previous_ticks = ticks;
    if (!a.primed) { a.primed = true; return; }
    ++a.frames;
    const double since_signal_ms = double(now - auto_signal_time.load(std::memory_order_relaxed)) / 79800.;
    if (auto_hz.load(std::memory_order_relaxed) == 60) {
        a.late[a.at++ % 30] = passed >= 2;
        unsigned n = 0; for (unsigned char x : a.late) n += x;
        if (n >= 12) a.want_down = true;
        if (a.want_down && !(a.steps60 & 1u)) {
            const bool quick = a.last_up && a.frames - a.last_up < 180;
            a.hold_frames = quick ? std::min(a.hold_frames * 2u, 1800u) : 150u;
            auto_hz.store(30, std::memory_order_relaxed);
            a.want_down = false; a.entered = a.frames; a.at = 0; ++a.switches;
            for (auto& x : a.heavy) x = 1;
            std::fprintf(stderr, "[fixed-step-auto] 60->30 frame=%llu late=%u/30 next_return_hold_s=%u\n", (unsigned long long)a.frames, n, a.hold_frames / 30u);
            return;
        }
        ++a.steps60;
    } else {
        a.heavy[a.at++ % 60] = !(passed == 2 && since_signal_ms < 14.0);
        unsigned n = 0; for (unsigned char x : a.heavy) n += x;
        if (a.frames - a.entered >= a.hold_frames && n <= 3) {
            auto_hz.store(60, std::memory_order_relaxed);
            a.steps60 = 0; a.at = 0; a.last_up = a.frames; ++a.switches;
            for (auto& x : a.late) x = 0;
            std::fprintf(stderr, "[fixed-step-auto] 30->60 frame=%llu heavy=%u/60\n", (unsigned long long)a.frames, n);
        }
    }
}
}
extern "C" unsigned macross_fixed_step_auto_vblanks(void) { return fixed_step_auto() && auto_hz.load(std::memory_order_relaxed) == 30 ? 2u : 1u; }
extern "C" void macross_fixed_step_auto_tick(void) { auto_ticks.fetch_add(1, std::memory_order_relaxed); }
extern "C" void macross_fixed_step_auto_signal(void) { auto_signal_time.store(ppu_timebase_now(), std::memory_order_relaxed); }
static unsigned fixed_step_hz() { return fixed_step_auto() ? auto_hz.load(std::memory_order_relaxed) : fixed_step_env_hz(); }
extern "C" bool macross_runtime_has_measured_step(void) {
    return dt_enabled() && !fixed_step_hz();
}

extern "C" void macross_realtime_step_begin(uint64_t thread_id) {
    if (!settings().active()) return; // OFF: no clock, VM reads or TLS writes.
    auto& f=frame;
    if (f.depth) { if (f.owner == thread_id) ++f.depth; return; }
    if (f.owner != thread_id) { f=FrameTime{}; f.owner=thread_id; }
    f.depth=1; f.consumers=0; f.seen[0]=f.seen[1]=f.seen[2]=false;
    f.variable_valid=settings().variable && settings().mode == Caller && variable_guard();
    const uint64_t measured_sample=ppu_timebase_now();
    // fixed step: the frame clock itself advances by exactly 1/hz per presented frame, so every
    // clock derived from it (elapsed, timers, input and presentation clocks) is fixed too
    f.sampled=(fixed_step_hz() && f.previous) ? f.previous + 79800000ull/fixed_step_hz() : measured_sample;
    census_frame.fetch_add(1, std::memory_order_relaxed);
    if (frame_hist_path()) { frame_hist_sample(measured_sample); diagnostics_writer_start(); }
    if (!f.report_at) f.report_at=f.sampled;
    const bool first=!f.previous || f.sampled < f.previous;
    const double raw=first ? 0. : double(f.sampled-f.previous)/frequency;
    f.wall=first ? initial_step : float(raw > maximum_step ? maximum_step : raw);
    f.game = f.presentation = first ? double(initial_step) : std::min(raw, maximum_step);
    f.dt_valid = dt_enabled();
    if (f.dt_valid) {
        if (first) { f.timer_ticks=1; f.timer_remainder=0; }
        else {
            const uint64_t elapsed=f.sampled-f.previous;
            const uint64_t bounded=elapsed > 3990000ull ? 3990000ull : elapsed;
            const uint64_t budget=f.timer_remainder + bounded*30ull;
            f.timer_ticks=unsigned(budget/79800000ull);
            f.timer_remainder=budget%79800000ull;
        }
        publish_elapsed(f.wall);
    }
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
    if (f.dt_valid && std::isfinite(incoming)) {
        if (incoming != double(f.wall)) f.game = incoming; // final guest override, including pause/reverse
        publish_elapsed(float(incoming));
    }
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
    if (f.dt_valid) effective=f.game;
    else if (cfg.variable && (cfg.mode != Caller || !f.variable_valid || !variable_guard())) {
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

// Retained for old generated wait hooks. Sampling now belongs to the scoped
// game frame, before any consumer runs; this wait site must not resample it.
extern "C" void macross_frame_stall(void);
extern "C" void macross_dt_step_frame(void) { fixed_step_auto_frame(); macross_frame_stall(); }

// Read the current frame clock without incrementing the elapsed-consumer statistics.
extern "C" double macross_frame_elapsed_seconds(uint64_t thread) {
    const auto& f = frame;
    if (!f.dt_valid || !f.depth || f.owner != thread ||
        !std::isfinite(f.game) || f.game < 0 || f.game > maximum_guest_step) return 1.0 / 30.0;
    return f.game;
}

extern "C" double macross_main_elapsed_step(uint64_t thread_id, double original) {
    if (!settings().dt) return original;
    auto& f=frame;
    if (!f.dt_valid || !f.depth || f.owner != thread_id) return original;
    ++f.window.main_calls; f.window.main_sum+=double(f.wall);
    return double(f.wall);
}

extern "C" double macross_animation_elapsed_step(uint64_t thread_id,
    double original, double elapsed) {
    if (!settings().dt) return original;
    auto& f=frame;
    if (!f.dt_valid || !f.depth || f.owner != thread_id ||
        !std::isfinite(original) || !std::isfinite(elapsed) || elapsed < 0) return original;
    // Cached fields stay in original 30 Hz asset units. Preserve per-object
    // pause/reverse/speed values and the caller's game-time override.
    const double result=double(float(original * (elapsed / double(initial_step))));
    ++f.window.animation_calls; f.window.animation_sum+=result;
    return result;
}

extern "C" int macross_fixed_timer_ticks(uint64_t thread_id, uint32_t pc) {
    if (!settings().dt) return -1;
    auto& f=frame;
    if (!f.dt_valid || !f.depth || f.owner != thread_id ||
        (pc!=0x00031D28 && pc!=0x00233A70 && pc!=0x00246450)) return -1;
    ++f.window.timer_calls; f.window.timer_ticks+=f.timer_ticks;
    return int(f.timer_ticks);
}

// Per-consumer residuals: an object first called between global asset tick
// boundaries must not inherit another object's phase. Reset on guest restart,
// callback replacement or inactivity. Duplicate calls in one frame do not age it.
namespace {
struct Progress { uint64_t generation=0; uint32_t counter=0, signature=0; double value=0; };
thread_local std::unordered_map<uint64_t, Progress> progress;
double advance(uint64_t thread, uint32_t pc, uint32_t object, uint32_t counter, uint32_t signature) {
    auto& f=frame;
    if (!settings().dt || !f.dt_valid || !f.depth || f.owner!=thread) return -1;
    const uint64_t key=(uint64_t(pc)<<32)|object;
    auto& p=progress[key];
    if (!p.generation || counter!=p.counter || signature!=p.signature || f.generation>p.generation+1) {
        p.value=counter; p.signature=signature;
    }
    if(p.generation!=f.generation) p.value+=std::max(0., f.game)*30.;
    p.generation=f.generation;
    // Float guest dt introduces tiny representational error at boundaries.
    p.counter=uint32_t(std::floor(p.value+1e-6));
    if ((f.generation%600)==0 && progress.size()>4096) {
        for(auto it=progress.begin();it!=progress.end();) {
            if(f.generation>it->second.generation+600) it=progress.erase(it); else ++it;
        }
    }
    return p.value;
}
}
extern "C" int macross_object_timer_ticks(uint64_t thread,uint32_t pc,uint32_t object,uint32_t counter,uint32_t registration) {
    const double value=advance(thread,pc,object,counter,registration);
    return value<0 ? -1 : int(std::floor(value+1e-6)-counter);
}
extern "C" uint32_t macross_fade_counter(uint64_t thread,uint32_t object,uint32_t counter,uint32_t duration) {
    const double value=advance(thread,0x31D28,object,counter,duration ^ vm_read32(object+8) ^ vm_read32(object+16));
    return value<0 ? counter+1 : uint32_t(std::floor(value+1e-6));
}
extern "C" double macross_fade_ratio(uint64_t thread,uint32_t object,double original) {
    if (!settings().dt || !frame.dt_valid || !frame.depth || frame.owner!=thread) return original;
    auto it=progress.find((uint64_t(0x31D28)<<32)|object);
    const uint32_t duration=vm_read32(object+0x14);
    if(it==progress.end() || !duration) return original;
    return std::min(1.,it->second.value/duration);
}

extern "C" uint32_t macross_input_clock(uint64_t thread, uint32_t clock, uint32_t original) {
    const auto& f=frame;
    if (!settings().dt || !f.dt_valid || !f.depth || f.owner!=thread) return original;
    struct InputClock {
        uint64_t owner=0, origin=0, last_sample=0;
        uint32_t base=0, last_original=0;
        bool valid=false;
    };
    thread_local std::unordered_map<uint32_t, InputClock> clocks;
    auto& c=clocks[clock];
    // A wrapped unsigned counter keeps continuity; a guest restart rebases it.
    if (!c.valid || c.owner!=thread || f.sampled<c.last_sample ||
        uint32_t(original-c.last_original)>0x7fffffffu) {
        c.valid=true; c.owner=thread; c.origin=f.sampled; c.base=original;
    }
    c.last_sample=f.sampled; c.last_original=original;
    const uint64_t elapsed=f.sampled-c.origin;
    // Input deadlines must expire across a stall, pause or slow motion, even
    // when the simulation clamps/discards that elapsed time.
    return c.base+uint32_t((elapsed/79800000ull)*30ull+
                           (elapsed%79800000ull)*30ull/79800000ull);
}

extern "C" uint32_t macross_script_counter(uint64_t thread, uint32_t address,
    uint32_t original, int direction) {
    const auto& f=frame;
    if ((direction!=1 && direction!=-1) || !settings().dt || !f.dt_valid ||
        !f.depth || f.owner!=thread || !std::isfinite(f.game) || f.game<0)
        return original+uint32_t(direction);
    struct ScriptCounter {
        uint64_t generation=0, owner=0;
        uint32_t expected=0;
        double remainder=0;
        bool valid=false;
    };
    thread_local std::unordered_map<uint64_t,ScriptCounter> counters;
    auto& c=counters[(uint64_t(address)<<1)|(direction<0)];
    const bool reset=!c.valid || c.owner!=thread || original!=c.expected ||
        f.generation<c.generation;
    if (!reset && c.generation==f.generation) return original;
    if (reset) c.remainder=0;
    // WaitFrame performs an immediate first poll when SetFrame is called.
    // Preserve that setup poll; subsequent yields age by elapsed game time.
    const double budget=(reset && direction<0) ? 1. : c.remainder+f.game*30.;
    const uint32_t ticks=uint32_t(std::floor(budget+1e-6));
    c.remainder=budget-ticks;
    if (c.remainder<0) c.remainder=0;
    c.valid=true;c.owner=thread;c.generation=f.generation;
    c.expected=direction>0 ? original+ticks : original-ticks;
    return c.expected;
}

namespace {
struct UiCounter {
    uint64_t owner=0, generation=0, seen=0;
    uint32_t expected=0, limit=0;
    double value=0, before=0;
    bool valid=false;
};
thread_local std::unordered_map<uint32_t,UiCounter> ui_counters;
thread_local uint64_t ui_collection_generation=0;

UiCounter* ui_counter(uint64_t thread,uint32_t address,uint32_t original,uint32_t limit) {
    const auto& f=frame;
    if (!settings().dt || !f.dt_valid || !f.depth || f.owner!=thread ||
        !std::isfinite(f.presentation) || f.presentation<0 || !limit ||
        limit>0x7fffffffu || original>limit) return nullptr;
    // Completed, abandoned presentation clocks can be reclaimed. Pending clocks
    // retain their fraction across a long native pause or hidden UI interval.
    if (ui_counters.size()>4096 && f.generation%600==0 && ui_collection_generation!=f.generation) {
        ui_collection_generation=f.generation;
        for(auto it=ui_counters.begin();it!=ui_counters.end();) {
            if(it->second.value>=it->second.limit && f.generation>it->second.seen+600)
                it=ui_counters.erase(it);
            else ++it;
        }
    }
    auto& c=ui_counters[address];
    if (!c.valid || c.owner!=thread || c.expected!=original || c.limit!=limit || f.generation<c.generation) {
        c=UiCounter{};c.valid=true;c.owner=thread;c.expected=original;c.limit=limit;c.value=original;
    }
    c.seen=f.generation;
    return &c;
}

double ui_transition_phase(uint64_t thread,uint32_t object,uint32_t original,uint32_t duration) {
    if (!duration || duration>=0x7fffffffu) return -1;
    auto* c=ui_counter(thread,object+0xb8,original,duration+1);
    return c ? (c->generation==frame.generation ? c->before : c->value) : -1;
}
}

extern "C" void macross_ui_counter_reset(uint32_t address) {
    if (settings().dt) ui_counters.erase(address);
}

extern "C" uint32_t macross_ui_counter(uint64_t thread,uint32_t address,
    uint32_t original,uint32_t limit) {
    auto* c=ui_counter(thread,address,original,limit);
    if (!c) return original+1;
    if (c->generation!=frame.generation) {
        // UI animation originally keeps ticking when the game-time override is
        // paused. Use the frame's presentation delta, preserving native UI
        // pause queries and call gating rather than freezing the pause menu.
        c->before=c->value;
        c->value=std::min(double(limit),c->value+frame.presentation*30.);
        c->generation=frame.generation;
        c->expected=uint32_t(std::floor(c->value+1e-6));
    }
    return c->expected;
}

extern "C" uint32_t macross_ui_transition_counter(uint64_t thread,uint32_t object,
    uint32_t original,uint32_t duration) {
    // Zero-duration transitions complete immediately in the original code.
    if (!duration || duration>=0x7fffffffu) return original+1;
    return macross_ui_counter(thread,object+0xb8,original,duration+1);
}

extern "C" uint32_t macross_ui_transition_alpha(uint64_t thread,uint32_t object,
    uint32_t original_counter,uint32_t duration,uint32_t original_alpha) {
    const double phase=ui_transition_phase(thread,object,original_counter,duration);
    if (phase<0) return original_alpha;
    const double alpha=(double(duration)-std::min(phase,double(duration)))*255./duration;
    return uint32_t(std::max(0.,std::min(255.,std::floor(alpha+1e-6))));
}

extern "C" double macross_ui_transition_axis(uint64_t thread,uint32_t object,
    uint32_t original_counter,uint32_t duration,int wide,int axis,double original) {
    const double phase=ui_transition_phase(thread,object,original_counter,duration);
    if (phase<0 || (axis!=0 && axis!=1)) return original;
    // Preserve the authored integer slopes and endpoints at 30 Hz while
    // evaluating them at fractional phases between original asset frames.
    const uint32_t extent=axis ? (wide ? 176u : 376u) : (wide ? 152u : 312u);
    const double movement=std::min(phase,double(duration))*double(extent/duration);
    return axis ? movement-extent : extent-movement;
}

namespace {
// Innermost actor scope per thread. Nested actor updates push their own value.
struct ActorStep { double elapsed[16]; uint64_t serial[16]; uint64_t next_serial = 0; unsigned depth = 0, overflow = 0; };
thread_local ActorStep actor_step;
struct ActorObjectTimer {
    uint64_t generation = 0, scope = 0;
    uint32_t pc = 0, signature = 0, expected = 0;
    double value = 0, before = 0;
    std::vector<uint32_t> events;
};
thread_local std::unordered_map<uint32_t, ActorObjectTimer> actor_object_timers;
struct ActorWindow {
    uint64_t generation = 0, scope = 0;
    uint16_t expected = 0;
    double remaining = 0;
};
thread_local std::unordered_map<uint32_t, ActorWindow> actor_windows;
}

extern "C" void macross_actor_object_timer_reset(uint32_t actor) {
    actor_object_timers.erase(actor);
}

extern "C" int macross_actor_object_timer_ticks(uint64_t thread, uint32_t pc, uint32_t object,
                                                uint32_t counter, uint32_t registration) {
    // These callbacks belong to the actor's post-override elapsed clock.
    // A second original actor update in the same frame owns its own elapsed;
    // a re-entry in the same scope must not accrue that elapsed twice.
    if (!settings().dt || !frame.dt_valid || !frame.depth || frame.owner != thread ||
        !actor_step.depth || actor_step.depth > 16)
        return macross_object_timer_ticks(thread, pc, object, counter, registration);
    const double elapsed = actor_step.elapsed[actor_step.depth - 1];
    if (!std::isfinite(elapsed) || std::abs(elapsed) > maximum_guest_step)
        return macross_object_timer_ticks(thread, pc, object, counter, registration);
    auto& p = actor_object_timers[object];
    const uint64_t scope = actor_step.serial[actor_step.depth - 1];
    if (!p.generation || p.pc != pc || p.signature != registration || p.expected != counter ||
        frame.generation < p.generation) {
        p = ActorObjectTimer{};p.value = counter;p.pc = pc;p.signature = registration;
    }
    if (p.generation != frame.generation || p.scope != scope) {
        p.before = p.value;
        p.value += std::max(0.0, elapsed) * 30.0;
    }
    p.generation = frame.generation;p.scope = scope;
    const double whole = std::floor(p.value + 1e-6);
    p.expected = uint32_t(std::fmod(whole, 4294967296.0));
    if (whole >= 4294967296.0) p.value -= 4294967296.0;
    const int count = int(p.expected - counter);
    if (actor_object_timers.size() > 4096 && frame.generation % 600 == 0) {
        for (auto it = actor_object_timers.begin(); it != actor_object_timers.end();)
            if (frame.generation > it->second.generation + 600) it = actor_object_timers.erase(it); else ++it;
    }
    return count;
}

extern "C" double macross_actor_object_timer_phase(uint64_t thread, uint32_t pc,
                                                   uint32_t actor, uint32_t counter) {
    if (!settings().dt || !frame.dt_valid || !frame.depth || frame.owner != thread ||
        !actor_step.depth || actor_step.depth > 16) return counter;
    const auto it = actor_object_timers.find(actor);
    if (it == actor_object_timers.end()) return counter;
    const auto& p = it->second;
    if (p.pc != pc || p.expected != counter || p.generation != frame.generation ||
        p.scope != actor_step.serial[actor_step.depth - 1]) return counter;
    // Keep authored integer phases exact, including 30 Hz identity.
    const double rounded = std::round(p.value);
    return std::abs(p.value - rounded) < 1e-6 ? rounded : p.value;
}

extern "C" bool macross_actor_object_timer_event(uint64_t thread, uint32_t pc,
                                                  uint32_t actor, uint32_t counter, uint32_t target) {
    if (!settings().dt || !frame.dt_valid || !frame.depth || frame.owner != thread ||
        !actor_step.depth || actor_step.depth > 16) return counter == target;
    const auto it = actor_object_timers.find(actor);
    if (it == actor_object_timers.end()) return counter == target;
    auto& p = it->second;
    if (p.pc != pc || p.expected != counter || p.generation != frame.generation ||
        p.scope != actor_step.serial[actor_step.depth - 1]) return counter == target;
    if (!(p.before < target - 1e-6 && p.value + 1e-6 >= target)) return false;
    if (std::find(p.events.begin(), p.events.end(), target) != p.events.end()) return false;
    p.events.push_back(target);
    return true;
}

extern "C" void macross_actor_window_reset(uint32_t address) {
    actor_windows.erase(address);
}

extern "C" uint16_t macross_actor_window_decrement(uint64_t thread, uint32_t address, uint16_t value) {
    if (!settings().dt || !frame.dt_valid || !frame.depth || frame.owner != thread ||
        !actor_step.depth || actor_step.depth > 16) return uint16_t(value - 1);
    const double elapsed = actor_step.elapsed[actor_step.depth - 1];
    if (!std::isfinite(elapsed) || std::abs(elapsed) > maximum_guest_step) return uint16_t(value - 1);
    if (int16_t(value) <= 0) return value;
    auto& p = actor_windows[address];
    if (!p.generation || p.expected != value || frame.generation < p.generation) {
        p = ActorWindow{};p.remaining = value;
    }
    const uint64_t scope = actor_step.serial[actor_step.depth - 1];
    if (p.generation != frame.generation || p.scope != scope)
        p.remaining = std::max(0.0, p.remaining - std::max(0.0, elapsed) * 30.0);
    p.expected = uint16_t(std::max(0.0, std::ceil(p.remaining - 1e-6)));
    p.generation = frame.generation;p.scope = scope;
    if (actor_windows.size() > 4096 && frame.generation % 600 == 0) {
        for (auto stale = actor_windows.begin(); stale != actor_windows.end();)
            if (frame.generation > stale->second.generation + 600) stale = actor_windows.erase(stale); else ++stale;
    }
    return p.expected;
}

extern "C" void macross_actor_step_push(uint64_t thread, double elapsed) {
    auto& a = actor_step;
    if (a.depth < 16) { a.elapsed[a.depth] = elapsed; a.serial[a.depth] = ++a.next_serial; } else ++a.overflow;
    ++a.depth;
    (void)thread;
}

extern "C" void macross_actor_step_pop(uint64_t thread) {
    auto& a = actor_step;
    if (a.depth) --a.depth;
    if (a.overflow && a.depth >= 16) --a.overflow;
    (void)thread;
}

static bool actor_dt_enabled() {
    // MACROSS_ACTOR_DT=0 keeps every actor-movement hook on the original path
    // (k == 1) for A/B comparison; other delta-time features are unaffected.
    static const bool enabled = [] {
        const char* e = std::getenv("MACROSS_ACTOR_DT");
        const bool on = !e || std::strcmp(e, "0");
        std::fprintf(stderr, "[actor-dt] %s\n", on ? "enabled" : "disabled by MACROSS_ACTOR_DT=0");
        return on;
    }();
    return enabled;
}

namespace {
// A/B diagnostics: MACROSS_ACTOR_DT_OFF=<list> keeps chosen helper groups on
// the original per-call path while the rest of actor delta stays active.
//   ticks (action dispatch), counter (residual frame counters),
//   scaled_int (accumulated integer amounts such as boost heat).
enum : unsigned { off_ticks = 1, off_counter = 2, off_scaled_int = 4 };
unsigned actor_dt_off() {
    static const unsigned mask = [] {
        const char* e = std::getenv("MACROSS_ACTOR_DT_OFF");
        unsigned m = 0;
        if (e) {
            const std::string s(e);
            if (s.find("ticks") != std::string::npos) m |= off_ticks;
            if (s.find("counter") != std::string::npos) m |= off_counter;
            if (s.find("scaled_int") != std::string::npos) m |= off_scaled_int;
            std::fprintf(stderr, "[actor-dt] groups kept original: %s (mask %u)\n", e, m);
        }
        return m;
    }();
    return mask;
}
// MACROSS_ACTOR_DT_TRACE=1: count why k was or was not applied (diagnostics).
struct ActorTrace { uint64_t calls=0, disabled=0, no_frame=0, no_scope=0, invalid=0, applied=0, last=0; double k_sum=0; };
thread_local ActorTrace actor_trace;
bool actor_trace_enabled() {
    static const bool on = [] { const char* e = std::getenv("MACROSS_ACTOR_DT_TRACE"); return e && !std::strcmp(e, "1"); }();
    return on;
}
void actor_trace_report() {
    auto& t = actor_trace;
    if (!frame.sampled || frame.sampled - t.last < uint64_t(frequency) * 2) return;
    t.last = frame.sampled;
    std::fprintf(stderr, "[actor-dt-trace] calls=%llu applied=%llu mean_k=%.4f no_scope=%llu no_frame=%llu invalid=%llu disabled=%llu depth=%u\n",
        (unsigned long long)t.calls, (unsigned long long)t.applied, t.applied ? t.k_sum / t.applied : 0.0,
        (unsigned long long)t.no_scope, (unsigned long long)t.no_frame, (unsigned long long)t.invalid,
        (unsigned long long)t.disabled, actor_step.depth);
    t = ActorTrace{}; t.last = frame.sampled;
}
}

extern "C" float macross_actor_k(uint64_t thread) {
    const auto& f = frame;
    const auto& a = actor_step;
    const bool trace = actor_trace_enabled();
    if (trace) { ++actor_trace.calls; actor_trace_report(); }
    if (!actor_dt_enabled()) { if (trace) ++actor_trace.disabled; return 1.0f; }
    if (!settings().dt || !f.dt_valid || !f.depth || f.owner != thread) { if (trace) ++actor_trace.no_frame; return 1.0f; }
    if (!a.depth || a.depth > 16) { if (trace) ++actor_trace.no_scope; return 1.0f; }
    const double elapsed = a.elapsed[a.depth - 1];
    // The producer already bounds elapsed time (50 ms); reject anything else.
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > maximum_guest_step) { if (trace) ++actor_trace.invalid; return 1.0f; }
    if (trace) { ++actor_trace.applied; actor_trace.k_sum += elapsed * 30.0; }
    return float(elapsed * 30.0);
}

extern "C" float macross_actor_damping(uint64_t thread, float factor) {
    const float k = macross_actor_k(thread);
    if (k == 1.0f || !std::isfinite(factor)) return factor;
    // k > 1 (below 30 FPS) with strong damping would overshoot past zero.
    return std::max(0.0f, 1.0f - k * (1.0f - factor));
}

extern "C" float macross_actor_decay(uint64_t thread, float factor) {
    const float k = macross_actor_k(thread);
    if (k == 1.0f || !std::isfinite(factor) || factor < 0 || factor > 1) return factor;
    if (k == 0.0f) return 1.0f;
    return std::pow(factor, k);
}

extern "C" int32_t macross_actor_scaled_int(uint64_t thread, uint32_t address, int32_t delta) {
    if (actor_dt_off() & off_scaled_int) return delta;
    const float k = macross_actor_k(thread);
    if (k == 1.0f) return delta;
    // Fractions are carried per guest field so the long-run sum equals
    // delta * elapsed * 30 (heating and cooling share one carry).
    struct Carry { uint64_t generation = 0; double value = 0; };
    thread_local std::unordered_map<uint32_t, Carry> carries;
    auto& c = carries[address];
    if (frame.generation > c.generation + 600 || frame.generation < c.generation) c.value = 0;
    c.generation = frame.generation;
    const double v = c.value + double(delta) * double(k);
    const double out = std::trunc(v);
    c.value = v - out;
    if (carries.size() > 4096 && (frame.generation % 600) == 0) {
        for (auto it = carries.begin(); it != carries.end();)
            if (frame.generation > it->second.generation + 600) it = carries.erase(it); else ++it;
    }
    return int32_t(out);
}

extern "C" uint32_t macross_actor_counter(uint64_t thread, uint32_t address, uint32_t original) {
    // Inside an actor update use its elapsed time; otherwise (input pre-pass)
    // the frame's post-override game time. Disabled/outside a frame: +1.
    if (actor_dt_off() & off_counter) return original + 1;
    float k = macross_actor_k(thread);
    const auto& f = frame;
    if (k == 1.0f && !actor_step.depth && actor_dt_enabled() && settings().dt && f.dt_valid &&
        f.depth && f.owner == thread && std::isfinite(f.game) && f.game >= 0 && f.game <= maximum_guest_step)
        k = float(f.game * 30.0);
    if (k == 1.0f) return original + 1;
    struct Counter { uint64_t generation = 0; uint32_t expected = 0; double residual = 0; bool valid = false; };
    thread_local std::unordered_map<uint32_t, Counter> counters;
    auto& c = counters[address];
    if (!c.valid || c.expected != original || frame.generation < c.generation) { c = Counter{}; c.valid = true; }
    // A fresh counter starts with the full original step so the first call
    // after a guest reset (counter := 0) behaves as at 30 Hz.
    const double budget = c.residual + double(k) + (c.generation == 0 ? 1.0 - double(k) : 0.0);
    const uint32_t step = budget >= 1.0 - 1e-6 ? 1u : 0u;
    c.residual = std::min(1.0, std::max(0.0, budget - step));
    c.generation = frame.generation ? frame.generation : 1;
    c.expected = original + step;
    if (counters.size() > 4096 && (frame.generation % 600) == 0) {
        for (auto it = counters.begin(); it != counters.end();)
            if (frame.generation > it->second.generation + 600) it = counters.erase(it); else ++it;
    }
    return c.expected;
}

namespace {
struct ModeClock {
    uint64_t owner = 0, generation = 0, scope = 0;
    uint32_t mode = 0, expected = 0;
    double residual = 0;
};
thread_local std::unordered_map<uint32_t, ModeClock> mode_clocks;
}

extern "C" void macross_actor_mode_reset(uint32_t actor) {
    mode_clocks.erase(actor);
}

extern "C" uint32_t macross_actor_mode_counter(uint64_t thread, uint32_t actor, uint32_t counter) {
    if ((actor_dt_off() & off_counter) || !actor_dt_enabled() || !settings().dt ||
        !frame.dt_valid || !frame.depth || frame.owner != thread ||
        !actor_step.depth || actor_step.depth > 16) return counter + 1;
    const double elapsed = actor_step.elapsed[actor_step.depth - 1];
    if (!std::isfinite(elapsed) || std::abs(elapsed) > maximum_guest_step) return counter + 1;
    const uint32_t mode = vm_read32(actor + 0x4508);
    const uint64_t scope = actor_step.serial[actor_step.depth - 1];
    auto& c = mode_clocks[actor];
    if (!c.generation || c.owner != thread || c.mode != mode || c.expected != counter ||
        frame.generation < c.generation) c = ModeClock{};
    if (c.generation != frame.generation || c.scope != scope) {
        // This field is elapsed time, not a first-call event. Retain fractions
        // across inactive updates and FPS changes; no immediate tick on reset.
        // Reverse time freezes this forward-only mode eligibility threshold.
        const double amount = c.residual + std::max(0.0, elapsed) * 30.0;
        const uint32_t ticks = uint32_t(std::floor(amount + 1e-6));
        c.residual = std::max(0.0, amount - ticks);
        c.expected = counter + ticks;
    }
    c.owner = thread; c.mode = mode; c.generation = frame.generation; c.scope = scope;
    if (mode_clocks.size() > 4096 && frame.generation % 600 == 0) {
        for (auto it = mode_clocks.begin(); it != mode_clocks.end();)
            if (frame.generation > it->second.generation + 600) it = mode_clocks.erase(it); else ++it;
    }
    return c.expected;
}

namespace {
struct CommandClock {
    uint64_t generation = 0, scope = 0;
    uint32_t command = 0, expected = 0, before = 0;
    double residual = 0;
    bool valid = false;
    // Only the three audited equality consumers use this clock. Independent
    // bits let each original event site fire once in the same command epoch.
    unsigned events = 0;
};
thread_local std::unordered_map<uint32_t, CommandClock> command_clocks;
bool command_clock_k(uint64_t thread, double& k) {
    if ((actor_dt_off() & off_counter) || !actor_dt_enabled() || !settings().dt ||
        !frame.dt_valid || !frame.depth || frame.owner != thread || !actor_step.depth || actor_step.depth > 16)
        return false;
    const double elapsed = actor_step.elapsed[actor_step.depth - 1];
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > maximum_guest_step) return false;
    k = elapsed * 30.0;
    return true;
}
}

extern "C" void macross_actor_command_reset(uint32_t actor) {
    command_clocks.erase(actor);
}

extern "C" uint32_t macross_actor_command_counter(uint64_t thread, uint32_t actor, uint32_t original_result) {
    double k;
    if (!command_clock_k(thread, k)) return original_result;
    const uint32_t command = vm_read32(actor + 0x44F0);
    const uint32_t previous = vm_read32(actor + 0x44F4);
    const uint32_t original = vm_read32(actor + 0x44F8);
    auto& c = command_clocks[actor];
    const uint64_t scope = actor_step.serial[actor_step.depth - 1];
    if (c.valid && c.command == command && c.expected == original && c.scope == scope &&
        c.generation == frame.generation) return c.expected;
    if (!c.valid || c.command != command || c.expected != original || frame.generation < c.generation ||
        frame.generation > c.generation + 600) c = CommandClock{};
    c.valid = true; c.command = command; c.generation = frame.generation; c.scope = scope;
    c.before = original;
    if (command != previous) {
        // Preserve the original zero on a command transition, but do not
        // create a new epoch again if the controller is re-entered unchanged.
        c.residual = 0; c.before = 0; c.expected = 0;
    } else {
        const double amount = c.residual + k;
        const uint32_t steps = uint32_t(std::floor(amount + 1e-6));
        c.residual = std::max(0.0, amount - steps);
        c.expected = original + steps;
    }
    if (command_clocks.size() > 4096 && frame.generation % 600 == 0) {
        for (auto it = command_clocks.begin(); it != command_clocks.end();)
            if (frame.generation > it->second.generation + 600) it = command_clocks.erase(it); else ++it;
    }
    return c.expected;
}

extern "C" bool macross_actor_command_event(uint64_t thread, uint32_t actor, uint32_t consumer, uint32_t target) {
    const uint32_t current = vm_read32(actor + 0x44F8);
    double k;
    if (!command_clock_k(thread, k)) return current == target;
    // A paused update must not re-emit an event held at an integer boundary.
    if (k == 0) return false;
    const unsigned bit = consumer == 0x002768B8 ? 1u : consumer == 0x002A5884 ? 2u :
                         consumer == 0x002AC324 ? 4u : 0u;
    if (!bit) return current == target;
    auto& c = command_clocks[actor];
    const uint32_t command = vm_read32(actor + 0x44F0);
    // Some action handlers reach these consumers without the command producer.
    // Track that entry explicitly too; an active delta frame must never fall
    // back to repeating an equality event simply because its clock is new.
    if (!c.valid || c.command != command || current < c.expected || frame.generation < c.generation ||
        frame.generation > c.generation + 600) c = CommandClock{};
    if (!c.valid) { c.valid = true; c.command = command; c.before = current; c.expected = current; }
    else if (c.expected != current) { c.before = c.expected; c.expected = current; }
    c.generation = frame.generation;
    const bool crossed = target != 0 && c.before < target && current >= target;
    if ((current != target && !crossed) || (c.events & bit)) return false;
    c.events |= bit;
    return true;
}

extern "C" int macross_actor_ticks(uint64_t thread, uint32_t pc, uint32_t actor) {
    (void)pc;
    if (actor_dt_off() & off_ticks) return -1;
    // Inactive (disabled, outside a frame or actor scope): -1, call once.
    const float k = macross_actor_k(thread);
    if (k == 1.0f && (!actor_dt_enabled() || !settings().dt || !frame.dt_valid || !frame.depth ||
                      frame.owner != thread || !actor_step.depth || actor_step.depth > 16)) return -1;
    const uint64_t scope = actor_step.serial[actor_step.depth - 1];
    struct Phase { uint64_t generation = 0, scope = 0; double value = 0; int ticks = 0; };
    thread_local std::unordered_map<uint32_t, Phase> phases;
    auto& p = phases[actor];
    if (p.generation && p.scope == scope) return p.ticks;   // re-entry within one update
    if (!p.generation || frame.generation > p.generation + 1 || frame.generation < p.generation)
        p.value = k > 0 ? 1.0 - double(k) : 0.0;
    p.generation = frame.generation; p.scope = scope;
    p.value += double(k);
    const int ticks = int(std::floor(p.value + 1e-6));
    p.value = std::max(0.0, p.value - ticks);
    p.ticks = std::min(ticks, 2);
    if (phases.size() > 4096 && (frame.generation % 600) == 0) {
        for (auto it = phases.begin(); it != phases.end();)
            if (frame.generation > it->second.generation + 600) it = phases.erase(it); else ++it;
    }
    return p.ticks;
}

namespace {
// Diagnostics only (MACROSS_SITE_CENSUS=<prefix>). Per (site, address): hits,
// frames with a hit, frames with 2+ hits, and the longest run of consecutive
// completed frames with exactly one hit. Each thread owns one preallocated
// open-addressing table (no allocation, lock, clock read or I/O per hit); a
// background thread snapshots the published counters every few seconds.
// Cells that do not fit are counted as dropped, never merged.
constexpr uint32_t census_capacity = 1u << 18, census_probe = 32, census_max_threads = 64;
struct CensusCell {
    std::atomic<uint64_t> key{0};                         // (site << 32 | address) + 1; 0 = empty
    std::atomic<uint64_t> hits{0}, frames{0}, multi{0}, best_run{0};
    uint64_t last = ~0ull, run_end = ~0ull, run = 0; uint32_t cur = 0;   // owner thread only
};
struct CensusTable {
    CensusCell cells[census_capacity];
    std::atomic<uint64_t> dropped{0};
    unsigned id = 0;
};
const char* census_path() {
    static const char* p = [] { const char* e = std::getenv("MACROSS_SITE_CENSUS"); return (e && *e) ? e : nullptr; }();
    return p;
}
std::atomic<CensusTable*> census_tables[census_max_threads];
std::atomic<unsigned> census_threads{0};
std::atomic<uint64_t> census_lost_threads{0};
thread_local CensusTable* census_local = nullptr;
thread_local bool census_failed = false;
inline void census_store(std::atomic<uint64_t>& a, uint64_t v) { a.store(v, std::memory_order_relaxed); }
inline uint64_t census_load(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }

void census_write_snapshot() {
    const auto started = std::chrono::steady_clock::now();
    uint64_t cells_written = 0;
    const unsigned threads = std::min(census_threads.load(), census_max_threads);
    for (unsigned i = 0; i < threads; ++i) {
        CensusTable* t = census_tables[i].load(std::memory_order_acquire);
        if (!t) continue;
        char name[1024], temp[1040];
        std::snprintf(name, sizeof name, "%s.%u.json", census_path(), t->id);
        std::snprintf(temp, sizeof temp, "%s.tmp", name);
        FILE* f = std::fopen(temp, "wb");
        if (!f) continue;
        std::fprintf(f, "{\"frame\":%llu,\"dropped\":%llu,\"capacity\":%u,\"cells\":[",
            (unsigned long long)census_frame.load(), (unsigned long long)census_load(t->dropped), census_capacity);
        bool first = true;
        for (const auto& c : t->cells) {
            const uint64_t key = census_load(c.key);
            if (!key) continue;
            std::fprintf(f, "%s[%u,%u,%llu,%llu,%llu,%llu]", first ? "" : ",", unsigned((key - 1) >> 32), unsigned(key - 1),
                (unsigned long long)census_load(c.hits), (unsigned long long)census_load(c.frames),
                (unsigned long long)census_load(c.multi), (unsigned long long)census_load(c.best_run));
            first = false; ++cells_written;
        }
        std::fprintf(f, "]}\n");
        std::fclose(f);
        std::remove(name); std::rename(temp, name);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    char name[1024];
    std::snprintf(name, sizeof name, "%s.meta.json", census_path());
    if (FILE* f = std::fopen(name, "wb")) {
        std::fprintf(f, "{\"frame\":%llu,\"threads\":%u,\"lost_threads\":%llu,\"cells\":%llu,\"table_bytes\":%llu,\"dump_ms\":%.3f}\n",
            (unsigned long long)census_frame.load(), threads, (unsigned long long)census_lost_threads.load(),
            (unsigned long long)cells_written, (unsigned long long)sizeof(CensusTable) * threads, ms);
        std::fclose(f);
    }
}
void frame_hist_write() {
    char name[1024], temp[1040];
    std::snprintf(name, sizeof name, "%s.json", frame_hist_path());
    std::snprintf(temp, sizeof temp, "%s.tmp", name);
    FILE* f = std::fopen(temp, "wb");
    if (!f) return;
    std::fprintf(f, "{\"bucket_ms\":0.25,\"frame\":%llu,\"census\":%s,\"counts\":[",
        (unsigned long long)census_frame.load(), census_path() ? "true" : "false");
    for (unsigned i = 0; i <= frame_hist_buckets; ++i)
        std::fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)frame_hist[i].load(std::memory_order_relaxed));
    std::fprintf(f, "]}\n");
    std::fclose(f);
    std::remove(name); std::rename(temp, name);
}
void diagnostics_writer_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (census_path()) census_write_snapshot();
                if (frame_hist_path()) frame_hist_write();
                motion_trace_write();
            }
        }).detach();
    });
}
CensusTable* census_table() {
    if (census_local || census_failed) return census_local;
    const unsigned slot = census_threads.fetch_add(1);
    if (slot >= census_max_threads) { census_failed = true; census_lost_threads.fetch_add(1); return nullptr; }
    // One allocation per thread, on its first instrumented store.
    census_local = new (std::nothrow) CensusTable;
    if (!census_local) { census_failed = true; census_lost_threads.fetch_add(1); return nullptr; }
    census_local->id = slot + 1;
    census_tables[slot].store(census_local, std::memory_order_release);
    diagnostics_writer_start();
    return census_local;
}
}

extern "C" void macross_site_hit(uint32_t site, uint32_t address) {
    if (!census_path()) return;
    CensusTable* t = census_table();
    if (!t) return;
    const uint64_t now = census_frame.load(std::memory_order_relaxed);
    const uint64_t key = ((uint64_t(site) << 32) | address) + 1;
    const uint64_t h = (key * 0x9E3779B97F4A7C15ull) >> 46;
    CensusCell* c = nullptr;
    for (uint32_t i = 0; i < census_probe; ++i) {
        CensusCell& x = t->cells[(h + i) & (census_capacity - 1)];
        const uint64_t k = census_load(x.key);
        if (k == key) { c = &x; break; }
        if (!k) { census_store(x.key, key); c = &x; break; }
    }
    if (!c) { census_store(t->dropped, census_load(t->dropped) + 1); return; }
    census_store(c->hits, census_load(c->hits) + 1);
    if (c->last == now) {
        if (++c->cur == 2) census_store(c->multi, census_load(c->multi) + 1);
        return;
    }
    // Commit the previous frame of this cell; it is complete once a later
    // frame hits. The current frame is never counted in best_run.
    if (c->last != ~0ull) {
        if (c->cur == 1) {
            c->run = (c->run_end != ~0ull && c->run_end + 1 == c->last) ? c->run + 1 : 1;
            c->run_end = c->last;
            if (c->run > census_load(c->best_run)) census_store(c->best_run, c->run);
        } else { c->run = 0; c->run_end = ~0ull; }
    }
    c->last = now; c->cur = 1;
    census_store(c->frames, census_load(c->frames) + 1);
}

namespace {
// Append-only publication: the writer never observes an unfinished row and the
// producer never overwrites a published row. Full buffers drop explicitly.
// No per-probe lock, file I/O, allocation, clock sample or guest state write.
struct FollowOperands {
    uint64_t generation=0;
    uint32_t field=0, status=0;
    float current[4]{}, target[4]{}, alpha[4]{}, difference[4]{}, addend[4]{}, result[4]{};
};
thread_local FollowOperands follow_operands;
struct MotionRow {
    uint64_t frame, sampled, thread, scope;
    double wall, game, actor_elapsed;
    uint32_t site, actor, small, input, held, form, state, command, mode, phase;
    float position[3], velocity[3], tilt[3], angular[3];
    uint32_t camera_entry, camera_counter;
    float camera0[3], camera10[3];
    FollowOperands follow;
};
thread_local uint32_t motion_camera_manager = 0;
constexpr unsigned motion_capacity = 1u << 18;
struct MotionTrace {
    MotionRow rows[motion_capacity];
    std::atomic<unsigned> count{0};
    std::atomic<uint64_t> dropped{0};
    unsigned written = 0;
    FILE* output = nullptr;
    uint64_t owner = 0;
};
std::atomic<MotionTrace*> motion_trace{nullptr};
const char* motion_trace_path() {
    static const char* path = [] { const char* p = std::getenv("MACROSS_MOTION_TRACE"); return p && *p ? p : nullptr; }();
    return path;
}
float motion_float(uint32_t address) {
    const uint32_t bits = vm_read32(address); float value;
    std::memcpy(&value, &bits, sizeof value); return value;
}
void motion_trace_write() {
    MotionTrace* t = motion_trace.load(std::memory_order_acquire);
    if (!t) return;
    if (!t->output) {
        t->output = std::fopen(motion_trace_path(), "wb");
        if (!t->output) return;
        std::fputs("frame,sampled,thread,scope,wall,game,actor_elapsed,site,actor,small,input,held,form,state,command,mode,phase,x,y,z,vx,vy,vz,tiltx,tilty,tiltz,angularx,angulary,angularz,camera_entry,camera_counter,camera0x,camera0y,camera0z,camera10x,camera10y,camera10z,follow_generation,follow_field,follow_status", t->output);
        for (const char* name:{"follow_current","follow_target","follow_alpha","follow_difference","follow_addend","follow_result"})
            for(const char* lane:{"x","y","z","w"})std::fprintf(t->output,",%s%s",name,lane);
        std::fputc('\n',t->output);
    }
    const unsigned count = t->count.load(std::memory_order_acquire);
    for (; t->written < count; ++t->written) {
        const auto& r = t->rows[t->written];
        std::fprintf(t->output, "%llu,%llu,%llu,%llu,%.9f,%.9f,%.9f,%08X,%08X,%08X,%08X,%08X,%u,%u,%u,%u,%u",
            (unsigned long long)r.frame, (unsigned long long)r.sampled,
            (unsigned long long)r.thread, (unsigned long long)r.scope,
            r.wall, r.game, r.actor_elapsed, r.site, r.actor, r.small, r.input, r.held,
            r.form, r.state, r.command, r.mode, r.phase);
        for (const float* vector : {r.position, r.velocity, r.tilt, r.angular})
            for (unsigned lane = 0; lane < 3; ++lane) std::fprintf(t->output, ",%.9g", double(vector[lane]));
        std::fprintf(t->output, ",%08X,%u", r.camera_entry, r.camera_counter);
        for (const float* vector : {r.camera0, r.camera10})
            for (unsigned lane = 0; lane < 3; ++lane) std::fprintf(t->output, ",%.9g", double(vector[lane]));
        std::fprintf(t->output,",%llu,%08X,%u",(unsigned long long)r.follow.generation,r.follow.field,r.follow.status);
        for(const float* vector:{r.follow.current,r.follow.target,r.follow.alpha,r.follow.difference,r.follow.addend,r.follow.result})
            for(unsigned lane=0;lane<4;++lane)std::fprintf(t->output,",%.9g",double(vector[lane]));
        std::fputc('\n', t->output);
    }
    std::fflush(t->output);
    char meta[1200]; std::snprintf(meta, sizeof meta, "%s.meta.json", motion_trace_path());
    if (FILE* f = std::fopen(meta, "wb")) {
        std::fprintf(f, "{\"published\":%u,\"written\":%u,\"capacity\":%u,\"dropped\":%llu}\n",
            count, t->written, motion_capacity, (unsigned long long)t->dropped.load());
        std::fclose(f);
    }
}
}

extern "C" void macross_nan_trap_probe(uint64_t thread, uint32_t site, uint32_t actor);
extern "C" void macross_actor_motion_probe(uint64_t thread, uint32_t site, uint32_t actor) {
    macross_nan_trap_probe(thread, site, actor);
    if (!motion_trace_path() || !frame.depth || frame.owner != thread || !actor) return;
    const uint32_t input = vm_read32(actor + 0x30);
    if (!input) return;
    const uint32_t small = vm_read32(actor);
    if (!small) return;
    MotionTrace* t = motion_trace.load(std::memory_order_acquire);
    if (!t) {
        auto* created = new (std::nothrow) MotionTrace;
        if (!created) return;
        created->owner = thread;
        if (!motion_trace.compare_exchange_strong(t, created, std::memory_order_release, std::memory_order_acquire)) delete created;
        else { t = created; diagnostics_writer_start(); }
    }
    if (t->owner != thread) return; // one game-frame producer owns this stream
    const unsigned index = t->count.load(std::memory_order_relaxed);
    if (index == motion_capacity) { t->dropped.fetch_add(1, std::memory_order_relaxed); return; }
    MotionRow& row = t->rows[index];
    row.frame = frame.generation; row.sampled = frame.sampled; row.thread = thread;
    row.scope = actor_step.depth && actor_step.depth <= 16 ? actor_step.serial[actor_step.depth - 1] : 0;
    row.actor_elapsed = actor_step.depth && actor_step.depth <= 16 ? actor_step.elapsed[actor_step.depth - 1] : 0;
    row.wall = frame.wall; row.game = frame.game; row.site = site;
    row.follow = follow_operands.generation==frame.generation ? follow_operands : FollowOperands{};
    row.actor = actor; row.small = small; row.input = input; row.held = vm_read32(input);
    row.form = vm_read32(actor + 0x44E4); row.state = vm_read32(actor + 0x44E8);
    row.command = vm_read32(actor + 0x44F0); row.mode = vm_read32(actor + 0x4508);
    row.phase = vm_read32(actor + 0x4FC8);
    for (unsigned lane = 0; lane < 3; ++lane) {
        row.position[lane] = motion_float(small + 0x50 + lane*4);
        row.velocity[lane] = motion_float(small + 0x70 + lane*4);
        row.tilt[lane] = motion_float(small + 0x80 + lane*4);
        row.angular[lane] = motion_float(small + 0xB0 + lane*4);
        row.camera0[lane] = row.camera10[lane] = 0;
    }
    row.camera_entry = row.camera_counter = 0;
    if (motion_camera_manager) {
        const uint32_t count = vm_read32(motion_camera_manager);
        const uint32_t entries = vm_read32(motion_camera_manager + 4);
        if (entries && count <= 2) for (unsigned i = 0; i < count; ++i) {
            const uint32_t entry = entries + i*0x100;
            if (vm_read32(entry + 0x4C) != actor) continue;
            row.camera_entry = entry; row.camera_counter = vm_read32(entry + 0x50);
            for (unsigned lane = 0; lane < 3; ++lane) {
                row.camera0[lane] = motion_float(entry + lane*4);
                row.camera10[lane] = motion_float(entry + 0x10 + lane*4);
            }
            break;
        }
    }
    t->count.store(index + 1, std::memory_order_release);
}

extern "C" void macross_camera_motion_probe(uint64_t thread, uint32_t site, uint32_t manager) {
    if (!motion_trace_path() || !frame.depth || frame.owner != thread || !manager) return;
    motion_camera_manager = manager;
    const uint32_t count = vm_read32(manager), entries = vm_read32(manager + 4);
    if (!entries || count > 2) return;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t actor = vm_read32(entries + i*0x100 + 0x4C);
        if (actor) macross_actor_motion_probe(thread, site, actor);
    }
}

extern "C" void macross_camera_follow_probe(uint64_t thread,uint32_t field,unsigned status,
    const float* current,const float* target,const float* alpha,const float* difference,
    const float* addend,const float* result) {
    if(!motion_trace_path() || !frame.depth || frame.owner!=thread)return;
    follow_operands.generation=frame.generation;follow_operands.field=field;follow_operands.status=status;
    std::memcpy(follow_operands.current,current,16);std::memcpy(follow_operands.target,target,16);
    std::memcpy(follow_operands.alpha,alpha,16);std::memcpy(follow_operands.difference,difference,16);
    std::memcpy(follow_operands.addend,addend,16);std::memcpy(follow_operands.result,result,16);
}

extern "C" void macross_camera_stage_probe(uint64_t thread,uint32_t site,uint32_t entry) {
    if(!motion_trace_path() || !frame.depth || frame.owner!=thread || !entry)return;
    macross_actor_motion_probe(thread,site,vm_read32(entry+0x4C));
}

namespace {
struct CameraFollow {
    uint64_t generation=0;
    float expected[4]{}, target[4]{}, alpha[4]{};
    bool temporal=false;
};
thread_local std::unordered_map<uint32_t,CameraFollow> camera_follow;
bool camera_delta_enabled() {
    static const bool enabled=[] { const char* p=std::getenv("MACROSS_CAMERA_DT"); return !p || std::strcmp(p,"0")!=0; }();
    return enabled;
}
}
extern "C" void macross_camera_follow_reset() { camera_follow.clear(); }

extern "C" bool macross_camera_follow_step(uint64_t thread, uint32_t field,
    const float* current, const float* target, const float* alpha, float* result, unsigned lanes) {
    if (!camera_delta_enabled() || !settings().dt || !frame.dt_valid ||
        !frame.depth || frame.owner!=thread) return false;
    const double k=frame.presentation*30.;
    if (!std::isfinite(k) || k<0) return false;
    auto& history=camera_follow[field];
    const bool continuous=history.temporal && history.generation+1==frame.generation;
    const bool original_tick=std::abs(k-1.)<1e-6;
    // Fractional power of the ORIGINAL coupled map:
    // target' = target + velocity; camera' = r*camera + (1-r)*target'.
    // q=r^k, w=(1-q)/(1-r); camera' = target' + q*(camera-target)
    //                                      - r*w*velocity.
    // The velocity term is essential: alpha^k alone changes steady lag with
    // frame partition when the target moves. Measured target samples provide
    // its piecewise-linear trajectory; no guest velocity unit is guessed.
    bool changed=false;
    for (int lane=0;lane<4;++lane) {
        const float old=current[lane], t=target[lane], a=alpha[lane];
        const bool valid=((lanes>>lane)&1u) && std::isfinite(old) && std::isfinite(t) &&
            std::isfinite(a) && std::isfinite(result[lane]) && a>=0 && a<=1;
        if (valid && !original_tick) {
            double next=old;
            if (k>0 && a==1) next=t;
            else if (k>0 && a>0) {
                const double r=1.-a, blend=-std::expm1(std::log1p(-double(a))*k), q=1.-blend;
                next=double(old)+blend*(double(t)-old);
                if (continuous && history.expected[lane]==old && history.alpha[lane]==a)
                    next+=(double(t)-history.target[lane])*(q-r*(blend/double(a))/k);
            }
            if (std::isfinite(next)) { result[lane]=float(next); changed=true; }
        }
        history.expected[lane]=result[lane];history.target[lane]=t;history.alpha[lane]=a;
    }
    if (!history.generation && motion_trace_path())
        std::fprintf(stderr,"[camera-follow-dt] field=%08X alpha=%.6g,%.6g,%.6g presentation_k=%.6g coupled_target=1\n",
            field,double(alpha[0]),double(alpha[1]),double(alpha[2]),k);
    history.generation=frame.generation;history.temporal=k>0;
    if (camera_follow.size()>512 && frame.generation%600==0)
        for (auto it=camera_follow.begin();it!=camera_follow.end();)
            if (frame.generation>it->second.generation+600) it=camera_follow.erase(it); else ++it;
    return changed;
}
