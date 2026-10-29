/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include <time.h>
#include <stddef.h>
#include "spu_coherency.h"
#include "spu_workload.h"
#include "spu_context.h"
#include "../platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "ps3emu/log_first.h"
#include "spurs_job_stage_perf.h"
#include "edge_job_perf.h"
#include "spurs_job_snapshot.h"

/* SPURS_JOB_DIAG_CONFIG_BEGIN
 * Process-start options only; no mutable gate-file state is cached.
 * Preserve presence semantics: empty and "0" still enable DUMP/DESCDUMP. */
typedef struct spurs_job_diag_options {
    int descdump_present, descdump_level, dump_present;
    int probe_image_present, probe_image, probe_only;
} spurs_job_diag_options;
static INIT_ONCE job_diag_once = INIT_ONCE_STATIC_INIT;
static int job_diag_ready;
static spurs_job_diag_options job_diag_values;
static BOOL WINAPI job_diag_initialize(PINIT_ONCE a, PVOID b, PVOID* c)
{
    (void)a; (void)b; (void)c;
    const char* value = getenv("SPURS_JOB_DESCDUMP");
    job_diag_values.descdump_present = value != NULL;
    job_diag_values.descdump_level = value ? atoi(value) : 0;
    value = getenv("SPURS_JOB_DUMP");
    job_diag_values.dump_present = value != NULL;
    value = getenv("SPURS_JOB_PROBE_IMAGE");
    job_diag_values.probe_image_present = value != NULL;
    job_diag_values.probe_image = value ? atoi(value) : 0;
    value = getenv("SPURS_JOB_PROBE_ONLY");
    job_diag_values.probe_only = value && atoi(value) != 0;
    __atomic_store_n(&job_diag_ready, 1, __ATOMIC_RELEASE);
    return TRUE;
}
static const spurs_job_diag_options* job_diag_config(void)
{
    if (!__atomic_load_n(&job_diag_ready, __ATOMIC_ACQUIRE))
        InitOnceExecuteOnce(&job_diag_once, job_diag_initialize, NULL, NULL);
    return &job_diag_values;
}
/* SPURS_JOB_DIAG_CONFIG_END */

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * The job context is a stack local, so by the time the PPU looks there is
 * nothing to read and every poll fails. Retain the store and let the syscall
 * resolve a chain handle to it. */
#if defined(_MSC_VER)
static __declspec(thread) uint32_t s_job_input_handle;
#else
static __thread uint32_t s_job_input_handle;
#endif
uint32_t spurs_job_set_ls_handle(uint32_t handle)
{
    uint32_t previous = s_job_input_handle;
    s_job_input_handle = handle;
    return previous;
}
void spurs_job_retire_ls(uint32_t handle)
{
    (void)sjs_retire(handle);
}
int spurs_job_read_ls(uint32_t handle, uint32_t offset, uint8_t* out, uint32_t size)
{
    return sjs_read(handle, offset, out, size);
}

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* Read by the drain loop's stuck-job report (spu_drain.c). */
SPURS_JOB_TLS uint64_t g_spurs_job_start_qpc; SPURS_JOB_TLS uint32_t g_spurs_job_cur_ea;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }

/* Direct staging keeps the existing stack-local context and LS ownership.
 * The default path remains available for differential validation. */
#ifdef _WIN32
static INIT_ONCE direct_stage_once = INIT_ONCE_STATIC_INIT;
static int direct_stage_on;
static BOOL CALLBACK direct_stage_init(PINIT_ONCE a, PVOID b, PVOID* c)
{
    (void)a; (void)b; (void)c;
    const char* e = getenv("SPURS_JOB_DIRECT_STAGE");
    direct_stage_on = e && strcmp(e, "1") == 0;
    return TRUE;
}
static int direct_stage_enabled(void)
{
    InitOnceExecuteOnce(&direct_stage_once, direct_stage_init, NULL, NULL);
    return direct_stage_on;
}
#else
static int direct_stage_enabled(void) { return 0; }
#endif

/* Per-thread job contexts, one per nesting depth. A real SPU does not clear
 * local store between jobs; zeroing a fresh 256 KB stack context per job was
 * ~15% of a geometry worker (memset + page traffic). Registers and state are
 * still reset each job; the binary and DMA inputs are copied in as before. */
#define SPURS_JOB_CTX_DEPTHS 4u
static SPURS_JOB_TLS unsigned t_job_ctx_depth;
static SPURS_JOB_TLS spu_context* t_job_ctx[SPURS_JOB_CTX_DEPTHS];
static spu_context* spurs_job_ctx_slot(spu_context* fallback)
{
    const unsigned d = t_job_ctx_depth;
    if (d >= SPURS_JOB_CTX_DEPTHS) return fallback;
    if (!t_job_ctx[d]) {
#ifdef _WIN32
        t_job_ctx[d] = (spu_context*)VirtualAlloc(NULL, sizeof(spu_context), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        t_job_ctx[d] = (spu_context*)aligned_alloc(4096, (sizeof(spu_context) + 4095) & ~(size_t)4095);
        if (t_job_ctx[d]) memset(t_job_ctx[d], 0, sizeof(spu_context));
#endif
        if (!t_job_ctx[d]) return fallback;
    }
    return t_job_ctx[d];
}
static void spurs_job_ctx_reset(spu_context* c, const spu_context* fallback)
{
    if (c == fallback) { spu_context_init(c, 0); return; }
    const size_t ls_at = offsetof(spu_context, ls_store), ls_end = ls_at + SPU_LS_SIZE;
    memset(c, 0, ls_at);
    memset((char*)c + ls_end, 0, sizeof(*c) - ls_end);
    c->ls = c->ls_store;
    c->spu_id = 0;
    c->status = SPU_STATUS_STOPPED;
}
/* Edge job capture (edge_cap file in the working directory): for the next
 * 1,500 Edge geometry jobs (any geom image) write edgecap/<img>_<n>.bin
 *   header  "EDGECAP1", u32 job_ea, u32 image, u32 desc_size, desc bytes,
 *           u32 n_dma, n_dma x {u32 size, u32 ea, bytes}
 *   records 'G' u32 ea, u32 lsa, u32 size, bytes   (GET, incl. GETLLAR|0x80000000)
 *           'P' u32 ea, u32 size, bytes            (PUT, incl. PUTLLC |0x80000000)
 * Ground truth for a native reimplementation of the Edge job. */
__declspec(thread) int g_job_cap_on;
static __declspec(thread) FILE* t_cap_file;
/* Title-native SPURS jobs. Macross links native/macross30/spu_notify_native.c
 * (images 10001/10002); a title that does not gets these defaults, which keep
 * every job on the SPU (/alternatename as spu_coherency.c does for
 * ppu_resv_break_line; weak definitions elsewhere). */
int  macross_notify_job_native(int, uint32_t, const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t*, uint32_t*);
int  macross_notify_shadow_active(void);
void macross_notify_cap_get(uint32_t, uint32_t, const void*, uint32_t);
void macross_notify_cap_put(uint32_t, const void*, uint32_t);
void macross_notify_job_after_spu(int, int);
#if defined(_MSC_VER)
int  macross_notify_job_native_absent(int i, uint32_t e, const uint8_t* d, uint32_t ds, uint32_t n, uint32_t s,
                                      uint32_t b, uint32_t bs, uint32_t* m, uint32_t* mi)
{ (void)i; (void)e; (void)d; (void)ds; (void)n; (void)s; (void)b; (void)bs; (void)m; (void)mi; return 0; }
int  macross_notify_shadow_active_absent(void) { return 0; }
void macross_notify_cap_get_absent(uint32_t e, uint32_t l, const void* d, uint32_t s) { (void)e; (void)l; (void)d; (void)s; }
void macross_notify_cap_put_absent(uint32_t e, const void* d, uint32_t s) { (void)e; (void)d; (void)s; }
void macross_notify_job_after_spu_absent(int i, int f) { (void)i; (void)f; }
#pragma comment(linker, "/alternatename:macross_notify_job_native=macross_notify_job_native_absent")
#pragma comment(linker, "/alternatename:macross_notify_shadow_active=macross_notify_shadow_active_absent")
#pragma comment(linker, "/alternatename:macross_notify_cap_get=macross_notify_cap_get_absent")
#pragma comment(linker, "/alternatename:macross_notify_cap_put=macross_notify_cap_put_absent")
#pragma comment(linker, "/alternatename:macross_notify_job_after_spu=macross_notify_job_after_spu_absent")
#else
__attribute__((weak)) int macross_notify_job_native(int i, uint32_t e, const uint8_t* d, uint32_t ds, uint32_t n, uint32_t s,
                                                    uint32_t b, uint32_t bs, uint32_t* m, uint32_t* mi)
{ (void)i; (void)e; (void)d; (void)ds; (void)n; (void)s; (void)b; (void)bs; (void)m; (void)mi; return 0; }
__attribute__((weak)) int  macross_notify_shadow_active(void) { return 0; }
__attribute__((weak)) void macross_notify_cap_get(uint32_t e, uint32_t l, const void* d, uint32_t s) { (void)e; (void)l; (void)d; (void)s; }
__attribute__((weak)) void macross_notify_cap_put(uint32_t e, const void* d, uint32_t s) { (void)e; (void)d; (void)s; }
__attribute__((weak)) void macross_notify_job_after_spu(int i, int f) { (void)i; (void)f; }
#endif
void spurs_job_cap_get(uint32_t ea, uint32_t lsa, const void* data, uint32_t size)
{
    macross_notify_cap_get(ea, lsa, data, size);   /* notify-job shadow capture (no-op unless armed) */
    if (!t_cap_file) return;
    fputc('G', t_cap_file); fwrite(&ea, 4, 1, t_cap_file); fwrite(&lsa, 4, 1, t_cap_file);
    fwrite(&size, 4, 1, t_cap_file); fwrite(data, 1, size, t_cap_file);
}
static void edge_skin_rec_put(uint32_t ea, const void* data, uint32_t size);
static void edge_anim_rec_put(uint32_t ea, const void* data, uint32_t size);   /* image 10006 shadow, below */
void spurs_job_cap_put(uint32_t ea, const void* data, uint32_t size)
{
    edge_anim_rec_put(ea, data, size);
    edge_skin_rec_put(ea, data, size);
    macross_notify_cap_put(ea, data, size);        /* notify-job shadow capture (no-op unless armed) */
    if (!t_cap_file) return;
    fputc('P', t_cap_file); fwrite(&ea, 4, 1, t_cap_file);
    fwrite(&size, 4, 1, t_cap_file); fwrite(data, 1, size, t_cap_file);
}
static FILE* spurs_job_cap_open(int image_id, uint32_t job_ea, uint32_t dsz, uint32_t n_dma, uint32_t dma_base)
{
    /* Edge geometry images: geom1 10003, geom3 10004, geom2 10005, geom6 10011,
     * geom7 10012, 10013, geom5 10014 (not the anim/JOBCRT images 10006-10010). */
    if (image_id < 10003 || image_id > 10014 || (image_id >= 10006 && image_id <= 10010)) return NULL;
    static volatile LONG left, seq; static ULONGLONG chk;
    if (left <= 0) { const ULONGLONG now = GetTickCount64();
        if (now - chk < 300) return NULL;
        chk = now;
        if (GetFileAttributesA("edge_cap") == INVALID_FILE_ATTRIBUTES) return NULL;
        DeleteFileA("edge_cap"); CreateDirectoryA("edgecap", NULL); left = 1500; }
    if (InterlockedDecrement(&left) < 0) return NULL;
    char path[96];
    snprintf(path, sizeof path, "edgecap/%d_%03ld.bin", image_id, InterlockedIncrement(&seq));
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite("EDGECAP1", 1, 8, f);
    fwrite(&job_ea, 4, 1, f); fwrite(&image_id, 4, 1, f); fwrite(&dsz, 4, 1, f);
    fwrite(vm_base + job_ea, 1, dsz, f);
    fwrite(&n_dma, 4, 1, f);
    for (uint32_t i = 0; i < n_dma; i++) {
        const uint64_t e = g64(dma_base + i * 8u);
        const uint32_t sz = (uint32_t)((e >> 32) & 0x7FFFu), eal = (uint32_t)e;
        fwrite(&sz, 4, 1, f); fwrite(&eal, 4, 1, f);
        if (sz && eal) fwrite(vm_base + eal, 1, sz, f);
    }
    return f;
}

/* Native EdgeGeom (geom1, image 10003) -- the job without the SPU.
 *
 * geom1 is pure triangle culling (no output vertex stream). On the SPU it
 * reads the u16 index list, culls triangles, writes the survivors to its
 * output ring and fills the command-buffer hole with: invalidate (1710,
 * 1714x3), one format/offset pair (1740+4a, 1680+4a) per vertex attribute of
 * the INPUT vertices, 1714, index array (181C), BEGIN(5), DRAW_INDEX_ARRAY,
 * END, and a ring semaphore release.
 *
 * Here the hole draws the ORIGINAL index buffer (already IO-mapped; no ring
 * space is used) and the GPU culls: a host-only NOP marker tells the renderer
 * which winding Edge keeps. Measured on 1,059 captured gameplay jobs
 * (native/analysis/geom-ra, edgecap):
 *   - the attribute set is fixed by the input format (cfg[2], with cfg[3]
 *     its pair) and userData[6] bit 2 (set: position only) -- k_edge_fmt;
 *   - userData[7]: 1 keeps both windings, 2 keeps screen-negative (D3D cull
 *     BACK, front CW), 3 keeps screen-positive (cull FRONT, front CW);
 *     everything else Edge drops is off-screen/degenerate, which the GPU's
 *     clipping drops too.
 * Whole-draw visibility follows Edge's own matrices (edge_any_visible).
 * Inputs come from the queue-time snapshot when there is one (the PPU reuses
 * the config/matrix memory). Anything not matching returns 0 and runs on the
 * SPU. On by default (no_edge_native file or EDGE_NATIVE=0: off). */
#include "spurs_job_input_snap.h"
#include "edge_skin.h"
#include "edge_skin_output.h"
#include "edge_geom6_evidence.h"
#include "edge_skin_output_mode.h"
typedef es_attr edge_attr;   /* attr, off, fmt -- shared with the vertex-output path */
typedef struct { uint8_t f2, f3, stride, n; edge_attr a[12]; } edge_fmt;
static const edge_fmt k_edge_fmt[] = {
    {0x06,0x06,28,5,{{0,0,0x1C32},{2,12,0x1C16},{14,16,0x1C16},{15,20,0x1C16},{8,24,0x1C23}}},
    {0x07,0x07,16,2,{{0,0,0x1032},{8,12,0x1023}}},
    {0x0C,0x08,16,2,{{0,0,0x1032},{2,12,0x1016}}},
    {0x0D,0x09,20,3,{{0,0,0x1432},{2,12,0x1416},{8,16,0x1423}}},
    {0x0F,0x0B,20,3,{{0,0,0x1432},{8,12,0x1423},{3,16,0x1444}}},
    {0x11,0x0D,24,4,{{0,0,0x1832},{2,12,0x1816},{8,16,0x1823},{3,20,0x1844}}},
    {0x13,0x0F,32,6,{{0,0,0x2032},{2,12,0x2016},{14,16,0x2016},{15,20,0x2016},{8,24,0x2023},{3,28,0x2044}}},
};
/* geom3 (image 10004): the same culling job for 36/44-byte vertices. Its hole
 * (1,500 captured gameplay jobs, every one matched) has this attribute order;
 * pos-only jobs set attribute 0 alone, as geom1. Whole-job visibility agreed
 * with the SPU on 1,325; the other 175 the SPU dropped were all sub-pixel
 * (largest surviving triangle 0.8 px^2), which the GPU rasterises to nothing. */
static const edge_fmt k_edge_fmt3[] = {
    {0x20,0x1C,36,7,{{0,0,0x2432},{3,12,0x2444},{2,16,0x2416},{14,20,0x2416},{15,24,0x2416},{8,28,0x2423},{10,32,0x2423}}},
    {0x1E,0x1A,44,9,{{0,0,0x2C32},{3,12,0x2C44},{2,16,0x2C16},{14,20,0x2C16},{15,24,0x2C16},{8,28,0x2C23},{9,32,0x2C23},{10,36,0x2C23},{11,40,0x2C44}}},
};
/* geom2 (10005), geom6 (10011), geom7 (10012), geom5 (10014): the same culling
 * job again (hole + index ring + stats, no vertex output). Hole attribute
 * order from captures (geom5 658, geom6 487, geom2 257, geom7 88 jobs: one
 * order per input format). Whole-job visibility matched the SPU except
 * sub-pixel jobs the SPU dropped (largest surviving triangle 0.05 px^2). */
static const edge_fmt k_edge_fmt2[] = {
    {0x1D,0x19,32,6,{{0,0,0x2032},{2,12,0x2016},{14,16,0x2016},{15,20,0x2016},{8,24,0x2023},{9,28,0x2023}}},
    {0x1A,0x16,28,5,{{0,0,0x1C32},{2,12,0x1C16},{14,16,0x1C16},{8,20,0x1C23},{10,24,0x1C23}}},
};
/* The 56-byte terrain vertex also carries attribute 12 (half2 UV, +48) and
 * 13 (UNORM8x4, +52; its w is the rock/grass blend weight the terrain shader
 * reads). The capture-derived order had only the first 10, leaving bytes
 * 48..55 unbound: the shader then read attribute 13's default w=1 and drew
 * every mound and cliff as stretched grass (SPU Edge binds all 12). */
static const edge_fmt k_edge_fmt6[] = {
    {0x44,0x40,56,12,{{0,0,0x3832},{3,12,0x3844},{4,16,0x3844},{2,20,0x3816},{14,24,0x3816},{15,28,0x3816},{8,32,0x3823},{9,36,0x3823},{10,40,0x3823},{11,44,0x3823},{12,48,0x3823},{13,52,0x3844}}},
};
static const edge_fmt k_edge_fmt7[] = {
    {0x52,0x4E,40,8,{{0,0,0x2832},{3,12,0x2844},{4,16,0x2844},{2,20,0x2816},{8,24,0x2823},{9,28,0x2823},{10,32,0x2823},{12,36,0x2823}}},
};
static const edge_fmt k_edge_fmt5[] = {
    {0x3C,0x38,44,9,{{0,0,0x2C32},{3,12,0x2C44},{2,16,0x2C16},{14,20,0x2C16},{15,24,0x2C16},{8,28,0x2C23},{9,32,0x2C23},{10,36,0x2C23},{11,40,0x2C23}}},
};
/* The captured rows of an Edge image (NULL: none -- geom4 10013, whose rows
 * are learned from the SPU by ej_dyn_learn). */
static const edge_fmt* ej_table(int image_id, size_t* n)
{
    #define EJ_TABLE(t) (*n = sizeof (t) / sizeof (t)[0], (t))
    switch (image_id) {
    case 10003: return EJ_TABLE(k_edge_fmt);
    case 10004: return EJ_TABLE(k_edge_fmt3);
    case 10005: return EJ_TABLE(k_edge_fmt2);
    case 10011: return EJ_TABLE(k_edge_fmt6);
    case 10012: return EJ_TABLE(k_edge_fmt7);
    case 10014: return EJ_TABLE(k_edge_fmt5);
    default: *n = 0; return NULL;
    }
    #undef EJ_TABLE
}
/* EDGE_VERIFY=1: shadow mode. Every job the native path would take still
 * runs on the SPU (the game sees exactly EDGE_NATIVE=0), but the native hole
 * is first built into a scratch buffer and, after the SPU job, its vertex
 * attribute formats and offsets are compared with what the SPU wrote into the
 * real hole. A wrong format-table row (e.g. a vertex layout the tables were
 * never captured with) then shows up as "[edge-verify]" lines naming the
 * image, input format and both attribute sets. */
static __declspec(thread) uint8_t* t_ej_shadow;      /* non-NULL: write here, not the hole */
static __declspec(thread) uint32_t t_ej_hole, t_ej_len, t_ej_fmt, t_ej_visible, t_ej_stats, t_ej_ntri;
static void ej_put32(uint32_t ea, uint32_t v)
{
    uint8_t* p = t_ej_shadow ? t_ej_shadow + (ea - t_ej_hole) : vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t ej_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float ej_bf(const uint8_t* p) { const uint32_t v = ej_be32(p); float f; memcpy(&f, &v, 4); return f; }
/* Edge's own visibility verdict for the job: does ANY triangle survive its
 * frustum (GL clip space) and, for cull modes 2/3, the winding test? The
 * game relies on it -- some meshes are only hidden because Edge culls them
 * with its matrices while the vertex program would draw them on screen. */
static void edge_clip_vertex(const uint8_t* v, const float PVM[4][4], float c4[4], uint8_t* outcode)
{
    const float x = ej_bf(v), y = ej_bf(v + 4), z = ej_bf(v + 8);
    for (int r = 0; r < 4; r++) c4[r] = PVM[r][0] * x + PVM[r][1] * y + PVM[r][2] * z + PVM[r][3];
    const float w = c4[3]; uint8_t o = 0;
    if (c4[0] > w) o |= 1; if (c4[0] < -w) o |= 2; if (c4[1] > w) o |= 4; if (c4[1] < -w) o |= 8;
    if (c4[2] > w) o |= 16; if (c4[2] < -w) o |= 32;
    *outcode = o;
}
static int edge_any_visible(const uint8_t* vtx, uint32_t stride, uint32_t n_vtx, const uint8_t* ib, uint32_t n_idx,
                            const uint8_t* vp, const uint8_t* mtx, uint32_t cull)
{
    float M[3][4], VP[4][4], sc[2], of[2];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) M[r][c] = ej_bf(mtx + 16 * r + 4 * c);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) VP[r][c] = ej_bf(vp + 16 + 16 * r + 4 * c);
    sc[0] = ej_bf(vp + 80); sc[1] = ej_bf(vp + 84); of[0] = ej_bf(vp + 96); of[1] = ej_bf(vp + 100);
    float PVM[4][4];   /* VP * [M; 0 0 0 1] */
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        PVM[r][c] = VP[r][0] * M[0][c] + VP[r][1] * M[1][c] + VP[r][2] * M[2][c] + (c == 3 ? VP[r][3] : 0.0f);
    /* Most visible draws are decided by their first few triangles. Avoid
     * transforming the entire mesh, but cache every referenced vertex so
     * fully rejected meshes never transform a shared vertex twice. Small
     * meshes also avoid two heap allocations per job. No retained TLS heap
     * buffers: the serial/per-list worker fallback can use short-lived threads. */
    float local_clip[1024 * 4];
    uint8_t local_oc[1024];
    void* allocated = NULL;
    float* clip = local_clip;
    uint8_t* oc = local_oc;
    if (n_vtx > 1024u) {
        allocated = malloc((size_t)n_vtx * (4u * sizeof(float) + 1u));
        if (!allocated) return 1; /* preserve the existing fail-open verdict */
        clip = (float*)allocated;
        oc = (uint8_t*)(clip + (size_t)n_vtx * 4u);
    }
    memset(oc, 0x80, n_vtx); /* valid outcodes use only the low six bits */
    int visible = 0, eager = 0;
    for (uint32_t t = 0; t + 2 < n_idx && !visible; t += 3) {
        const uint8_t* ip = ib + 2u * t;
        const uint32_t a = (uint32_t)ip[0] << 8 | ip[1], b = (uint32_t)ip[2] << 8 | ip[3], c = (uint32_t)ip[4] << 8 | ip[5];
        if (a >= n_vtx || b >= n_vtx || c >= n_vtx) continue;
        if (!eager) {
            if (t >= 24u) {
                /* Bound speculative work on rejected meshes. Afterwards use
                 * the original cheap outcode-only triangle scan. */
                for (uint32_t k = 0; k < n_vtx; k++)
                    if (oc[k] & 0x80u)
                        edge_clip_vertex(vtx + (size_t)stride * k, PVM, clip + 4u * k, oc + k);
                eager = 1;
            } else {
                const uint32_t corners[3] = { a, b, c };
                for (unsigned j = 0; j < 3; j++) {
                    const uint32_t k = corners[j];
                    if (oc[k] & 0x80u)
                        edge_clip_vertex(vtx + (size_t)stride * k, PVM, clip + 4u * k, oc + k);
                }
            }
        }
        if (oc[a] & oc[b] & oc[c]) continue;
        if (cull == 2 || cull == 3) {
            const float *A = clip + 4 * a, *B = clip + 4 * b, *C = clip + 4 * c;
            if (A[3] > 0 && B[3] > 0 && C[3] > 0) {
                const float x0 = A[0] / A[3] * sc[0] + of[0], y0 = A[1] / A[3] * sc[1] + of[1];
                const float x1 = B[0] / B[3] * sc[0] + of[0], y1 = B[1] / B[3] * sc[1] + of[1];
                const float x2 = C[0] / C[3] * sc[0] + of[0], y2 = C[1] / C[3] * sc[1] + of[1];
                const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (cull == 2 ? area >= 0.0f : area <= 0.0f) continue;
            }
        }
        visible = 1;
    }
    free(allocated);
    return visible;
}
/* Why a job went to the SPU instead (counted per image, reported with the
 * native/SPU totals): tells a stage's unknown vertex formats from jobs the
 * native path cannot take at all. */
