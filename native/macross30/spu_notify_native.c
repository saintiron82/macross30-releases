/* spu_notify_native.c -- the Macross notify SPURS job (image 10001) on the host.
 *
 * What the SPU job does (raw job A62F00, lifted in spu_notify/spu_recomp.c;
 * derivation, field tables and ordering: native/analysis/
 * notify-graphics-jobs-spec.md). Descriptor user area, built by the PPU at
 * func_006C9708 for the wait object `obj` (func_006CA0C4 initialises it):
 *
 *   +0x30.. MFC list, n elements {u32 size, u32 ea}: the 8-byte items of the
 *           object's ring (obj+0x80..) that completed
 *   +0x60 u16 n elements           +0x62 u16 bytes the list moves
 *   +0x64 u16 waiters to release   +0x68 u32 obj (128-byte wait line)
 *   +0x6C u32 event-flag array     +0x70 u16 bits for flag[0]
 *   +0x72 u16 mask: flag[1+i] gets bit (15-i)
 *
 *   main (0x30):  fill scratch with quads 00..7F 00..7F; PUTL it over the
 *                 list (every item becomes u64 0x7F); wait for the tag;
 *                 0x268(obj, n); 0x7F0 on each flagged event flag; return.
 *   0x268:        GETLLAR/PUTLLC on obj: take min(n, queued) waiters off the
 *                 slot ring (bytes 0/1 = head/tail mod 62, u16 slots 1..31),
 *                 advance pos (+0x44, mod +0x74) and count (+0x48); then, last
 *                 taken first: 0xFFFF = PPU waiter -> throw_event(port +0x67),
 *                 else (wid<<8|task) -> cellSpursSendSignal.
 *   0x7F0:        SPU-side cellSpursEventFlagSet (RPCS3 CellSpursEventFlag
 *                 layout), satisfied SPU wait slots get SendSignal, a satisfied
 *                 PPU waiter gets throw_event(spuPort).
 *   0x620/0x538/0xC40: cellSpursSendSignal, the workload-info lookup and the
 *                 workload signal -- these read the SPURS kernel area of LS
 *                 (0x170, 0x1C0); here that is the job's own code (the job is
 *                 staged at LS 0), and the native path reads it the same way.
 *   0xCE8:        sys_spu_thread_throw_event: WrOutMbox(data1),
 *                 WrOutIntrMbox(port<<24 | 0x40000000 | data0).
 *
 * The native job reproduces every guest-visible effect in the SPU's order:
 * the PUTs (with the DMA engine's coherency notify, put-wake and put hook),
 * each lock-line commit (under the lock-line lock, with the reservation-lost
 * notify and the post-commit hooks PUTLLC runs) and each event (through the
 * runtime's own user-event hook, as WrOutIntrMbox does). A job is first run
 * "dry" (no side effects, writes kept in an overlay): anything the SPU would
 * assert on, or anything outside what the native code models, goes to the
 * SPU instead, before any effect. */
#include "spu_notify_native.h"
#include "spu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "synchronization.lib")
#endif

#if defined(_MSC_VER) || defined(_WIN32)
#  define MN_TLS __declspec(thread)
#else
#  define MN_TLS __thread
#endif

/* ---- runtime interface (runtime/spu, libs/spurs) ------------------------ */
extern uint8_t* vm_base;
extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern void spu_coh_reserve(spu_context* ctx, uint32_t ea);
extern int  spu_coh_is_reserved(uint32_t addr);
extern void spu_coh_notify_write(uint32_t ea);
extern void spu_coh_notify_write_except(uint32_t ea, const void* self);
extern void spu_taskset_line_committed(uint32_t line);
extern void (*g_spu_line_commit_hook)(uint32_t line);
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
extern int  (*g_spu_user_event_hook)(spu_context*, uint32_t);
extern void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id, int is_intr, uint32_t value);
#ifdef _WIN32
extern volatile uint32_t g_spu_put_wake_ea;
extern unsigned char g_vm_page_bitmap[65536 / 8];
#endif

/* ---- constants ----------------------------------------------------------- */
#define MN_IMAGE_NOTIFY   10001
#define MN_IMAGE_GRAPHICS 10002
#define MN_TRUST_AFTER    3
#define MN_FX_MAX         192
#define MN_TAKE_MAX       8      /* 0x268 keeps the taken slots in 8 u16 below its saved registers */
#define MN_JOB_STACK_DEFAULT 8192u
#define MN_ALIGN1024(x)   (((x) + 1023u) & ~1023u)

/* SPURS task error codes the job returns (cell/spurs/error.h) */
#define E_INVAL     0x80410902u
#define E_SRCH      0x80410905u
#define E_PERM      0x80410909u
#define E_STAT      0x8041090Fu
#define E_ALIGN     0x80410910u
#define E_NULL      0x80410911u
#define E_FATAL     0x80410914u
#define E_EINVAL    0x80010002u   /* throw_event on a port > 63 */

/* Code-path signature: which kinds of effect a job produced. Trust is per
 * signature, so a path is compared with the SPU before it goes native. */
enum {
    SIG_PUTL = 1u << 0,  SIG_TAKE = 1u << 1,  SIG_RING_PPU = 1u << 2, SIG_RING_TASK = 1u << 3,
    SIG_EF_MULTI = 1u << 4, SIG_EF_SINGLE = 1u << 5, SIG_EF_SPU = 1u << 6, SIG_EF_PPU = 1u << 7,
    SIG_SIGNALLED = 1u << 8, SIG_WKL = 1u << 9, SIG_IWL = 1u << 10, SIG_WRAP = 1u << 11,
    SIG_EF_DIR0 = 1u << 12,  /* ..15: event-flag directions seen */
    SIG_BADPORT = 1u << 16, SIG_EF_CLEAR = 1u << 17, SIG_SIG_NOOP = 1u << 18
};

/* ---- big-endian helpers -------------------------------------------------- */
static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void wbe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wbe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ---- per-job state ------------------------------------------------------- */
enum { FX_PUT = 1, FX_LINE = 2, FX_EVT = 3 };
typedef struct mn_fx {
    uint8_t  kind;
    uint32_t ea;      /* PUT/LINE: guest EA (LINE: 128-aligned); EVT: interrupt-mailbox value */
    uint32_t size;    /* PUT: bytes; EVT: outbound-mailbox data */
    uint32_t phase;   /* PUT: LS offset of the first byte within the 00..7F pattern */
    uint8_t  in[128], out[128];   /* LINE: as reserved / as committed */
} mn_fx;

