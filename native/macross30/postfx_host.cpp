/* Host side of the post-processing taskset replacement (postfx_host.h).
 *
 * The PPU kicks each taskset once per frame from func_00109B5C:
 *   posttask  func_00060F8C: counter=0, notify label on the last phase,
 *             EventFlagClear, header init (0x84D49C), SendSignal tasks 0..4,
 *             then func_0005E4B0 waits on the event flag (bit 1, OR).
 *   MLAA      func_0005E540: EventFlagClear, per-task blocks (0x84C948),
 *             SendSignal tasks 0..4 (0x84CAE0), func_0005E4F8 waits (bit 1).
 * The batch decision (SPU or host) is taken at the signal for task 0 and kept
 * for the rest of the batch: MLAA tasks meet at a barrier, so a batch split
 * between SPU and host would leave the SPU half waiting for ever. */
#include "postfx_host.h"
#include "postfx_host_core.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t);
int spu_coh_is_reserved(uint32_t);
void spurs_ef_set_from_spu(uint32_t flag_ea, uint16_t bits);
int macross_spu_postfx_skip(void);
/* GPU post-process inputs (vendor/ps3recomp-master/libs/video/rsx_live_draw.c),
 * normally captured by spu_posttask_native/guard.c from the SPU plugins. */
extern unsigned char g_pp_lut[768];
extern volatile long g_pp_lut_seq;
extern volatile unsigned g_pp_scale9, g_pp_scale10;
extern unsigned char g_pp_bloom_snap[640 * 360 * 4];
extern volatile long g_pp_bloom_seq;
/* libs/spurs/cellSpurs.c */
extern int (*g_spurs_send_signal_hook)(uint32_t taskset_ea, uint32_t task_id);
extern void (*g_spurs_ef_wait_done_hook)(uint32_t flag_ea);
}

