/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"
#include "l10n_bridge.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellVideoOut_set_resolution(uint8_t resolution_id);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
void     cellGcm_fifo_kick_wait(unsigned ms);
int      cellGcm_fifo_kick_wait_gpu(unsigned ms, void* gpu_event);
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" void rsx_d3d12_backend_present_completed(void);
extern "C" uint64_t rsx_d3d12_backend_present_count(void);
extern "C" int rsx_d3d12_guest_read(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" void rsx_d3d12_guest_wrote(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" void cellGcmSetLegacyTransferHooks(
    int (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t),
    void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720
static unsigned g_output_width = WINDOW_WIDTH, g_output_height = WINDOW_HEIGHT;
static uint32_t g_video_resolution_id = 2;

extern "C" uint32_t macross_internal_video_resolution_id(uint32_t original_id, uint32_t pc)
{
    if (g_video_resolution_id != 4 || original_id != 2 || pc != 0x0055AB68u) return original_id;
    const auto word = [](uint32_t address) {
        const uint8_t* p = vm_base + address;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    const bool guarded = vm_base && word(0x0055AB64u) == 0x880100ACu &&
        word(0x0055AB68u) == 0x38600002u && word(0x0055AB6Cu) == 0x38810074u &&
        word(0x0055AB70u) == 0x68000002u && word(0x0055AB80u) == 0x48381A2Du;
    static bool reported = false;
    if (!reported) {
        fprintf(stderr, "[video-mode] internal_reference pc=%08X guard=%s requested=%u selected=%u\n",
            pc, guarded ? "pass" : "fail", original_id, guarded ? 4u : original_id);
        reported = true;
    }
    return guarded ? 4u : original_id;
}

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;
extern "C" int rsx_live_draw_init(void*, uint32_t, uint32_t,
    const uint8_t* (*)(void*, uint32_t, uint32_t, uint32_t), void*);
extern "C" void rsx_live_draw_present(uint32_t);
extern "C" uint32_t rsx_live_draw_get_frames(void);
extern "C" int rsx_live_draw_async_enabled(void);
extern "C" void* rsx_live_draw_gpu_completion_event(void);
extern "C" void rsx_live_draw_set_present_request(uint32_t);
extern "C" int rsx_live_draw_take_gpu_completion(uint32_t*);
extern "C" int cellGcm_fifo_caught_up(void);
extern "C" int cellGcmSetAsyncCompletionMode(int);
extern "C" int cellGcmTakePresentRequest(uint32_t*,uint32_t*);
extern "C" int cellGcmNotifyGpuFlipComplete(uint32_t);
extern "C" int cellGcmAsyncCompletionHealthy(void);

extern "C" int rsx_null_backend_init(uint32_t, uint32_t, const char*);
extern "C" int rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void rsx_null_backend_suppress_present(int);
extern "C" uint32_t cellGcmResolveLocated(uint32_t, uint32_t);
extern "C" uint32_t cellGcmResolveIO(uint32_t);
extern "C" uint32_t cellGcmGetCurrentDisplayBufferId(void);
extern "C" uint32_t cellGcmReachedFlipCount(void);
static bool g_live_renderer = false;
static const uint8_t* live_guest_ptr(void*, uint32_t location,
                                    uint32_t offset, uint32_t bytes)
{
    uint32_t ea = location == 0 ? cellGcmResolveLocated(1, offset)
                                : cellGcmResolveIO(offset);
    if (!ea && location != 0) ea = cellGcmResolveLocated(0, offset);
    if (!vm_base || !ea || ea == UINT32_MAX ||
        uint64_t(ea) + bytes > 0x100000000ull) return nullptr;
    return reinterpret_cast<const uint8_t*>(vm_base) + ea;
}
extern "C" void ppu_gcm_pump(void);
extern "C" void ppu_set_callback_stack(uint32_t);

/* Opt-in independent VBlank publication. Rendering/flip completion stay on the
 * original thread; this worker never executes guest callbacks or touches RSX.
 * Pending GCM bits can still coalesce notifications. Late periods are skipped,
 * never replayed as a burst of fabricated callbacks, and reported separately. */
#ifdef _WIN32
static std::atomic<bool> s_independent_vblank_active{false};
struct MacrossVBlankClock {
    HANDLE timer;
    LONGLONG frequency;
    LONGLONG origin;
};
static DWORD WINAPI independent_vblank_clock(LPVOID opaque)
{
    MacrossVBlankClock* clock = static_cast<MacrossVBlankClock*>(opaque);
    uint64_t beat = 1, published = 0, skipped = 0;
    LONGLONG max_late = 0;
    for (;;) {
        LARGE_INTEGER now;
        if (!QueryPerformanceCounter(&now)) break;
        const LONGLONG deadline = clock->origin + LONGLONG(beat * uint64_t(clock->frequency) / 60u);
        if (now.QuadPart < deadline) {
            LARGE_INTEGER due;
            due.QuadPart = -((deadline - now.QuadPart) * 10000000ll / clock->frequency);
            if (!due.QuadPart) due.QuadPart = -1;
            if (!SetWaitableTimer(clock->timer, &due, 0, nullptr, nullptr, FALSE) ||
                WaitForSingleObject(clock->timer, INFINITE) != WAIT_OBJECT_0) break;
            continue;
        }
        const LONGLONG late = now.QuadPart - deadline;
        if (late > max_late) max_late = late;
        cellGcmTickVBlank();
        ++published;
        const uint64_t next = uint64_t(now.QuadPart - clock->origin) * 60u / uint64_t(clock->frequency) + 1u;
        if (next > beat + 1u) skipped += next - beat - 1u;
        beat = next > beat ? next : beat + 1u;
        if (published <= 4u || published % 600u == 0u) {
            fprintf(stderr, "[vblank-clock] published=%llu skipped_periods=%llu max_late_ms=%.6f callback_coalescing_unresolved=1\n",
                    (unsigned long long)published, (unsigned long long)skipped,
                    double(max_late) * 1000.0 / double(clock->frequency));
        }
    }
    fprintf(stderr, "[vblank-clock] timer failed; returning publication to legacy frame thread\n");
    s_independent_vblank_active.store(false, std::memory_order_release);
    CloseHandle(clock->timer);
    free(clock);
    return 0;
}
static void start_independent_vblank_clock(void)
{
    const char* enabled = getenv("MACROSS_VBLANK_INDEPENDENT");
    if (!enabled || strcmp(enabled, "1")) return;
    LARGE_INTEGER frequency, origin;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&origin)) return;
    /* Windows 10 high-resolution timer when available; ordinary waitable timer
     * otherwise. No global timer-resolution change and no spin loop. */
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002u, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) return;
    MacrossVBlankClock* clock = static_cast<MacrossVBlankClock*>(malloc(sizeof(MacrossVBlankClock)));
    if (!clock) { CloseHandle(timer); return; }
    *clock = {timer, frequency.QuadPart, origin.QuadPart};
    s_independent_vblank_active.store(true, std::memory_order_release);
    HANDLE worker = CreateThread(nullptr, 0, independent_vblank_clock, clock, 0, nullptr);
    if (!worker) {
        s_independent_vblank_active.store(false, std::memory_order_release);
        CloseHandle(timer);
        free(clock);
        return;
    }
    CloseHandle(worker);
    fprintf(stderr, "[vblank-clock] independent publication enabled exact_hz=60 flips_and_FIFO_unchanged=1\n");
}
static void publish_legacy_vblank(void)
{
    if (!s_independent_vblank_active.load(std::memory_order_acquire)) cellGcmTickVBlank();
}
#else
static void start_independent_vblank_clock(void) {}
static void publish_legacy_vblank(void) { cellGcmTickVBlank(); }
#endif