typedef struct mn_job {
    int dry;              /* 1: no guest side effects; writes are read back from fx */
    int halted;           /* the SPU job would stop on an assert (heqi) */
    int reject;           /* outside what the native job models: run the SPU */
    int locked;           /* live: lock-line lock held between line_get and line_put */
    uint32_t halt_pc;
    const char* why;
    uint32_t sig;
    uint8_t kls[0x150];   /* local store 0x80..0x1CF as the job sees it */
    uint32_t nfx;
    int overflow;
    mn_fx fx[MN_FX_MAX];
} mn_job;
#define KLS(j, lsa) (&(j)->kls[(lsa) - 0x80u])

static void mn_stop_reject(mn_job* j, const char* why) { if (!j->reject && !j->halted) { j->reject = 1; j->why = why; } }
static void mn_stop_halt(mn_job* j, uint32_t pc) { if (!j->reject && !j->halted) { j->halted = 1; j->halt_pc = pc; } }
#define MN_STOPPED(j) ((j)->halted || (j)->reject)

static mn_fx* mn_fx_new(mn_job* j, uint8_t kind)
{
    if (j->nfx >= MN_FX_MAX) {
        j->overflow = 1;
        if (j->dry) mn_stop_reject(j, "too many effects");
        return NULL;
    }
    mn_fx* f = &j->fx[j->nfx++];
    f->kind = kind;
    return f;
}

/* The 0x7F completion pattern the job writes into its scratch buffer: every
 * quadword is 00 00 00 00 00 00 00 7F 00 00 00 00 00 00 00 7F. */
static uint8_t mn_pattern(uint32_t ls_off) { return (ls_off & 7u) == 7u ? 0x7Fu : 0x00u; }

/* Guest range usable without the DMA engine's commit-on-demand. Committed
 * guest pages are never decommitted while the game runs, so the 64 KB pages
 * found committed are remembered (the runtime's page bitmap first, then a
 * few per thread) and VirtualQuery runs once per page. */
static MN_TLS uint32_t t_ok_page[8];
static MN_TLS unsigned t_ok_next;
static int mn_ea_ok(uint32_t ea, uint32_t size)
{
    if (!size || (uint64_t)ea + size > 0x100000000ull) return 0;
#ifdef _WIN32
    for (uint32_t pg = ea >> 16; pg <= (ea + size - 1u) >> 16; pg++) {
        if ((g_vm_page_bitmap[pg >> 3] >> (pg & 7u)) & 1u) continue;
        int known = 0;
        for (unsigned k = 0; k < 8 && !known; k++) known = t_ok_page[k] == pg + 1u;
        if (known) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + ((uintptr_t)pg << 16), &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        t_ok_page[t_ok_next++ & 7u] = pg + 1u;
    }
#endif
    return 1;
}

/* Guest bytes as this job sees them: memory, then (dry run) its own writes. */
static void mn_read(const mn_job* j, uint32_t ea, uint8_t* out, uint32_t n)
{
    memcpy(out, vm_base + ea, n);
    if (!j->dry) return;
    for (uint32_t k = 0; k < j->nfx; k++) {
        const mn_fx* f = &j->fx[k];
        if (f->kind == FX_LINE) {
            for (uint32_t i = 0; i < 128u; i++)
                if (f->ea + i - ea < n) out[f->ea + i - ea] = f->out[i];
        } else if (f->kind == FX_PUT) {
            for (uint32_t i = 0; i < f->size; i++)
                if (f->ea + i - ea < n) out[f->ea + i - ea] = mn_pattern(f->phase + i);
        }
    }
}

/* GETLLAR: the line as reserved. Live: under the lock-line lock until
 * mn_line_put / mn_line_drop (the reservation can therefore not be lost). */
static int mn_line_get(mn_job* j, uint32_t line, uint8_t L[128])
{
    if (j->dry) {
        if (!mn_ea_ok(line, 128)) { mn_stop_reject(j, "lock line not committed"); return 0; }
        mn_read(j, line, L, 128);
        return 1;
    }
    spu_lockline_lock();
    spu_coh_reserve(NULL, line);   /* the reserved-line mark a GETLLAR leaves (PPU stores to it take the lock) */
    j->locked = 1;
    memcpy(L, vm_base + line, 128);
    return 1;
}
/* GETLLAR without a PUTLLC (the job returned an error first). */
static void mn_line_drop(mn_job* j)
{
    if (j->locked) { j->locked = 0; spu_lockline_unlock(); }
}
/* Successful PUTLLC: commit, lose every peer reservation, then the hooks a
 * committing PUTLLC runs after the lock (spu_channels.c MFC_PUTLLC_CMD). */
static void mn_line_put(mn_job* j, uint32_t line, const uint8_t in[128], const uint8_t L[128])
{
    mn_fx* f = mn_fx_new(j, FX_LINE);
    if (f) { f->ea = line; f->size = 128; f->phase = 0; memcpy(f->in, in, 128); memcpy(f->out, L, 128); }
    if (j->dry) return;
    memcpy(vm_base + line, L, 128);
    spu_coh_notify_write(line);
    j->locked = 0;
    spu_lockline_unlock();
    spu_taskset_line_committed(line);
    if (g_spu_line_commit_hook) g_spu_line_commit_hook(line);
}

/* One list element of the PUTL: the DMA engine's PUT (spu_dma.h
 * mfc_do_transfer_observed, PUT branch). */
static void mn_put(mn_job* j, uint32_t ea, uint32_t size, uint32_t phase)
{
    mn_fx* f = mn_fx_new(j, FX_PUT);
    if (f) { f->ea = ea; f->size = size; f->phase = phase; }
    if (j->dry) return;
    uint8_t* p = vm_base + ea;
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { reserved = 1; break; } if (a == a1) break; }
    if (reserved) {
        spu_lockline_lock();
        for (uint32_t i = 0; i < size; i++) p[i] = mn_pattern(phase + i);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, NULL); if (a == a1) break; }
        spu_lockline_unlock();
    } else {
        for (uint32_t i = 0; i < size; i++) p[i] = mn_pattern(phase + i);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) { spu_lockline_lock(); spu_coh_notify_write_except(a, NULL); spu_lockline_unlock(); }
            if (a == a1) break;
        }
    }