enum { EJR_DMA = 1, EJR_SIZES, EJR_CFG, EJR_NOFMT, EJR_IDX, EJR_UD, EJR_IO, EJR_HOLE, EJR_COUNT };
static volatile LONG s_ej_reject[12][EJR_COUNT];
static __declspec(thread) int t_ej_reject;
static __declspec(thread) uint32_t t_ej_vtx_io, t_ej_f23;   /* for learning a missing format */
/* Job shape: the same input-format bytes are used both by culling-only jobs
 * and by jobs whose SPU output differs (seen in the cave stage: some 1F/1B
 * and 40/3C holes bind attributes outside the input vertex). Learning and
 * trust are therefore per shape -- format bytes, the cfg flag bytes, the
 * userData mode bits, and the DMA list layout (sizes of every element except
 * the matrix/viewport/config ones; the index and vertex streams only count
 * as present or absent, their sizes follow the mesh). */
static __declspec(thread) uint32_t t_ej_sig;
static __declspec(thread) uint32_t t_ej_udmode;   /* ud6 low bits | cull << 8, for EJR_UD jobs */
static __declspec(thread) uint8_t t_ej_cfg[16];
static __declspec(thread) uint32_t t_ej_ndma, t_ej_dmasz[32];
static uint32_t ej_fnv(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 16777619u; }
    return h;
}
static int ej_reject(int image_id, int why)
{
    t_ej_reject = why;
    if (image_id >= 10003 && image_id <= 10014) InterlockedIncrement(&s_ej_reject[image_id - 10003][why]);
    return 0;
}

/* Formats learned at run time. A stage whose vertex layout was never in the
 * captures (the k_edge_fmt* rows) sends all its jobs to the SPU, which then
 * uses its output ring for every draw. When such a job's SPU hole binds only
 * attributes inside one input vertex (culling-only, as every captured job),
 * the row is taken from the hole itself; the ej_trust check then compares
 * further jobs with the SPU before the format goes native. */
static edge_fmt s_ej_dyn[64];
static int s_ej_dyn_image[64];
static uint32_t s_ej_dyn_sig[64];
static volatile LONG s_ej_dyn_n;
static SRWLOCK s_ej_dyn_lock = SRWLOCK_INIT;
static const edge_fmt* ej_dyn_find(int image_id, uint8_t f2, uint8_t f3, uint32_t sig)
{
    const edge_fmt* r = NULL;
    AcquireSRWLockShared(&s_ej_dyn_lock);
    for (LONG i = 0; i < s_ej_dyn_n; i++)
        if (s_ej_dyn_image[i] == image_id && s_ej_dyn_sig[i] == sig && s_ej_dyn[i].f2 == f2 && s_ej_dyn[i].f3 == f3) { r = &s_ej_dyn[i]; break; }
    ReleaseSRWLockShared(&s_ej_dyn_lock);
    return r;
}

static int edge_geom1_native(uint32_t job_ea, uint32_t n_dma, const sj_snapshot* snap, int image_id)
{
    t_ej_reject = 0;
    /* Default on; EDGE_NATIVE=0 sends every geometry job back to the SPU. */
    static LONG on = -1;
    if (on < 0) { const char* e = getenv("EDGE_NATIVE"); on = !(e && *e == '0'); }
    {   /* runtime A/B: a no_edge_native file in the working directory sends
         * every geometry job to the SPU while it exists (checked each second) */
        static volatile ULONGLONG checked; static volatile LONG file_off;
        const ULONGLONG now = GetTickCount64();
        if (now - checked > 1000) {
            checked = now;
            const LONG off = GetFileAttributesA("no_edge_native") != INVALID_FILE_ATTRIBUTES;
            if (off != file_off) fprintf(stderr, "[edge-native] %s by no_edge_native file\n", off ? "OFF" : "ON");
            file_off = off;
        }
        if (file_off) return 0;
    }
    size_t n_table;
    const edge_fmt* const table = ej_table(image_id, &n_table);
    /* geom4 (10013): never captured; its rows are learned from the SPU. */
    if (!table && image_id != 10013) return 0;
    if (!on) return 0;
    if (n_dma < 17 || n_dma > SJ_SNAP_MAX_DMA) return ej_reject(image_id, EJR_DMA);
    const uint32_t need = JH_SIZE + n_dma * 8u + 48u;
    if (snap && snap->dsz < need) snap = NULL;
    const uint8_t* D = snap ? snap->desc : vm_base + job_ea;
    #define EJ_DMA(i) ((uint64_t)ej_be32(D + JH_SIZE + 8u * (i)) << 32 | ej_be32(D + JH_SIZE + 8u * (i) + 4u))
    #define EJ_SEG(i, ea) ((snap && snap->seg[i]) ? snap->seg[i] : vm_base + (ea))
    const uint64_t e_idx = EJ_DMA(1), e_vtx = EJ_DMA(8), e_cfg = EJ_DMA(16), e_vp = EJ_DMA(14), e_mtx = EJ_DMA(15);
    const uint32_t idx_ea = (uint32_t)e_idx, idx_sz = (uint32_t)(e_idx >> 32) & 0x7FFFu;
    const uint32_t vtx_ea = (uint32_t)e_vtx, cfg_ea = (uint32_t)e_cfg, vp_ea = (uint32_t)e_vp, mtx_ea = (uint32_t)e_mtx;
    if (!idx_ea || !vtx_ea || !cfg_ea || !vp_ea || !mtx_ea || ((e_cfg >> 32) & 0x7FFFu) != 16u) return ej_reject(image_id, EJR_SIZES);
    if (((e_vp >> 32) & 0x7FFFu) < 112u || ((e_mtx >> 32) & 0x7FFFu) < 48u) return ej_reject(image_id, EJR_SIZES);
    const uint8_t* cfg = EJ_SEG(16, cfg_ea);
    if (cfg[4] != 0x00 || cfg[6] != 0x00 || cfg[7] != 0x00) return ej_reject(image_id, EJR_CFG);
    {
        const uint8_t* uds = D + JH_SIZE + n_dma * 8u;
        uint32_t h = 2166136261u;
        h = ej_fnv(h, (uint32_t)image_id); h = ej_fnv(h, n_dma);
        h = ej_fnv(h, (uint32_t)cfg[0] | (uint32_t)cfg[2] << 8 | (uint32_t)cfg[3] << 16 | (uint32_t)cfg[5] << 24);
        h = ej_fnv(h, (uint32_t)cfg[12] | (uint32_t)cfg[13] << 8 | (uint32_t)cfg[14] << 16 | (uint32_t)cfg[15] << 24);
        h = ej_fnv(h, ej_be32(uds + 24) & 0xFu); h = ej_fnv(h, ej_be32(uds + 28));
        t_ej_ndma = n_dma;
        for (uint32_t i = 0; i < n_dma; i++) {
            const uint32_t sz = (uint32_t)(EJ_DMA(i) >> 32) & 0x7FFFu;
            if (i < 32) t_ej_dmasz[i] = sz;
            h = ej_fnv(h, (i >= 14 && i <= 16) ? sz : (sz != 0));   /* mesh-sized streams: presence only */
        }
        t_ej_sig = h;
        memcpy(t_ej_cfg, cfg, 16);
    }
    const edge_fmt* fmt = NULL;
    for (size_t k = 0; k < n_table; k++)
        if (table[k].f2 == cfg[2] && table[k].f3 == cfg[3]) fmt = &table[k];
    if (!fmt) fmt = ej_dyn_find(image_id, cfg[2], cfg[3], t_ej_sig);
    if (!fmt) {
        const uint8_t* ud0 = D + JH_SIZE + n_dma * 8u;
        t_ej_vtx_io = ej_be32(ud0 + 24) & ~0xFu;
        t_ej_f23 = (uint32_t)cfg[2] << 8 | cfg[3];
        t_ej_hole = ej_be32(ud0 + 12); t_ej_len = (uint32_t)cfg[1] * 16u;
        return ej_reject(image_id, EJR_NOFMT);
    }
    const uint32_t hole_bytes = (uint32_t)cfg[1] * 16u;
    const uint32_t n_vtx = ((uint32_t)cfg[8] << 8) | cfg[9];
    const uint32_t n_idx = ((uint32_t)cfg[10] << 8) | cfg[11];
    if (n_idx % 3u) return ej_reject(image_id, EJR_IDX);
    if (n_idx * 2u > idx_sz && idx_sz) {   /* index list longer than one DMA element */
        if ((uint32_t)EJ_DMA(2) != idx_ea + idx_sz) return ej_reject(image_id, EJR_IDX);   /* must continue contiguously */
    }
    const uint8_t* ud = D + JH_SIZE + n_dma * 8u;
    const uint32_t hole = ej_be32(ud + 12), ud6 = ej_be32(ud + 24), cull = ej_be32(ud + 28), stats = ej_be32(ud + 44);
    if (cull < 1 || cull > 3 || (ud6 & 3u) != 3u) {
        t_ej_vtx_io = ud6 & ~0xFu; t_ej_f23 = (uint32_t)cfg[2] << 8 | cfg[3];
        t_ej_hole = hole; t_ej_len = hole_bytes; t_ej_udmode = (ud6 & 0xFu) | cull << 8;
        return ej_reject(image_id, EJR_UD);
    }
    const int pos_only = (ud6 & 4u) != 0;
    const uint32_t vtx_io = ud6 & ~0xFu;
    const uint32_t io_base = vtx_ea - vtx_io;          /* EA of IO offset 0 */
    if (idx_ea < io_base || hole == 0) return ej_reject(image_id, EJR_IO);
    const uint32_t idx_io = idx_ea - io_base;
    const uint32_t n_attr = pos_only ? 1u : fmt->n;
    const uint32_t draws = (n_idx + 255u) / 256u;
    const uint32_t words = 2 * 4 + 4 * n_attr + 2 + 3 + 2 + 2 + (draws ? 1 + draws : 0) + 2;
    if (words * 4u > hole_bytes) return ej_reject(image_id, EJR_HOLE);
    const int visible = n_idx && edge_any_visible(vm_base + vtx_ea, fmt->stride, n_vtx, vm_base + idx_ea, n_idx,
                                                  EJ_SEG(14, vp_ea), EJ_SEG(15, mtx_ea), cull);
    t_ej_hole = hole; t_ej_len = hole_bytes; t_ej_visible = (uint32_t)visible;
    t_ej_fmt = (uint32_t)pos_only << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
    t_ej_stats = stats; t_ej_ntri = n_idx / 3u;
    if (t_ej_shadow && hole_bytes > 4096u) return 0;   /* scratch buffer size */
    uint32_t w = hole, end = hole + hole_bytes;
    #define EJ(v) do { ej_put32(w, (v)); w += 4; } while (0)
    if (visible) {
        EJ(0x00041710u); EJ(0);
        EJ(0x00041714u); EJ(0); EJ(0x00041714u); EJ(0); EJ(0x00041714u); EJ(0);
        for (uint32_t k = 0; k < n_attr; k++) {
            const edge_attr* a = &fmt->a[k];
            EJ(0x00041740u + 4u * a->attr); EJ(pos_only ? ((uint32_t)fmt->stride << 8 | 0x32u) : a->fmt);
            EJ(0x00041680u + 4u * a->attr); EJ(0x80000000u | (vtx_io + a->off));
        }
        EJ(0x00041714u); EJ(0);
        EJ(0x0008181Cu); EJ(idx_io); EJ(0x00000011u);   /* index array: main, u16 */
        EJ(0x00040100u); EJ(0xED6E0000u | cull);        /* host marker: winding Edge keeps */
        EJ(0x00041808u); EJ(5u);                        /* BEGIN triangles */
        EJ(0x40001824u | (draws << 18));
        for (uint32_t k = 0, first = 0; k < draws; k++, first += 256u) {
            const uint32_t c = n_idx - first < 256u ? n_idx - first : 256u;
            EJ(((c - 1u) << 24) | first);
        }
        EJ(0x00041808u); EJ(0);                         /* END */
    }
    while (w < end) EJ(0);                              /* the rest: NOPs */
    #undef EJ
    #undef EJ_DMA
    #undef EJ_SEG
    if (stats && !t_ej_shadow) {   /* Edge statistics: triangles processed / kept */
        extern void spu_lockline_lock(void); extern void spu_lockline_unlock(void);
        extern void spu_coh_notify_write(uint32_t);
        spu_lockline_lock();
        ej_put32(stats, g32(stats) + n_idx / 3u);
        ej_put32(stats + 4, g32(stats + 4) + n_idx / 3u);
        spu_coh_notify_write(stats & ~127u);
        spu_lockline_unlock();
    }
    static volatile LONG logged;
    if (!t_ej_shadow && InterlockedIncrement(&logged) <= 4)
        fprintf(stderr, "[edge-native] image=%d job=%08X fmt=%02X attrs=%u cull=%u idx=%u hole=%08X/%u draws=%u snap=%d\n",
                image_id, job_ea, cfg[2], n_attr, cull, n_idx, hole, hole_bytes, draws, snap != NULL);
    return 1;
}