static DWORD WINAPI guest_callback_clock(LPVOID)
{
    // Interrupt callbacks must run while ordinary PPU threads block on real
    // conditions. Keep this separate from the RSX drain and its scratch stack
    // separate from callbacks invoked on the main guest thread.
    for (;;) { Sleep(1); ppu_gcm_pump(); }
}

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* allow_new: take a new present request. Only the vblank tick does: the
 * hardware scans a flipped buffer out at the next vblank, and this title
 * keeps drawing its HUD into the buffer after the flip command until then. */
static void present_guest_frame(bool allow_new = true)
{
    if (g_live_renderer) {
        if(rsx_live_draw_async_enabled()){
            if(!cellGcmAsyncCompletionHealthy())abort();
            uint32_t seq,buffer;int rc;
            int completed = 0;
            while((rc=rsx_live_draw_take_gpu_completion(&seq))>0){
                if(cellGcmNotifyGpuFlipComplete(seq)<0)abort();
                InterlockedIncrement(&g_frames_presented);
                completed = 1;
            }
            if(rc<0)abort();
            /* The flip was presented at its vblank and the GPU has finished
             * it: complete it now instead of at the next 16 ms tick, which
             * added a whole tick to every frame the guest waits on.
             * LD_ASYNC_FLIP_ON_GPU=0: complete at the tick only. */
            { static int on = -1;
              if (on < 0) { const char* e = getenv("LD_ASYNC_FLIP_ON_GPU"); on = e ? atoi(e) : 1; }
              if (completed && on) cellGcmTickFlip(); }
            static bool held = false; static uint32_t held_buf = 0, held_seq = 0; static ULONGLONG held_at = 0;
            /* Release a held flip as soon as the FIFO walker has consumed all
             * the guest has written -- the post-flip HUD draws are then in. */
            { static int early = -1;
              if (early < 0) { const char* e = getenv("LD_ASYNC_PRESENT_EARLY"); early = e ? atoi(e) : 1; }
              if (early && held && cellGcm_fifo_caught_up()) {
                  rsx_live_draw_set_present_request(held_seq);rsx_live_draw_present(held_buf);held=false;
              } }
            /* LD_ASYNC_PRESENT_ANYTIME (default 1, with no lag): present a
             * reached flip on any loop pass, not only at the 16 ms tick --
             * the guest polls the flip status, so waiting for the tick added
             * ~8 ms to every frame. */
            static int anytime = -1;
            if (anytime < 0) { const char* e = getenv("LD_ASYNC_PRESENT_ANYTIME"); anytime = e ? atoi(e) : 1; }
            static int lag0 = -1;
            if (lag0 < 0) { const char* e = getenv("LD_ASYNC_PRESENT_LAG"); lag0 = e ? atoi(e) : 0; }
            if(!allow_new && !(anytime && !lag0))return;
            /* Present one flip late: this title keeps drawing into a buffer
             * after its flip command, so a buffer is only certainly complete
             * after the next vblank tick -- what the hardware does: a flip
             * takes effect at the vblank after its command. Released on the
             * next flip or at the following tick (held >= 8 ms; ticks are
             * 16 ms apart). LD_ASYNC_PRESENT_LAG=0: off. */
            static int lag = -1;
            /* Default off: the late present covered flips that took the
             * immediate path (no FIFO room) and so were requested before
             * their frame was walked; those now go into the FIFO too. */
            if (lag < 0) { const char* e = getenv("LD_ASYNC_PRESENT_LAG"); lag = e ? atoi(e) : 0; }
            const ULONGLONG now = GetTickCount64();
            rc=cellGcmTakePresentRequest(&buffer,&seq);
            if(rc<0)abort();
            if(!lag){
                if(rc>0){rsx_live_draw_set_present_request(seq);rsx_live_draw_present(buffer);}
                return;
            }
            if(held && (rc>0 || now-held_at>=8)){
                rsx_live_draw_set_present_request(held_seq);rsx_live_draw_present(held_buf);held=false;
            }
            if(rc>0){held=true;held_buf=buffer;held_seq=seq;held_at=now;}
            return;
        }
        static uint32_t presented_reached = 0;
        const uint32_t reached = cellGcmReachedFlipCount();
        if (reached == presented_reached) return;
        presented_reached = reached;
        // cellGcmSys consumes the flip marker before its generic method hook;
        // the dispatcher's sink_flip is therefore not called by this walker.
        rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
        InterlockedExchange(&g_frames_presented, LONG(rsx_live_draw_get_frames()));
        return;
    }
#if defined(_WIN32)
    const uint64_t before = rsx_d3d12_backend_present_count();
    rsx_d3d12_backend_present_completed();
    const uint64_t after = rsx_d3d12_backend_present_count();
    for (uint64_t frame = before; frame < after; ++frame)
        InterlockedIncrement(&g_frames_presented);
#else
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
#endif
}