#ifdef _WIN32
    MemoryBarrier();
    { const uint32_t w = g_spu_put_wake_ea;
      if (w && w - ea < size) WakeByAddressAll(vm_base + w); }
#endif
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}

/* The job context's mailboxes for event delivery: what WrOutMbox /
 * WrOutIntrMbox do on a SPURS job's context (spu_channels.c spu_wrch). One
 * per thread, image/group/spu ids as the job context has them. */
static MN_TLS spu_context* t_evt_ctx;
static spu_context* mn_evt_ctx(int image_id)
{
    if (!t_evt_ctx) {
        t_evt_ctx = (spu_context*)calloc(1, sizeof(spu_context));
        if (!t_evt_ctx) return NULL;
        t_evt_ctx->ls = t_evt_ctx->ls_store;
    }
    t_evt_ctx->image_id = image_id;
    t_evt_ctx->spu_id = 0;
    t_evt_ctx->spu_group_id = 0;
    return t_evt_ctx;
}
static void mn_evt_reset(spu_context* c)
{
    memset(&c->ch_out_mbox, 0, sizeof c->ch_out_mbox);
    memset(&c->ch_out_intr_mbox, 0, sizeof c->ch_out_intr_mbox);
    memset(&c->ch_in_mbox, 0, sizeof c->ch_in_mbox);
}

static MN_TLS int t_image;   /* image of the job being run (event context) */
static void mn_event(mn_job* j, uint32_t value, uint32_t data1)
{
    mn_fx* f = mn_fx_new(j, FX_EVT);
    if (f) { f->ea = value; f->size = data1; f->phase = 0; }
    if (j->dry) return;
    spu_context* c = mn_evt_ctx(t_image);
    if (!c) return;
    spu_channel_write(&c->ch_out_mbox, data1);
    if (g_spu_user_event_hook && g_spu_user_event_hook(c, value)) return;
    spu_channel_write(&c->ch_out_intr_mbox, value);
    if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(c->spu_group_id, c->spu_id, 1, value);
}

/* 0xCE8 sys_spu_thread_throw_event(spup = r3, data0 = r4, data1 = r5). */
static uint32_t mn_throw(mn_job* j, uint32_t r3, uint32_t r4, uint32_t r5)
{
    if ((r3 & 0xFFu) > 63u) { j->sig |= SIG_BADPORT; return E_EINVAL; }
    mn_event(j, (r3 << 24) | 0x40000000u | (r4 & 0x00FFFFFFu), r5);
    return 0;
}

/* 0x538: workload wid's info.arg (the taskset EA), via a 32-byte GET of
 * spurs->wklInfo{1,2}[wid & 15] into LS 0x80. 0 for wid >= 16 unless the
 * kernel's copy of spurs->flags1 (LS 0x174) has 32-workload mode. */
static uint64_t mn_wkl_arg(mn_job* j, uint32_t wid)
{
    const uint32_t spurs_lo = be32(KLS(j, 0x1C4));
    uint32_t off;
    if (wid > 15u) {
        if (!(*KLS(j, 0x174) & 0x40u)) return 0;
        off = 0x1000u + (wid & 15u) * 32u;
    } else {
        off = 0xB00u + (wid & 15u) * 32u;
    }
    const uint32_t ea = spurs_lo + off;   /* EAH is not used by the DMA engine */
    /* The DMA engine refuses an LSA/EA 16-byte phase mismatch (and moves
     * nothing); LS 0x80 then keeps what it held. */
    if (((ea ^ 0x80u) & 15u) == 0) {
        if (j->dry && !mn_ea_ok(ea, 32)) { mn_stop_reject(j, "workload info not committed"); return 0; }
        mn_read(j, ea, KLS(j, 0x80), 32);
    }
    j->sig |= SIG_WKL;
    return be64(KLS(j, 0x88));
}

/* 0xC40: set workload wid's signal bit in spurs->wklSignal1/2 (line 0 of the
 * SPURS instance, through LS 0x100). */
static void mn_wkl_signal(mn_job* j, uint32_t wid)
{
    uint8_t L[128], in[128];
    const uint32_t line = be32(KLS(j, 0x1C4)) & ~127u;
    if (!mn_line_get(j, line, L)) return;
    memcpy(in, L, 128);
    const uint32_t b1 = wid < 16u ? 0x8000u >> wid : 0u;              /* rotma 0x8000, -wid */
    const uint32_t b2 = wid < 32u ? (0x80000000u >> wid) & 0xFFFFu : 0u; /* rotm 0x80000000, -wid */
    wbe16(L + 0x70, be16(L + 0x70) | b1);
    wbe16(L + 0x78, be16(L + 0x78) | b2);
    memcpy(KLS(j, 0x100), L, 128);
    mn_line_put(j, line, in, L);
    j->sig |= SIG_WKL;
}

/* 0x620: cellSpursSendSignal(taskset, task) from the SPU. */
static uint32_t mn_send_signal(mn_job* j, uint64_t taskset, uint32_t task)
{
    const uint32_t lo = (uint32_t)taskset;
    if (!taskset) return E_NULL;
    if (lo & 127u) return E_ALIGN;
    if (task > 127u) return E_INVAL;
    uint8_t L[128], in[128];
    if (!mn_line_get(j, lo, L)) return 0;
    memcpy(in, L, 128);
    memcpy(KLS(j, 0x80), L, 128);
    /* taskset->spurs must be the kernel's SPURS (LS 0x1C0, 64 bits) */
    if (be64(L + 0x60) != be64(KLS(j, 0x1C0))) { mn_line_drop(j); return E_INVAL; }
    const uint32_t wid = be32(L + 0x74);
    if (wid > 31u) { mn_line_drop(j); return E_INVAL; }
    /* bitsets: running 0x00, ready 0x10, pending_ready 0x20, enabled 0x30,
     * signalled 0x40, waiting 0x50; task t is bit 0x80 >> (t & 7) of byte t >> 3 */
    int invalid = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t run = L[i], rdy = L[0x10 + i], pnd = L[0x20 + i], ena = L[0x30 + i], sig = L[0x40 + i], wai = L[0x50 + i];
        if ((((run | rdy | pnd | sig | wai) & (uint8_t)~ena) | (run & wai) | (rdy & pnd)) != 0) invalid = 1;
    }
    const uint32_t byte = task >> 3;
    const uint8_t bit = (uint8_t)(0x80u >> (task & 7u));
    uint32_t ret = E_SRCH;
    if (!invalid && (L[0x30 + byte] & bit)) {
        ret = (L[0x50 + byte] & (uint8_t)~L[0x40 + byte] & bit) ? 1u : 0u;
        L[0x40 + byte] |= bit;
        j->sig |= SIG_SIGNALLED;
    } else {
        j->sig |= SIG_SIG_NOOP;
    }
    memcpy(KLS(j, 0x80), L, 128);
    mn_line_put(j, lo, in, L);   /* PUTLLC even when nothing changed */
    if (ret == 1u) { mn_wkl_signal(j, be32(KLS(j, 0xF4))); return 0; }
    return ret;
}