namespace {

/* ---- guest access ---------------------------------------------------- */

uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
uint64_t bswap64(uint64_t v)
{
    return (uint64_t(bswap32(uint32_t(v))) << 32) | bswap32(uint32_t(v >> 32));
}
/* Main memory and the label window (0x20000000 in this runtime). */
bool guest_ok(uint32_t ea, uint32_t n)
{
    return vm_base && ea >= 0x10000u && uint64_t(ea) + n <= 0x40000000ull;
}
uint32_t guest_u32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int g_read(void*, uint32_t ea, void* dst, uint32_t n)
{
    if (!guest_ok(ea, n)) return 0;
    memcpy(dst, vm_base + ea, n);
    return 1;
}
uint64_t g_load64(void*, uint32_t ea)
{
    std::atomic_ref<uint64_t> w(*reinterpret_cast<uint64_t*>(vm_base + ea));
    return bswap64(w.load(std::memory_order_acquire));
}
/* The SPU's GETLLAR/PUTLLC on this line commits under the lock-line lock;
 * the same lock keeps this CAS out of any PUTLLC window, and the notify makes
 * a reserving SPU see its reservation lost (same as native_zlib.cpp). */
int g_cas64(void*, uint32_t ea, uint64_t expect, uint64_t desired)
{
    spu_lockline_lock();
    std::atomic_ref<uint64_t> w(*reinterpret_cast<uint64_t*>(vm_base + ea));
    uint64_t raw = bswap64(expect);
    const bool ok = w.compare_exchange_strong(raw, bswap64(desired), std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    if (ok) spu_coh_notify_write(ea & ~127u);
    spu_lockline_unlock();
    return ok ? 1 : 0;
}
/* A 4-byte SPU PUT (spu_dma.h contract, as native_zlib.cpp guest_put). */
void g_put32(void*, uint32_t ea, uint32_t value)
{
    const uint32_t raw = bswap32(value), line = ea & ~127u;
    const bool reserved = spu_coh_is_reserved(line) != 0;
    if (reserved) spu_lockline_lock();
    std::atomic_ref<uint32_t> w(*reinterpret_cast<uint32_t*>(vm_base + ea));
    w.store(raw, std::memory_order_release);
    if (reserved) { spu_coh_notify_write(line); spu_lockline_unlock(); }
}
/* SPU 0x5DA0 / MLAA 0xB0C0: GETLLAR/PUTLLC +1 on the BE word. */
uint32_t g_inc32(void*, uint32_t ea)
{
    spu_lockline_lock();
    std::atomic_ref<uint32_t> w(*reinterpret_cast<uint32_t*>(vm_base + ea));
    uint32_t raw = w.load(std::memory_order_acquire), old;
    do {
        old = bswap32(raw);
    } while (!w.compare_exchange_weak(raw, bswap32(old + 1u), std::memory_order_acq_rel,
                                      std::memory_order_acquire));
    spu_coh_notify_write(ea & ~127u);
    spu_lockline_unlock();
    return old;
}
/* The task-side cellSpursEventFlagSet (posttask 0x5458, MLAA 0xA2F0) on a
 * SPU2PPU flag with no SPU waiter: OR the bits and wake the PPU waiter. */
void g_ef_set(void*, uint32_t ea, uint16_t bits) { spurs_ef_set_from_spu(ea, bits); }

/* Stragglers: a SPU task woken for the previous frame can still hold an item
 * of this one (it sees the re-initialised header before its next wait). The
 * host spins like the SPU's own 0xFFFE loop, bounded; then the SPU path. */
int g_yield(void* user, unsigned spins)
{
    ULONGLONG* start = static_cast<ULONGLONG*>(user);
    if (spins == 1) *start = GetTickCount64();
    if (GetTickCount64() - *start > 200u) return 0;
    if (spins < 64) YieldProcessor(); else SwitchToThread();
    return 1;
}

/* Host captures that guard.c normally takes from the plugins. Family 10:
 * the 768-byte tone table is the plugin's parameter block (LS 0x6590 = 0x6200
 * + code size 0x390). Family 11: param words 3 and 4 are the scene / bloom
 * gains the inner kernel receives in r9/r10 (LS 0x6D90 + 12/16). */
void capture_phase(void*, uint16_t, const postfx_desc* d)
{
    if (d->code_ea == POSTFX_F10_CODE_EA && d->param_size >= 768u && guest_ok(d->param_ea, 768u)) {
        memcpy(g_pp_lut, vm_base + d->param_ea, 768);
        InterlockedIncrement(&g_pp_lut_seq);
    } else if (d->code_ea == POSTFX_F11_CODE_EA && d->param_size >= 20u && guest_ok(d->param_ea, 20u)) {
        g_pp_scale9 = guest_u32(d->param_ea + 12u);
        g_pp_scale10 = guest_u32(d->param_ea + 16u);
        /* guard.c: the bloom buffer at the start of the composite, <= 50/s. */
        static ULONGLONG last_snap;
        const ULONGLONG now = GetTickCount64();
        if (now - last_snap > 20u && guest_ok(0x19AC0000u, 640u * 360u * 4u)) {
            last_snap = now;
            memcpy(g_pp_bloom_snap, vm_base + 0x19AC0000u, sizeof g_pp_bloom_snap);
            InterlockedIncrement(&g_pp_bloom_seq);
        }
    }
}

postfx_guest make_guest(void* user, bool host_captures)
{
    postfx_guest g{};
    g.user = user;
    g.read = g_read;
    g.load64 = g_load64;
    g.cas64 = g_cas64;
    g.put32 = g_put32;
    g.inc32 = g_inc32;
    g.ef_set = g_ef_set;
    g.phase_begin = host_captures ? capture_phase : nullptr;
    g.yield = g_yield;
    return g;
}

/* ---- mode -------------------------------------------------------------- */

std::once_flag g_mode_once;
int g_mode = MACROSS_POSTFX_COMPARE;

void read_mode()
{
    int mode = MACROSS_POSTFX_COMPARE;
    if (const char* e = getenv("MACROSS_POSTFX_HOST"); e && *e) {
        if (!strcmp(e, "1") || !_stricmp(e, "host")) mode = MACROSS_POSTFX_HOST;
        else if (!strcmp(e, "0") || !_stricmp(e, "off")) mode = MACROSS_POSTFX_OFF;
        else if (!strcmp(e, "2") || !_stricmp(e, "compare")) mode = MACROSS_POSTFX_COMPARE;
    }
    if (FILE* f = fopen("no_postfx_host", "rb")) { fclose(f); mode = MACROSS_POSTFX_OFF; }
    g_mode = mode;
    fprintf(stderr, "[postfx-host] mode=%s (MACROSS_POSTFX_HOST)\n",
            mode == MACROSS_POSTFX_HOST ? "host" : mode == MACROSS_POSTFX_COMPARE ? "compare" : "off");
}

/* ---- taskset identification -------------------------------------------- */

enum Kind { KIND_OTHER = 0, KIND_POST = 1, KIND_MLAA = 2 };
constexpr uint32_t kTaskInfo = 0x80u, kTaskInfoSize = 48u, kMaxTasks = 16u;

uint32_t task_elf(uint32_t taskset, uint32_t id)
{
    const uint32_t ti = taskset + kTaskInfo + kTaskInfoSize * id;
    if (!guest_ok(ti, kTaskInfoSize)) return 0;
    return guest_u32(ti + 0x14u) & ~7u;   /* be u64 ELF EA, low 3 bits are flags */
}
bool task_args(uint32_t taskset, uint32_t id, uint8_t out[16])
{
    return g_read(nullptr, taskset + kTaskInfo + kTaskInfoSize * id, out, 16) != 0;
}

struct Taskset {
    uint32_t ea = 0;
    Kind kind = KIND_OTHER;
    unsigned tasks = 0;                /* ids 0..tasks-1 run this image */
    bool batch_host = false;           /* decision of the current batch */
    bool batch_open = false;
};
std::mutex g_lock;
std::vector<Taskset> g_tasksets;

Taskset& taskset_of(uint32_t ea)
{
    for (Taskset& t : g_tasksets) if (t.ea == ea) return t;
    Taskset t;
    t.ea = ea;
    const uint32_t elf = task_elf(ea, 0);
    t.kind = elf == POSTFX_POST_ELF_EA ? KIND_POST : elf == POSTFX_MLAA_ELF_EA ? KIND_MLAA : KIND_OTHER;
    if (t.kind != KIND_OTHER) {
        while (t.tasks < kMaxTasks && task_elf(ea, t.tasks) == elf) ++t.tasks;
        fprintf(stderr, "[postfx-host] taskset 0x%08X = %s, %u tasks\n", ea,
                t.kind == KIND_POST ? "posttask (318051041)" : "Edge MLAA (452987026)", t.tasks);
    }
    g_tasksets.push_back(t);
    return g_tasksets.back();
}

/* ---- statistics / logging ---------------------------------------------- */

struct Stats {
    unsigned long long post_host = 0, post_spu = 0, post_fallback = 0;
    unsigned long long mlaa_host = 0, mlaa_spu = 0, mlaa_fallback = 0;
    unsigned long long cmp_frames = 0, cmp_ok = 0, cmp_diff = 0, capture_diff = 0, barrier_diff = 0;
} g_stats;
unsigned long long g_diff_lines, g_capture_lines;

bool diff_log_allowed()
{
    ++g_diff_lines;
    return g_diff_lines <= 64 || g_diff_lines % 512 == 0;
}
/* Capture differences are expected now and then (see compare_post_done);
 * a separate budget keeps them from using up the protocol-diff lines. */
bool capture_log_allowed()
{
    ++g_capture_lines;
    return g_capture_lines <= 16 || g_capture_lines % 1024 == 0;
}

void report(const char* why)
{
    fprintf(stderr, "[postfx-host] %s post host=%llu spu=%llu fallback=%llu | mlaa host=%llu spu=%llu "
                    "fallback=%llu | compare frames=%llu ok=%llu diff=%llu capture_diff=%llu "
                    "barrier_diff=%llu\n", why, g_stats.post_host, g_stats.post_spu,
            g_stats.post_fallback, g_stats.mlaa_host, g_stats.mlaa_spu, g_stats.mlaa_fallback,
            g_stats.cmp_frames, g_stats.cmp_ok, g_stats.cmp_diff, g_stats.capture_diff,
            g_stats.barrier_diff);
}
void maybe_report()
{
    const unsigned long long n = g_stats.post_host + g_stats.post_spu + g_stats.mlaa_host + g_stats.mlaa_spu;
    if (n && n % 1200 == 0) report("stats");
}

/* ---- compare mode -------------------------------------------------------- */

struct PendingPost {
    bool active = false;
    unsigned long long seq = 0;
    postfx_post_args args{};
    postfx_post_expect expect{};
    unsigned signals = 0;
};
struct PendingCounter { bool active = false; uint32_t ea = 0, expect = 0; };
struct PendingMlaa {
    bool active = false;
    unsigned long long seq = 0;
    uint32_t flag = 0;
    unsigned blocks = 0;
    postfx_mlaa_block block[kMaxTasks]{};
    uint32_t counter_ea = 0, counter_expect = 0;
};
PendingPost g_post;
PendingCounter g_post_counter;
PendingMlaa g_mlaa;
unsigned long long g_seq;
uint8_t g_barrier_prev[128];
uint32_t g_barrier_prev_ea;

void cmp_result(bool ok) { ok ? ++g_stats.cmp_ok : ++g_stats.cmp_diff; }

void compare_post_kick(Taskset& ts)
{
    uint8_t raw[16];
    if (!task_args(ts.ea, 0, raw)) return;
    PendingPost p;
    postfx_post_args_parse(raw, &p.args);
    postfx_guest g = make_guest(nullptr, false);
    const int rc = postfx_post_expect_compute(&g, &p.args, &p.expect);
    if (g_post.active && diff_log_allowed())
        fprintf(stderr, "[postfx-host] DIFF post frame seq=%llu: no event-flag wait before the next kick\n",
                g_post.seq);
    if (rc) {
        if (diff_log_allowed())
            fprintf(stderr, "[postfx-host] post kick: host would fall back to the SPU (rc=%d) header=%08X\n",
                    rc, p.args.header_ea);
        g_post.active = false;
        return;
    }
    p.active = true;
    p.seq = ++g_seq;
    p.signals = 1;
    if (p.seq <= 3) {
        postfx_header h;
        postfx_post_validate(&g, &p.args, &h);
        fprintf(stderr, "[postfx-host] post frame: header=%08X counter=%08X flag=%08X phases=%u descs=%08X "
                        "labels=%u f10=%d@%u param %08X/%u f11=%d@%u param %08X/%u final=%04X/%04X/%04X/%04X\n",
                p.args.header_ea, p.args.counter_ea, p.args.flag_ea, h.phase_count, h.desc_ea,
                p.expect.label_count, p.expect.has_f10, p.expect.f10_phase, p.expect.f10_param_ea,
                p.expect.f10_param_size, p.expect.has_f11, p.expect.f11_phase, p.expect.f11_param_ea,
                p.expect.f11_param_size, p.expect.final_state.phase, p.expect.final_state.next,
                p.expect.final_state.done, p.expect.final_state.total);
        for (unsigned i = 0; i < p.expect.label_count; ++i)
            fprintf(stderr, "[postfx-host]   notify %08X <- %08X\n", p.expect.label_ea[i], p.expect.label_value[i]);
    }
    g_post = p;
}

void compare_post_done()
{
    PendingPost& p = g_post;
    p.active = false;
    ++g_stats.cmp_frames;
    bool ok = true;
    const uint64_t live = g_load64(nullptr, p.args.header_ea);
    const uint64_t want = postfx_state_pack(p.expect.final_state);
    if (live != want) {
        ok = false;
        if (diff_log_allowed())
            fprintf(stderr, "[postfx-host] DIFF post seq=%llu header state spu=%016llX host=%016llX\n",
                    p.seq, (unsigned long long)live, (unsigned long long)want);
    }
    for (unsigned i = 0; i < p.expect.label_count; ++i) {
        bool last = true;   /* only the last write to an EA survives */
        for (unsigned j = i + 1; j < p.expect.label_count; ++j)
            if (p.expect.label_ea[j] == p.expect.label_ea[i]) last = false;
        if (!last || !guest_ok(p.expect.label_ea[i], 4)) continue;
        const uint32_t v = guest_u32(p.expect.label_ea[i]);
        if (v != p.expect.label_value[i]) {
            ok = false;
            if (diff_log_allowed())
                fprintf(stderr, "[postfx-host] DIFF post seq=%llu notify %08X spu=%08X host=%08X\n", p.seq,
                        p.expect.label_ea[i], v, p.expect.label_value[i]);
        }
    }
    cmp_result(ok);
    /* Captures guard.c took from the SPU plugins vs what the host reads from
     * the parameter blocks. Family 11 runs only ~4/s under the GPU
     * post-process (guard.c pp_gpu_skip), so a gain lag is expected there. */
    if (p.expect.has_f11 && p.expect.f11_param_size >= 20u && guest_ok(p.expect.f11_param_ea, 20u)) {
        const uint32_t s9 = guest_u32(p.expect.f11_param_ea + 12u), s10 = guest_u32(p.expect.f11_param_ea + 16u);
        if (s9 != g_pp_scale9 || s10 != g_pp_scale10) {
            ++g_stats.capture_diff;
            if (capture_log_allowed())
                fprintf(stderr, "[postfx-host] capture seq=%llu f11 gains spu=%08X/%08X host=%08X/%08X\n",
                        p.seq, g_pp_scale9, g_pp_scale10, s9, s10);
        }
    }
    if (p.expect.has_f10 && p.expect.f10_param_size >= 768u && guest_ok(p.expect.f10_param_ea, 768u) &&
        memcmp(g_pp_lut, vm_base + p.expect.f10_param_ea, 768)) {
        ++g_stats.capture_diff;
        unsigned at = 0;
        while (at < 768u && g_pp_lut[at] == vm_base[p.expect.f10_param_ea + at]) ++at;
        if (capture_log_allowed())
            fprintf(stderr, "[postfx-host] capture seq=%llu f10 lut first_diff=%u spu=%02X host=%02X\n",
                    p.seq, at, g_pp_lut[at], vm_base[p.expect.f10_param_ea + at]);
    }
    /* Each task adds 1 after it sees the frame done, maybe after this wait
     * returned: checked at the next wait on the flag (the MLAA one). */
    g_post_counter.active = p.args.counter_ea && guest_ok(p.args.counter_ea, 4);
    g_post_counter.ea = p.args.counter_ea;
    g_post_counter.expect = p.signals;
}

void compare_post_counter()
{
    if (!g_post_counter.active) return;
    g_post_counter.active = false;
    const uint32_t v = guest_u32(g_post_counter.ea);
    cmp_result(v == g_post_counter.expect);
    if (v != g_post_counter.expect && diff_log_allowed())
        fprintf(stderr, "[postfx-host] DIFF post wake counter %08X spu=%u host=%u\n", g_post_counter.ea, v,
                g_post_counter.expect);
}

bool read_mlaa_blocks(Taskset& ts, PendingMlaa& m)
{
    m.blocks = 0;
    for (unsigned k = 0; k < ts.tasks; ++k) {
        uint8_t raw[16], blk[64];
        uint32_t block_ea = 0, flag = 0;
        if (!task_args(ts.ea, k, raw)) return false;
        postfx_mlaa_args_parse(raw, &block_ea, &flag);
        if (k == 0) m.flag = flag;
        else if (flag != m.flag) return false;
        if ((block_ea & 15u) || !g_read(nullptr, block_ea, blk, sizeof blk)) return false;
        postfx_mlaa_block_parse(blk, &m.block[k]);
        const postfx_mlaa_block& b = m.block[k];
        if ((b.label_ea & 3u) || (b.counter_ea & 3u)) return false;
        if (b.label_ea && !guest_ok(b.label_ea, 4)) return false;
        if (b.counter_ea && !guest_ok(b.counter_ea, 4)) return false;
        ++m.blocks;
    }
    return m.blocks > 0 && (!m.flag || ((m.flag & 127u) == 0 && guest_ok(m.flag, 128)));
}

void compare_mlaa_kick(Taskset& ts)
{
    PendingMlaa m;
    if (!read_mlaa_blocks(ts, m)) {
        if (diff_log_allowed())
            fprintf(stderr, "[postfx-host] mlaa kick: host would fall back to the SPU (blocks unreadable)\n");
        g_mlaa.active = false;
        return;
    }
    if (g_mlaa.active && diff_log_allowed())
        fprintf(stderr, "[postfx-host] DIFF mlaa frame seq=%llu: no event-flag wait before the next kick\n",
                g_mlaa.seq);
    m.active = true;
    m.seq = ++g_seq;
    unsigned counters = 0;
    for (unsigned k = 0; k < m.blocks; ++k) {
        if (m.block[k].counter_ea) {
            if (m.counter_ea && m.counter_ea != m.block[k].counter_ea) counters = 1000;   /* not modelled */
            m.counter_ea = m.block[k].counter_ea;
            ++counters;
        }
    }
    if (m.counter_ea) m.counter_expect = guest_u32(m.counter_ea) + counters;
    if (m.seq <= 6)
        for (unsigned k = 0; k < m.blocks; ++k)
            fprintf(stderr, "[postfx-host] mlaa block %u: label %08X <- %08X counter=%08X barrier=%08X mode=%02X flag=%08X\n",
                    k, m.block[k].label_ea, m.block[k].label_value, m.block[k].counter_ea,
                    m.block[k].barrier_ea, m.block[k].mode, m.flag);
    /* Skipping whole frames leaves the barrier untouched: it must look the
     * same at every kick for host frames to be safe between SPU frames. */
    const uint32_t bar = m.block[0].barrier_ea;
    if (bar && !(bar & 127u) && guest_ok(bar, 128)) {
        if (g_barrier_prev_ea == bar && memcmp(g_barrier_prev, vm_base + bar, 128)) {
            ++g_stats.barrier_diff;
            unsigned at = 0;
            while (at < 128 && g_barrier_prev[at] == vm_base[bar + at]) ++at;
            if (diff_log_allowed())
                fprintf(stderr, "[postfx-host] mlaa barrier %08X changed between kicks at +0x%02X (%02X -> %02X)\n",
                        bar, at, g_barrier_prev[at], vm_base[bar + at]);
        }
        memcpy(g_barrier_prev, vm_base + bar, 128);
        g_barrier_prev_ea = bar;
    }
    g_mlaa = m;
}

void compare_mlaa_done()
{
    PendingMlaa& m = g_mlaa;
    m.active = false;
    ++g_stats.cmp_frames;
    bool ok = true;
    for (unsigned k = 0; k < m.blocks; ++k) {
        const postfx_mlaa_block& b = m.block[k];
        if (b.label_ea && guest_u32(b.label_ea) != b.label_value) {
            ok = false;
            if (diff_log_allowed())
                fprintf(stderr, "[postfx-host] DIFF mlaa seq=%llu task %u label %08X spu=%08X host=%08X\n",
                        m.seq, k, b.label_ea, guest_u32(b.label_ea), b.label_value);
        }
    }
    if (m.counter_ea && guest_u32(m.counter_ea) != m.counter_expect) {
        ok = false;
        if (diff_log_allowed())
            fprintf(stderr, "[postfx-host] DIFF mlaa seq=%llu counter %08X spu=%u host=%u\n", m.seq,
                    m.counter_ea, guest_u32(m.counter_ea), m.counter_expect);
    }
    cmp_result(ok);
    if (g_stats.cmp_frames <= 4 || g_stats.cmp_frames % 600 == 0) report("compare");
}

/* ---- host mode ------------------------------------------------------------ */

ULONGLONG g_yield_start;

/* Posttask batch: the whole frame at task 0's signal, then one wake-counter
 * increment per suppressed signal. Returns false to hand the batch to the SPU. */
bool host_post_task0(Taskset& ts)
{
    uint8_t raw[16];
    if (!task_args(ts.ea, 0, raw)) return false;
    postfx_post_args a;
    postfx_post_args_parse(raw, &a);
    /* Every task must name the same frame. */
    for (unsigned k = 1; k < ts.tasks; ++k) {
        uint8_t rk[16];
        if (!task_args(ts.ea, k, rk) || memcmp(rk, raw, 12)) return false;
    }
    if (a.counter_ea && !guest_ok(a.counter_ea, 4)) return false;
    if (a.flag_ea && !guest_ok(a.flag_ea, 128)) return false;
    postfx_guest g = make_guest(&g_yield_start, true);
    postfx_run_stats st;
    const int rc = postfx_post_run(&g, &a, &st);
    if (rc) {
        ++g_stats.post_fallback;
        if (diff_log_allowed())
            fprintf(stderr, "[postfx-host] post frame -> SPU (rc=%d claimed=%u completed=%u)\n", rc,
                    st.claimed, st.completed);
        return false;
    }
    if (a.counter_ea) g_inc32(nullptr, a.counter_ea);
    if (++g_stats.post_host <= 3)
        fprintf(stderr, "[postfx-host] post frame on host: items=%u phases=%u notifies=%u ef_sets=%u\n",
                st.completed, st.phases_advanced, st.notifies, st.ef_sets);
    return true;
}

void host_post_task(Taskset& ts, uint32_t id)
{
    uint8_t raw[16];
    if (!task_args(ts.ea, id, raw)) return;
    postfx_post_args a;
    postfx_post_args_parse(raw, &a);
    if (a.counter_ea) g_inc32(nullptr, a.counter_ea);
}

bool host_mlaa_task0(Taskset& ts)
{
    PendingMlaa m;
    if (!read_mlaa_blocks(ts, m)) { ++g_stats.mlaa_fallback; return false; }
    postfx_guest g = make_guest(nullptr, false);
    postfx_mlaa_tail(&g, &m.block[0], m.flag);
    if (++g_stats.mlaa_host <= 3)
        fprintf(stderr, "[postfx-host] mlaa frame on host: label %08X <- %08X counter=%08X\n",
                m.block[0].label_ea, m.block[0].label_value, m.block[0].counter_ea);
    return true;
}

void host_mlaa_task(Taskset& ts, uint32_t id)
{
    uint8_t raw[16], blk[64];
    uint32_t block_ea = 0, flag = 0;
    if (!task_args(ts.ea, id, raw)) return;
    postfx_mlaa_args_parse(raw, &block_ea, &flag);
    if (!g_read(nullptr, block_ea, blk, sizeof blk)) return;
    postfx_mlaa_block b;
    postfx_mlaa_block_parse(blk, &b);
    postfx_guest g = make_guest(nullptr, false);
    postfx_mlaa_tail(&g, &b, flag);
}

struct Install {
    Install()
    {
        g_spurs_send_signal_hook = macross_postfx_host_send_signal;
        g_spurs_ef_wait_done_hook = macross_postfx_host_ef_wait_done;
    }
} g_install;

} // namespace

extern "C" int macross_postfx_host_mode(void)
{
    std::call_once(g_mode_once, read_mode);
    return g_mode;
}

extern "C" int macross_postfx_host_send_signal(uint32_t taskset_ea, uint32_t task_id)
{
    const int mode = macross_postfx_host_mode();
    if (mode == MACROSS_POSTFX_OFF || !vm_base) return 0;
    std::lock_guard<std::mutex> guard(g_lock);
    Taskset& ts = taskset_of(taskset_ea);
    if (ts.kind == KIND_OTHER || task_id >= ts.tasks) return 0;
    if (task_id == 0) {
        ts.batch_open = true;
        ts.batch_host = false;
        if (mode == MACROSS_POSTFX_HOST && macross_spu_postfx_skip())
            ts.batch_host = ts.kind == KIND_POST ? host_post_task0(ts) : host_mlaa_task0(ts);
        if (!ts.batch_host) {
            ++(ts.kind == KIND_POST ? g_stats.post_spu : g_stats.mlaa_spu);
            if (mode == MACROSS_POSTFX_COMPARE) {
                if (ts.kind == KIND_POST) compare_post_kick(ts);
                else compare_mlaa_kick(ts);
            }
        }
        maybe_report();
        return ts.batch_host ? 1 : 0;
    }
    if (!ts.batch_open || !ts.batch_host) {
        if (ts.kind == KIND_POST && g_post.active) ++g_post.signals;
        return 0;
    }
    if (ts.kind == KIND_POST) host_post_task(ts, task_id);
    else host_mlaa_task(ts, task_id);
    return 1;
}

extern "C" void macross_postfx_host_ef_wait_done(uint32_t flag_ea)
{
    if (macross_postfx_host_mode() != MACROSS_POSTFX_COMPARE || !vm_base) return;
    std::lock_guard<std::mutex> guard(g_lock);
    /* Both tasksets share the flag; the PPU kicks and waits one after the
     * other, so the newer pending frame is the one this wait was for. */
    const bool post = g_post.active && g_post.args.flag_ea == flag_ea;
    const bool mlaa = g_mlaa.active && g_mlaa.flag == flag_ea;
    if (post && mlaa) {
        if (g_post.seq > g_mlaa.seq) { g_mlaa.active = false; compare_post_done(); }
        else { compare_post_counter(); g_post.active = false; compare_mlaa_done(); }
        return;
    }
    if (post) { compare_post_done(); return; }
    if (g_post_counter.active && g_post_counter.ea && flag_ea == g_post.args.flag_ea) compare_post_counter();
    if (mlaa) compare_mlaa_done();
}