// Opt-in host-loop accounting. Categories are disjoint wall-time scopes on
// this thread; FIFO time includes its renderer callbacks, and present time
// includes readback/driver waits. These are not CPU- or GPU-utilization clocks.
struct MacrossFrameTiming {
    enum { Kick, Fifo, Present, Tick, Pump, Count };
    bool enabled = false;
    LONGLONG frequency = 0, origin = 0;
    LONGLONG elapsed[Count] = {};
    uint64_t calls[Count] = {};
    LONGLONG last_kick_ticks = 0, kick_ticks[4] = {};
    uint64_t kick_calls[4] = {};
    bool kick_split = false;
    unsigned first_frame = 0, last_frame = 0, reports = 0;
    unsigned report_limit = 512; // 0: continue until process exit for live diagnosis
    uint64_t cpu_origin = 0;
    FILE* interval_file = nullptr;
    LONGLONG previous_present = 0;
    static uint64_t cpu_time() {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
        return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
               (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
    }

    MacrossFrameTiming() {
        const char* value = getenv("MACROSS_FRAME_TIMING");
        enabled = value && strcmp(value, "1") == 0;
        if (!enabled) return;
        if (const char* limit = getenv("MACROSS_FRAME_TIMING_REPORTS")) {
            char* end = nullptr;
            const unsigned long parsed = strtoul(limit, &end, 10);
            if (*limit && end && !*end && parsed <= 1000000ul && *limit != '-')
                report_limit = (unsigned)parsed;
            else fprintf(stderr, "[macross-frame-time] invalid report limit; using 512\n");
        }
        fprintf(stderr, "[macross-frame-time] report_limit=%u (0=until exit)\n", report_limit);
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        frequency = f.QuadPart;
        origin = t.QuadPart;
        cpu_origin = cpu_time();
        first_frame = last_frame = (unsigned)g_frames_presented;
        const char* interval_path = getenv("MACROSS_FRAME_INTERVAL_CSV");
        if (interval_path && *interval_path) {
            interval_file = fopen(interval_path, "w");
            if (interval_file)
                fprintf(interval_file, "frame,frame_delta,qpc,frequency,observed_interval_ms\n");
            else fprintf(stderr, "[macross-frame-time] cannot open interval CSV\n");
        }
    }
    ~MacrossFrameTiming() { if (interval_file) fclose(interval_file); }
    template<class F> void measure(unsigned kind, F&& action) {
        if (!enabled) { action(); return; }
        LARGE_INTEGER begin, end;
        QueryPerformanceCounter(&begin);
        action();
        QueryPerformanceCounter(&end);
        elapsed[kind] += end.QuadPart - begin.QuadPart;
        calls[kind]++;
        if (kind == Kick) last_kick_ticks = end.QuadPart - begin.QuadPart;
    }
    void note_kick(int result, bool gpu_event) {
        if (!enabled) return;
        kick_split = kick_split || gpu_event;
        if (result < 0 || result > 3) return;
        kick_ticks[result] += last_kick_ticks;
        kick_calls[result]++;
    }
    void after_present() {
        if (!enabled) return;
        const unsigned frame = (unsigned)g_frames_presented;
        if (frame == last_frame) return; // failed/skipped requests are not frames
        const unsigned frame_delta = frame - last_frame;
        last_frame = frame;
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        if (interval_file && previous_present)
            fprintf(interval_file, "%u,%u,%lld,%lld,%.6f\n", frame, frame_delta,
                (long long)end.QuadPart, (long long)frequency,
                1000.0 * double(end.QuadPart - previous_present) / double(frequency));
        previous_present = end.QuadPart;
        const LONGLONG wall = end.QuadPart - origin;
        if (wall < frequency) return; // at most one line per second
        LONGLONG accounted = 0;
        for (unsigned i = 0; i < Count; ++i) accounted += elapsed[i];
        const double ms = 1000.0 / double(frequency);
        const uint64_t cpu_end = cpu_time();
        const double cpu_ms = cpu_origin && cpu_end >= cpu_origin ? double(cpu_end - cpu_origin) / 10000.0 : -1.0;
        fprintf(stderr,
            "[macross-frame-time] frames=%u..%u completed=%u wall_ms=%.3f "
            "kick_ms=%.3f/%llu fifo_ms=%.3f/%llu present_ms=%.3f/%llu "
            "tick_ms=%.3f/%llu pump_ms=%.3f/%llu unaccounted_ms=%.3f thread_cpu_ms=%.3f qpc=%lld frequency=%lld\n",
            first_frame, frame, frame - first_frame, double(wall) * ms,
            double(elapsed[Kick]) * ms, (unsigned long long)calls[Kick],
            double(elapsed[Fifo]) * ms, (unsigned long long)calls[Fifo],
            double(elapsed[Present]) * ms, (unsigned long long)calls[Present],
            double(elapsed[Tick]) * ms, (unsigned long long)calls[Tick],
            double(elapsed[Pump]) * ms, (unsigned long long)calls[Pump],
            double(wall - accounted) * ms, cpu_ms, (long long)end.QuadPart, (long long)frequency);
        if (kick_split)
            fprintf(stderr, "[macross-kick-time] frames=%u..%u skip_or_legacy_ms=%.3f/%llu fifo_wake_ms=%.3f/%llu gpu_wake_ms=%.3f/%llu timeout_ms=%.3f/%llu\n",
                first_frame, frame,
                double(kick_ticks[0]) * ms, (unsigned long long)kick_calls[0],
                double(kick_ticks[1]) * ms, (unsigned long long)kick_calls[1],
                double(kick_ticks[2]) * ms, (unsigned long long)kick_calls[2],
                double(kick_ticks[3]) * ms, (unsigned long long)kick_calls[3]);
        if (interval_file) fflush(interval_file);
        // Logging overhead appears in the following interval's unaccounted
        // time. Do not silently subtract it from the measured wall clock.
        origin = end.QuadPart;
        cpu_origin = cpu_end;
        first_frame = frame;
        for (unsigned i = 0; i < 4; ++i) { kick_ticks[i] = 0; kick_calls[i] = 0; }
        for (unsigned i = 0; i < Count; ++i) {
            elapsed[i] = 0;
            calls[i] = 0;
        }
        ++reports;
        if (report_limit && reports >= report_limit) {
            fprintf(stderr, "[macross-frame-time] report limit=%u reached; timing disabled\n", report_limit);
            enabled = false;
        }
    }
};

static DWORD WINAPI frame_clock(LPVOID)
{
    fprintf(stderr, "[host-thread-role] role=render thread_id=%lu\n", (unsigned long)GetCurrentThreadId());
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    const char* renderer = getenv("MACROSS_RENDERER");
    g_live_renderer = renderer && strcmp(renderer, "live") == 0;
    int rsx_ok;
    if (g_live_renderer) {
        /* LD_RENDER_SCALE (rsx_live_draw.c): the window shows the scaled
         * canvas at 1:1. */
        unsigned win_w = WINDOW_WIDTH, win_h = WINDOW_HEIGHT;
        if (const char* rs = getenv("LD_RENDER_SCALE")) {
            const double v = atof(rs);
            if (v > 1.24 && v < 2.01) { win_w = (unsigned)(WINDOW_WIDTH * v + 0.5); win_h = (unsigned)(WINDOW_HEIGHT * v + 0.5); }
        }
        rsx_ok = rsx_null_backend_init(win_w, win_h, title) == 0;
        if (rsx_ok) {
            rsx_null_backend_suppress_present(1);
            const char* async_env=getenv("LD_ASYNC_FRAME_RESOURCES");
            const bool async_requested=async_env&&strcmp(async_env,"1")==0;
            if(async_requested&&cellGcmSetAsyncCompletionMode(1)<0)abort();
            rsx_ok = rsx_live_draw_init(rsx_null_backend_get_hwnd(),
                g_output_width, g_output_height, live_guest_ptr, nullptr) == 0;
            if(async_requested&&(!rsx_ok||!rsx_live_draw_async_enabled()))
                if(cellGcmSetAsyncCompletionMode(0)<0)abort();
        }
        // Fail closed: never create a second window or a second swap chain.
    } else rsx_ok = rsx_backend_init(g_output_width, g_output_height, title) == 0;
#if defined(_WIN32)
    if (rsx_ok && !g_live_renderer)
        cellGcmSetLegacyTransferHooks(rsx_d3d12_guest_read, rsx_d3d12_guest_wrote);
#endif
    fprintf(stderr, "[rsx] %s backend init %s\n", g_live_renderer ? "live D3D12" : RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    start_independent_vblank_clock();

    ULONGLONG next_tick = GetTickCount64();
    unsigned fifo_idle_ms = 4;
    if (const char* value = getenv("MACROSS_FIFO_IDLE_MS")) {
        char* end = nullptr;
        const unsigned long parsed = strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed <= 16)
            fifo_idle_ms = static_cast<unsigned>(parsed);
    }
    fprintf(stderr, "[rsx] FIFO idle timeout=%u ms\n", fifo_idle_ms);
    const char* fifo_idle_control = getenv("MACROSS_FIFO_IDLE_MS_FILE");
    ULONGLONG fifo_idle_check_at = 0;
    MacrossFrameTiming timing;
    const auto timed_present = [&](bool allow_new = true) {
        timing.measure(MacrossFrameTiming::Present, [&]() { present_guest_frame(allow_new); });
        timing.after_present();
    };

    for (;;) {
        // Async GPU completion and deferred labels progress even with no new
        // flip request (including the final in-flight frame / guest WaitFlip).
        if(rsx_ok && g_live_renderer && rsx_live_draw_async_enabled())
            timed_present(false);   /* GPU completions only; presents at the tick */

        // Opt-in host scheduling experiment: change only the idle timeout,
        // retaining the same running scene and all guest FIFO semantics.
        if (fifo_idle_control) {
            const ULONGLONG control_now = GetTickCount64();
            if (control_now >= fifo_idle_check_at) {
                fifo_idle_check_at = control_now + 500;
                if (FILE* control = fopen(fifo_idle_control, "rb")) {
                    unsigned requested; char extra;
                    const int fields = fscanf(control, "%u %c", &requested, &extra);
                    fclose(control);
                    if (fields == 1 && requested <= 16 && requested != fifo_idle_ms) {
                        fifo_idle_ms = requested;
                        fprintf(stderr, "[rsx] FIFO idle timeout=%u ms monotonic_ms=%llu presented=%ld\n",
                            fifo_idle_ms, (unsigned long long)control_now, (long)g_frames_presented);
                    }
                }
            }
        }
        // Match the SDK host loop: a guest fence waiter can wake the RSX
        // consumer immediately. Timeout preserves the ordinary idle cadence.
        int kick_result = 0;
        bool gpu_wait = false;
        timing.measure(MacrossFrameTiming::Kick, [&]() {
            void* gpu_event = g_live_renderer ? rsx_live_draw_gpu_completion_event() : nullptr;
            gpu_wait = gpu_event != nullptr;
            kick_result = cellGcm_fifo_kick_wait_gpu(fifo_idle_ms, gpu_event);
            if (kick_result < 0) abort();
        });
        timing.note_kick(kick_result, gpu_wait);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            timing.measure(MacrossFrameTiming::Tick, [&]() {
                publish_legacy_vblank();
                cellGcmTickFlip();
            });
            /* Submit the batch at its reached GPU flip marker before decoding
             * another frame. The FIFO stops at that marker in completed-flip
             * mode; a CPU flip request alone does not own presentation. */
            /* Async: every tick, so a flip held for one frame (see
             * present_guest_frame) is released by its timeout even when the
             * guest waits for it before flipping again. */
            if (rsx_ok && (cellGcm_take_flip_pending() || (g_live_renderer && rsx_live_draw_async_enabled()))) {
                timed_present();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) timing.measure(MacrossFrameTiming::Fifo,
                [&]() { cellGcm_rsx_process_fifo(); });
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            /* Async: present only at the vblank tick (above). */
            if (cellGcm_take_flip_pending() && !(g_live_renderer && rsx_live_draw_async_enabled())) {
                timed_present();
            }
            timing.measure(MacrossFrameTiming::Fifo,
                [&]() { cellGcm_rsx_process_fifo(); });

            int pump_result = 0;
            timing.measure(MacrossFrameTiming::Pump, [&]() {
                pump_result = g_live_renderer ? rsx_null_backend_pump_messages() : rsx_backend_pump();
            });
            if (pump_result != 0) {
                /* Guest worker threads are not cooperatively stoppable yet.
                 * Closing only the renderer left the game running invisibly
                 * and kept the executable locked against the next build. */
                fprintf(stderr, "[macross] game window closed; exiting host process\n");
                fflush(NULL);
                exit(0);
            }
            /* Only parsed GPU flip markers complete a frame. CPU-queued flip
             * requests may still precede pending draws and must not present. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc == 0 && !g_live_renderer) {
                timing.measure(MacrossFrameTiming::Present, [&]() { rsx_backend_present(); });
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* MEM_WRITE_WATCH: the renderer asks the OS which guest pages were
     * written since its last look (GetWriteWatch), so it can skip re-hashing
     * textures nothing has touched. MACROSS_NO_WRITE_WATCH=1: plain reserve. */
    vm_base = getenv("MACROSS_NO_WRITE_WATCH") ? nullptr :
        (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!vm_base) vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* Bound early bring-up runs so a stalled guest never leaves an unattended
 * process behind. A present before the first guest flip is not gameplay. */
extern "C" void ppu_report_guest_lrs(void);
extern "C" void ppu_thread_report_live(void);
extern "C" void spu_thread_report_live(void);
extern "C" void spurs_report_chains(void);
extern "C" void macross_notify_register(void);
extern "C" void macross_gameplay_register(void);
extern "C" void macross_font_probe_register(void);
extern "C" void macross_sail_install(void);
extern "C" void macross_main_cpu_probe_install(void);
extern "C" void macross_geom_cpu_probe_install(void);
extern "C" void macross_geom_c5b0_capture_install(void);
extern void macross_report_bridges(void);
static DWORD WINAPI macross_bringup_watchdog(LPVOID arg)
{
    unsigned seconds = (unsigned)(uintptr_t)arg;
    for (unsigned elapsed = 0; elapsed < seconds; ++elapsed) {
        Sleep(1000);
        if ((elapsed + 1) % 10 == 0) {
            ppu_thread_report_live();
            spu_thread_report_live();
            spurs_report_chains();
            macross_report_bridges();
        }
    }
    fprintf(stderr, "[macross] bring-up timeout: %u seconds, guest frames presented: %u\n",
            seconds, ppu_boot_frames_presented());
    fflush(NULL);
    exit(124);
    return 0;
}

int main(int argc, char** argv)
{
#ifdef MACROSS_GAMEPLAY_AVX2
    if (!IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */)) {
        fprintf(stderr, "Gameplay SIMD build requires AVX2; rebuild with MACROSS_GAMEPLAY_AVX2=OFF.\n");
        return 1;
    }
    fprintf(stderr, "[macross] captured geometry/animation jobs use exact AVX2 float helpers\n");
#endif
#ifdef MACROSS_SPU_SSE41
    if (!IsProcessorFeaturePresent(PF_SSE4_1_INSTRUCTIONS_AVAILABLE)) {
        fprintf(stderr, "SPU SIMD build requires SSE4.1; rebuild with MACROSS_SPU_SSE41=OFF.\n");
        return 1;
    }
    fprintf(stderr, "[macross] lifted SPU modules use SSE4.1 byte shuffles\n");
#endif
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== Macross 30 static recompilation bring-up ===\n");
    /* On demand: a dump_threads file in the working directory prints what every
     * guest PPU thread, SPU thread and job chain is doing (the bring-up
     * watchdog's report), then the file is removed. For a game that stops
     * advancing while it still renders. */
    CreateThread(NULL, 0, [](LPVOID) -> DWORD {
        for (;;) {
            Sleep(1000);
            if (GetFileAttributesA("dump_threads") == INVALID_FILE_ATTRIBUTES) continue;
            DeleteFileA("dump_threads");
            fprintf(stderr, "[dump-threads] ---- begin ----\n");
            ppu_thread_report_live();
            spu_thread_report_live();
            spurs_report_chains();
            macross_report_bridges();
            fprintf(stderr, "[dump-threads] ---- end ----\n");
            fflush(stderr);
        }
    }, NULL, 0, NULL);
    if (const char* timeout = getenv("MACROSS_RUN_SECONDS")) {
        unsigned seconds = (unsigned)strtoul(timeout, nullptr, 10);
        if (seconds > 0 && seconds <= 3600)
            CreateThread(NULL, 0, macross_bringup_watchdog,
                         (LPVOID)(uintptr_t)seconds, 0, NULL);
    }

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    if (argc >= 3) ppu_vfs_root = argv[2];
    else derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    macross_notify_register();
    macross_gameplay_register();
    macross_font_probe_register();
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    extern void macross_register_bridges(void);
    macross_register_bridges();
    macross_l10n_install();
    macross_sail_install();
    const char* video_mode = getenv("MACROSS_VIDEO_MODE");
    if (video_mode && (!strcmp(video_mode, "480") || !strcmp(video_mode, "480p"))) {
        g_video_resolution_id = 4;
        g_output_width = 720; g_output_height = 480;
        cellVideoOut_set_resolution(4);
        fprintf(stderr, "[video-mode] output=720x480 internal_reference_override=480p window=1280x720\n");
    }

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    CreateThread(NULL, 4u * 1024 * 1024, guest_callback_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    macross_main_cpu_probe_install();
    macross_geom_cpu_probe_install();
    macross_geom_c5b0_capture_install();
    fprintf(stderr, "[host-thread-role] role=main thread_id=%lu\n", (unsigned long)GetCurrentThreadId());
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