/* Vertex attribute format/offset pairs written into a hole (NV4097 1740+4a /
 * 1680+4a); mask bit a set when attribute a is given. */
typedef struct { uint32_t mask, fmt[16], off[16]; } ej_attrs;
static void ej_scan_attrs(const uint8_t* h, uint32_t len, ej_attrs* o)
{
    memset(o, 0, sizeof *o);
    for (uint32_t i = 0; i + 8 <= len;) {
        const uint32_t w = ej_be32(h + i);
        if (w == 0 || (w & 0xE0000003u)) { i += 4; continue; }   /* NOP / non-increment header */
        const uint32_t m = w & 0xFFFCu, n = (w >> 18) & 0x7FFu;
        for (uint32_t k = 0; k < n && i + 8 + 4 * k <= len; k++) {
            const uint32_t mk = m + 4 * k, v = ej_be32(h + i + 4 + 4 * k);
            if (mk >= 0x1740 && mk < 0x1780) { const uint32_t a = (mk - 0x1740) / 4; o->mask |= 1u << a; o->fmt[a] = v; }
            if (mk >= 0x1680 && mk < 0x16C0) { const uint32_t a = (mk - 0x1680) / 4; o->off[a] = v; }
        }
        i += 4 + 4 * n;
    }
}
static int ej_verify(int image_id, uint32_t fmtkey, uint32_t hole, uint32_t len, const uint8_t* native)
{
    static volatile LONG compared, bad;
    ej_attrs a, b;
    ej_scan_attrs(native, len, &a);
    ej_scan_attrs(vm_base + hole, len, &b);
    if (!a.mask || !b.mask) return -1;   /* one side drew nothing (sub-pixel job): not comparable */
    int diff = a.mask != b.mask;
    for (int k = 0; k < 16 && !diff; k++)
        if ((a.mask >> k) & 1u) diff = a.fmt[k] != b.fmt[k] || a.off[k] != b.off[k];
    const LONG c = InterlockedIncrement(&compared);
    if (diff) {
        const LONG nb = InterlockedIncrement(&bad);
        /* first few per format key */
        static volatile LONG seen[256];
        if (InterlockedIncrement(&seen[(fmtkey ^ (uint32_t)image_id) & 255u]) <= 3) {
            char sa[512], sb[512]; int la = 0, lb = 0;
            for (int k = 0; k < 16; k++) {
                if ((a.mask >> k) & 1u) la += snprintf(sa + la, sizeof sa - la, " a%d=%04X@%08X", k, a.fmt[k], a.off[k]);
                if ((b.mask >> k) & 1u) lb += snprintf(sb + lb, sizeof sb - lb, " a%d=%04X@%08X", k, b.fmt[k], b.off[k]);
            }
            fprintf(stderr, "[edge-verify] MISMATCH image=%d fmt=%02X/%02X%s hole=%08X\n  native:%s\n  spu:   %s\n",
                    image_id, (fmtkey >> 8) & 255u, fmtkey & 255u, (fmtkey >> 16) ? " pos-only" : "", hole, sa, sb);
        }
        (void)nb;
    }
    if ((c & 0x3FFF) == 0)
        fprintf(stderr, "[edge-verify] compared=%ld mismatched=%ld\n", (long)c, (long)bad);
    return !diff;
}

/* Learn a format row from an SPU hole (see s_ej_dyn). Only when every bound
 * attribute lies inside one vertex of the input stream at vtx_io: then the
 * job is the culling-only kind the native path reproduces. */
static void ej_dyn_learn(int image_id, uint32_t f23, uint32_t sig, uint32_t vtx_io, uint32_t hole, uint32_t len,
                         const uint8_t* cfg, uint32_t ndma, const uint32_t* dmasz)
{
    static volatile LONG said[64];
    ej_attrs b;
    ej_scan_attrs(vm_base + hole, len, &b);
    if (!(b.mask & 1u)) return;                       /* nothing drawn, or no position */
    const uint32_t stride = (b.fmt[0] >> 8) & 0xFFu;
    edge_fmt row; memset(&row, 0, sizeof row);
    row.f2 = (uint8_t)(f23 >> 8); row.f3 = (uint8_t)f23; row.stride = (uint8_t)stride;
    int ok = stride != 0;
    for (int k = 0; k < 16 && ok; k++) {
        if (!((b.mask >> k) & 1u)) continue;
        const uint32_t off = (b.off[k] & 0x7FFFFFFFu) - vtx_io;
        if (((b.fmt[k] >> 8) & 0xFFu) != stride || off >= stride || row.n >= 12) { ok = 0; break; }
        row.a[row.n].attr = (uint8_t)k; row.a[row.n].off = (uint8_t)off; row.a[row.n].fmt = (uint16_t)b.fmt[k];
        row.n++;
    }
    if (row.n == 1 && b.mask == 1u) return;           /* a pos-only hole: wait for a full one */
    const uint32_t slot = (sig ^ f23 ^ (uint32_t)image_id) & 63u;
    if (!ok) {
        if (InterlockedIncrement(&said[slot]) == 1) {
            char t[512]; int l = 0;
            for (int k = 0; k < 16; k++)
                if ((b.mask >> k) & 1u) l += snprintf(t + l, sizeof t - l, " a%d=%04X@%08X", k, b.fmt[k], b.off[k]);
            l += snprintf(t + l, sizeof t - l, " | vtx_io=%08X cfg=", vtx_io);
            for (int k = 0; k < 16; k++) l += snprintf(t + l, sizeof t - l, "%02X", cfg[k]);
            l += snprintf(t + l, sizeof t - l, " dma=");
            for (uint32_t k = 0; k < ndma && k < 32; k++) l += snprintf(t + l, sizeof t - l, "%s%X", k ? "," : "", dmasz[k]);
            fprintf(stderr, "[edge-native] image=%d fmt=%02X/%02X sig=%08X: SPU hole is not culling-only -- stays on the SPU:%s\n",
                    image_id, f23 >> 8, f23 & 255u, sig, t);
        }
        return;
    }
    AcquireSRWLockExclusive(&s_ej_dyn_lock);
    int have = 0;
    for (LONG i = 0; i < s_ej_dyn_n; i++)
        if (s_ej_dyn_image[i] == image_id && s_ej_dyn_sig[i] == sig && s_ej_dyn[i].f2 == row.f2 && s_ej_dyn[i].f3 == row.f3) have = 1;
    if (!have && s_ej_dyn_n < 64) {
        s_ej_dyn[s_ej_dyn_n] = row; s_ej_dyn_image[s_ej_dyn_n] = image_id; s_ej_dyn_sig[s_ej_dyn_n] = sig;
        MemoryBarrier();
        s_ej_dyn_n++;
        char t[256]; int l = 0;
        for (int k = 0; k < row.n; k++) l += snprintf(t + l, sizeof t - l, " {%u,%u,0x%04X}", row.a[k].attr, row.a[k].off, row.a[k].fmt);
        fprintf(stderr, "[edge-native] image=%d fmt=%02X/%02X sig=%08X learned from the SPU: stride %u,%s\n",
                image_id, row.f2, row.f3, sig, stride, t);
    }
    ReleaseSRWLockExclusive(&s_ej_dyn_lock);
}

/* Jobs in a userData mode the native path does not take (cull outside 1..3
 * or ud6 low bits other than 3): show what the SPU wrote, a few per mode, so
 * the mode can be implemented natively. */
static void ej_ud_report(int image_id, uint32_t mode, uint32_t f23, uint32_t vtx_io, uint32_t hole, uint32_t len,
                         const uint8_t* cfg, uint32_t ndma, const uint32_t* dmasz)
{
    static volatile LONG seen[256];
    if (InterlockedIncrement(&seen[(mode ^ (mode >> 8) * 7u ^ f23 ^ (uint32_t)image_id) & 255u]) > 3) return;
    char t[2048]; int l = 0;
    l += snprintf(t + l, sizeof t - l, "[edge-ud] image=%d ud6&F=%X cull=%u fmt=%02X/%02X vtx_io=%08X cfg=",
                  image_id, mode & 0xFu, mode >> 8, f23 >> 8, f23 & 255u, vtx_io);
    for (int k = 0; k < 16; k++) l += snprintf(t + l, sizeof t - l, "%02X", cfg[k]);
    l += snprintf(t + l, sizeof t - l, " dma=");
    for (uint32_t k = 0; k < ndma && k < 32; k++) l += snprintf(t + l, sizeof t - l, "%s%X", k ? "," : "", dmasz[k]);
    l += snprintf(t + l, sizeof t - l, "\n  hole %08X/%u:", hole, len);
    for (uint32_t i = 0; i < len && i < 160; i += 4) l += snprintf(t + l, sizeof t - l, " %08X", g32(hole + i));
    fprintf(stderr, "%s\n", t);
}

/* Native Edge learns each input format before trusting it. The format tables
 * were derived from captures of some stages; a mesh layout that matches a row
 * by its format bytes but binds different attributes (a stage never captured)
 * would draw stretched textures, as the 56-byte terrain did. So the first jobs
 * of every (image, format, pos-only) key run on the SPU with the native hole
 * built aside and compared; after EJ_TRUST_AFTER matches the key goes native,
 * one mismatch keeps it on the SPU for the rest of the run (logged). */
#define EJ_TRUST_AFTER 2
typedef struct { volatile LONG key; volatile LONG ok; volatile LONG bad; } ej_trust_slot;
static ej_trust_slot s_ej_trust[1024];
static ej_trust_slot* ej_trust(int image_id, uint32_t fmt, uint32_t sig)
{
    const LONG key = (LONG)(ej_fnv(ej_fnv(sig, fmt), (uint32_t)image_id) | 0x80000000u);
    uint32_t h = ((uint32_t)key * 2654435761u) >> 22;
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & 1023u) {
        const LONG k = s_ej_trust[h].key;
        if (k == key) return &s_ej_trust[h];
        if (k == 0 && InterlockedCompareExchange(&s_ej_trust[h].key, key, 0) == 0) return &s_ej_trust[h];
        if (s_ej_trust[h].key == key) return &s_ej_trust[h];
    }
    return NULL;   /* table full: stay on the SPU */
}
/* Publish a scratch-built hole: every word but the first, then the first --
 * the GPU may be parked on the hole's first word. */
static void ej_commit(const uint8_t* scratch)
{
    if (t_ej_len > 4) memcpy(vm_base + t_ej_hole + 4, scratch + 4, t_ej_len - 4);
    MemoryBarrier();
    memcpy(vm_base + t_ej_hole, scratch, 4);
    if (t_ej_stats) {   /* Edge statistics: triangles processed / kept */
        extern void spu_lockline_lock(void); extern void spu_lockline_unlock(void);
        extern void spu_coh_notify_write(uint32_t);
        spu_lockline_lock();
        ej_put32(t_ej_stats, g32(t_ej_stats) + t_ej_ntri);
        ej_put32(t_ej_stats + 4, g32(t_ej_stats + 4) + t_ej_ntri);
        spu_coh_notify_write(t_ej_stats & ~127u);
        spu_lockline_unlock();
    }
}

/* ---- Native Edge vertex output (skinning) -------------------------------
 *
 * Jobs with userData[6] bit 1 clear do not just cull: the SPU re-emits every
 * vertex (skinned when cfg[5] low nibble is 1) into its per-SPU output ring
 * and the hole binds the ring copy. That ring (label wait protocol, one per
 * SPU number) is what hung and corrupted the cave stage. Here the vertices
 * are produced on the host (edge_skin.h) into a host-owned arena in IO space
 * and the hole binds those, drawing the ORIGINAL index list with GPU culling
 * exactly as the culling-only path does. Output vertices keep the input
 * order and count, so no index rewrite is needed.
 *
 * Taken: scheme 0 (copy) and 1 (skinned), index scheme 1 (u16 list), no
 * blend shapes, full or position-only output (ud6 bit 2), inputs only in the
 * slots the job reads for that (index 1+2, palette 3+4, weights 5+6, vertices
 * 8+9, viewport 14, matrix 15, config 16), a format row whose layout
 * es_layout_ok knows. Anything else runs on the SPU, counted per reason.
 *
 * Trust, as for formats: the first jobs of every key (image, cfg[2]/cfg[3],
 * scheme, pos-only, job shape t_ej_sig) run on the SPU with the native output
 * built aside; right after the job the ring vertices are read back through
 * the SPU hole's offsets and compared per attribute. EJ_SKIN_TRUST_AFTER
 * matches take the key native, one mismatch keeps it on the SPU for the run.
 * EDGE_VERIFY=1 compares every job and keeps all on the SPU. Off: EDGE_SKIN=0,
 * or the no_edge_native file / EDGE_NATIVE=0 (which stop every native job). */
#define EJ_SKIN_TRUST_AFTER 3
#define EJ_SKIN_POS_ULP 2u     /* position: max ulp from the SPU's float */
#define EJ_SKIN_CMP_LSB 1u     /* CMP direction: max difference per field */

/* Arena: guest memory no allocator hands out, mapped into free IO space by
 * cellGcm_host_io_block on first use. Allocated as a ring; a block becomes
 * free again EJ_ARENA_FLIPS flips after it was handed out (the flip count
 * moves when the FIFO walker reaches a flip, and the walker reads a draw's
 * vertices when it reaches its hole, at most a frame or so later). Full: the
 * job runs on the SPU. */