/* 0x268: release up to n waiters of the wait line at ea. */
static uint32_t mn_ring_release(mn_job* j, uint32_t ea, uint32_t n)
{
    uint8_t L[128], in[128];
    uint16_t taken[MN_TAKE_MAX];
    const uint32_t line = ea & ~127u;
    if (!mn_line_get(j, line, L)) return 0;
    memcpy(in, L, 128);
    const uint32_t head0 = L[0], tail = L[1];
    const uint32_t pos = be16(L + 0x44), size = be16(L + 0x74), count = be32(L + 0x48);
    const uint32_t d = tail - head0;
    const uint32_t avail = head0 > tail ? d + 62u : d;
    const uint32_t sum = pos + n;
    const uint32_t newpos = size > sum ? (sum & 0xFFFFu) : (((sum & 0xFFFFu) - size) & 0xFFFFu);
    const int32_t take = (int32_t)(avail > n ? n : avail);
    if (take > MN_TAKE_MAX) { mn_line_drop(j); mn_stop_reject(j, "more than 8 waiters"); return 0; }
    if (size <= sum) j->sig |= SIG_WRAP;
    for (int32_t i = 0; i < take; i++) {
        const uint32_t head = L[0];
        const uint32_t slot = head > 30u ? head - 30u : head + 1u;
        const uint32_t next = head > 30u ? (head == 61u ? 0u : (head + 1u) & 0xFFu) : (head + 1u) & 0xFFu;
        if (slot > 63u) { mn_line_drop(j); mn_stop_reject(j, "ring head outside the line"); return 0; }
        taken[i] = (uint16_t)be16(L + 2u * slot);
        wbe16(L + 2u * slot, 0xFFFFu);
        L[0] = (uint8_t)next;
    }
    wbe16(L + 0x44, newpos);
    wbe32(L + 0x48, count - n + (uint32_t)take);
    mn_line_put(j, line, in, L);
    if (take > 0) j->sig |= SIG_TAKE;
    for (int32_t i = take - 1; i >= 0; i--) {
        const uint32_t h = taken[i];
        if (h == 0xFFFFu) {
            j->sig |= SIG_RING_PPU;
            (void)mn_throw(j, be32(L + 0x64), 0, 0);
        } else {
            j->sig |= SIG_RING_TASK;
            const uint64_t ts = mn_wkl_arg(j, h >> 8);
            if (MN_STOPPED(j)) return 0;
            const uint32_t rc = mn_send_signal(j, ts & 0xFFFFFFFFull, h & 0xFFu);
            if (MN_STOPPED(j)) return 0;
            if (rc) return rc;
        }
    }
    return 0;
}

/* 0x7F0: SPU-side cellSpursEventFlagSet(ef, bits). */
static uint32_t mn_ef_set(mn_job* j, uint64_t ef, uint32_t bits)
{
    const uint32_t lo = (uint32_t)ef;
    if (!ef) return E_NULL;
    if (lo & 127u) return E_ALIGN;
    uint8_t L[128], in[128];
    if (!mn_line_get(j, lo, L)) return 0;
    memcpy(in, L, 128);
    memcpy(KLS(j, 0x80), L, 128);
    const int32_t dir = (int8_t)L[0x0E];
    if ((uint32_t)dir > 3u || dir == 2) { mn_line_drop(j); return E_PERM; }
    j->sig |= SIG_EF_DIR0 << dir;
    uint32_t ev = (be16(L) | bits) & 0xFFFFu, clear = 0, met = 0;
    if (dir == 0 || dir == 3) {                       /* SPU receivers */
        const uint32_t used = be16(L + 0x08) & ~be16(L + 0x02) & 0xFFFFu, mode = be16(L + 0x0A);
        for (uint32_t s = 0; s < 16u; s++) {
            const uint32_t bit = 0x8000u >> s;
            if (!(used & bit)) continue;
            const uint32_t mask = be16(L + 0x10 + 2u * s), rel = ev & mask;
            if (rel == mask || (!(mode & bit) && rel != 0)) {
                met |= bit;
                clear |= rel;
                wbe16(L + 0x30 + 2u * s, be16(L + 0x30 + 2u * s) | rel);
            }
        }
        wbe16(L + 0x02, be16(L + 0x02) | met);
        if (met) j->sig |= SIG_EF_SPU;
    }
    int ppu = 0;
    if (dir == 1 || dir == 3) {                       /* PPU receiver */
        const uint32_t pmask = be16(L + 0x04);
        if (pmask) {
            const uint32_t prel = pmask & ev;
            if ((pmask & ~prel) == 0 || ((L[0x06] & 15u) == 0 && prel != 0)) {
                ppu = 1;
                clear |= prel;
                wbe16(L + 0x30 + 2u * (L[0x06] >> 4), prel);
                L[0x07] = 1;
                wbe16(L + 0x04, 0);
                j->sig |= SIG_EF_PPU;
            }
        }
    }
    if (L[0x0F] == 0) {                               /* CELL_SPURS_EVENT_FLAG_CLEAR_AUTO */
        if (ev & clear) j->sig |= SIG_EF_CLEAR;
        ev &= ~clear;
    }
    wbe16(L, ev);
    memcpy(KLS(j, 0x80), L, 128);
    mn_line_put(j, lo, in, L);
    /* the job works from registers loaded right after the commit */
    uint8_t C[128];
    memcpy(C, L, 128);
    uint64_t taskset = be64(C + 0x70);
    if (C[0x0D]) j->sig |= met ? SIG_IWL : 0;
    for (uint32_t s = 0; s < 16u && met; s++) {
        if (!(met & (0x8000u >> s))) continue;
        if (C[0x0D]) {                                /* isIwl: taskset from the workload */
            const uint32_t wid = C[0x60 + s];
            if (wid > 31u) { mn_stop_halt(j, 0xB6C); return 0; }
            taskset = mn_wkl_arg(j, wid);
            if (MN_STOPPED(j)) return 0;
        }
        const uint32_t rc = mn_send_signal(j, taskset, C[0x50 + s]);
        if (MN_STOPPED(j)) return 0;
        if (rc == E_INVAL || rc == E_STAT) return E_FATAL;
        if (rc) { mn_stop_halt(j, 0xBC0); return 0; }
    }
    if (ppu) (void)mn_throw(j, be32(C + 0x09), 0, 0);
    return 0;
}