#define EJ_ARENA_MAX (64u << 20)    /* tried first; halved down to 16 MB if IO space is short */
#define EJ_ARENA_FLIPS 4u
static SRWLOCK s_ej_arena_lock = SRWLOCK_INIT;
static struct {
    int state;                                /* 0 not mapped yet, 1 mapped, -1 mapping failed */
    uint32_t ea, io, size, head, tail, first, n;   /* live data: [tail, head) around the ring */
    struct { uint32_t flip, end; } ep[64];    /* per flip: where its blocks end */
} s_ej_arena;
static uint32_t ej_arena_alloc(uint32_t need, uint32_t* io)
{
    extern uint32_t cellGcm_host_io_block(uint32_t size, uint32_t* io_out);
    extern uint32_t cellGcm_flip_request_count(void);
    need = (need + 127u) & ~127u;
    AcquireSRWLockExclusive(&s_ej_arena_lock);
    if (s_ej_arena.state == 0) {
        for (uint32_t size = EJ_ARENA_MAX; size >= (16u << 20) && !s_ej_arena.ea; size >>= 1)
            if ((s_ej_arena.ea = cellGcm_host_io_block(size, &s_ej_arena.io)) != 0) s_ej_arena.size = size;
        s_ej_arena.state = s_ej_arena.ea ? 1 : -1;
        fprintf(stderr, "[edge-skin] arena %s ea=%08X io=%08X (%u MB)\n", s_ej_arena.ea ? "mapped" : "NOT mapped -- vertex-output jobs stay on the SPU",
                s_ej_arena.ea, s_ej_arena.io, s_ej_arena.size >> 20);
    }
    uint32_t at = 0xFFFFFFFFu;
    if (s_ej_arena.state > 0 && need <= s_ej_arena.size / 4u) {
        const uint32_t now = cellGcm_flip_request_count();
        while (s_ej_arena.n && now - s_ej_arena.ep[s_ej_arena.first].flip >= EJ_ARENA_FLIPS) {
            s_ej_arena.tail = s_ej_arena.ep[s_ej_arena.first].end;
            s_ej_arena.first = (s_ej_arena.first + 1u) & 63u; s_ej_arena.n--;
        }
        const uint32_t head = s_ej_arena.head, tail = s_ej_arena.tail;
        if (!s_ej_arena.n) at = 0;                                    /* empty */
        else if (head > tail) at = s_ej_arena.size - head >= need ? head : (need <= tail ? 0 : at);
        else if (head < tail && tail - head >= need) at = head;       /* head == tail: full */
        if (at != 0xFFFFFFFFu) {
            s_ej_arena.head = at + need;
            if (!s_ej_arena.n) s_ej_arena.tail = at;
            const uint32_t last = (s_ej_arena.first + s_ej_arena.n - 1u) & 63u;
            if (s_ej_arena.n && s_ej_arena.ep[last].flip == now) s_ej_arena.ep[last].end = s_ej_arena.head;
            else if (s_ej_arena.n < 64u) {
                const uint32_t k = (s_ej_arena.first + s_ej_arena.n++) & 63u;
                s_ej_arena.ep[k].flip = now; s_ej_arena.ep[k].end = s_ej_arena.head;
            } else { s_ej_arena.ep[last].flip = now; s_ej_arena.ep[last].end = s_ej_arena.head; }   /* holds older blocks longer */
        }
    }
    const uint32_t ea = at != 0xFFFFFFFFu ? s_ej_arena.ea + at : 0;
    if (ea) *io = s_ej_arena.io + at;
    ReleaseSRWLockExclusive(&s_ej_arena_lock);
    return ea;
}

enum { ESR_MODE, ESR_BLEND, ESR_CFG, ESR_DMA, ESR_FMT, ESR_SIZES, ESR_PAL, ESR_HOLE, ESR_ARENA, ESR_TRUST, ESR_PASSTHROUGH, ESR_COUNT };
static volatile LONG s_es_reject[12][ESR_COUNT], s_es_native[12], s_es_shadow[12];
static int es_reject(int image_id, int why)
{
    InterlockedIncrement(&s_es_reject[image_id - 10003][why]);
    return 0;
}
/* Per-thread buffers, grown as meshes need. */
static uint8_t* ej_buf(uint8_t** b, size_t* cap, size_t need)
{
    if (need > *cap) {
        uint8_t* n = (uint8_t*)realloc(*b, need);
        if (!n) return NULL;
        *b = n; *cap = need;
    }
    return *b;
}
static __declspec(thread) uint8_t *t_es_vtx, *t_es_idx, *t_es_in, *t_es_ib, *t_es_wt;
static __declspec(thread) size_t t_es_vtx_cap, t_es_idx_cap, t_es_in_cap, t_es_ib_cap, t_es_wt_cap;
static __declspec(thread) uint32_t t_es_pal[256 * 12];
static __declspec(thread) uint8_t t_es_hole[4096];
/* The shadow job in flight on this thread (compared after its SPU run). */
typedef struct {
    int active, image, pos_only, visible;
    uint32_t key, sig, hole, len, n_vtx, n_idx, nw, scheme;
    ej_trust_slot* slot;
    edge_fmt fmt;
    uint8_t words[512 * 4];     /* the native draw, big-endian, vertices at io 0 */
} ej_skin_shadow;
static __declspec(thread) ej_skin_shadow t_es;
static __declspec(thread) es_output_log* t_es_output;
static __declspec(thread) int t_es_output_on;
static __declspec(thread) uint8_t* t_es_spu;
static __declspec(thread) size_t t_es_spu_cap;
static void edge_skin_rec_put(uint32_t ea, const void* data, uint32_t size)
{
    if (t_es_output_on && t_es_output && !(ea & 0x80000000u)) es_output_put(t_es_output, ea, data, size);
}

/* A DMA element as the job stages it: the queue-time copy when there is one. */
static const uint8_t* ej_seg(const uint8_t* D, const sj_snapshot* snap, uint32_t i, uint32_t* sz)
{
    const uint8_t* e = D + JH_SIZE + 8u * i;
    const uint32_t ea = ej_be32(e + 4);
    *sz = ej_be32(e) & 0x7FFFu;
    if (!*sz || !ea) { *sz = 0; return NULL; }
    return (snap && i < SJ_SNAP_MAX_DMA && snap->seg[i]) ? snap->seg[i] : vm_base + ea;
}
/* One packed stream, using exactly its assigned DMA slots. Queue-time
 * snapshots are used for every fragment, including the third vertex slot. */
static const uint8_t* ej_stream(const uint8_t* D, const sj_snapshot* snap, uint32_t i, uint32_t count,
                                uint32_t need, uint8_t** b, size_t* cap)
{
    es_dma_part parts[3];
    if (!count || count > 3) return NULL;
    for (uint32_t k = 0; k < count; k++) parts[k].data = ej_seg(D, snap, i + k, &parts[k].size);
    if (!es_dma_prefix_fits(parts, count, need)) return NULL;
    if (parts[0].size >= need) return parts[0].data;
    uint8_t* r = ej_buf(b, cap, need);
    return es_copy_dma_prefix(parts, count, need, r, *cap) ? r : NULL;
}
/* Which size check sent a vertex-output job to the SPU: the first few of
 * each kind per image, with the DMA element sizes, so the reason is visible. */
static volatile LONG s_es_size_seen[12][6];
static int es_size_reject(int image_id, int sub, const uint8_t* D, const sj_snapshot* snap,
                          uint32_t n_vtx, uint32_t stride, uint32_t n_idx, uint32_t hole_bytes)
{
    static const char* const what[6] = { "counts", "vp/mtx", "vertices", "indices", "weights", "buffer" };
    if (InterlockedIncrement(&s_es_size_seen[image_id - 10003][sub]) <= 4) {
        uint32_t z[17];
        for (uint32_t i = 0; i < 17; i++) ej_seg(D, snap, i, &z[i]);
        fprintf(stderr, "[edge-skin] image=%d sizes(%s): vtx %u x %u = %u idx %u hole %u | dma 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u "
                "8:%u 9:%u 10:%u 11:%u 12:%u 13:%u 14:%u 15:%u 16:%u\n", image_id, what[sub], n_vtx, stride, n_vtx * stride, n_idx,
                hole_bytes, z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10], z[11], z[12], z[13], z[14], z[15], z[16]);
    }
    return es_reject(image_id, ESR_SIZES);
}

/* A cfg rejection alone cannot tell an unported skin scheme from an
 * unsupported image of a ported scheme. Keep bounded evidence for next work. */
static int es_scheme_reject(int image_id, uint32_t scheme, const uint8_t* cfg)
{
    static volatile LONG seen[12][16];
    if (InterlockedIncrement(&seen[image_id - 10003][scheme & 15u]) <= 4)
        fprintf(stderr, "[edge-skin] scheme-reject: image=%d scheme=%u reason=%s cfg=%02X/%02X flags=%02X/%02X/%02X/%02X\n",
                image_id, scheme, scheme >= 2u && scheme <= 3u ? "unported-image" : "unsupported-scheme",
                cfg[2], cfg[3], cfg[0], cfg[4], cfg[6], cfg[7]);
    return es_reject(image_id, ESR_CFG);
}

/* A learned row for these format bytes, whatever job shape it was learned
 * from: the vertex-output path checks the layout against the SPU anyway. */
static const edge_fmt* ej_dyn_find_any(int image_id, uint8_t f2, uint8_t f3)
{
    const edge_fmt* r = NULL;
    AcquireSRWLockShared(&s_ej_dyn_lock);
    for (LONG i = 0; i < s_ej_dyn_n; i++)
        if (s_ej_dyn_image[i] == image_id && s_ej_dyn[i].f2 == f2 && s_ej_dyn[i].f3 == f3) { r = &s_ej_dyn[i]; break; }
    ReleaseSRWLockShared(&s_ej_dyn_lock);
    return r;
}

/* 1: the job was done natively (hole committed). 0: it runs on the SPU; with
 * t_es.active set, its result is compared afterwards (ej_skin_verify). */
static int edge_skin_job(uint32_t job_ea, uint32_t n_dma, const sj_snapshot* snap, int image_id, int verify_all)
{
    t_es.active = 0;
    static LONG on = -1, call_on = -1;
    if (on < 0) { const char* e = getenv("EDGE_SKIN"); on = !(e && *e == '0'); }
    if (call_on < 0) { const char* e = getenv("EDGE_SKIN_CALL"); call_on = !(e && *e == '0'); }
    if (!on || n_dma < 17 || n_dma > SJ_SNAP_MAX_DMA) return 0;
    if (snap && snap->dsz < JH_SIZE + n_dma * 8u + 48u) snap = NULL;
    const uint8_t* D = snap ? snap->desc : vm_base + job_ea;
    const uint8_t* ud = D + JH_SIZE + n_dma * 8u;
    const uint32_t ud1 = ej_be32(ud + 4), hole = ej_be32(ud + 12), n_blend = ej_be32(ud + 16), ud6 = ej_be32(ud + 24);
    const uint32_t cull = ej_be32(ud + 28), stats = ej_be32(ud + 44);
    if (ud6 & 2u) return 0;                                  /* culling-only: edge_geom1_native's kind */
    /* ud[1] bit 1: output at a fixed address instead of the ring */
    if (cull < 1 || cull > 3 || (ud1 & 2u) || !hole) return es_reject(image_id, ESR_MODE);
    uint32_t sz;
    const uint8_t* cfg = ej_seg(D, snap, 16, &sz);
    if (!cfg || sz != 16u) return es_reject(image_id, ESR_CFG);
    if (n_blend || cfg[4]) return es_reject(image_id, ESR_BLEND);
    const uint32_t scheme = cfg[5] & 0xFu;
    if ((cfg[0] & 0x10u) || cfg[6] || cfg[7] || (cfg[5] >> 4) != 1u) return es_reject(image_id, ESR_CFG);
    const int scheme_image = (image_id >= 10003 && image_id <= 10005) || (image_id >= 10011 && image_id <= 10014);
    if (scheme > 3u || (scheme >= 2u && !scheme_image)) return es_scheme_reject(image_id, scheme, cfg);
    for (uint32_t i = 0; i < n_dma; i++) {
        const uint32_t used = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 9 |
                              1u << 10 | 1u << 14 | 1u << 15 | 1u << 16;
        if (!((used >> i) & 1u) && ej_seg(D, snap, i, &sz)) return es_reject(image_id, ESR_DMA);
    }
    size_t n_table;
    const edge_fmt* const table = ej_table(image_id, &n_table);
    const edge_fmt* fmt = NULL;
    for (size_t k = 0; k < n_table; k++)
        if (table[k].f2 == cfg[2] && table[k].f3 == cfg[3]) fmt = &table[k];
    if (!fmt) fmt = ej_dyn_find_any(image_id, cfg[2], cfg[3]);
    const int pos_only = (ud6 & 4u) != 0;
    if (es_spu_only_output_mode(image_id, cfg[2], cfg[3], scheme, pos_only))
        return es_reject(image_id, ESR_PASSTHROUGH);
    if (!fmt || !es_layout_ok(fmt->a, fmt->n, fmt->stride, pos_only)) return es_reject(image_id, ESR_FMT);
    const uint32_t stride = fmt->stride, hole_bytes = (uint32_t)cfg[1] * 16u;
    const uint32_t cfg_vtx = (uint32_t)cfg[8] << 8 | cfg[9], n_idx = (uint32_t)cfg[10] << 8 | cfg[11];
    uint32_t n_vtx = cfg_vtx;
    if (!n_vtx || !n_idx || n_idx % 3u || hole_bytes < 8u || hole_bytes > sizeof t_es_hole)
        return es_size_reject(image_id, 0, D, snap, n_vtx, stride, n_idx, hole_bytes);
    uint32_t vp_sz, mtx_sz;
    const uint8_t* vp = ej_seg(D, snap, 14, &vp_sz);
    const uint8_t* mtx = ej_seg(D, snap, 15, &mtx_sz);
    const uint8_t* idx = ej_stream(D, snap, 1, 2, n_idx * 2u, &t_es_ib, &t_es_ib_cap);
    if (!idx) return es_size_reject(image_id, 3, D, snap, cfg_vtx, stride, n_idx, hole_bytes);
    /* The cfg count is SIMD-padded: captured jobs have 1..7 unreferenced
     * tail vertices without corresponding DMA bytes. Keep original indices
     * and transform their entire referenced prefix, not the unused tail. */
    n_vtx = es_index_vertex_span(idx, n_idx, cfg_vtx);
    if (!n_vtx) return es_reject(image_id, ESR_SIZES);
    const uint8_t* vtx = ej_stream(D, snap, 8, 3, n_vtx * stride, &t_es_in, &t_es_in_cap);
    if (!vp || vp_sz < 112u || !mtx || mtx_sz < 48u || !vtx || !idx)
        return es_size_reject(image_id, (!vp || vp_sz < 112u || !mtx || mtx_sz < 48u) ? 1 : !vtx ? 2 : 3, D, snap, n_vtx, stride, n_idx, hole_bytes);
    /* Palette: slots 3 and 4 back to back, 48-byte matrices (A784). The SPU
     * skips skinning for scheme 0 or an empty palette: a copy. */
    const uint8_t* wts = NULL; uint32_t n_mat = 0;
    if (scheme >= 1u && scheme <= 3u) {
        uint32_t s3, s4;
        const uint8_t* p3 = ej_seg(D, snap, 3, &s3);
        const uint8_t* p4 = ej_seg(D, snap, 4, &s4);
        if ((s3 & 15u) || (s3 + s4) % 48u) return es_reject(image_id, ESR_PAL);
        n_mat = (s3 + s4) / 48u;
        if (n_mat > 256u) n_mat = 256u;                     /* a bone index is one byte */
        for (uint32_t e = 0; e < n_mat * 12u; e++)
            t_es_pal[e] = 4u * e < s3 ? ej_be32(p3 + 4u * e) : ej_be32(p4 + 4u * e - s3);
        if (n_mat && !(wts = ej_stream(D, snap, 5, 2, n_vtx * 8u, &t_es_wt, &t_es_wt_cap)))
            return es_size_reject(image_id, 4, D, snap, n_vtx, stride, n_idx, hole_bytes);
    }
    if (n_vtx < cfg_vtx) {
        static volatile LONG tail_seen[12];
        if (InterlockedIncrement(&tail_seen[image_id - 10003]) <= 4)
            fprintf(stderr, "[edge-skin] image=%d vertex-tail: cfg=%u referenced=%u stride=%u idx=%u (shadow/trust)\n",
                    image_id, cfg_vtx, n_vtx, stride, n_idx);
    }
    {
        uint32_t s8, s9, s10;
        ej_seg(D, snap, 8, &s8); ej_seg(D, snap, 9, &s9); ej_seg(D, snap, 10, &s10);
        if (s10 && n_vtx * stride > s8 + s9) {
            static volatile LONG dma3_seen[12];
            if (InterlockedIncrement(&dma3_seen[image_id - 10003]) <= 4)
                fprintf(stderr, "[edge-skin] image=%d dma3: vertices=%u bytes=%u segments=%u/%u/%u (shadow/trust)\n",
                        image_id, n_vtx, n_vtx * stride, s8, s9, s10);
        }
    }
    /* Version trust for recorded-output checks and cross-image kernels. */
    const uint32_t key = 0x04000000u | scheme << 20 | (uint32_t)pos_only << 16 | (uint32_t)cfg[2] << 8 | cfg[3];
    ej_trust_slot* const slot = verify_all ? NULL : ej_trust(image_id, key, t_ej_sig);
    const int trusted = slot && slot->bad == 0 && slot->ok >= EJ_SKIN_TRUST_AFTER;
    if (!trusted && !verify_all && !(slot && slot->bad == 0)) return es_reject(image_id, ESR_TRUST);

    uint8_t* const out = ej_buf(&t_es_vtx, &t_es_vtx_cap, (size_t)n_vtx * stride);
    uint8_t* const ib = ej_buf(&t_es_idx, &t_es_idx_cap, (size_t)n_idx * 2u);
    if (!out || !ib) return es_size_reject(image_id, 5, D, snap, n_vtx, stride, n_idx, hole_bytes);
    if (es_skin_mesh_scheme(vtx, out, n_vtx, stride, fmt->a, fmt->n, pos_only, wts, t_es_pal, n_mat, scheme) < 0)
        return es_reject(image_id, ESR_PAL);
    memcpy(ib, idx, (size_t)n_idx * 2u);
    /* Edge's own whole-draw verdict, on the skinned positions. */
    const int visible = edge_any_visible(out, stride, n_vtx, ib, n_idx, vp, mtx, cull);
    es_draw d = { fmt->a, fmt->n, stride, pos_only, 0, 0, n_idx, cull };
    uint32_t w[512];
    const uint32_t nw = es_draw_words(&d, w, 512);
    const int use_call = nw * 4u > hole_bytes;
    if (!nw || (use_call && !call_on)) return es_reject(image_id, ESR_HOLE);

    if (!trusted) {   /* shadow: compare with the SPU's output after its run */
        if (!t_es_output) t_es_output = (es_output_log*)malloc(sizeof *t_es_output);
        if (!t_es_output) return es_reject(image_id, ESR_SIZES);
        t_es.image = image_id; t_es.pos_only = pos_only; t_es.visible = visible; t_es.key = key; t_es.sig = t_ej_sig;
        t_es.hole = hole; t_es.len = hole_bytes; t_es.n_vtx = n_vtx; t_es.n_idx = n_idx; t_es.nw = nw;
        t_es.scheme = scheme; t_es.slot = slot; t_es.fmt = *fmt;
        for (uint32_t k = 0; k < nw; k++) es_wbe32(t_es.words + 4u * k, w[k]);
        t_es.active = 1;
        InterlockedIncrement(&s_es_shadow[image_id - 10003]);
        return 0;
    }
    memset(t_es_hole, 0, hole_bytes);                       /* not visible: all NOPs, as the SPU */
    uint32_t io = 0;
    if (visible) {
        const uint32_t vb = (n_vtx * stride + 15u) & ~15u, ibb = (n_idx * 2u + 15u) & ~15u;
        const uint32_t ea = ej_arena_alloc(vb + ibb + (use_call ? (nw + 1u) * 4u : 0u), &io);
        if (!ea) return es_reject(image_id, ESR_ARENA);
        memcpy(vm_base + ea, out, (size_t)n_vtx * stride);
        memcpy(vm_base + ea + vb, ib, (size_t)n_idx * 2u);
        d.vtx_io = io; d.idx_io = io + vb;
        es_draw_words(&d, w, 512);
        uint8_t* cmd = use_call ? vm_base + ea + vb + ibb : t_es_hole;
        for (uint32_t k = 0; k < nw; k++) es_wbe32(cmd + 4u * k, w[k]);
        if (use_call) {   /* hole too small for the full index list: CALL a list in the arena */
            es_wbe32(cmd + 4u * nw, ES_RET);
            es_wbe32(t_es_hole, es_call(io + vb + ibb));
        }
    }
    t_ej_hole = hole; t_ej_len = hole_bytes; t_ej_stats = stats; t_ej_ntri = n_idx / 3u;
    ej_commit(t_es_hole);
    InterlockedIncrement(&s_es_native[image_id - 10003]);
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 4)
        fprintf(stderr, "[edge-skin] native image=%d job=%08X fmt=%02X/%02X scheme=%u%s verts=%u idx=%u mats=%u hole=%08X/%u io=%08X%s%s\n",
                image_id, job_ea, cfg[2], cfg[3], scheme, pos_only ? " pos-only" : "", n_vtx, n_idx, n_mat, hole, hole_bytes,
                io, use_call ? " call" : "", visible ? "" : " (not visible)");
    return 1;
}

/* Index ranges a hole draws (181C/1820 index array, 1824 runs). */
typedef struct { uint32_t off, mode, n; uint32_t first[256], count[256]; } ej_draws;
static void ej_scan_draw(const uint8_t* h, uint32_t len, ej_draws* o)
{
    o->off = o->mode = o->n = 0;
    for (uint32_t i = 0; i + 4 <= len;) {
        const uint32_t w = ej_be32(h + i);
        if (w == 0 || (w & 0xA0000003u)) { i += 4; continue; }   /* NOP, jump, call */
        const uint32_t m = w & 0xFFFCu, n = (w >> 18) & 0x7FFu, ni = (w >> 30) & 1u;
        for (uint32_t k = 0; k < n && i + 8 + 4 * k <= len; k++) {
            const uint32_t mk = ni ? m : m + 4 * k, v = ej_be32(h + i + 4 + 4 * k);
            if (mk == 0x181C) o->off = v;
            if (mk == 0x1820) o->mode = v;
            if (mk == 0x1824 && o->n < 256) { o->first[o->n] = v & 0xFFFFFFu; o->count[o->n] = (v >> 24) + 1u; o->n++; }
        }
        i += 4 + 4 * n;
    }
}
/* Distance in units in the last place, sign-magnitude floats. */
static uint32_t ej_ulp(uint32_t a, uint32_t b)
{
    const int64_t x = (a >> 31) ? -(int64_t)(a & 0x7FFFFFFFu) : (int64_t)a;
    const int64_t y = (b >> 31) ? -(int64_t)(b & 0x7FFFFFFFu) : (int64_t)b;
    const int64_t d = x > y ? x - y : y - x;
    return d > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)d;
}
static uint32_t ej_cmp_lsb(uint32_t a, uint32_t b)
{
    const int32_t d[3] = { ((int32_t)(a << 21) >> 21) - ((int32_t)(b << 21) >> 21),
                           ((int32_t)(a << 10) >> 21) - ((int32_t)(b << 10) >> 21), ((int32_t)a >> 22) - ((int32_t)b >> 22) };
    uint32_t m = 0;
    for (int c = 0; c < 3; c++) { const uint32_t v = (uint32_t)(d[c] < 0 ? -d[c] : d[c]); if (v > m) m = v; }
    return m;
}
/* Every triangle the SPU kept must be one of the job's (the SPU only drops).
 * 1 match, 0 mismatch, -1 incomplete evidence/allocation failure. */
static int ej_tris_subset(const uint8_t* orig, uint32_t n_idx, const ej_draws* dr, char* why, size_t wn)
{
    extern uint32_t gcm_io_offset_to_ea(uint32_t io);
    if ((dr->mode & 0xFu) != 1u || ((dr->mode >> 4) & 0xFu) != 1u) {
        snprintf(why, wn, "SPU index array mode %X (not main/u16)", dr->mode); return 0;
    }
    uint32_t cap = 64; while (cap < n_idx) cap <<= 1;     /* >= 3x the triangle count */
    uint64_t* set = (uint64_t*)calloc(cap, sizeof *set);
    if (!set) { snprintf(why, wn, "triangle comparison allocation failed"); return -1; }
    #define EJ_TRI(a, b, c) (1ull << 63 | (uint64_t)(a) << 32 | (uint64_t)(b) << 16 | (c))
    #define EJ_U16(p, i) ((uint32_t)(p)[2 * (i)] << 8 | (p)[2 * (i) + 1])
    for (uint32_t t = 0; t + 2 < n_idx; t += 3) {
        const uint64_t k = EJ_TRI(EJ_U16(orig, t), EJ_U16(orig, t + 1), EJ_U16(orig, t + 2));
        uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1u);
        while (set[h] && set[h] != k) h = (h + 1u) & (cap - 1u);
        set[h] = k;
    }
    /* The draw runs (at most 256 indices each) form one index stream inside
     * BEGIN/END: a triangle may straddle two runs. Join them, then check. */
    uint32_t total = 0;
    for (uint32_t r = 0; r < dr->n; r++) total += dr->count[r];
    uint8_t* joined = (uint8_t*)malloc((size_t)total * 2u + 2u);
    if (!joined) { free(set); snprintf(why, wn, "index comparison allocation failed"); return -1; }
    int ok = 1;
    for (uint32_t r = 0, at = 0; r < dr->n && ok; r++) {
        const uint32_t io = dr->off + 2u * dr->first[r], bytes = 2u * dr->count[r];
        const uint32_t ea = gcm_io_offset_to_ea(io);
        if (!ea || gcm_io_offset_to_ea(io + bytes - 1u) != ea + bytes - 1u) {
            snprintf(why, wn, "SPU draw %u: index run io %08X+%u unreadable", r, io, dr->count[r]);
            ok = -1; break;
        }
        if (!es_output_read(t_es_output, ea, joined + 2u * at, bytes)) {
            snprintf(why, wn, "missing recorded index PUT %08X/%u", ea, bytes); ok = -1; break;
        }
        at += dr->count[r];
    }
    if (ok > 0 && total % 3u) { snprintf(why, wn, "SPU index stream of %u is not whole triangles", total); ok = 0; }
    {
        const uint8_t* p = joined;
        for (uint32_t t = 0; t < total && ok > 0; t += 3) {
            const uint32_t a = EJ_U16(p, t), b = EJ_U16(p, t + 1), c = EJ_U16(p, t + 2);
            int found = 0;
            for (int rot = 0; rot < 3 && !found; rot++) {   /* the same triangle may start at another corner */
                const uint64_t k = rot == 0 ? EJ_TRI(a, b, c) : rot == 1 ? EJ_TRI(b, c, a) : EJ_TRI(c, a, b);
                uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1u);
                while (set[h] && set[h] != k) h = (h + 1u) & (cap - 1u);
                found = set[h] == k;
            }
            if (!found) { snprintf(why, wn, "SPU triangle %u/%u/%u is not in the job's index list", a, b, c); ok = 0; }
        }
    }
    #undef EJ_TRI
    #undef EJ_U16
    free(joined);
    free(set);
    return ok;
}
/* Native output vs what the SPU just wrote: 1 match, 0 mismatch (why), -1
 * nothing to compare (either side drew nothing). */
static int ej_skin_compare(char* why, size_t wn, uint32_t* max_ulp, uint32_t* max_lsb)
{
    extern uint32_t gcm_io_offset_to_ea(uint32_t io);
    const ej_skin_shadow* s = &t_es;
    ej_attrs n, p;
    ej_scan_attrs(s->words, s->nw * 4u, &n);
    uint8_t recorded_hole[sizeof t_es_hole];
    if (!es_output_read(t_es_output, s->hole, recorded_hole, s->len)) {
        snprintf(why, wn, "missing/overflowed recorded hole PUT"); return -1;
    }
    ej_scan_attrs(recorded_hole, s->len, &p);
    *max_ulp = *max_lsb = 0;
    if (!p.mask || !s->visible) return -1;
    if (n.mask != p.mask) { snprintf(why, wn, "attribute set native %04X spu %04X", n.mask, p.mask); return 0; }
    for (int k = 0; k < 16; k++) {
        if (!((n.mask >> k) & 1u)) continue;
        if (n.fmt[k] != p.fmt[k] || !(p.off[k] & 0x80000000u) || p.off[k] - p.off[0] != n.off[k] - n.off[0]) {
            snprintf(why, wn, "attribute %d: native %04X@+%u spu %04X@%08X (attr 0 at %08X)", k, n.fmt[k],
                     n.off[k] - n.off[0], p.fmt[k], p.off[k], p.off[0]);
            return 0;
        }
    }
    const uint32_t stride = s->fmt.stride, bytes = s->n_vtx * stride, io0 = p.off[0] & 0x7FFFFFFFu;
    const uint32_t ea0 = gcm_io_offset_to_ea(io0);
    if (!ea0 || gcm_io_offset_to_ea(io0 + bytes - 1u) != ea0 + bytes - 1u) return -1;
    uint8_t* const sv = ej_buf(&t_es_spu, &t_es_spu_cap, bytes);
    if (!sv || !es_output_read(t_es_output, ea0, sv, bytes)) {
        snprintf(why, wn, "missing recorded vertex PUT %08X/%u", ea0, bytes); return -1;
    }
    const uint32_t na = s->pos_only ? 1u : s->fmt.n;
    int bad = 0;
    for (uint32_t v = 0; v < s->n_vtx; v++)
        for (uint32_t k = 0; k < na; k++) {
            const es_attr* a = &s->fmt.a[k];
            const uint8_t *x = t_es_vtx + (size_t)stride * v + a->off, *y = sv + (size_t)stride * v + a->off;
            const int kind = es_kind(a);
            int diff = 0;
            if (kind == ES_POS) {
                for (int c = 0; c < 3; c++) {
                    const uint32_t u = ej_ulp(es_be32(x + 4 * c), es_be32(y + 4 * c));
                    if (u > *max_ulp) *max_ulp = u;
                    diff |= s->scheme >= 2u ? es_be32(x + 4 * c) != es_be32(y + 4 * c) : u > EJ_SKIN_POS_ULP;
                }
            } else if (kind == ES_DIR) {
                const uint32_t l = ej_cmp_lsb(es_be32(x), es_be32(y));
                if (l > *max_lsb) *max_lsb = l;
                diff = s->scheme >= 2u ? memcmp(x, y, 4) != 0 : l > EJ_SKIN_CMP_LSB;
            } else diff = memcmp(x, y, es_attr_bytes(a->fmt)) != 0;
            if (diff && !bad++) {
                const uint32_t nb = es_attr_bytes(a->fmt);
                int l = snprintf(why, wn, "vertex %u attr %u (%04X): native", v, a->attr, a->fmt);
                for (uint32_t i = 0; i < nb && i < 12 && l > 0 && (size_t)l < wn; i++) l += snprintf(why + l, wn - l, "%s%02X", i % 4 ? "" : " ", x[i]);
                if (l > 0 && (size_t)l < wn) l += snprintf(why + l, wn - l, " spu");
                for (uint32_t i = 0; i < nb && i < 12 && l > 0 && (size_t)l < wn; i++) l += snprintf(why + l, wn - l, "%s%02X", i % 4 ? "" : " ", y[i]);
            }
        }
    if (bad) {
        const size_t l = strlen(why);
        snprintf(why + l, wn - l, " (%d differing)", bad);
        return 0;
    }
    static __declspec(thread) ej_draws dr;
    ej_scan_draw(recorded_hole, s->len, &dr);
    if (dr.n) {
        const int subset = ej_tris_subset(t_es_idx, s->n_idx, &dr, why, wn);
        if (subset < 0) return -1;
        if (!subset) return 0;
    }
    return 1;
}
static void ej_skin_verify(void)
{
    ej_skin_shadow* const s = &t_es;
    s->active = 0;
    char why[400] = "";
    uint32_t ulp, lsb;
    const int r = ej_skin_compare(why, sizeof why, &ulp, &lsb);
    static volatile LONG compared, mismatched, skipped;
    if (r < 0) {
        InterlockedIncrement(&skipped);
        static volatile LONG missing[12];
        if (*why && InterlockedIncrement(&missing[s->image - 10003]) <= 4)
            fprintf(stderr, "[edge-skin] compare-skip image=%d: %s (PUTs=%u bytes=%u overflow=%d)\n",
                    s->image, why, t_es_output->n, t_es_output->bytes, t_es_output->overflow);
        return;
    }
    const LONG c = InterlockedIncrement(&compared);
    if (!r) InterlockedIncrement(&mismatched);
    char key[160];
    snprintf(key, sizeof key, "image=%d fmt=%02X/%02X%s scheme=%u sig=%08X", s->image, (s->key >> 8) & 255u, s->key & 255u,
             s->pos_only ? " pos-only" : "", s->scheme, s->sig);
    if (s->slot) {
        if (r) {
            const LONG ok = InterlockedIncrement(&s->slot->ok);
            if (ok <= EJ_SKIN_TRUST_AFTER)
                fprintf(stderr, "[edge-skin] key %s match %ld/%d (max %u ulp, %u lsb; %u verts, %u idx)%s\n", key, (long)ok,
                        EJ_SKIN_TRUST_AFTER, ulp, lsb, s->n_vtx, s->n_idx, ok == EJ_SKIN_TRUST_AFTER ? " -- native from now on" : "");
        } else if (InterlockedIncrement(&s->slot->bad) == 1)
            fprintf(stderr, "[edge-skin] key %s MISMATCH -- kept on the SPU: %s (max %u ulp, %u lsb)\n", key, why, ulp, lsb);
    } else {   /* EDGE_VERIFY=1: every job compared, a few lines per key */
        static volatile LONG seen[256];
        if (!r && InterlockedIncrement(&seen[(s->key ^ s->sig ^ (uint32_t)s->image) & 255u]) <= 3)
            fprintf(stderr, "[edge-skin] key %s MISMATCH: %s (max %u ulp, %u lsb)\n", key, why, ulp, lsb);
    }
    if ((c & 0xFFF) == 0)
        fprintf(stderr, "[edge-skin] compared=%ld mismatched=%ld not comparable=%ld\n", (long)c, (long)mismatched, (long)skipped);
}