/* The job's LS layout (spurs_job.c staging, _cellSpursCheckJob arithmetic):
 * the scratch buffer and how much of it the job may fill. */
typedef struct { uint32_t s_ls, scr_bytes, dsz; } mn_layout;

/* main (0x30). D = descriptor. */
static void mn_notify_job(mn_job* j, const uint8_t* D, const mn_layout* lay)
{
    const uint32_t nlist = be16(D + 0x60);
    const uint32_t fill = (be16(D + 0x62) >> 4) * 16u;   /* scratch bytes set to the pattern */
    if (fill > lay->scr_bytes) { mn_stop_reject(j, "scratch fill beyond the scratch buffer"); return; }
    if (0x30u + 8u * nlist > lay->dsz) { mn_stop_reject(j, "MFC list beyond the descriptor"); return; }
    /* PUTL (cmd 0x24) of the scratch buffer over the list; LS cursor per
     * element: (cursor & ~15) | (ea & 15), then advanced by the rounded size. */
    uint32_t cur = 0;
    for (uint32_t k = 0; k < nlist; k++) {
        const uint32_t w0 = be32(D + 0x30 + 8u * k), ea = be32(D + 0x34 + 8u * k);
        const uint32_t size = w0 & 0x7FFFu;
        if (w0 >> 31) { mn_stop_reject(j, "stall-and-notify list element"); return; }
        if (!size) continue;
        const uint32_t at = (cur & ~15u) | (ea & 15u);
        if (size > 0x4000u || (size >= 16u && (size & 15u)) || ea < 0x10000u) { mn_stop_reject(j, "malformed list element"); return; }
        if (at + size > fill || lay->s_ls + at + size > SPU_LS_SIZE) { mn_stop_reject(j, "list element beyond the filled scratch"); return; }
        if (j->dry && !mn_ea_ok(ea, size)) { mn_stop_reject(j, "list element EA not committed"); return; }
        mn_put(j, ea, size, at);
        if (MN_STOPPED(j)) return;
        j->sig |= SIG_PUTL;
        cur = (cur & ~15u) + ((size + 15u) & ~15u);
    }
    uint32_t rc = mn_ring_release(j, be32(D + 0x68), be16(D + 0x64));
    if (MN_STOPPED(j)) return;
    if (rc) { mn_stop_halt(j, 0x16C); return; }
    const uint32_t multi = be16(D + 0x72), base = be32(D + 0x6C);
    for (uint32_t i = 0; i < 16u; i++) {
        const uint32_t bit = 1u << (15u - i);
        if (!(multi & bit)) continue;
        j->sig |= SIG_EF_MULTI;
        rc = mn_ef_set(j, (uint64_t)(uint32_t)(base + 128u * (i + 1u)), bit);
        if (MN_STOPPED(j)) return;
        if (rc) { mn_stop_halt(j, 0x1E8); return; }
    }
    const uint32_t single = be16(D + 0x70);
    if (single) {
        j->sig |= SIG_EF_SINGLE;
        rc = mn_ef_set(j, (uint64_t)base, single);
        if (MN_STOPPED(j)) return;
        if (rc) { mn_stop_halt(j, 0x234); return; }
    }
}

/* ---- staging view -------------------------------------------------------- */
static void mn_stage(mn_job* j, int dry, const uint8_t* bin, uint32_t size_bin, uint32_t spu_num)
{
    j->dry = dry; j->halted = 0; j->reject = 0; j->locked = 0; j->halt_pc = 0; j->why = NULL;
    j->sig = 0; j->nfx = 0; j->overflow = 0;
    memset(j->kls, 0, sizeof j->kls);
    if (size_bin > 0x80u) memcpy(j->kls, bin + 0x80, size_bin - 0x80u < sizeof j->kls ? size_bin - 0x80u : sizeof j->kls);
    /* spurs_job.c's kernel-area overlays */
    if (size_bin >= 0x3C && !memcmp(bin + 0x30, "JOBCRT Ver13", 12)) {
        memset(KLS(j, 0x1C0), 0, 0x10);
        wbe32(KLS(j, 0x1C8), spu_num); wbe32(KLS(j, 0x1CC), 20u);
        memset(KLS(j, 0x170), 0, 0x10);
    } else {
        static const uint8_t helper[12] = { 0x30,0x80,0x38,0x03, 0x3F,0x82,0x01,0x83, 0x35,0x00,0x00,0x00 };
        for (uint32_t o = 0; o + 12u <= size_bin; o += 4)
            if (!memcmp(bin + o, helper, 12)) { wbe32(KLS(j, 0x1C8), spu_num); break; }
    }
}

static int mn_layout_of(const uint8_t* D, uint32_t desc_size, mn_layout* lay)
{
    if (desc_size < 0x80u) return 0;
    const uint32_t size_bin = be16(D + 0x08) << 4, size_io = be32(D + 0x14), size_out = be32(D + 0x18);
    const uint32_t size_scr = be16(D + 0x1E) << 4;
    if (!size_bin || size_io > SPU_LS_SIZE || size_out > SPU_LS_SIZE) return 0;
    lay->s_ls = MN_ALIGN1024(size_bin) + MN_ALIGN1024(size_io) + MN_ALIGN1024(size_out);
    lay->scr_bytes = size_scr;
    lay->dsz = desc_size > 0x400u ? 0x400u : desc_size;
    return lay->s_ls + size_scr < SPU_LS_SIZE - 0x440u;
}