/* Which Edge job owns a command-buffer hole (for a FIFO parked on one). */
typedef struct { uint32_t hole, job, spu; int image; int path; LONG seq; } ej_hole_rec;
static ej_hole_rec s_ej_holes[4096];
static volatile LONG s_ej_hole_seq;
static void ej_hole_note(uint32_t hole, uint32_t job, int image, int path)
{
    const LONG n = InterlockedIncrement(&s_ej_hole_seq);
    ej_hole_rec* r = &s_ej_holes[(uint32_t)n & 4095u];
    r->hole = hole; r->job = job; r->image = image; r->path = path; r->spu = g_spurs_job_spu_num; r->seq = n;
}
void spurs_edge_hole_report(uint32_t ea)
{
    static const char* const why[] = { "SPU started", "native", "SPU done", "SPU failed" };
    const LONG now = s_ej_hole_seq;
    int found = 0;
    uint32_t spu = 0xFFFFFFFFu;
    for (LONG i = now; i > now - 4096 && i > 0; i--) {
        const ej_hole_rec* r = &s_ej_holes[(uint32_t)i & 4095u];
        if (r->seq != i || r->hole != ea) continue;
        fprintf(stderr, "[edge-hole] FIFO parked on hole %08X: job %08X image=%d spu=%u %s (%ld Edge jobs ago)\n",
                ea, r->job, r->image, r->spu, why[r->path & 3], (long)(now - i));
        if (spu == 0xFFFFFFFFu) spu = r->spu;
        if (++found >= 3) break;
    }
    /* The output ring is per SPU number: list that SPU's recent SPU-path jobs
     * in the order they ran. A hole later in the command buffer than the
     * parked one that ran before it means the ring was filled out of FIFO
     * order -- the parked job then waits for space only a later hole frees. */
    if (spu != 0xFFFFFFFFu) {
        int shown = 0;
        for (LONG i = now; i > now - 4096 && i > 0 && shown < 16; i--) {
            const ej_hole_rec* r = &s_ej_holes[(uint32_t)i & 4095u];
            if (r->seq != i || r->spu != spu || r->path == 1) continue;
            fprintf(stderr, "[edge-hole]   spu=%u seq=%ld hole=%08X%s job=%08X image=%d %s\n", spu, (long)i, r->hole,
                    r->hole > ea ? " (after parked)" : r->hole == ea ? " (parked)" : "", r->job, r->image, why[r->path & 3]);
            shown++;
        }
    }
    if (!found)
        fprintf(stderr, "[edge-hole] FIFO parked at %08X: no Edge job with that hole among the last 4096 (latest seq %ld)\n",
                ea, (long)now);
}

#include "spurs_job_input_snap.h"
__declspec(thread) const sj_snapshot* g_sj_snap;
static inline uint64_t sj_be(const uint8_t* p, int n)
{
    uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v;
}

/* ===== Native Edge Animation (images 10006 and 10002) ===========================
 *
 * Image 10002 is a smaller build of the same Edge Animation job (no completion
 * flag, no post-process, tree rooted at branch 0); edge_anim.h takes both.
 * edge_anim.h runs the whole job on the host (blend tree, clip evaluation,
 * blends, local->world, joint matrices) in a private local store staged the
 * way this file stages the SPU's, and lists the PUTs the job makes: the
 * matrix / user-channel block to user word 0x34, then the 16-byte completion
 * PUTF (byte 0 = 1) to user word 0x3C that the main thread waits on
 * (docs/wiki/fix-log.md, object+0xA0). Spec: native/analysis/edge-anim-spec.md.
 *
 * Shadow first: the first jobs of every key (skeleton, tree size, blend ops
 * and channel kinds used) still run on the SPU; the native PUTs are built
 * aside and compared byte for byte with the PUTs the SPU makes
 * (spurs_job_cap_put). EA_TRUST_AFTER matches take the key native, one
 * mismatch keeps it on the SPU for the run. Jobs the port does not take
 * (edge_anim.h reasons) and every job while it is switched off run on the SPU.
 * Off: EDGE_ANIM=0, or a no_anim_native file in the working directory
 * (checked each second). EDGE_ANIM_VERIFY=1: compare every job, never native.
 * SPURS_JOB_DIRECT_STAGE=1 leaves the SPU local store unzeroed between jobs;
 * bytes the job never writes are then stale, so the port stays off.
 * Capture: an anim_cap file -> the next 500 jobs, SPU run, to
 * animcap/<n>.bin (ANIMCAP1, read by tools/tests/edge_anim). */
#include "edge_anim.h"
#define EA_TRUST_AFTER 3
enum { EAG_OFF, EAG_DIRECT, EAG_UNTRUSTED, EAG_TABLE, EAG_CAPTURE, EAG_COUNT };
static volatile LONG s_ea_native, s_ea_shadow, s_ea_compared, s_ea_match, s_ea_mismatch, s_ea_skipped;
static volatile LONG s_ea_reason[EAN_R_COUNT], s_ea_gate[EAG_COUNT];
typedef struct { volatile LONG key, ok, bad; } ea_trust_slot;
static ea_trust_slot s_ea_trust[512];
static ea_trust_slot* ea_trust(uint32_t key32)
{
    const LONG key = (LONG)(key32 | 0x80000000u);
    uint32_t h = ((uint32_t)key * 2654435761u) >> 23;
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & 511u) {
        if (s_ea_trust[h].key == key) return &s_ea_trust[h];
        if (s_ea_trust[h].key == 0 && InterlockedCompareExchange(&s_ea_trust[h].key, key, 0) == 0) return &s_ea_trust[h];
        if (s_ea_trust[h].key == key) return &s_ea_trust[h];
    }
    return NULL;
}
/* the SPU's PUTs during a shadowed run */
#define EA_REC_MAX 32
typedef struct { int on, overflow; uint32_t n, bytes, ea[EA_REC_MAX], size[EA_REC_MAX], off[EA_REC_MAX]; uint8_t data[64u << 10]; } ea_rec;
static __declspec(thread) ea_rec* t_ea_rec;
static void edge_anim_rec_put(uint32_t ea, const void* data, uint32_t size)
{
    ea_rec* r = t_ea_rec;
    if (!r || !r->on) return;
    if (r->n >= EA_REC_MAX || r->bytes + size > sizeof r->data) { r->overflow = 1; return; }
    r->ea[r->n] = ea; r->size[r->n] = size; r->off[r->n] = r->bytes;
    memcpy(r->data + r->bytes, data, size);
    r->bytes += size; r->n++;
}
/* the native result of the job in flight on this thread */
static __declspec(thread) struct {
    int active; uint32_t key, job_ea, nput, bytes; ean_put put[EAN_MAX_PUTS]; uint8_t data[64u << 10]; ea_trust_slot* slot;
} *t_eas;
static __declspec(thread) uint8_t* t_ea_ls;
static __declspec(thread) FILE* t_ea_cap;   /* capture header written, handed to t_cap_file */
static int ea_guest_get(void* u, uint32_t ea, uint8_t* dst, uint32_t size)
{
    (void)u;
    if (!size) return 0;
    if ((uint64_t)ea + size > 0xF0000000ull) return -1;
    memcpy(dst, vm_base + ea, size);
    return 0;
}
/* a PUT as runtime/spu/spu_dma.h performs it: lock-line coherency, the
 * completion-byte waiter wake, the PUT hook */
static void ea_guest_put(uint32_t ea, const uint8_t* src, uint32_t size)
{
    extern int spu_coh_is_reserved(uint32_t);
    extern void spu_coh_notify_write(uint32_t);
    extern void spu_lockline_lock(void);
    extern void spu_lockline_unlock(void);
    extern volatile uint32_t g_spu_put_wake_ea;
    extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { reserved = 1; break; } if (a == a1) break; }
    if (reserved) {
        spu_lockline_lock();
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write(a); if (a == a1) break; }
        spu_lockline_unlock();
    } else {
        memcpy(vm_base + ea, src, size);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) { spu_lockline_lock(); spu_coh_notify_write(a); spu_lockline_unlock(); }
            if (a == a1) break;
        }
    }
    MemoryBarrier();
    const uint32_t w = g_spu_put_wake_ea;
    if (w && w - ea < size) WakeByAddressAll(vm_base + w);
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}
static void edge_anim_stats(void)
{
    static volatile ULONGLONG last;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    char t[512];
    int l = snprintf(t, sizeof t, "[edge-anim] native=%ld shadowed=%ld compared=%ld match=%ld mismatch=%ld not-compared=%ld | spu:",
                     (long)s_ea_native, (long)s_ea_shadow, (long)s_ea_compared, (long)s_ea_match, (long)s_ea_mismatch, (long)s_ea_skipped);
    static const char* const gn[EAG_COUNT] = { "off", "direct-stage", "untrusted", "trust-table-full", "capture" };
    for (int k = 0; k < EAG_COUNT; k++) if (s_ea_gate[k]) l += snprintf(t + l, sizeof t - l, " %s=%ld", gn[k], (long)s_ea_gate[k]);
    for (int k = 1; k < EAN_R_COUNT; k++) if (s_ea_reason[k]) l += snprintf(t + l, sizeof t - l, " %s=%ld", ean_reason_name[k], (long)s_ea_reason[k]);
    fprintf(stderr, "%s\n", t);
}
/* ANIMCAP1 header: descriptor and inputs as the stager sees them */
static FILE* edge_anim_cap_open(int image_id, uint32_t job_ea, const uint8_t* D, uint32_t dsz, const sj_snapshot* snap)
{
    static volatile LONG left, seq; static volatile ULONGLONG chk;
    if (left <= 0) {
        const ULONGLONG now = GetTickCount64();
        if (now - chk < 1000) return NULL;
        chk = now;
        if (GetFileAttributesA("anim_cap") == INVALID_FILE_ATTRIBUTES) return NULL;
        DeleteFileA("anim_cap"); CreateDirectoryA("animcap", NULL); left = 500;
        fprintf(stderr, "[edge-anim] capturing 500 jobs to animcap/\n");
    }
    if (InterlockedDecrement(&left) < 0) return NULL;
    char path[96];
    snprintf(path, sizeof path, "animcap/%d_%05ld.bin", image_id, InterlockedIncrement(&seq));
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    const uint32_t n_dma = ej_be32(D + 0x08) & 0xFFFFu, nd = n_dma / 8u;
    uint32_t n_cache = ej_be32(D + 0x24) / 8u;
    if (n_cache > 4) n_cache = 4;
    fwrite("ANIMCAP1", 1, 8, f);
    fwrite(&job_ea, 4, 1, f); fwrite(&g_spurs_job_spu_num, 4, 1, f); fwrite(&dsz, 4, 1, f); fwrite(D, 1, dsz, f);
    static const uint8_t zero[0x8000];
    fwrite(&nd, 4, 1, f);
    for (uint32_t i = 0; i < nd; i++) {
        const uint32_t sz = ej_be32(D + 0x30 + 8u * i) & 0x7FFFu, ea = ej_be32(D + 0x34 + 8u * i);
        fwrite(&sz, 4, 1, f); fwrite(&ea, 4, 1, f);
        const uint8_t* src = (snap && i < snap->n_dma && snap->seg[i]) ? snap->seg[i] : ea ? vm_base + ea : zero;
        fwrite(src, 1, sz, f);
    }
    fwrite(&n_cache, 4, 1, f);
    for (uint32_t i = 0; i < n_cache; i++) {
        uint32_t sz = ej_be32(D + 0x30 + 8u * (nd + i)), ea = ej_be32(D + 0x34 + 8u * (nd + i));
        if (sz > 0x40000u || !ea) sz = 0;
        fwrite(&sz, 4, 1, f); fwrite(&ea, 4, 1, f);
        if (sz) fwrite(vm_base + ea, 1, sz, f);
    }
    return f;
}
/* 1: done natively. 2: runs on the SPU, compare afterwards (edge_anim_verify).
 * 0: runs on the SPU. */
static int edge_anim_job(int image_id, uint32_t job_ea, uint32_t job_desc_size, const sj_snapshot* snap)
{
    static LONG on = -1, verify = -1;
    if (on < 0) { const char* e = getenv("EDGE_ANIM"); on = !(e && *e == '0'); }
    if (verify < 0) { const char* e = getenv("EDGE_ANIM_VERIFY"); verify = e && *e && *e != '0';
                      if (verify) fprintf(stderr, "[edge-anim] verify: every job runs on the SPU, native compared\n"); }
    {   static volatile ULONGLONG checked; static volatile LONG file_off;
        const ULONGLONG now = GetTickCount64();
        if (now - checked > 1000) {
            checked = now;
            edge_anim_stats(); /* expose off/direct-stage early-return gates */
            const LONG off = GetFileAttributesA("no_anim_native") != INVALID_FILE_ATTRIBUTES;
            if (off != file_off) fprintf(stderr, "[edge-anim] native %s by no_anim_native file\n", off ? "OFF" : "ON");
            file_off = off;
        }
        if (file_off || !on) { InterlockedIncrement(&s_ea_gate[EAG_OFF]); return 0; }
    }
    if (direct_stage_enabled()) { InterlockedIncrement(&s_ea_gate[EAG_DIRECT]); return 0; }
    if (!t_ea_ls) t_ea_ls = (uint8_t*)VirtualAlloc(NULL, EAN_LS_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t_eas) t_eas = VirtualAlloc(NULL, sizeof *t_eas, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t_ea_rec) t_ea_rec = (ea_rec*)VirtualAlloc(NULL, sizeof *t_ea_rec, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t_ea_ls || !t_eas || !t_ea_rec) return 0;
    t_eas->active = 0; t_ea_rec->on = 0;
    uint32_t dsz = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    const uint8_t* D = (snap && dsz <= snap->dsz) ? snap->desc : vm_base + job_ea;
    t_ea_cap = edge_anim_cap_open(image_id, job_ea, D, dsz, snap);
    ean_job J;
    memset(&J, 0, sizeof J);
    J.desc = D; J.dsz = dsz; J.job_ea = job_ea; J.spu_num = g_spurs_job_spu_num; J.image = image_id;
    J.seg = snap ? snap->seg : NULL; J.nseg = snap ? snap->n_dma : 0;
    J.get = ea_guest_get; J.ls = t_ea_ls;
    const int r = ean_run_job(&J);
    if (r) { InterlockedIncrement(&s_ea_reason[r < EAN_R_COUNT ? r : 0]); edge_anim_stats(); return 0; }
    /* key: skeleton, tree size, ops, channel kinds -- the code paths the job took */
    const uint8_t* cl = D + 0x30 + 8u * ((ej_be32(D + 0x08) & 0xFFFFu) / 8u);   /* cache list */
    uint32_t key = 2166136261u;
    const uint32_t nd = (ej_be32(D + 0x08) & 0xFFFFu) / 8u, nc = ej_be32(D + 0x24) / 8u;
    key = ej_fnv(key, (uint32_t)image_id); key = ej_fnv(key, nd); key = ej_fnv(key, nc);
    if (nc >= 2) { key = ej_fnv(key, ej_be32(cl + 8)); key = ej_fnv(key, ej_be32(cl + 12)); }   /* skeleton cache element */
    key = ej_fnv(key, J.leaves << 16 | J.branches); key = ej_fnv(key, J.opmask); key = ej_fnv(key, J.chanmask);
    ea_trust_slot* const slot = verify ? NULL : ea_trust(key);
    if (!verify && !slot) { InterlockedIncrement(&s_ea_gate[EAG_TABLE]); return 0; }
    if (!verify && slot->bad) { InterlockedIncrement(&s_ea_gate[EAG_UNTRUSTED]); edge_anim_stats(); return 0; }
    if (!verify && !t_ea_cap && slot->ok >= EA_TRUST_AFTER) {
        for (uint32_t i = 0; i < J.nput; i++) {
            if (i + 1 == J.nput) MemoryBarrier();   /* data before the completion flag */
            ea_guest_put(J.put[i].ea, t_ea_ls + J.put[i].lsa, J.put[i].size);
        }
        InterlockedIncrement(&s_ea_native);
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            fprintf(stderr, "[edge-anim] native image=%d job=%08X joints=%u leaves=%u branches=%u key=%08X puts=%u\n",
                    image_id, job_ea, J.njoints, J.leaves, J.branches, key, J.nput);
        edge_anim_stats();
        return 1;
    }
    if (t_ea_cap) InterlockedIncrement(&s_ea_gate[EAG_CAPTURE]);
    /* shadow: keep the native PUTs, record the SPU's */
    t_eas->bytes = 0; t_eas->nput = 0;
    for (uint32_t i = 0; i < J.nput; i++) {
        if (t_eas->bytes + J.put[i].size > sizeof t_eas->data) return 0;
        t_eas->put[i] = J.put[i];
        t_eas->put[i].lsa = t_eas->bytes;
        memcpy(t_eas->data + t_eas->bytes, t_ea_ls + J.put[i].lsa, J.put[i].size);
        t_eas->bytes += J.put[i].size; t_eas->nput++;
    }
    t_eas->key = key; t_eas->job_ea = job_ea; t_eas->slot = slot; t_eas->active = 1;
    t_ea_rec->n = t_ea_rec->bytes = 0; t_ea_rec->overflow = 0; t_ea_rec->on = 1;
    InterlockedIncrement(&s_ea_shadow);
    return 2;
}
static void edge_anim_verify(int failed)
{
    if (!t_eas || !t_eas->active) return;
    t_eas->active = 0; t_ea_rec->on = 0;
    if (failed || t_ea_rec->overflow) { InterlockedIncrement(&s_ea_skipped); return; }
    char why[256] = "";
    int bad = 0;
    if (t_ea_rec->n != t_eas->nput) { snprintf(why, sizeof why, "%u PUTs native, %u SPU", t_eas->nput, t_ea_rec->n); bad = 1; }
    for (uint32_t i = 0; i < t_eas->nput && i < t_ea_rec->n && !bad; i++) {
        const ean_put* p = &t_eas->put[i];
        if (p->ea != t_ea_rec->ea[i] || p->size != t_ea_rec->size[i]) {
            snprintf(why, sizeof why, "PUT %u: native %08X/%u, SPU %08X/%u", i, p->ea, p->size, t_ea_rec->ea[i], t_ea_rec->size[i]);
            bad = 1; break;
        }
        const uint8_t *a = t_eas->data + p->lsa, *b = t_ea_rec->data + t_ea_rec->off[i];
        for (uint32_t o = 0; o < p->size; o++)
            if (a[o] != b[o]) {
                const uint32_t q = o & ~3u;
                snprintf(why, sizeof why, "PUT %u (ea %08X, %u bytes) first differs at +0x%X: native %08X SPU %08X", i, p->ea, p->size, o,
                         ej_be32(a + q), ej_be32(b + q));
                bad = 1; break;
            }
    }
    InterlockedIncrement(&s_ea_compared);
    InterlockedIncrement(bad ? &s_ea_mismatch : &s_ea_match);
    ea_trust_slot* s = t_eas->slot;
    if (s) {
        if (!bad) {
            const LONG ok = InterlockedIncrement(&s->ok);
            if (ok == EA_TRUST_AFTER)
                fprintf(stderr, "[edge-anim] key %08X matched the SPU %d times -- native from now on\n", t_eas->key, EA_TRUST_AFTER);
        } else if (InterlockedIncrement(&s->bad) == 1)
            fprintf(stderr, "[edge-anim] key %08X MISMATCH job=%08X -- kept on the SPU: %s\n", t_eas->key, t_eas->job_ea, why);
    } else if (bad) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 16)
            fprintf(stderr, "[edge-anim] verify MISMATCH key %08X job=%08X: %s\n", t_eas->key, t_eas->job_ea, why);
    }
    edge_anim_stats();
}
/* ===== end of native Edge Animation ============================================ */
/* Effect ribbon vertices (image 10009, JOBCRT Ver14): the port and its shadow /
 * trust / capture glue (uses the Edge Animation PUT recorder and ea_guest_put
 * above). Spec: native/analysis/jobcrt10009-spec.md. */