/* ---- trust ---------------------------------------------------------------- */
typedef struct { volatile LONG key; volatile LONG ok; volatile LONG bad; } mn_trust_slot;
static mn_trust_slot s_trust[256];
static mn_trust_slot* mn_trust(int image_id, uint32_t sig)
{
    const LONG key = (LONG)(((uint32_t)(image_id - 10000) << 24 ^ sig) | 0x80000000u);
    uint32_t h = ((uint32_t)key * 2654435761u) >> 24;
    for (int probe = 0; probe < 32; probe++, h = (h + 1u) & 255u) {
        const LONG k = s_trust[h].key;
        if (k == key) return &s_trust[h];
        if (k == 0 && InterlockedCompareExchange(&s_trust[h].key, key, 0) == 0) return &s_trust[h];
        if (s_trust[h].key == key) return &s_trust[h];
    }
    return NULL;
}

static void mn_sig_text(uint32_t sig, char* t, size_t n)
{
    static const char* const name[] = { "putl", "take", "ring-ppu", "ring-task", "ef-multi", "ef-single",
        "ef-spu", "ef-ppu", "signalled", "wkl", "iwl", "wrap", "dir0", "dir1", "dir2", "dir3",
        "bad-port", "ef-clear", "signal-noop" };
    size_t l = 0; t[0] = 0;
    for (unsigned b = 0; b < sizeof name / sizeof name[0] && l + 16 < n; b++)
        if (sig >> b & 1u) l += (size_t)snprintf(t + l, n - l, "%s%s", l ? "," : "", name[b]);
    if (!l) snprintf(t, n, "none");
}

/* ---- statistics ----------------------------------------------------------- */
enum { ST_NATIVE, ST_SPU_OFF, ST_SPU_HALT, ST_SPU_REJECT, ST_SPU_SHADOW, ST_SPU_UNTRUSTED, ST_SPU_GRAPHICS,
       ST_FAILED, ST_COMPARED, ST_MISMATCH, ST_SKIPPED, ST_DIVERGED, ST_COUNT };
static volatile LONG s_stat[ST_COUNT];
static volatile LONG s_jobs;
static void mn_stat(int k) { InterlockedIncrement(&s_stat[k]); }
static void mn_stat_line(void)
{
    fprintf(stderr, "[notify-native] jobs=%ld native=%ld failed=%ld spu: off %ld halt %ld reject %ld shadow %ld untrusted %ld graphics(10002) %ld"
                    " | compared=%ld mismatched=%ld not-comparable=%ld diverged=%ld\n",
            (long)s_jobs, (long)s_stat[ST_NATIVE], (long)s_stat[ST_FAILED], (long)s_stat[ST_SPU_OFF], (long)s_stat[ST_SPU_HALT],
            (long)s_stat[ST_SPU_REJECT], (long)s_stat[ST_SPU_SHADOW], (long)s_stat[ST_SPU_UNTRUSTED], (long)s_stat[ST_SPU_GRAPHICS],
            (long)s_stat[ST_COMPARED], (long)s_stat[ST_MISMATCH], (long)s_stat[ST_SKIPPED], (long)s_stat[ST_DIVERGED]);
}

/* ---- switches -------------------------------------------------------------- */
static LONG s_on = -1, s_verify_all = -1;
static int mn_enabled(void)
{
    if (s_on < 0) {
        const char* e = getenv("NOTIFY_NATIVE");
        const char* v = getenv("NOTIFY_VERIFY");
        s_verify_all = v && *v && *v != '0';
        s_on = !(e && *e == '0');
        fprintf(stderr, "[notify-native] %s%s\n", s_on ? "on" : "off (NOTIFY_NATIVE=0)",
                s_on && s_verify_all ? ", NOTIFY_VERIFY=1: every job on the SPU, compared" : "");
    }
    if (!s_on) return 0;
#ifdef _WIN32
    {   /* runtime A/B: a no_notify_native file sends every job to the SPU */
        static volatile ULONGLONG checked; static volatile LONG file_off;
        const ULONGLONG now = GetTickCount64();
        if (now - checked > 1000) {
            checked = now;
            const LONG off = GetFileAttributesA("no_notify_native") != INVALID_FILE_ATTRIBUTES;
            if (off != file_off) fprintf(stderr, "[notify-native] %s by no_notify_native file\n", off ? "OFF" : "ON");
            file_off = off;
        }
        if (file_off) return 0;
    }
#endif
    return 1;
}

/* ---- shadow capture -------------------------------------------------------- */
typedef struct mn_cap {
    uint8_t kind;               /* FX_PUT / FX_LINE / FX_EVT */
    uint32_t ea, size;          /* as mn_fx */
    uint32_t off;               /* data in the capture pool */
    int have_in;                /* LINE: the GETLLAR it committed against is at off + 128 */
} mn_cap;
typedef struct mn_shadow {
    int armed, capturing, evt_ok;
    mn_trust_slot* slot;
    mn_job* job;                /* the dry run, kept for the comparison */
    mn_cap rec[MN_FX_MAX];
    uint32_t n;
    int overflow;
    uint8_t* pool; uint32_t npool, cap;
    uint32_t llar_ea[8]; uint8_t llar[8][128]; uint32_t nllar;
} mn_shadow;
static MN_TLS mn_shadow* t_sh;
static MN_TLS mn_job *t_dry, *t_live;

static uint8_t* mn_pool(mn_shadow* s, uint32_t need)
{
    if (s->npool + need > s->cap) {
        uint32_t c = s->cap ? s->cap : 4096u;
        while (s->npool + need > c) c *= 2u;
        uint8_t* p = (uint8_t*)realloc(s->pool, c);
        if (!p) return NULL;
        s->pool = p; s->cap = c;
    }
    uint8_t* r = s->pool + s->npool;
    s->npool += need;
    return r;
}
static mn_cap* mn_cap_new(mn_shadow* s)
{
    if (s->n >= MN_FX_MAX) { s->overflow = 1; return NULL; }
    return &s->rec[s->n++];
}

int macross_notify_shadow_active(void) { return t_sh && t_sh->armed; }

void macross_notify_cap_get(uint32_t ea, uint32_t lsa, const void* data, uint32_t size)
{
    (void)lsa;
    mn_shadow* s = t_sh;
    if (!s || !s->capturing || !(ea & 0x80000000u) || size != 128u) return;   /* GETLLARs only */
    const uint32_t line = ea & 0x7FFFFF80u;
    uint32_t k = 0;
    while (k < s->nllar && s->llar_ea[k] != line) k++;
    if (k == s->nllar) { if (s->nllar < 8) s->nllar++; else k = 7; }
    s->llar_ea[k] = line;
    memcpy(s->llar[k], data, 128);
}

void macross_notify_cap_put(uint32_t ea, const void* data, uint32_t size)
{
    mn_shadow* s = t_sh;
    if (!s || !s->capturing) return;
    mn_cap* c = mn_cap_new(s);
    if (!c) return;
    const int atomic = (ea & 0x80000000u) != 0;
    c->kind = atomic ? FX_LINE : FX_PUT;
    c->ea = atomic ? (ea & 0x7FFFFF80u) : ea;
    c->size = size;
    c->have_in = 0;
    uint8_t* p = mn_pool(s, atomic ? 256u : size);
    if (!p) { s->overflow = 1; s->n--; return; }
    c->off = (uint32_t)(p - s->pool);
    memcpy(p, data, size);
    if (atomic && size == 128u)
        for (uint32_t k = 0; k < s->nllar; k++)
            if (s->llar_ea[k] == c->ea) { memcpy(p + 128, s->llar[k], 128); c->have_in = 1; break; }
}

/* Events from the SPU job being shadowed: the runtime's user-event hook,
 * wrapped once (it is installed at syscall registration and never changes). */
static int (*s_evt_orig)(spu_context*, uint32_t);
static int mn_evt_wrap(spu_context* spu, uint32_t value)
{
    mn_shadow* s = t_sh;
    if (s && s->capturing) {
        mn_cap* c = mn_cap_new(s);
        if (c) {
            c->kind = FX_EVT; c->ea = value;
            c->size = spu->ch_out_mbox.count ? spu->ch_out_mbox.value : 0xFFFFFFFFu;
            c->off = 0; c->have_in = 0;
        }
    }
    return s_evt_orig ? s_evt_orig(spu, value) : 0;
}
#ifdef _WIN32
static INIT_ONCE s_wrap_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK mn_wrap_init(PINIT_ONCE a, PVOID b, PVOID* c)
{
    (void)a; (void)b; (void)c;
    if (g_spu_user_event_hook && g_spu_user_event_hook != mn_evt_wrap) {
        s_evt_orig = g_spu_user_event_hook;
        g_spu_user_event_hook = mn_evt_wrap;
    }
    return TRUE;
}
#endif
static int mn_evt_capturable(void)
{
#ifdef _WIN32
    InitOnceExecuteOnce(&s_wrap_once, mn_wrap_init, NULL, NULL);
#endif
    return s_evt_orig != NULL && g_spu_user_event_hook == mn_evt_wrap;
}

/* 1 match, 0 mismatch (why), -1 not comparable (a line changed between the
 * dry run and the SPU's own GETLLAR, or the capture overflowed). */
static int mn_compare(const mn_shadow* s, const mn_job* j, char* why, size_t wn)
{
    if (s->overflow || j->overflow) { snprintf(why, wn, "capture overflow"); return -1; }
    /* A lock line the SPU reserved with other contents than the dry run saw
     * means another agent wrote it in between: the two runs saw different
     * inputs and cannot be compared. */
    for (uint32_t i = 0; i < s->n; i++) {
        const mn_cap* c = &s->rec[i];
        if (c->kind != FX_LINE || !c->have_in) continue;
        for (uint32_t k = 0; k < j->nfx; k++)
            if (j->fx[k].kind == FX_LINE && j->fx[k].ea == c->ea) {
                if (memcmp(j->fx[k].in, s->pool + c->off + 128, 128)) { snprintf(why, wn, "line %08X changed under the job", c->ea); return -1; }
                break;
            }
    }
    uint32_t a = 0, b = 0;
    for (;; a++, b++) {
        while (b < s->n && s->rec[b].kind == FX_EVT && !s->evt_ok) b++;
        while (a < j->nfx && j->fx[a].kind == FX_EVT && !s->evt_ok) a++;
        if (a >= j->nfx || b >= s->n) break;
        const mn_fx* f = &j->fx[a];
        const mn_cap* c = &s->rec[b];
        static const char* const kn[] = { "?", "PUT", "PUTLLC", "event" };
        if (f->kind != c->kind) { snprintf(why, wn, "effect %u: native %s, SPU %s", a, kn[f->kind], kn[c->kind]); return 0; }
        if (f->kind == FX_PUT) {
            if (f->ea != c->ea || f->size != c->size) { snprintf(why, wn, "PUT %u: native %08X/%u, SPU %08X/%u", a, f->ea, f->size, c->ea, c->size); return 0; }
            for (uint32_t i = 0; i < f->size; i++)
                if (s->pool[c->off + i] != mn_pattern(f->phase + i)) { snprintf(why, wn, "PUT %08X byte %u differs", f->ea, i); return 0; }
        } else if (f->kind == FX_LINE) {
            if (f->ea != c->ea || c->size != 128u) { snprintf(why, wn, "PUTLLC %u: native line %08X, SPU %08X", a, f->ea, c->ea); return 0; }
            for (uint32_t i = 0; i < 128u; i++)
                if (f->out[i] != s->pool[c->off + i]) { snprintf(why, wn, "PUTLLC %08X byte 0x%02X: native %02X, SPU %02X", f->ea, i, f->out[i], s->pool[c->off + i]); return 0; }
        } else {
            if (f->ea != c->ea || f->size != c->size) { snprintf(why, wn, "event %u: native %08X/%08X, SPU %08X/%08X", a, f->ea, f->size, c->ea, c->size); return 0; }
        }
    }
    if (a < j->nfx || b < s->n) { snprintf(why, wn, "native %u effects, SPU %u", j->nfx, s->n); return 0; }
    return 1;
}