#define RBN_SPURS_GLUE
#include "jobcrt_ribbon.h"
int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    const spurs_job_diag_options* const diag = job_diag_config();
    const uint32_t snapshot_handle = s_job_input_handle;
    sj_scope stage_perf = sj_begin(image_id);
    ejp_scope edge_perf = ejp_begin(image_id);
    if (!entry || !job_ea) { ejp_end(&edge_perf, 1); sj_end(&stage_perf, 1); return -1; }

    /* ---- descriptor ---------------------------------------------------- */
    /* Read the descriptor from the queue-time snapshot when there is one
     * (spurs_job_input_snap.h), else from guest memory. */
    const sj_snapshot* snap = (g_sj_snap && g_sj_snap->job_ea == job_ea) ? g_sj_snap : NULL;
#define SJD(off, n) ((snap && (uint32_t)(off) + (n) <= snap->dsz) ? sj_be(snap->desc + (off), (n)) :                      ((n) == 8 ? g64(job_ea + (off)) : (n) == 4 ? (uint64_t)g32(job_ea + (off)) : (uint64_t)g16(job_ea + (off))))
    uint32_t ea_bin   = (uint32_t)(SJD(JH_EA_BINARY, 8) & ~1ull);
    uint32_t size_bin = (uint32_t)SJD(JH_SIZE_BINARY, 2) << 4;
    uint32_t n_dma    = (uint32_t)SJD(JH_SIZE_DMA_LIST, 2) / 8u;
    uint32_t use_io   = (uint32_t)SJD(JH_USE_INOUT_BUFFER, 4);
    uint32_t size_io  = (uint32_t)SJD(JH_SIZE_IN_OR_INOUT, 4);
    uint32_t size_out = (uint32_t)SJD(JH_SIZE_OUT, 4);
    uint32_t size_stk = (uint32_t)SJD(JH_SIZE_STACK, 2) << 4;
    uint32_t size_scr = (uint32_t)SJD(JH_SIZE_SCRATCH, 2) << 4;
    uint32_t n_cache  = (uint32_t)SJD(JH_SIZE_CACHE_DMA_LIST, 4) / 8u;

    /* Edge geometry jobs: native when the input format is known and has been
     * checked against the SPU (ej_trust); EDGE_VERIFY=1 runs all on the SPU
     * and compares every one. */
    uint8_t* ev_buf = NULL; uint32_t ev_hole = 0, ev_len = 0, ev_fmt = 0;
    ej_trust_slot* ev_slot = NULL;
    int ev_learn = 0, ev_ud = 0; uint32_t ev_learn_f23 = 0, ev_learn_io = 0, ev_learn_sig = 0, ev_sig = 0, ev_udmode = 0;
    uint32_t ej_hole_ea = 0;
    int es_shadow = 0;                 /* vertex-output job: compare its ring output (t_es) */
    static int ev_on = -1;
    if (ev_on < 0) { const char* e = getenv("EDGE_VERIFY"); ev_on = e && *e && *e != '0';
                     if (ev_on) fprintf(stderr, "[edge-verify] on: every Edge job runs on the SPU, native holes compared\n"); }
    if (image_id >= 10003 && image_id <= 10014 && !(image_id >= 10006 && image_id <= 10010)) {
        static volatile LONG nat[12], spu[12];
        static __declspec(thread) uint8_t scratch[4096];
        const int g = image_id - 10003;
        if (n_dma >= 17) {   /* hole EA from userData, for the parked-FIFO report */
            const uint32_t ud = JH_SIZE + n_dma * 8u + 12u;
            ej_hole_ea = (snap && ud + 4u <= snap->dsz) ? ej_be32(snap->desc + ud) : g32(job_ea + ud);
        }
        t_ej_shadow = scratch;
        const int can = edge_geom1_native(job_ea, n_dma, snap, image_id);
        t_ej_shadow = NULL;
        /* Vertex-output jobs: native output, or the SPU with a shadow check. */
        int skin = 0;
        if (!can && (t_ej_reject == EJR_UD || t_ej_reject == EJR_NOFMT)) {
            skin = edge_skin_job(job_ea, n_dma, snap, image_id, ev_on);
            es_shadow = !skin && t_es.active;
            if (skin) InterlockedDecrement(&s_ej_reject[g][t_ej_reject]);   /* counted as native below */
        }
        if (!can && !skin && t_ej_reject == EJR_UD) {
            ev_ud = 1; ev_learn_f23 = t_ej_f23; ev_learn_io = t_ej_vtx_io; ev_hole = t_ej_hole; ev_len = t_ej_len;
            ev_udmode = t_ej_udmode;
        }
        if (!can && !skin && t_ej_reject == EJR_NOFMT && !ev_on) {
            ev_learn = 1; ev_learn_f23 = t_ej_f23; ev_learn_io = t_ej_vtx_io; ev_hole = t_ej_hole; ev_len = t_ej_len;
            ev_learn_sig = t_ej_sig;
        }
        int done = skin;
        if (can) {
            ej_trust_slot* slot = ev_on ? NULL : ej_trust(image_id, t_ej_fmt, t_ej_sig);
            if (slot && slot->bad == 0 && slot->ok >= EJ_TRUST_AFTER) {
                ej_commit(scratch);
                done = 1;
            } else if (t_ej_visible && (ev_on || (slot && slot->bad == 0))) {
                ev_buf = scratch; ev_hole = t_ej_hole; ev_len = t_ej_len; ev_fmt = t_ej_fmt; ev_slot = slot; ev_sig = t_ej_sig;
            }
        }
        const LONG total = InterlockedIncrement(done ? &nat[g] : &spu[g]) + (done ? spu[g] : nat[g]);
        if ((total & 0x7FFF) == 0) {
            const volatile LONG* r = s_ej_reject[g];
            fprintf(stderr, "[edge-native] image=%d jobs native=%ld spu=%ld (spu: dma %ld sizes %ld cfg %ld nofmt %ld idx %ld ud %ld io %ld hole %ld)\n",
                    image_id, (long)nat[g], (long)spu[g], (long)r[EJR_DMA], (long)r[EJR_SIZES], (long)r[EJR_CFG],
                    (long)r[EJR_NOFMT], (long)r[EJR_IDX], (long)r[EJR_UD], (long)r[EJR_IO], (long)r[EJR_HOLE]);
            const volatile LONG* s = s_es_reject[g];
            LONG any = s_es_native[g] | s_es_shadow[g];
            for (int k = 0; k < ESR_COUNT; k++) any |= s[k];
            if (any)
                fprintf(stderr, "[edge-skin] image=%d vertex-output jobs native=%ld shadowed=%ld (spu: mode %ld blend %ld cfg %ld dma %ld "
                        "fmt %ld sizes %ld pal %ld hole %ld arena %ld trust %ld passthrough %ld)\n", image_id, (long)s_es_native[g], (long)s_es_shadow[g],
                        (long)s[ESR_MODE], (long)s[ESR_BLEND], (long)s[ESR_CFG], (long)s[ESR_DMA], (long)s[ESR_FMT], (long)s[ESR_SIZES],
                        (long)s[ESR_PAL], (long)s[ESR_HOLE], (long)s[ESR_ARENA], (long)s[ESR_TRUST], (long)s[ESR_PASSTHROUGH]);
        }
        if (ej_hole_ea) ej_hole_note(ej_hole_ea, job_ea, image_id, done ? 1 : 0);
        if (done) { ejp_end(&edge_perf, 0); sj_end(&stage_perf, 0); return 0; }
    }
    ejp_next(&edge_perf, EJP_SETUP);
    /* Edge animation (10006, 10002): native, or the SPU with a shadow check */
    int ea_shadow = 0;
    if (image_id == 10006 || image_id == 10002) {
        const int ea = edge_anim_job(image_id, job_ea, job_desc_size, snap);
        if (ea == 1) {
            g_spurs_job_mbox_valid = 0; g_spurs_job_mbox = g_spurs_job_mbox_intr = 0;
            { const uint32_t cb = g32(job_ea + 0x4C); g_spurs_job_cmd = cb ? (g32(cb) >> 16) : 0; }
            ejp_end(&edge_perf, 0); sj_end(&stage_perf, 0);
            return 0;
        }
        ea_shadow = ea == 2;
    }
    /* Effect ribbon vertices (10009): native, or the SPU with a shadow check */
    int rb_shadow = 0;
    if (image_id == 10009) {
        const int rb = ribbon_job(job_ea, job_desc_size, snap);
        if (rb == 1) {
            g_spurs_job_mbox_valid = 0; g_spurs_job_mbox = g_spurs_job_mbox_intr = 0;
            { const uint32_t cb = g32(job_ea + 0x4C); g_spurs_job_cmd = cb ? (g32(cb) >> 16) : 0; }
            ejp_end(&edge_perf, 0); sj_end(&stage_perf, 0);
            return 0;
        }
        rb_shadow = rb == 2;
    }
    /* Macross notify job (image 10001) natively, after it has been checked
     * against the SPU (10002 is Edge Animation, above). 0: run the SPU
     * below -- a shadow comparison of that run may be armed (nn_shadow). */
    int nn_shadow = 0;
    if (image_id == 10001) {
        uint32_t nn_dsz = job_desc_size ? job_desc_size : 256u;
        if (nn_dsz > 0x400u) nn_dsz = 0x400u;
        const uint8_t* nn_desc = (snap && nn_dsz <= snap->dsz) ? snap->desc : vm_base + job_ea;
        uint32_t nn_mbox = 0, nn_intr = 0;
        const int nn = macross_notify_job_native(image_id, job_ea, nn_desc, nn_dsz, n_dma, g_spurs_job_spu_num,
                                                 ea_bin, size_bin, &nn_mbox, &nn_intr);
        if (nn != 0) {   /* 1: done natively, -1: the job asserted (as the SPU job would) */
            g_spurs_job_mbox = nn_mbox;
            g_spurs_job_mbox_intr = nn_intr;
            g_spurs_job_mbox_valid = nn > 0 && (nn_mbox || nn_intr);
            { uint32_t _cb = g32(job_ea + 0x4C);
              g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
            ejp_end(&edge_perf, nn < 0); sj_end(&stage_perf, nn < 0);
            return nn > 0 ? 0 : -1;
        }
        nn_shadow = macross_notify_shadow_active();
    }
    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        ejp_end(&edge_perf, 1); sj_end(&stage_perf, 1);
        return -1;
    }

    spu_context stack_ctx;
    spu_context* const ctxp = spurs_job_ctx_slot(&stack_ctx);