void macross_notify_job_after_spu(int image_id, int failed)
{
    mn_shadow* s = t_sh;
    if (!s || !s->armed) return;
    s->armed = 0; s->capturing = 0;
    if (failed) return;
    char why[200] = "";
    const int r = mn_compare(s, s->job, why, sizeof why);
    char text[200];
    mn_sig_text(s->job->sig, text, sizeof text);
    if (r < 0) { mn_stat(ST_SKIPPED); return; }
    mn_stat(ST_COMPARED);
    if (r == 0) mn_stat(ST_MISMATCH);
    if (s->slot) {
        if (r) {
            const LONG ok = InterlockedIncrement(&s->slot->ok);
            if (ok <= MN_TRUST_AFTER)
                fprintf(stderr, "[notify-native] image=%d path=%05X (%s) matches the SPU %ld/%d%s\n", image_id, s->job->sig, text,
                        (long)ok, MN_TRUST_AFTER, ok == MN_TRUST_AFTER ? " -- native from now on" : "");
        } else if (InterlockedIncrement(&s->slot->bad) == 1) {
            fprintf(stderr, "[notify-native] image=%d path=%05X (%s) MISMATCH -- kept on the SPU: %s\n", image_id, s->job->sig, text, why);
        }
    } else if (!r) {   /* NOTIFY_VERIFY=1 */
        static volatile LONG seen;
        if (InterlockedIncrement(&seen) <= 16)
            fprintf(stderr, "[notify-native] image=%d path=%05X (%s) MISMATCH: %s\n", image_id, s->job->sig, text, why);
    }
}

/* ---- entry ------------------------------------------------------------------ */
static mn_job* mn_job_buf(mn_job** p)
{
    if (!*p) *p = (mn_job*)malloc(sizeof(mn_job));
    return *p;
}

int macross_notify_job_native(int image_id, uint32_t job_ea, const uint8_t* desc, uint32_t desc_size,
                              uint32_t n_dma, uint32_t spu_num, uint32_t ea_bin, uint32_t size_bin,
                              uint32_t* mbox, uint32_t* mbox_intr)
{
    (void)n_dma;   /* the notify job reads its descriptor directly, not its input list */
    if (mbox) *mbox = 0;
    if (mbox_intr) *mbox_intr = 0;
    if (t_sh) { t_sh->armed = 0; t_sh->capturing = 0; }
    const LONG total = InterlockedIncrement(&s_jobs);
    if ((total & 0xFFF) == 0) mn_stat_line();
    if (image_id == MN_IMAGE_GRAPHICS) { mn_stat(ST_SPU_GRAPHICS); return MN_SPU; }   /* Edge Animation: not native */
    if (image_id != MN_IMAGE_NOTIFY) return MN_SPU;
    if (!mn_enabled()) { mn_stat(ST_SPU_OFF); return MN_SPU; }
    mn_layout lay;
    if (!desc || !mn_layout_of(desc, desc_size, &lay) || size_bin < 0x1D0u || !mn_ea_ok(ea_bin, size_bin)) {
        mn_stat(ST_SPU_REJECT); return MN_SPU;
    }
    mn_job* dry = mn_job_buf(&t_dry);
    if (!dry) { mn_stat(ST_SPU_REJECT); return MN_SPU; }
    t_image = image_id;

    /* 1. dry run: what the job would do, with no side effects */
    mn_stage(dry, 1, vm_base + ea_bin, size_bin, spu_num);
    mn_notify_job(dry, desc, &lay);
    if (dry->halted) { mn_stat(ST_SPU_HALT); return MN_SPU; }   /* the SPU fails it the same way */
    if (dry->reject) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 8)
            fprintf(stderr, "[notify-native] image=%d job=%08X on the SPU: %s\n", image_id, job_ea, dry->why);
        mn_stat(ST_SPU_REJECT);
        return MN_SPU;
    }

    /* 2. trusted path: run it for real; otherwise shadow the SPU job */
    mn_trust_slot* slot = s_verify_all ? NULL : mn_trust(image_id, dry->sig);
    if (slot && slot->bad == 0 && slot->ok >= MN_TRUST_AFTER) {
        mn_job* live = mn_job_buf(&t_live);
        spu_context* ec = mn_evt_ctx(image_id);
        if (!live || !ec) { mn_stat(ST_SPU_REJECT); return MN_SPU; }
        mn_evt_reset(ec);
        mn_stage(live, 0, vm_base + ea_bin, size_bin, spu_num);
        mn_notify_job(live, desc, &lay);
        mn_line_drop(live);   /* never held here; defensive */
        if (live->sig != dry->sig) {
            mn_stat(ST_DIVERGED);
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 8)
                fprintf(stderr, "[notify-native] image=%d job=%08X: memory changed between the dry and the live run, path %05X -> %05X\n",
                        image_id, job_ea, dry->sig, live->sig);
        }
        if (mbox) *mbox = ec->ch_out_mbox.count ? ec->ch_out_mbox.value : 0;
        if (mbox_intr) *mbox_intr = ec->ch_out_intr_mbox.count ? ec->ch_out_intr_mbox.value : 0;
        if (live->halted || live->reject) {
            mn_stat(ST_FAILED);
            fprintf(stderr, "[notify-native] image=%d job=%08X stopped after %u effects (%s pc=0x%05X)\n", image_id, job_ea,
                    live->nfx, live->halted ? "assert" : live->why, live->halt_pc);
            return MN_FAILED;
        }
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            fprintf(stderr, "[notify-native] image=%d job=%08X native: path %05X, %u effects\n", image_id, job_ea, live->sig, live->nfx);
        mn_stat(ST_NATIVE);
        return MN_DONE;
    }
    if (!slot && !s_verify_all) { mn_stat(ST_SPU_UNTRUSTED); return MN_SPU; }   /* trust table full */
    if (slot && slot->bad) { mn_stat(ST_SPU_UNTRUSTED); return MN_SPU; }
    if (!t_sh) { t_sh = (mn_shadow*)calloc(1, sizeof(mn_shadow)); if (!t_sh) { mn_stat(ST_SPU_UNTRUSTED); return MN_SPU; } }
    mn_shadow* s = t_sh;
    s->evt_ok = mn_evt_capturable();
    s->slot = slot;
    s->job = dry;
    s->n = 0; s->npool = 0; s->nllar = 0; s->overflow = 0;
    s->armed = 1; s->capturing = 1;
    mn_stat(ST_SPU_SHADOW);
    return MN_SPU;
}