#define ctx (*ctxp)
    const int direct_stage = direct_stage_enabled();
    uint8_t* ls;
    if (direct_stage) {
        spurs_job_ctx_reset(&ctx, &stack_ctx);
        ls = ctx.ls;
    } else {
        ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
        if (!ls) { ejp_end(&edge_perf, 1); sj_end(&stage_perf, 1); return -1; }
    }
    sj_next(&stage_perf, SJ_STAGE);

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ----
     * A worker's persistent context usually runs the same Edge binary job
     * after job (~17k/s); re-copying its 90 KB each time was ~15% of worker
     * time. Skip the copy when this context already holds that binary.
     * Safety: every 64th skip compares the LS copy with the guest binary,
     * and one mismatch (a job that writes into its own image) turns the
     * skip off for good. File no_job_bin_cache disables it. */
    {
        static __declspec(thread) uint8_t* t_bin_ls; static __declspec(thread) uint32_t t_bin_ea, t_bin_size;
        static volatile LONG s_bin_cache_off = -1; static volatile LONG s_bin_checks;
        if (s_bin_cache_off < 0)
            s_bin_cache_off = GetFileAttributesA("no_job_bin_cache") != INVALID_FILE_ATTRIBUTES;
        int have = direct_stage && !s_bin_cache_off && t_bin_ls == ls && t_bin_ea == ea_bin && t_bin_size == size_bin;
        LONG nchk = 0;
        if (have && ((nchk = InterlockedIncrement(&s_bin_checks)) <= 4096 || (nchk & 63) == 0) &&
            memcmp(ls, vm_base + ea_bin, size_bin) != 0) {
            fprintf(stderr, "[spurs-job] binary cache: LS copy of %08X differs from guest -- caching off\n", ea_bin);
            s_bin_cache_off = 1; have = 0;
        }
        if (!have) memcpy(ls, vm_base + ea_bin, size_bin);
        t_bin_ls = direct_stage ? ls : NULL; t_bin_ea = ea_bin; t_bin_size = size_bin;
    }

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint32_t io_ls     = p;                p += ALIGN1024(size_io);
    uint32_t out_ls    = p;                p += ALIGN1024(size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = p;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = SJD(cache_base - job_ea + i * 8u, 8);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (p + sz > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = SJD(dma_base - job_ea + i * 8u, 8);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, (snap && i < snap->n_dma && snap->seg[i]) ? snap->seg[i] : vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (p > ctx_ls)
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
    if (snap && dsz <= snap->dsz) memcpy(ls + desc_ls, snap->desc, dsz);
    else memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13" crt (WWS/Edge jobs,
     * Twisted Metal's JobQueue jobs): jm2 parks an LS pointer to its kernel
     * block there, and the crt returns 0x80410A11 and STOPs before main when
     * it is 0 -- so the job never runs and never clears its completion
     * counter. Hand it the free 16 bytes after the context (zeroed); the crt
     * only stashes the pointer and clears the field again. */
    uint32_t jm_iface = memcmp(ls + 0x30, "JOBCRT Ver13", 12) == 0 ? ctx_ls + 0x30 : 0;
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data, so overlay
     * the kernel's view: SPU 0, our DMA tag, tracing off. */
    if (jm_iface) {
        memset(ls + 0x170, 0, 0x10);
        memset(ls + 0x1C0, 0, 0x10);
        ls32(ls, 0x1C8, g_spurs_job_spu_num);
        ls32(ls, 0x1CC, JOB_DMA_TAG);
        memset(ls + 0x210, 0, 0x10);
    }
    /* The other job CRTs also link cellSpursGetCurrentSpuId, without the
     * Ver13 marker. Recognize its actual instruction contract, independent of
     * image id or helper address: lqa r3,0x1c0; rotqbyi r3,r3,8; bi r0.
     * geom1/geom3 otherwise read the literal 300 at LS1C8 as an SPU index,
     * selecting unrelated memory as their ring descriptor. Only replace the
     * ABI's four-byte spuNum field: adjacent low-LS bytes belong to the raw
     * image and must remain intact. This is staging, not a binary fingerprint
     * change or an execution bypass. */
    if (!jm_iface) {
        static const uint8_t spu_num_service[12] = {
            0x30,0x80,0x38,0x03, 0x3F,0x82,0x01,0x83, 0x35,0x00,0x00,0x00
        };
        /* The scan walked the whole ~90 KB image on every job (most of the
         * ~10 us staging cost). The image is immutable, so remember where
         * the helper is (or that it is absent) per binary, per thread. */
        static __declspec(thread) struct { uint32_t ea, size, off; } t_scan[8];
        static __declspec(thread) unsigned t_scan_next;
        uint32_t start = 0, stop = size_bin;
        for (unsigned k = 0; k < 8; k++)
            if (t_scan[k].ea == ea_bin && t_scan[k].size == size_bin) {
                if (t_scan[k].off == 0xFFFFFFFFu) { start = stop = 0; }
                else { start = t_scan[k].off; stop = start + sizeof(spu_num_service); }
                goto scan_known;
            }
        {   uint32_t found = 0xFFFFFFFFu;
            for (uint32_t o = 0; o + sizeof(spu_num_service) <= size_bin; o += 4)
                if (!memcmp(ls + o, spu_num_service, sizeof(spu_num_service))) { found = o; break; }
            const unsigned k = t_scan_next++ & 7u;
            t_scan[k].ea = ea_bin; t_scan[k].size = size_bin; t_scan[k].off = found;
            if (found == 0xFFFFFFFFu) { start = stop = 0; }
            else { start = found; stop = found + sizeof(spu_num_service); } }
    scan_known:
        for (uint32_t o = start; o + sizeof(spu_num_service) <= stop; o += 4) {
            if (memcmp(ls + o, spu_num_service, sizeof(spu_num_service))) continue;
            uint32_t old = (uint32_t)ls[0x1C8]<<24 | (uint32_t)ls[0x1C9]<<16 |
                           (uint32_t)ls[0x1CA]<<8 | ls[0x1CB];
            ls32(ls, 0x1C8, g_spurs_job_spu_num);
            static int n;
            if (PS3_LOG_FIRST(n, 8))
                fprintf(stderr, "[spurs-job] image=%d current-SPU ABI helper=%05X LS1C8=%u -> %u\n",
                        image_id, o, old, g_spurs_job_spu_num);
            break;
        }
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static int _d = 0;
      if (diag->descdump_present && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (diag->descdump_level >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (diag->dump_present) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* ---- enter the job -------------------------------------------------- */
    sj_next(&stage_perf, SJ_CONTEXT);
    if (!direct_stage) {
        spu_context_init(&ctx, 0);
        memcpy(ctx.ls, ls, SPU_LS_SIZE);
    }
    ctx.image_id = image_id;
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctx.gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* The SPU ABI's r1 is more than its preferred address word: word 1
     * tracks remaining dynamic stack bytes. Compiler stack checks subtract
     * the frame size from both words, then xswd/heqi tests word 1's sign.
     * Leaving it zero makes every checked entry report overflow on its first
     * frame (Macross geom1 0x508 / geom3 0x420), before doing any job work.
     * Reserve the same 32-byte caller frame from the DESCRIPTOR'S stack
     * allowance; do not substitute the absolute SP or include scratch bytes,
     * which would suppress genuine overflows into the adjacent regions.
     * IBM SDK 3.0 Programmer's Guide, "SPE stack debugging / Overview":
     * https://arcm.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/accessibility/sdkpg/CBE_Programmers_Guide_v3.0.toc.html
     */
    ctx.gpr[1]._u32[1] = size_stk > 32u ? size_stk - 32u : 0u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctx.gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctx.gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx.gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx.gpr[3]._u32[0], ctx.gpr[4]._u32[0], ctx.gpr[1]._u32[0],
                  image_id); }
    /* The dispatcher recognizes the job's return-to-manager transfer. A
     * compiler HALT assert is an actual failure, not a completion signal. */
    /* Optional bounded offline probe: one matching job for the whole process,
     * fixed filenames in the run directory, never an unbounded per-job dump. */
    static volatile long probe_claimed;
    int probe = 0;
    if (diag->probe_image_present && diag->probe_image == image_id) {
#ifdef _WIN32
        probe = InterlockedCompareExchange(&probe_claimed, 1, 0) == 0;
#else
        probe = __sync_bool_compare_and_swap(&probe_claimed, 0, 1);
#endif
    }
    if (probe) {
        FILE* f = fopen("spurs-job-probe-before.ls", "wb");
        if (f) { fwrite(ls, 1, SPU_LS_SIZE, f); fclose(f); }
        f = fopen("spurs-job-probe-before.regs", "wb");
        if (f) { fwrite(ctx.gpr, 1, sizeof ctx.gpr, f); fclose(f); }
        f = fopen("spurs-job-probe.desc", "wb");
        if (f) { fwrite(vm_base + job_ea, 1, dsz, f); fclose(f); }
        f = fopen("spurs-job-probe-context.json", "wb");
        if (f) {
            fprintf(f, "{\"image_id\":%d,\"job_ea\":%u,\"binary_ea\":%u,\"binary_bytes\":%u,"
                "\"descriptor_bytes\":%u,\"context_ls\":%u,\"descriptor_ls\":%u,"
                "\"io_ls\":%u,\"io_bytes\":%u,\"io_staged_bytes\":%u,\"dma_entries\":%u,"
                "\"out_ls\":%u,\"out_bytes\":%u,\"scratch_ls\":%u,\"scratch_bytes\":%u,"
                "\"stack_top\":%u,\"stack_bytes\":%u,\"jm_iface\":%u,"
                "\"jobcrt13_marker\":%d,\"jobcrt14_marker\":%d,"
                "\"pc\":%u,\"status\":%u,\"r0\":%u,\"r1\":%u,\"r1_stack_remaining\":%u,"
                "\"r3\":%u,\"r4\":%u,\"guest_executed_at_capture\":false}\n",
                image_id, job_ea, ea_bin, size_bin, dsz, ctx_ls, desc_ls, io_ls, size_io,
                io_used, n_dma, out_ls, size_out, s_ls, size_scr, stack_top, size_stk, jm_iface,
                size_bin >= 0x3C && !memcmp(ls + 0x30, "JOBCRT Ver13", 12),
                size_bin >= 0x3C && !memcmp(ls + 0x30, "JOBCRT Ver14", 12),
                (uint32_t)ctx.pc, ctx.status, ctx.gpr[0]._u32[0], ctx.gpr[1]._u32[0],
                ctx.gpr[1]._u32[1], ctx.gpr[3]._u32[0], ctx.gpr[4]._u32[0]);
            fclose(f);
        }
        fprintf(stderr, "[spurs-job-probe] image=%d job=%08X desc=%u binary=%08X io=%05X out=%05X scratch=%05X stack=%05X\n",
                image_id, job_ea, dsz, ea_bin, io_ls, out_ls, s_ls, stack_top);
        /* An explicit staging-only probe must fail the chain honestly. It
         * neither executes the image nor publishes a synthetic completion. */
        if (diag->probe_only) {
            fprintf(stderr, "[spurs-job-probe] staging-only image=%d job=%08X; no guest execution or completion\n",
                    image_id, job_ea);
            g_spurs_job_mbox_valid = 0;
            spu_mfc_release(&ctx);
            spu_coh_unregister(&ctx);
            if (!direct_stage) free(ls);
            ejp_end(&edge_perf, 1); sj_end(&stage_perf, 1);
            return -1;
        }
    }
    sj_next(&stage_perf, SJ_BODY);
    t_job_ctx_depth++;   /* a job run from inside this one takes the next slot */
    const uint64_t outer_start = g_spurs_job_start_qpc; const uint32_t outer_ea = g_spurs_job_cur_ea;
    { struct timespec ts; timespec_get(&ts, TIME_UTC); /* microseconds, read by spu_drain.c */
      g_spurs_job_start_qpc = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u; g_spurs_job_cur_ea = job_ea; }
    t_cap_file = spurs_job_cap_open(image_id, job_ea,
        job_desc_size ? (job_desc_size > 0x400 ? 0x400 : job_desc_size) : 256u, n_dma, job_ea + JH_SIZE);
    if ((image_id == 10006 || image_id == 10002) && t_ea_cap && !t_cap_file) t_cap_file = t_ea_cap;   /* ANIMCAP1 header written */
    t_ea_cap = NULL;
    if (es_shadow) es_output_reset(t_es_output);
    t_es_output_on = es_shadow;
    g_job_cap_on = t_cap_file != NULL || es_shadow || ea_shadow || nn_shadow || rb_shadow;   /* *_shadow: record the SPU's effects */
    if (es_shadow && image_id == 10011 && t_es.pos_only && t_es.scheme == 0 &&
        t_es.fmt.f2 == 0x44 && t_es.fmt.f3 == 0x40)
        g6e_begin(&ctx, t_es.fmt.stride, t_es.n_vtx, t_es.n_idx, t_es.sig, t_es.hole, t_es_vtx, t_es_idx);
    ejp_next(&edge_perf, EJP_BODY);
    int halted = spu_run_with_halt(entry, &ctx);
    if (es_shadow && image_id == 10011) g6e_finish(&ctx, t_es_output);
    ejp_next(&edge_perf, EJP_POST);
    t_es_output_on = 0;
    g_job_cap_on = 0;
    if (t_cap_file) { if (image_id == 10006 || image_id == 10002) fputc('E', t_cap_file); fclose(t_cap_file); t_cap_file = NULL; }
    t_job_ctx_depth--;
    g_spurs_job_start_qpc = outer_start; g_spurs_job_cur_ea = outer_ea;
    sj_next(&stage_perf, SJ_SNAPSHOT);
    if (probe) {
        FILE* f = fopen("spurs-job-probe-after.ls", "wb");
        if (f) { fwrite(ctx.ls, 1, SPU_LS_SIZE, f); fclose(f); }
        f = fopen("spurs-job-probe-after.regs", "wb");
        if (f) { fwrite(ctx.gpr, 1, sizeof ctx.gpr, f); fclose(f); }
    }
    /* Manager returns are the only successful non-local exits. Watchdogs may
     * leave status unchanged, so also check the actual terminal PC. */
    uint32_t exit_pc = (uint32_t)ctx.pc & SPU_LS_MASK;
    int failed = (ctx.status & SPU_STATUS_STOPPED_BY_HALT) ||
        (halted && (ctx.stop_code || (exit_pc != 0x3FF00u && exit_pc != 0)));
    if (probe) {
        FILE* f = fopen("spurs-job-probe-exit.json", "wb");
        if (f) {
            fprintf(f, "{\"image_id\":%d,\"job_ea\":%u,\"pc\":%u,\"status\":%u,"
                "\"stop_code\":%u,\"halted\":%d,\"runner_failed\":%d,\"r0\":%u,\"r3\":%u}\n",
                image_id, job_ea, exit_pc, ctx.status, ctx.stop_code, halted, failed,
                ctx.gpr[0]._u32[0], ctx.gpr[3]._u32[0]);
            fclose(f);
        }
        fprintf(stderr, "[spurs-job-probe] exit image=%d job=%08X pc=%05X status=%X stop=%X halted=%d failed=%d\n",
            image_id, job_ea, exit_pc, ctx.status, ctx.stop_code, halted, failed);
    }
    /* An Edge geometry job (images 10003..10014) that hits its assert(0)
     * stop: on hardware this never happens; here entering the ship feeds
     * geom3 an out-of-range selector (r80, switch 26..40) and failing the
     * job failed its whole chain, so the completion never published and
     * the main thread waited on event flag 0x110BCB80 forever. Skip that
     * one mesh -- end the job as completed -- and log it.
     * no_edge_assert_skip file: keep the old fail-the-chain behaviour. */
    if (failed && ctx.stop_code == 0x3FFFu && image_id >= 10003 && image_id <= 10014 &&
        GetFileAttributesA("no_edge_assert_skip") == INVALID_FILE_ATTRIBUTES) {
        static volatile long s_skips;
        if (InterlockedIncrement(&s_skips) <= 32)
            fprintf(stderr, "[spurs-job] edge assert skipped job=0x%08X image=%d pc=0x%05X line=%u r80=%08X r4=%08X\n",
                    job_ea, image_id, exit_pc, ctx.gpr[6]._u32[0], ctx.gpr[80]._u32[0], ctx.gpr[4]._u32[0]);
        failed = 0;
    }
    if (failed) {
        fprintf(stderr, "[spurs-job] FAILED job=0x%08X image=%d status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, image_id, ctx.status, ctx.stop_code, exit_pc);
        fflush(stderr);
        if (image_id == 10003 && exit_pc == 0x8C0C) {
            FILE* fault_file = fopen("spurs-job-failure.ls", "wb");
            if (fault_file) { fwrite(ctx.ls, 1, SPU_LS_SIZE, fault_file); fclose(fault_file); }
            fault_file = fopen("spurs-job-failure.regs", "wb");
            if (fault_file) { fwrite(ctx.gpr, 1, sizeof(ctx.gpr), fault_file); fclose(fault_file); }
            for (unsigned r = 0; r < 128; ++r) {
                fprintf(stderr, "[spurs-ring-reg] r%u=%08X,%08X,%08X,%08X\n", r,
                        ctx.gpr[r]._u32[0], ctx.gpr[r]._u32[1],
                        ctx.gpr[r]._u32[2], ctx.gpr[r]._u32[3]);
            }
            fprintf(stderr, "[spurs-ring] lr=%05X ring=%05X consumerEA=%08X start=%08X end=%08X current=%08X candidate=%08X next=%08X cached=%08X\n",
                    ctx.gpr[0]._u32[0], ctx.gpr[13]._u32[0], ctx.gpr[22]._u32[0],
                    ctx.gpr[15]._u32[0], ctx.gpr[18]._u32[0], ctx.gpr[11]._u32[0],
                    ctx.gpr[19]._u32[0], ctx.gpr[10]._u32[0], ctx.gpr[57]._u32[0]);
            fprintf(stderr, "[spurs-ring] LS15F00:");
            for (unsigned o = 0x15F00; o < 0x15F40; o += 4) {
                const uint8_t* q = ctx.ls + o;
                fprintf(stderr, " %08X", (uint32_t)q[0]<<24 | (uint32_t)q[1]<<16 | (uint32_t)q[2]<<8 | q[3]);
            }
            fputc('\n', stderr);
#ifdef _WIN32
            uint32_t ea = ctx.gpr[22]._u32[0];
            MEMORY_BASIC_INFORMATION mbi;
            const uint8_t* pconsumer = vm_base + ea;
            if (ea <= 0xFFFFFFFCu && VirtualQuery(pconsumer, &mbi, sizeof(mbi)) &&
                mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                (uintptr_t)pconsumer + 4 <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
                fprintf(stderr, "[spurs-ring] consumer[%08X]=%08X committed\n", ea, g32(ea));
            else fprintf(stderr, "[spurs-ring] consumer[%08X] unreadable\n", ea);
#endif
            fflush(stderr);
        }
    }

    /* Notify job shadowed: compare the captured SPU effects with the native
     * ones, update that path's trust. */
    if (nn_shadow) macross_notify_job_after_spu(image_id, failed);

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    /* The only reader is the SPU printf service, which looks into the LS
     * after a job posts an outbound mailbox. Copying the full 256 KB under
     * one global lock after EVERY job serialized the geometry workers
     * (~2200 jobs a frame, ~13 us each). Keep it for jobs that posted a
     * mailbox; file job_ls_publish_all restores the copy after every job. */
    { static LONG s_pub_all = -1;
      if (s_pub_all < 0) s_pub_all = GetFileAttributesA("job_ls_publish_all") != INVALID_FILE_ATTRIBUTES;
      if (s_pub_all || spu_channel_has_data(&ctx.ch_out_mbox) || spu_channel_has_data(&ctx.ch_out_intr_mbox))
          (void)sjs_publish(snapshot_handle, ctx.ls); /* complete per-chain retained LS */ }
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static int img = -2; static uint32_t lsa; static int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctx.ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctx.ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }

    g_spurs_job_mbox      = spu_channel_has_data(&ctx.ch_out_mbox)
                          ? ctx.ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx.ch_out_intr_mbox)
                          ? ctx.ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = !failed && (g_spurs_job_mbox || g_spurs_job_mbox_intr);
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (diag->dump_present) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx.status, ctx.stop_code, ctx.pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    if (ej_hole_ea) ej_hole_note(ej_hole_ea, job_ea, image_id, failed ? 3 : 2);
    if (ev_ud && !failed && ev_hole && ev_len)
        ej_ud_report(image_id, ev_udmode, ev_learn_f23, ev_learn_io, ev_hole, ev_len, t_ej_cfg, t_ej_ndma, t_ej_dmasz);
    if (ev_learn && !failed && ev_hole && ev_len)
        ej_dyn_learn(image_id, ev_learn_f23, ev_learn_sig, ev_learn_io, ev_hole, ev_len, t_ej_cfg, t_ej_ndma, t_ej_dmasz);
    ejp_next(&edge_perf, EJP_VERIFY);
    /* Compare private PUT payloads; other jobs may already have reused the ring. */
    if (es_shadow && !failed) ej_skin_verify();
    if (ea_shadow) edge_anim_verify(failed);   /* images 10006/10002: native PUTs vs the SPU's */
    if (rb_shadow) ribbon_verify(failed);      /* image 10009: the same, and the ribbon_cap capture */
    t_es.active = 0;
    if (ev_buf && !failed) {
        const int r = ej_verify(image_id, ev_fmt, ev_hole, ev_len, ev_buf);
        if (ev_slot && r == 1 && InterlockedIncrement(&ev_slot->ok) == EJ_TRUST_AFTER)
            fprintf(stderr, "[edge-native] image=%d fmt=%02X/%02X%s sig=%08X checked against the SPU: native from now on\n",
                    image_id, (ev_fmt >> 8) & 255u, ev_fmt & 255u, (ev_fmt >> 16) ? " pos-only" : "", ev_sig);
        if (ev_slot && r == 0 && InterlockedIncrement(&ev_slot->bad) == 1)
            fprintf(stderr, "[edge-native] image=%d fmt=%02X/%02X%s sig=%08X does NOT match the SPU: kept on the SPU\n",
                    image_id, (ev_fmt >> 8) & 255u, ev_fmt & 255u, (ev_fmt >> 16) ? " pos-only" : "", ev_sig);
    }

    ejp_next(&edge_perf, EJP_CLEANUP);
    sj_next(&stage_perf, SJ_CLEANUP);
    spu_mfc_release(&ctx);
    spu_coh_unregister(&ctx);   /* stack local: out of the reserver set */
    { extern void spu_mfc_release(spu_context*); spu_mfc_release(&ctx); }
    if (!direct_stage) free(ls);
    ejp_end(&edge_perf, failed); sj_end(&stage_perf, failed);
    return failed ? -1 : 0;
#undef ctx
}
