/*
 * ps3recomp - Track B live NV4097 draw path (implementation)
 *
 * See rsx_live_draw.h. This is the validated capture-replay D3D12 engine
 * (libs/video/tests/replay_main.c) lifted into a runtime module and driven by
 * the live FIFO consumer instead of an .rxs file:
 *   - rsx_dispatch register-file model (shared, unchanged)
 *   - NV40 VP/FP -> HLSL decompilers (shared, unchanged)
 *   - B1 render/sampler state -> D3D12 PSO + dynamic samplers + mip chains
 *
 * Differences from the harness:
 *   - guest memory comes from an injected resolver (the runtime's vm_base map),
 *     not a private arena;
 *   - present goes to a swap chain bound to the runtime's window, not a PPM
 *     readback;
 *   - the whole engine is gated behind RSX_DRAW (default ON; "0" = off).
 *
 * Clean-room: NV40 ISA/register facts from envytools rnndb + Mesa nv30 +
 * psdevwiki; RPCS3 as a read-only fact oracle only.
 */

#include "rsx_live_draw.h"
#include "rsx_commands.h"
#include "rsx_texture_layout.h"
#include "ps3emu/yz_runtime_config.h"
#include "ps3emu/yz_frontier_trace.h"
#include "ps3emu/log_first.h"
#include "ps3emu/ps3_ftrace.h"

#if !defined(_WIN32)

/* Non-Windows: the whole path is a no-op (D3D12 is Windows-only). */
int  rsx_live_draw_enabled(void) { return 0; }
int  rsx_live_draw_init(void* hwnd, u32 w, u32 h, rsx_live_guest_ptr_fn f, void* u)
{ (void)hwnd; (void)w; (void)h; (void)f; (void)u; return 0; }
void rsx_live_draw_seed_registers(const u32* r, u32 n) { (void)r; (void)n; }
void rsx_live_draw_seed_transform_program(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_seed_transform_constants(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_set_display_buffer(
    u32 b, u32 l, u32 o, u32 p, u32 w, u32 h)
{ (void)b; (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
void rsx_live_draw_set_fifo_position(u32 g, u32 p) { (void)g; (void)p; }
void rsx_live_draw_note_inline_transfer(u32 d, u32 o, u32 v)
{ (void)d; (void)o; (void)v; }
void rsx_live_draw_note_guest_write(u32 l, u32 o, u32 n)
{ (void)l; (void)o; (void)n; }
void rsx_live_draw_audit_guest_put(u32 ea,u32 bytes){(void)ea;(void)bytes;}
int  rsx_live_draw_blit(u32 sl, u32 sa, u32 sp, u32 dl, u32 da, u32 dp, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)dl; (void)da; (void)dp; (void)w; (void)h; return 0; }
int  rsx_live_draw_resolve_blit(u32 sl, u32 sa, u32 sp, u32 ds, u32 dt, u32 dl, u32 da,
                                u32 dp, u32 x, u32 y, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)ds; (void)dt; (void)dl; (void)da; (void)dp;
  (void)x; (void)y; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 sl, u32 sa, u32 dl, u32 da)
{ (void)sl; (void)sa; (void)dl; (void)da; }
void rsx_live_draw_flush(void) {}
void rsx_live_draw_before_label(void) {}
void rsx_live_draw_user_command(u32 c) { (void)c; }
void rsx_live_draw_present(u32 b) { (void)b; }
void rsx_live_draw_set_movie_mode(int on) { (void)on; }
void rsx_live_draw_set_vsync(int on) { (void)on; }
void rsx_live_draw_present_rgba(const uint8_t* r, u32 w, u32 h) { (void)r; (void)w; (void)h; }
u32  rsx_live_draw_get_frames(void) { return 0; }
int rsx_live_draw_async_enabled(void){return 0;}
void* rsx_live_draw_gpu_completion_event(void){return NULL;}
void rsx_live_draw_set_present_request(u32 s){(void)s;}
int rsx_live_draw_take_gpu_completion(u32 *s){(void)s;return 0;}
void* rsx_live_draw_get_present_thread_handle(void) { return NULL; }
u32  rsx_live_draw_get_last_draws(void) { return 0; }
double rsx_live_draw_get_present_fps(void) { return 0.0; }
void rsx_live_draw_dump_present_samples(void) {}
void rsx_live_draw_a010_probe_begin(void) {}
int  rsx_live_draw_a010_probe_active(void) { return 0; }
int  rsx_live_draw_a010_world_ready(void) { return 1; }
int  rsx_live_draw_debug_dump_surface(u32 l, u32 o, const char* p)
{ (void)l; (void)o; (void)p; return -1; }
void rsx_live_draw_set_a010_camera_matrix(const float* m) { (void)m; }
void rsx_live_draw_shutdown(void) {}

#else /* _WIN32 */

#define _CRT_SECURE_NO_WARNINGS

#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
static unsigned long long g_ld_bind_white = 0, g_ld_bind_real = 0, g_ld_bind_surf = 0;
/* Set once PS1 VRAM actually holds a drawn frame (see the PS1_PC heartbeat).
 * Every surface dump before this is an early-boot snapshot of a framebuffer the
 * PS1 has not drawn into yet -- which is what made nonblack=0 look like a broken
 * pipeline when it was the correct reading of an empty source. */
static volatile long g_ld_ps1_vram_ready = 0;
#include "rsx_restart_cuts.h"
#include "rsx_vertex_compact.h"
#include "rsx_vertex_raw28.h"
#include "rsx_vertex_rawn.h"
#include "rsx_cpu_draw_phases.h"
#include "rsx_texture_hash.h"
#include "rsx_shader_metadata_cache.h"
#include "rsx_vp_decompiler.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(YZ_PERF_CLEAN)
#define LD_DIAG_ENABLED(name) 0
#else
/* Cached per call site: several of these sit on per-draw paths. */
#define LD_DIAG_ENABLED(name) ({ static int _ld_diag = -1; \
    if (_ld_diag < 0) _ld_diag = getenv(name) != NULL; _ld_diag; })
#endif
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"
#include "rsx_shader_prep_runtime.h"
static HRESULT ld_builtin_compile(LPCVOID source, SIZE_T bytes, LPCSTR name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors);
#include <sys/stat.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* Armed by the a010 AUTH repair only after its animation palette is resident. */
extern volatile LONG g_yz_a010_reference_camera_active;
/* Published when the Sunshine Orphanage AUTH root becomes live. */
extern volatile LONG g_yz_a010_root_active;
/* Opt-in headless gameplay-proof handshake, owned jointly with cellPad.c. */
extern volatile LONG g_yz_movement_proof_phase;
extern volatile unsigned long long g_yz_auto_start_tick;

/* ---------------------------------------------------------------------------
 * Engine state (module-static; single live RSX)
 * -----------------------------------------------------------------------*/

#define LD_SWAP_BUFFERS  2
/* Macross field passes exhaust 64 after boot/menu mip targets. Keep every
 * distinct typed target alive; never evict an in-flight resource to fit. */
#define MAX_SURFACES     256
#define MAX_SURFACE_DEMAND_KEYS 4096
#define MAX_TEXTURES     1024
#define MAX_OWNED_CUBES 64
#define MAX_RETIRED_TEXTURES 4096
#define MAX_VTEX         64
#define MAX_SAMPLERS     128
/*
 * A full live boot carries shader pairs from startup movies, menus, and AUTH
 * into the orphanage.  The old 256-entry cap was already exhausted there,
 * causing 155/328 sampled a010 groups to return NULL from get_pso().  The
 * one-frame reference alone uses 197 pairs.  A full visible boot through
 * orphanage and into Akiyama measured 2,296 distinct requested PSO keys before
 * the first presentation stall, so use the next power of two with headroom.
 */
#define MAX_PSOS         8192
#define FORMER_MAX_SHADER_BLOBS 2048
#define MAX_SHADER_BLOBS 8192
#define MAX_REJECTED_PSO_KEYS 8192
#define UPLOAD_SIZE      (64u * 1024 * 1024)
#define SHADER_DISK_CACHE_MAGIC   0x43535A59u /* "YZSC" */
/* v2: OP_KIL now emits discard (was a dropped TODO) — cached v1 pixel
 * shaders would silently keep the old no-discard codegen. */
#define SHADER_DISK_CACHE_VERSION 2u
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)

#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_ZDEPTH_BASE  (SRV_SURFACE_BASE + MAX_SURFACES)
#define SRV_TEXTURE_BASE (SRV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_VTEX_BASE    (SRV_TEXTURE_BASE + MAX_TEXTURES)
#define SRV_HEAP_SLOTS   (SRV_VTEX_BASE + MAX_VTEX)
#define SRV_DEPTH_SOURCE_BASE SRV_HEAP_SLOTS
#define UAV_ZDEPTH_BASE   (SRV_DEPTH_SOURCE_BASE + MAX_SURFACES)
/* Per-surface copy a draw samples when it reads its own render target. */
#define SRV_FEEDBACK_BASE (UAV_ZDEPTH_BASE + MAX_SURFACES)
/* Top-left windows of a surface for smaller textures that alias it (see surface_window). */
#define LD_SWIN_PER       2
#define SRV_SURF_WIN_BASE (SRV_FEEDBACK_BASE + MAX_SURFACES)
#define SRV_ZPACK_BASE (SRV_SURF_WIN_BASE + MAX_SURFACES * LD_SWIN_PER)
#define SRV_STENCIL_SOURCE_BASE (SRV_ZPACK_BASE + MAX_SURFACES)
#define UAV_ZPACK_BASE (SRV_STENCIL_SOURCE_BASE + MAX_SURFACES)
#define SRV_OWNED_CUBE_BASE (UAV_ZPACK_BASE + MAX_SURFACES)
#define SRV_CPU_HEAP_SLOTS (SRV_OWNED_CUBE_BASE + MAX_OWNED_CUBES)
#define SRV_TABLE_SIZE   16
#define SRV_RING_TABLES  4096

#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_SAMPLERS
#define SMP_TABLE_SIZE   16
/* A shader-visible SAMPLER heap is hard-capped at 2048 descriptors by D3D12
 * (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE). SMP_RING_TABLES*SMP_TABLE_SIZE
 * must stay <= 2048, else CreateDescriptorHeap fails. Identical sampler
 * tables share entries, so this is 128 unique tables between GPU flushes. */
#define SMP_RING_TABLES  128

#define CB_BLOCK_BYTES   ((512 + 2) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)
#define PS_CB_RING_BYTES CB_RING_BYTES
#define LD_VARIANT_SET_CAPACITY (MAX_SHADER_BLOBS * 2u)

#define VERT_STRIDE      (16 * 4 * 4)   /* 16 attrs * float4                  */
#define VERT_BUFFER_SIZE (256u * 1024u * 1024u)
#define INDEX_BUFFER_SIZE (64u * 1024u * 1024u)
#define LD_INVALID_SURFACE 0xFFFFFFFFu

/* gcm texture format bytes (mirror rsx_dispatch.h) */
#define TEX_FMT_B8         0x81
#define TEX_FMT_A1R5G5B5   0x82
#define TEX_FMT_A4R4G4B4   0x83
#define TEX_FMT_R5G6B5     0x84
#define TEX_FMT_A8R8G8B8   0x85
#define TEX_FMT_DXT1       0x86
#define TEX_FMT_DXT23      0x87
#define TEX_FMT_DXT45      0x88
#define TEX_FMT_G8B8       0x8B
#define TEX_FMT_DEPTH24_D8 0x90
#define TEX_FMT_X16        0x94 /* unsigned normalized 16-bit, not half float */
#define TEX_FMT_LINEAR     0x20
#define TEX_FMT_UNNORM     0x40
#define TEX_FMT_BASE_MASK  0x9F

typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    u32 w, h;
    DXGI_FORMAT fmt;
    ID3D12Resource* fb_tex;   /* feedback copy, see surface_feedback_srv */
    u32 pitch;                /* guest row pitch of colour target 0 (bytes), 0 = unknown */
    u32 raster_type;          /* current guest layout: 1 linear, 2 Morton/swizzled */
    /* Top-left windows of the surface for smaller aliasing textures (see surface_window). */
    struct { ID3D12Resource* tex; ID3D12Resource* src; u32 w, h, wgen, used; } swin[LD_SWIN_PER];
    u32 swin_wgen;            /* bumped by every write into the surface; a window older than it is recopied */
    /* Guest-write ordering (see ld_gw_surface_written): write-watch epoch after the guest pages
     * under the surface were drained at its last GPU write, and frame+1 of that drain (0 = never). */
    u32 gw_epoch, gw_frame;
    u32 gw_chk_frame, gw_chk_offset, gw_chk_span, gw_chk_newer, gw_chk_full;   /* per-frame bind check cache */
#if !defined(YZ_PERF_CLEAN)
    u32 resource_serial;
    u32 last_write_generation;
    u32 last_write_kind;
    u32 last_create_generation;
    u32 last_draw_generation;
    u32 last_clear_generation;
    u32 last_copy_generation;
    u32 last_blit_generation;
    u32 last_resolve_generation;
    u32 last_other_generation;
    u32 last_guest_blit_generation;
    u32 last_present_copy_generation;
#endif
    u32 ct_draw_gen;   /* generation of the last draw that really wrote colour here */
    /* Internal render scale (LD_RENDER_SCALE): w/h stay the GUEST size used
     * by every guest-facing comparison; hw/hh is the host resource size. */
    u32 hw, hh, scaled;
} surface_t;
/* LD_RENDER_SCALE=1.25|1.5|2: canvas-size colour targets in RSX local memory
 * (and the zeta bound with them) are rendered at that scale. Guest-visible
 * targets (main memory, write-backs, small LUT/luminance targets) keep the
 * guest size. 1 (default): every host size equals the guest size. */
static u32 g_rs_num = 1, g_rs_den = 1;
static void ld_rs_config(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char* e = getenv("LD_RENDER_SCALE");
    const double v = e ? atof(e) : 1.0;
    if (v > 1.24 && v < 1.26) { g_rs_num = 5; g_rs_den = 4; }
    else if (v > 1.49 && v < 1.51) { g_rs_num = 3; g_rs_den = 2; }
    else if (v > 1.99 && v < 2.01) { g_rs_num = 2; g_rs_den = 1; }
    else if (v > 2.49 && v < 2.51) { g_rs_num = 5; g_rs_den = 2; }
    else if (v > 2.99 && v < 3.01) { g_rs_num = 3; g_rs_den = 1; }
    if (g_rs_num != g_rs_den)
        fprintf(stderr, "[render-scale] %u/%u for canvas-size local colour targets\n", g_rs_num, g_rs_den);
}
static inline u32 ld_rs_up(u32 v) { return (u32)(((u64)v * g_rs_num) / g_rs_den); }
static inline u32 ld_sx(const surface_t* s, u32 v) { return s->scaled ? ld_rs_up(v) : v; }
static inline float ld_sxf(const surface_t* s) { return s->scaled ? (float)g_rs_num / (float)g_rs_den : 1.0f; }
/* Fractional scale rounds equal guest spans differently at different origins.
 * CopyTextureRegion cannot resample that phase; use the existing draw path. */
static int ld_copy_span_matches(const surface_t* src, const surface_t* dst,
                                u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h)
{
    return ld_sx(src, sx + w) - ld_sx(src, sx) == ld_sx(dst, dx + w) - ld_sx(dst, dx) &&
           ld_sx(src, sy + h) - ld_sx(src, sy) == ld_sx(dst, dy + h) - ld_sx(dst, dy);
}
static inline u32 ld_host_w(const surface_t* s) { return s->hw ? s->hw : s->w; }   /* host resource size */
static inline u32 ld_host_h(const surface_t* s) { return s->hh ? s->hh : s->h; }
typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
    int resolve_src;   /* surface slot + 1 a scaled blit resolves into this buffer; 0 = none */
} display_buffer_t;
typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    ID3D12Resource* snapshot;
    ID3D12Resource* packed_snapshot;
    /* Three inactive extents plus the active snapshot: bounded GPU reuse. */
    ID3D12Resource* snapshot_spare[3];
    D3D12_RESOURCE_STATES snapshot_spare_state[3];
    u32 snapshot_spare_w[3], snapshot_spare_h[3], snapshot_spare_next;
    D3D12_RESOURCE_STATES packed_state;
    u32 packed_w, packed_h, depth_format;
    u64 snapshot_generation, packed_generation;
    u32 w, h;
    u32 snapshot_w, snapshot_h;
    D3D12_RESOURCE_STATES snapshot_state;
    int cleared;
    float clear_depth;   /* depth of the latest guest clear */
    int had_write;
    int snapshot_valid;
    u64 draws;
    u64 depth_test_draws;
    u64 depth_write_draws;
    u64 depth_both_draws;
    int reject_logged;
    u32 scaled;           /* render-scaled resource (zdepth_get's zscaled): w/h/snapshot_* are host sizes */
    u32 ds_write_gen;     /* surface generation at its last depth/stencil-writing draw */
    u32 color_import_gen; /* aliased colour generation last imported (ld_zeta_import_color) */
} zdepth_t;
typedef struct {
    u32 location, offset, format, width, height, pitch, remap;
    u32 cubemap, mipmaps, dimension;
    ID3D12Resource* tex;
    u64 content_hash;
    int content_readable;
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 ww_epoch;         /* write-watch epoch at its last hash */
    u8 ww_ok, ww_bad;     /* write-watch "unchanged" verdicts confirmed / ever wrong */
} texcache_t;
typedef struct {
    u32 location, offset, w, h, format, remap;
    u32 face_slot[6], face_generation[6];
    u64 last_use;
    ID3D12Resource* tex;
} owned_cube_t;
typedef struct {
    u32 location, offset, format, width, height, pitch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} vtexcache_t;
typedef struct { u64 key; ID3D12PipelineState* pso; char* capture_ps_hlsl; } psocache_t;
typedef struct {
    u64 hash;
    u32 source_length;
    char* source;
    ID3DBlob* blob;
} shader_blob_t;
typedef struct {
    shader_blob_t entries[MAX_SHADER_BLOBS];
    u32 count;
    u64 retained_source_bytes;
    u64 retained_blob_bytes;
} shader_blob_cache_t;
typedef struct {
    u32 magic;
    u32 version;
    u32 stage;
    u32 source_length;
    u32 blob_length;
    u32 reserved;
    u64 source_hash;
} shader_disk_cache_header;

typedef struct {
    int              enabled;    /* RSX_DRAW resolved                     */
    int              ready;      /* device + resources up                    */

    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;

    IDXGISwapChain3*           swap;
    ID3D12Resource*            backbuf[LD_SWAP_BUFFERS];
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;

    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;
    owned_cube_t               owned_cubes[MAX_OWNED_CUBES];
    u64                        owned_cube_use;
    display_buffer_t           display_buffers[8];

    ID3D12DescriptorHeap*      dsv_heap;
    u32                        dsv_step;
    ID3D12Resource*            depth;
    int                        depth_cleared;
    zdepth_t                   zdepths[MAX_SURFACES];
    u32                        n_zdepths;

    ID3D12DescriptorHeap*      srv_cpu_heap;
    ID3D12DescriptorHeap*      srv_heap;
    u32                        srv_step, srv_ring_used;
    ID3D12Resource*            white_tex;
    texcache_t                 textures[MAX_TEXTURES];
    u32                        n_textures;
    vtexcache_t                vtex[MAX_VTEX];
    u32                        n_vtex;
    ID3D12Resource*            retired_textures[MAX_RETIRED_TEXTURES];
    u32                        n_retired_textures;
    ID3D12Resource*            upload;
    u8*                        upload_mapped;
    u32                        upload_used;

    /* Optional host-movie UI compositor. The guest keeps rendering captions
     * into its ordinary offscreen surface. Each guest flip reads back the
     * latest sparse overlay for blending over each 30 Hz host movie frame. */
    ID3D12Resource*            movie_upload;
    u8*                        movie_upload_mapped;
    ID3D12Resource*            movie_overlay_readback;
    u8*                        movie_overlay_rgba;
    u8*                        movie_overlay_mask;
    u32                        movie_overlay_pitch;
    int                        movie_overlay_valid;
    u64                        movie_overlay_frames;

    ID3D12DescriptorHeap*      smp_cpu_heap;
    ID3D12DescriptorHeap*      smp_heap;
    u32                        smp_step, smp_ring_used;
    u32                        smp_ring_slots[SMP_RING_TABLES][SMP_TABLE_SIZE];
    u64                        smp_keys[SMP_CACHE_SLOTS];
    u32                        n_samplers;

    ID3D12RootSignature*       rootsig_x;
    ID3D12RootSignature*       depth_snapshot_rootsig;
    ID3D12PipelineState*       depth_snapshot_pso;
    ID3D12RootSignature*       depth_pack_rootsig;
    ID3D12PipelineState*       depth_pack_pso;
    ID3D12RootSignature*       downsample_rootsig;
    ID3D12PipelineState*       downsample_pso[4];
    DXGI_FORMAT                downsample_fmt[4];
    psocache_t                 psos[MAX_PSOS];
    u32                        n_psos;
    shader_blob_cache_t        vs_blobs;
    shader_blob_cache_t        ps_blobs;

    ID3D12Resource*            cb;
    u8*                        cb_mapped;
    u32                        cb_used;

    /* Separate transient b1 arena for buffered fragment constants. It uses
     * the existing constant-buffer ring budget rather than changing any
     * texture or vertex upload-ring capacity. */
    ID3D12Resource*            ps_cb;
    u8*                        ps_cb_mapped;
    u32                        ps_cb_used;
    rsx_fp_constant_block      fp_constants;
    float                      fp_alpha_ref;
    char                       fp_constant_mode;

    ID3D12Resource*            vb;
    u8*                        vb_mapped;
    u32                        vb_used;
    ID3D12Resource*            ib;
    u32*                       ib_mapped;
    u32                        ib_used;

    u32                        width, height;
    rsx_live_guest_ptr_fn      guest_ptr;
    void*                      guest_user;

    rsx_dispatch               rsx;
    /*
     * Startup-selected Stage-1 isolation mode.  The hot draw path only reads
     * these bits; environment parsing is completed once in init.
     *
     * L = legacy
     * H = hoisted descriptor/base fetch
     * M = H + masked shader signature/input layout
     * C = M + compact payload/stride
     */
    u32                        vertex_features;
    char                       vertex_mode;
    char                       vertex_diag_dir[MAX_PATH];
} ld_state;

static ld_state g;
/* owned_cube_srv: the surface slots sitting at each face address of a cube
 * (none for a plain guest texture). That set changes only when a slot takes a
 * new (location, offset); then entries covering the new address or listing the
 * slot are dropped. Which candidate is current (written, newest) is still
 * decided per draw. */
enum { LD_CUBE_NEG_SLOTS = 256, LD_CUBE_CANDS = 4 };
static struct { u32 location, offset, stride, valid; u8 n[6]; u16 cand[6][LD_CUBE_CANDS]; } g_ld_cube_neg[LD_CUBE_NEG_SLOTS];
static u64 g_ld_cube_neg_hits, g_ld_cube_scans, g_ld_cube_owned;
static void ld_cube_neg_forget(u32 location, u32 offset, u32 slot)
{
    for (u32 i = 0; i < LD_CUBE_NEG_SLOTS; i++) {
        if (!g_ld_cube_neg[i].valid) continue;
        const u32 o = g_ld_cube_neg[i].offset, st = g_ld_cube_neg[i].stride;
        int drop = g_ld_cube_neg[i].location == location && offset >= o &&
            (st ? (offset - o) % st == 0 && (offset - o) / st < 6u : offset == o);
        for (u32 f = 0; f < 6 && !drop; f++)
            for (u32 k = 0; k < g_ld_cube_neg[i].n[f]; k++)
                if (g_ld_cube_neg[i].cand[f][k] == slot) drop = 1;
        if (drop) g_ld_cube_neg[i].valid = 0;
    }
}
/* A diagnostic toggle file in the working directory, checked at most once a
 * second per call site (a GetFileAttributes per draw-path call is a syscall). */
#define LD_FILE_FLAG(name) ({ static int _v; static ULONGLONG _t; const ULONGLONG _n = GetTickCount64();     if (_n - _t > 1000) { _t = _n; _v = GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES; } _v; })
/* Which colour targets are scaled: canvas-size ones (the scene, its main-
 * memory copy for the post-process, the display buffers). Their guest-
 * visible write-backs are downscaled (ld_rs_downscale). */
static inline int ld_rs_wants(u32 location, u32 w, u32 h)
{
    return g_rs_num != g_rs_den && location <= 1u && w == g.width && h == g.height;
}
/* Is the bound colour target (from raw surface clip values) scaled? Same
 * answer as g.surfaces[current_surface()].scaled: surface_get turns an unset
 * clip (0) into the canvas size before ld_rs_wants, so the raw clip must be
 * defaulted the same way (else a 0 clip read as unscaled while its surface
 * was scaled: zeta and WPOS at the wrong scale). */
static inline int ld_rs_wants_clip(u32 location, u32 clip_w, u32 clip_h)
{
    return ld_rs_wants(location, clip_w ? clip_w : g.width, clip_h ? clip_h : g.height);
}

#define LD_VERTEX_HOIST_FETCH       (1u << 0)
#define LD_VERTEX_MASK_SIGNATURE    (1u << 1)
#define LD_VERTEX_COMPACT_PAYLOAD   (1u << 2)

static int ld_vertex_hoist_fetch(void)
{
    return (g.vertex_features & LD_VERTEX_HOIST_FETCH) != 0;
}

static int ld_vertex_mask_signature(void)
{
    return (g.vertex_features & LD_VERTEX_MASK_SIGNATURE) != 0;
}

static int ld_vertex_compact_payload(void)
{
    return (g.vertex_features & LD_VERTEX_COMPACT_PAYLOAD) != 0;
}

/* Exact opt-in raw28 ABI. Existing capture files describe float4 IA payloads,
 * so those diagnostics retain their original CPU fetch path. */
static int g_ld_raw28_shader_failed;
/* General raw ABI (rsx_vertex_rawn.h), opt-in LD_VERTEX_RAWN=1. raw == 2 in
 * the PSO path means "rawn" and reads the draw's layout from here. */
static int g_ld_rawn_failed;
static const rsx_rawn_layout* g_ld_rawn_cur;
/* Runtime A/B for the vertex/index fast paths: a file of that name in the
 * working directory turns the path off while it exists (checked once a
 * second per flag). For hunting geometry corruption without a restart. */
static int ld_off_file(const char* name, volatile ULONGLONG* checked, volatile LONG* state)
{
    const ULONGLONG now = GetTickCount64();
    if (now - *checked > 1000) {
        *checked = now;
        const LONG v = GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
        if (v != *state) fprintf(stderr, "[ld-toggle] %s %s\n", name, v ? "present: path OFF" : "removed: path ON");
        *state = v;
    }
    return *state != 0;
}
static volatile ULONGLONG s_off_rawn_t, s_off_raw28_t, s_off_icache_t, s_off_vcache_t;
static volatile LONG s_off_rawn, s_off_raw28, s_off_icache, s_off_vcache;

static int ld_vertex_rawn_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("LD_VERTEX_RAWN");
        enabled = e && !strcmp(e, "1") && !getenv("LD_CANONICAL_CAPTURE_DIR") &&
                  !getenv("LD_DRAW_PAIR_DIR") && !getenv("RSX_VERTEX_DIAG_DIR");
        if (enabled) fprintf(stderr, "[vertex-rawn] enabled: raw guest vertex bytes decoded in the vertex shader\n");
    }
    if (enabled && ld_off_file("no_rawn", &s_off_rawn_t, &s_off_rawn)) return 0;
    return enabled && !g_ld_rawn_failed;
}
static int ld_vertex_raw28_enabled(void)
{
    static int enabled=-1;
    if(enabled<0){
        const char* e=getenv("LD_VERTEX_RAW28");
        enabled=e&&!strcmp(e,"1")&&!getenv("LD_CANONICAL_CAPTURE_DIR")&&
            !getenv("LD_DRAW_PAIR_DIR")&&!getenv("RSX_VERTEX_DIAG_DIR");
        if(enabled)fprintf(stderr,"[vertex-raw28] enabled guarded_mask=C10D upload_stride=28 original_stride=96\n");
    }
    if (enabled && ld_off_file("no_raw28", &s_off_raw28_t, &s_off_raw28)) return 0;
    return enabled&&!g_ld_raw28_shader_failed;
}

static const char* ld_vertex_mode_name(void)
{
    switch (g.vertex_mode) {
    case 'H': return "H-hoist";
    case 'M': return "M-mask";
    case 'C': return "C-compact";
    default:  return "L-legacy";
    }
}

static int ld_fp_constants_buffered(void)
{
    return g.fp_constant_mode == 'B';
}

static const char* ld_fp_constant_mode_name(void)
{
    return ld_fp_constants_buffered() ? "buffered" : "literal";
}

static int g_ld_dred_dumped = 0;
static u32 g_ld_frames = 0;
/* Bounded read-only provenance for one MAIN-memory producer region. Matching
 * intervals are half-open and 64-bit; GPU aliases are named by resolved slot,
 * never inferred solely from the guest texture address. Renderer-thread only. */
static int s_consumer_enabled=-1,s_consumer_armed;
static u32 s_consumer_base,s_consumer_bytes,s_consumer_rows;
static const char* s_consumer_gate;
static ULONGLONG s_consumer_check;
static int ld_consumer_trace_enabled(void) {
 if(s_consumer_enabled<0){const char*e=getenv("LD_RESOURCE_CONSUMER_TRACE");s_consumer_enabled=e&&e[0]=='1';
  if(s_consumer_enabled){e=getenv("LD_RESOURCE_CONSUMER_OFFSET");s_consumer_base=e?(u32)strtoul(e,0,16):0x01400000;
   e=getenv("LD_RESOURCE_CONSUMER_BYTES");s_consumer_bytes=e?(u32)strtoul(e,0,0):0x384000;
   s_consumer_gate=getenv("LD_RESOURCE_CONSUMER_TRACE_GATE");s_consumer_armed=!s_consumer_gate||!s_consumer_gate[0];}}
 return s_consumer_enabled&&s_consumer_rows<64;
}
static int ld_consumer_trace_match(u32 location,u32 offset,u64 bytes) {
 return location==1&&bytes&&(u64)offset<(u64)s_consumer_base+s_consumer_bytes&&(u64)s_consumer_base<(u64)offset+bytes;
}
static int ld_consumer_trace_take(void) {
 if(!ld_consumer_trace_enabled())return 0;
 if(!s_consumer_armed){ULONGLONG now=GetTickCount64();if(now>=s_consumer_check){s_consumer_check=now+250;
  FILE*f=fopen(s_consumer_gate,"rb");if(f){fclose(f);s_consumer_armed=1;}}}
 if(!s_consumer_armed)return 0;s_consumer_rows++;return 1;
}
static void ld_consumer_surface_event(const char*event,u32 slot,u32 peer) {
 if(!ld_consumer_trace_enabled()||slot>=g.n_surfaces)return;
 const surface_t*t=&g.surfaces[slot];
 if(!ld_consumer_trace_match(t->location,t->offset,t->pitch?(u64)t->pitch*t->h:1)&&
    !(peer<g.n_surfaces&&ld_consumer_trace_match(g.surfaces[peer].location,g.surfaces[peer].offset,g.surfaces[peer].pitch?(u64)g.surfaces[peer].pitch*g.surfaces[peer].h:1)))return;
 if(!ld_consumer_trace_take())return;
 u32 generation=0,serial=0;
#if !defined(YZ_PERF_CLEAN)
 generation=t->last_write_generation;serial=t->resource_serial;
#endif
 fprintf(stderr,"[resource-consumer] n=%u frame=%u event=%s slot=%u location=%u offset=%08X width=%u height=%u pitch=%u host_fmt=%u serial=%u write_generation=%u peer_slot=%u",s_consumer_rows,g_ld_frames,event,slot,t->location,t->offset,t->w,t->h,t->pitch,(u32)t->fmt,serial,generation,peer);
 if(peer<g.n_surfaces)fprintf(stderr," peer_location=%u peer_offset=%08X",g.surfaces[peer].location,g.surfaces[peer].offset);
 fprintf(stderr,"\n");
}

static ID3D12InfoQueue* g_ld_info_queue = NULL;
static int g_ld_debug_layer_enabled = 0;

typedef struct {
    int valid;
    u64 key;
    u64 vp_hash;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask;
    u32 input_mask, input_stride;
    u32 packed_offsets;
} ld_pso_metadata;

static ld_pso_metadata g_ld_current_pso;
/* Only populated when a canonical FP snapshot was explicitly requested. */
static u32 g_ld_capture_shadow_mask, g_ld_capture_row1d_mask, g_ld_capture_unnorm_mask;
static u32 g_ld_capture_unnorm_dim[16][2], g_ld_capture_row1d_width[16];
static rsx_fp_shadow_desc g_ld_capture_shadow[16];
static size_t g_ld_capture_ps_source_bytes;
static int ld_canonical_fp_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("LD_CANONICAL_CAPTURE_FP");
        enabled = e && atoi(e) != 0;
    }
    return enabled;
}
static u64 g_ld_flip_requested = 0;
static u64 g_ld_flip_consumed = 0;
static u32 g_ld_last_requested_buffer = UINT32_MAX;
static u32 g_ld_last_consumed_buffer = UINT32_MAX;
static u32 g_ld_last_present_target = UINT32_MAX;
#include "rsx_main_alias_audit.h"
static u64 g_ld_last_dump_fingerprint = 0;
/* Colour target of the latest draw that wrote colour (slot + its address, so
 * a recycled slot is not mistaken for it). */
static struct { u32 slot, location, offset; } g_ld_last_ct = { LD_INVALID_SURFACE, 0, 0 };
#if !defined(YZ_PERF_CLEAN)
static u32 g_ld_surface_generation = 0;
static u32 g_ld_surface_resource_serial = 0;
static u32 g_ld_guest_blit_generation = 0;
static u32 g_ld_present_copy_generation = 0;
static u64 g_ld_vertex_constant_ring_recycles = 0;
static u32 g_ld_frame_vertex_constant_ring_recycles = 0;
static u32 g_ld_fifo_get = 0;
static u32 g_ld_fifo_put = 0;
static volatile LONG g_ld_diag_post_movie_pending = 0;
static volatile LONG g_ld_diag_post_movie_presents = 0;

enum {
    LD_SURFACE_WRITE_NONE = 0,
    LD_SURFACE_WRITE_CREATE = 1,
    LD_SURFACE_WRITE_DRAW = 2,
    LD_SURFACE_WRITE_CLEAR = 3,
    LD_SURFACE_WRITE_COPY = 4,
    LD_SURFACE_WRITE_BLIT = 5,
    LD_SURFACE_WRITE_RESOLVE = 6,
    LD_SURFACE_WRITE_OTHER = 7
};

static void ld_gw_surface_written(u32 target);
static void ld_surface_note_write(u32 target, u32 kind)
{
    if (target >= MAX_SURFACES)
        return;
    surface_t* surface = &g.surfaces[target];
    const u32 generation = ++g_ld_surface_generation;
    surface->swin_wgen++;
    ld_gw_surface_written(target);
    surface->last_write_generation = generation;
    surface->last_write_kind = kind;
    switch (kind) {
    case LD_SURFACE_WRITE_CREATE:
        surface->last_create_generation = generation;
        break;
    case LD_SURFACE_WRITE_DRAW:
        surface->last_draw_generation = generation;
        break;
    case LD_SURFACE_WRITE_CLEAR:
        surface->last_clear_generation = generation;
        break;
    case LD_SURFACE_WRITE_COPY:
        surface->last_copy_generation = generation;
        break;
    case LD_SURFACE_WRITE_BLIT:
        surface->last_blit_generation = generation;
        break;
    case LD_SURFACE_WRITE_RESOLVE:
        surface->last_resolve_generation = generation;
        break;
    default:
        surface->last_other_generation = generation;
        break;
    }
    ld_maa_surface(target,"gpu_write",kind,0,0);
}
#else
static void ld_gw_surface_written(u32 target);
#define ld_surface_note_write(target, kind) \
    do { if ((target) < MAX_SURFACES) { g.surfaces[(target)].swin_wgen++; ld_gw_surface_written(target); \
         ld_maa_surface((target),#kind,0,0,0); } } while (0)
#endif

#define LD_LAYOUT_CACHE_CAP 256u
typedef struct {
    int valid;
    rsx_vertex_layout_plan plan;
} ld_layout_cache_entry;

static ld_layout_cache_entry g_ld_layout_cache[LD_LAYOUT_CACHE_CAP];
static u32 g_ld_layout_cache_count = 0;

static void ld_layout_plan_get(u32 mask, rsx_vertex_layout_plan* out)
{
    mask &= 0xFFFFu;
    for (u32 i = 0; i < g_ld_layout_cache_count; i++) {
        if (g_ld_layout_cache[i].valid &&
            g_ld_layout_cache[i].plan.mask == mask) {
            *out = g_ld_layout_cache[i].plan;
            return;
        }
    }
    rsx_vertex_layout_plan_init(out, mask);
    if (g_ld_layout_cache_count < LD_LAYOUT_CACHE_CAP) {
        ld_layout_cache_entry* entry =
            &g_ld_layout_cache[g_ld_layout_cache_count++];
        entry->valid = 1;
        entry->plan = *out;
    }
}

#define LD_RECENT_DRAW_CAP 128u
typedef struct {
    u64 serial;
    u64 pso_key;
    u64 descriptor_signature;
    u32 frame;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask, texture_mask;
    u32 primitive, vertices, target, zslot;
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 srv_ring_used, sampler_ring_used, cb_used, vb_used;
} ld_recent_draw;

static ld_recent_draw g_ld_recent_draws[LD_RECENT_DRAW_CAP];
static u64 g_ld_recent_draw_total = 0;

/*
 * Isolated pre-cleanup benchmark measurement. Record only successful
 * swap-chain presents: one QPC call and fixed-ring stores, with no allocation,
 * configuration lookup, sorting, or logging in the presentation path.
 */
#define LD_PRESENT_RING_CAP 16384u
typedef struct {
    u64 present_id;
    u32 guest_frame;
    LONGLONG qpc;
    u64 process_kernel_100ns;
    u64 process_user_100ns;
    u64 present_thread_kernel_100ns;
    u64 present_thread_user_100ns;
    u32 present_thread_id;
} ld_present_sample;
static ld_present_sample g_ld_present_ring[LD_PRESENT_RING_CAP];
static u64 g_ld_present_total = 0;
static LONGLONG g_ld_qpc_frequency = 0;
static int g_ld_present_dumped = 0;
static int g_ld_schedule_diag = 0;
#if defined(YZ_PPU_SAMPLE)
static HANDLE g_ld_present_thread_handle = NULL;
#endif

static void ld_present_measure_dump(void);
#if defined(YZ_PERF_PROFILE) && !defined(YZ_RSX_PROFILE_ONLY)
extern void spu_perf_window_begin(u32 guest_frame);
extern void spu_perf_window_dump(u32 guest_frame);
extern void spu_perf_frame_sample_record(u32 guest_frame);
extern void spu_perf_frame_sample_dump(u32 guest_start, u32 guest_end);
#endif
#if defined(YZ_PPU_SAMPLE)
extern void yz_ppu_perf_window_begin(u32 guest_frame);
extern void yz_ppu_perf_window_dump(u32 guest_frame);
#endif

static void ld_present_measure_init(void)
{
    LARGE_INTEGER frequency;
    memset(g_ld_present_ring, 0, sizeof(g_ld_present_ring));
    g_ld_present_total = 0;
    g_ld_present_dumped = 0;
    g_ld_schedule_diag = getenv("RSX_SCHEDULE_DIAG") != NULL;
    if (QueryPerformanceFrequency(&frequency))
        g_ld_qpc_frequency = frequency.QuadPart;
    else
        g_ld_qpc_frequency = 0;
}

static void ld_present_measure_record(u32 guest_frame)
{
#if defined(YZ_PPU_SAMPLE)
    if (!g_ld_present_thread_handle) {
        HANDLE duplicate = NULL;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            HANDLE previous = (HANDLE)InterlockedCompareExchangePointer(
                (PVOID volatile*)&g_ld_present_thread_handle, duplicate, NULL);
            if (previous)
                CloseHandle(duplicate);
        }
    }
#endif
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now))
        return;
    const u64 present_id = g_ld_present_total + 1u;
    ld_present_sample* sample =
        &g_ld_present_ring[(present_id - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    sample->present_id = present_id;
    sample->guest_frame = guest_frame;
    sample->qpc = now.QuadPart;
    if (g_ld_schedule_diag) {
        FILETIME creation = {0}, exit = {0}, kernel = {0}, user = {0};
        ULARGE_INTEGER value;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit,
                            &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->process_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->process_user_100ns = value.QuadPart;
        }
        if (GetThreadTimes(GetCurrentThread(), &creation, &exit,
                           &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->present_thread_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->present_thread_user_100ns = value.QuadPart;
        }
        sample->present_thread_id = GetCurrentThreadId();
    }
    g_ld_present_total = present_id;
#if defined(YZ_PPU_SAMPLE)
    {
        static int sample_present = -1;
        static u32 sample_start = 1578u;
        static u32 sample_end = 2178u;
        if (sample_present < 0) {
            sample_present =
                getenv("PPU_SAMPLE_RUN") != NULL &&
                getenv("PPU_SAMPLE_PRESENT") != NULL;
            const char* start_text = getenv("PPU_SAMPLE_START");
            const char* frames_text = getenv("PPU_SAMPLE_FRAMES");
            if (start_text) {
                const unsigned long parsed = strtoul(start_text, NULL, 0);
                if (parsed <= UINT32_MAX)
                    sample_start = (u32)parsed;
            }
            if (frames_text) {
                unsigned long parsed = strtoul(frames_text, NULL, 0);
                if (parsed < 4u) parsed = 4u;
                if (parsed > 600u) parsed = 600u;
                sample_end = sample_start + (u32)parsed;
            }
        }
        const int game_present = guest_frame == g_ld_frames + 1u;
#if defined(YZ_PERF_PROFILE) && !defined(YZ_RSX_PROFILE_ONLY)
        if (sample_present && game_present &&
            guest_frame >= sample_start && guest_frame <= sample_end)
            spu_perf_frame_sample_record(guest_frame);
#endif
        if (sample_present && game_present && guest_frame == sample_start)
            yz_ppu_perf_window_begin(guest_frame);
        else if (sample_present && game_present && guest_frame == sample_end) {
            yz_ppu_perf_window_dump(guest_frame);
#if defined(YZ_PERF_PROFILE) && !defined(YZ_RSX_PROFILE_ONLY)
            spu_perf_frame_sample_dump(sample_start, sample_end);
#endif
        }
    }
#endif
#if defined(YZ_PERF_PROFILE) && !defined(YZ_RSX_PROFILE_ONLY)
    if (guest_frame == 1578u)
        spu_perf_window_begin(guest_frame);
    else if (guest_frame == 1582u)
        spu_perf_window_dump(guest_frame);
#endif
    /*
     * The controlled A010 benchmark ends at guest frame 2293.  Flush the
     * already-recorded fixed ring exactly at that endpoint so an unrelated
     * host-window teardown cannot truncate the authoritative raw samples.
     * This is outside the measured timestamp operation and runs once.
     */
    if (guest_frame == 2293u)
        ld_present_measure_dump();
}

static void ld_present_measure_dump(void)
{
    if (g_ld_present_dumped || g_ld_present_total == 0 ||
        g_ld_qpc_frequency <= 0)
        return;
    g_ld_present_dumped = 1;

    char path[256];
    snprintf(path, sizeof(path), "scratch\\present_qpc_%lu.csv",
             (unsigned long)GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    if (!f)
        return;

    fprintf(f, "# qpc_frequency=%lld\n", g_ld_qpc_frequency);
    if (g_ld_schedule_diag) {
        fprintf(f, "present_id,guest_frame,qpc,process_kernel_100ns,"
                   "process_user_100ns,present_thread_kernel_100ns,"
                   "present_thread_user_100ns,present_thread_id\n");
    } else {
        fprintf(f, "present_id,guest_frame,qpc\n");
    }
    const u64 first =
        g_ld_present_total > LD_PRESENT_RING_CAP
            ? g_ld_present_total - LD_PRESENT_RING_CAP + 1u : 1u;
    for (u64 present_id = first; present_id <= g_ld_present_total;
         ++present_id) {
        const ld_present_sample* sample =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (sample->present_id == present_id) {
            if (g_ld_schedule_diag) {
                fprintf(f, "%llu,%u,%lld,%llu,%llu,%llu,%llu,%u\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc,
                        (unsigned long long)sample->process_kernel_100ns,
                        (unsigned long long)sample->process_user_100ns,
                        (unsigned long long)sample->present_thread_kernel_100ns,
                        (unsigned long long)sample->present_thread_user_100ns,
                        sample->present_thread_id);
            } else {
                fprintf(f, "%llu,%u,%lld\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc);
            }
        }
    }
    fclose(f);
    fprintf(stderr,
            "[present-qpc] preserved %llu successful presents at %lld Hz in %s\n",
            (unsigned long long)(g_ld_present_total - first + 1u),
            g_ld_qpc_frequency, path);
}

static u64 g_ld_texture_cache_full = 0;
static u64 g_ld_texture_use_serial = 0;

static const char* ld_d3d_severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
    }
}

static void ld_drain_info_queue(const char* where)
{
    if (!g_ld_info_queue) return;
    const u64 count =
        g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue);
    for (u64 i = 0; i < count; i++) {
        SIZE_T bytes = 0;
        if (FAILED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, NULL, &bytes)) ||
            !bytes || bytes > 1024u * 1024u)
            continue;
        D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(bytes);
        if (!message) break;
        if (SUCCEEDED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, message, &bytes))) {
            fprintf(stderr,
                    "[d3d-debug] where=%s frame=%u severity=%s id=%u %s\n",
                    where, g_ld_frames,
                    ld_d3d_severity_name(message->Severity),
                    (unsigned)message->ID,
                    message->pDescription
                        ? message->pDescription : "<no description>");
        }
        free(message);
    }
    if (count)
        g_ld_info_queue->lpVtbl->ClearStoredMessages(g_ld_info_queue);
}

static void ld_dump_recent_draws(void)
{
    const u64 first =
        g_ld_recent_draw_total > LD_RECENT_DRAW_CAP
            ? g_ld_recent_draw_total - LD_RECENT_DRAW_CAP + 1u : 1u;
    fprintf(stderr,
            "[draw-history] total=%llu retained=%llu..%llu\n",
            (unsigned long long)g_ld_recent_draw_total,
            (unsigned long long)first,
            (unsigned long long)g_ld_recent_draw_total);
    for (u64 serial = first; serial <= g_ld_recent_draw_total; serial++) {
        const ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        if (draw->serial != serial) continue;
        fprintf(stderr,
                "[draw-history] serial=%llu frame=%u key=%016llX "
                "vp=%u+%u fp=%u:0x%X+%u ctrl=0x%X "
                "mask{tex=%04X cube=%04X vtex=%04X txl=%04X} "
                "prim=%u verts=%u target=%u z=%u "
                "clip=%u,%u %ux%u desc=%016llX "
                "ring{srv=%u smp=%u cb=%u vb=%u}\n",
                (unsigned long long)draw->serial, draw->frame,
                (unsigned long long)draw->pso_key,
                draw->vp_start, draw->vp_instrs,
                draw->fp_location, draw->fp_offset, draw->fp_size,
                draw->fp_control, draw->texture_mask, draw->cube_mask,
                draw->vtex_mask, draw->txl_mask, draw->primitive,
                draw->vertices, draw->target, draw->zslot,
                draw->clip_x, draw->clip_y, draw->clip_w, draw->clip_h,
                (unsigned long long)draw->descriptor_signature,
                draw->srv_ring_used, draw->sampler_ring_used,
                draw->cb_used, draw->vb_used);
    }
}

static const char* ld_dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        default: return "other";
    }
}

static void ld_enable_dred(void)
{
    ID3D12DeviceRemovedExtendedDataSettings* settings = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&settings);
    if (FAILED(hr) || !settings) {
        fprintf(stderr, "[dred] settings unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    settings->lpVtbl->SetAutoBreadcrumbsEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->SetPageFaultEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->Release(settings);
    fprintf(stderr, "[dred] auto-breadcrumbs and page-fault tracking enabled\n");
}

static void ld_enable_debug_layer(void)
{
    if (!getenv("RSX_D3D_DEBUG")) return;
    ID3D12Debug* debug = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12Debug, (void**)&debug);
    if (FAILED(hr) || !debug) {
        fprintf(stderr,
                "[d3d-debug] validation layer unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    debug->lpVtbl->EnableDebugLayer(debug);
    debug->lpVtbl->Release(debug);
    g_ld_debug_layer_enabled = 1;
    fprintf(stderr, "[d3d-debug] validation layer enabled\n");
}

static void ld_open_info_queue(void)
{
    if (!g_ld_debug_layer_enabled || !g.dev) return;
    const HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12InfoQueue, (void**)&g_ld_info_queue);
    if (FAILED(hr) || !g_ld_info_queue) {
        fprintf(stderr,
                "[d3d-debug] info queue unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    D3D12_MESSAGE_SEVERITY severities[] = {
        D3D12_MESSAGE_SEVERITY_CORRUPTION,
        D3D12_MESSAGE_SEVERITY_ERROR,
        D3D12_MESSAGE_SEVERITY_WARNING
    };
    D3D12_INFO_QUEUE_FILTER filter = {0};
    filter.AllowList.NumSeverities =
        (UINT)(sizeof(severities) / sizeof(severities[0]));
    filter.AllowList.pSeverityList = severities;
    g_ld_info_queue->lpVtbl->AddStorageFilterEntries(
        g_ld_info_queue, &filter);
    g_ld_info_queue->lpVtbl->SetMessageCountLimit(
        g_ld_info_queue, 65536u);
    fprintf(stderr,
            "[d3d-debug] info queue attached (warning/error/corruption)\n");
}

static void ld_dump_dred(const char* where, HRESULT trigger_hr)
{
    if (g_ld_dred_dumped++ || !g.dev) return;
    ld_drain_info_queue(where);
    ld_dump_recent_draws();
    const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
    fprintf(stderr,
            "[dred] trigger=%s hr=0x%08lX removed=0x%08lX frame=%u "
            "surfaces=%u zetas=%u textures=%u psos=%u\n",
            where, (unsigned long)trigger_hr, (unsigned long)removed,
            g_ld_frames, g.n_surfaces, g.n_zdepths, g.n_textures, g.n_psos);

    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "[dred] query failed hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {0};
    hr = dred->lpVtbl->GetAutoBreadcrumbsOutput1(dred, &breadcrumbs);
    fprintf(stderr, "[dred] breadcrumbs hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node =
            breadcrumbs.pHeadAutoBreadcrumbNode;
        for (u32 node_index = 0; node && node_index < 16;
             node = node->pNext, node_index++) {
            const u32 last = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0;
            fprintf(stderr,
                    "[dred] node=%u list=%s queue=%s count=%u last=%u\n",
                    node_index,
                    node->pCommandListDebugNameA
                        ? node->pCommandListDebugNameA : "<unnamed>",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA : "<unnamed>",
                    node->BreadcrumbCount, last);
            if (!node->pCommandHistory || !node->BreadcrumbCount) continue;
            const u32 first = last > 8 ? last - 8 : 0;
            u32 end = last + 8;
            if (end >= node->BreadcrumbCount)
                end = node->BreadcrumbCount - 1;
            for (u32 i = first; i <= end; i++) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                fprintf(stderr,
                        "[dred]   op[%u]%s=%u(%s)\n",
                        i, i == last ? "*" : "",
                        (unsigned)op, ld_dred_op_name(op));
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {0};
    hr = dred->lpVtbl->GetPageFaultAllocationOutput1(dred, &fault);
    fprintf(stderr,
            "[dred] page-fault hr=0x%08lX va=0x%016llX\n",
            (unsigned long)hr,
            (unsigned long long)fault.PageFaultVA);
    if (SUCCEEDED(hr)) {
        const D3D12_DRED_ALLOCATION_NODE1* lists[2] = {
            fault.pHeadExistingAllocationNode,
            fault.pHeadRecentFreedAllocationNode
        };
        const char* labels[2] = {"existing", "recent-freed"};
        for (u32 list = 0; list < 2; list++) {
            const D3D12_DRED_ALLOCATION_NODE1* allocation = lists[list];
            for (u32 i = 0; allocation && i < 16;
                 allocation = allocation->pNext, i++) {
                fprintf(stderr,
                        "[dred] %s[%u] type=%u name=%s object=%p\n",
                        labels[list], i, (unsigned)allocation->AllocationType,
                        allocation->ObjectNameA
                            ? allocation->ObjectNameA : "<unnamed>",
                        (const void*)allocation->pObject);
            }
        }
    }
    dred->lpVtbl->Release(dred);
    fflush(stderr);
}
static u64 g_ld_texture_cache_evictions = 0;
static u64 g_ld_texture_decode_fail = 0;
static u64 g_ld_zdepth_srv_binds = 0;
static u64 g_ld_zdepth_srv_reject_no_write = 0;
static u64 g_ld_vtex_binds = 0;
static u64 g_ld_vtex_uploads = 0;
static u64 g_ld_vtex_refreshes = 0;
static u64 g_ld_vtex_unsupported = 0;
static u64 g_ld_vtex_enabled = 0;
static u64 g_ld_vtex_missing_for_txl = 0;
static u64 g_ld_divider_fetches = 0;
static char g_ld_shader_disk_dir[MAX_PATH];
static int g_ld_shader_disk_ready = -1;
static u64 g_ld_shader_disk_hits[2] = {0, 0};
static u64 g_ld_shader_disk_misses[2] = {0, 0};
static u64 g_ld_shader_disk_writes[2] = {0, 0};
static u64 g_ld_shader_disk_rejects = 0;

/*
 * Profile-lane-only renderer accounting.  These counters deliberately live
 * outside ld_state so the clean benchmark lane keeps the exact same state
 * layout and hot path.  Emit only slow/interesting frames plus a sparse
 * heartbeat; the orphanage window is therefore visible without turning the
 * log itself into a benchmark cost.
 */
typedef enum {
    LD_FLUSH_PRESENT = 0,
    LD_FLUSH_GUEST_REFERENCE,
    LD_FLUSH_VERTEX_RING,
    LD_FLUSH_VERTEX_CONSTANT_RING,
    LD_FLUSH_RETIRE_QUEUE,
    LD_FLUSH_MOVIE,
    LD_FLUSH_MOVIE_PRESENT,
    LD_FLUSH_READBACK,
    LD_FLUSH_PIXEL_CONSTANT_RING,
    LD_FLUSH_DESCRIPTOR_RING,
    LD_FLUSH_SHUTDOWN,
    LD_FLUSH_REASON_COUNT
} ld_flush_reason;

#if defined(YZ_PERF_PROFILE)
typedef enum {
    LD_REJECT_WORLD = 0,
    LD_REJECT_CHARACTER,
    LD_REJECT_UI,
    LD_REJECT_OTHER,
    LD_REJECT_CLASS_COUNT
} ld_reject_class;

typedef struct {
    u64 pso_lookups;
    u64 pso_hits;
    u64 pso_misses;
    u64 pso_probes;
    u64 pso_full;
    u64 vs_blob_lookups;
    u64 vs_blob_hits;
    u64 vs_blob_misses;
    u64 vs_blob_inserts;
    u64 vs_blob_full_rejects;
    u64 ps_blob_lookups;
    u64 ps_blob_hits;
    u64 ps_blob_misses;
    u64 ps_blob_inserts;
    u64 ps_blob_full_rejects;
    u64 vs_compile_calls;
    u64 ps_compile_calls;
    u64 vs_unique;
    u64 ps_unique;
    u64 vs_post_boundary_distinct;
    u64 vs_post_boundary_repeats;
    u64 ps_post_boundary_distinct;
    u64 ps_post_boundary_repeats;
    u64 create_pso_calls;
    u64 decompile_qpc;
    u64 vs_blob_lookup_qpc;
    u64 ps_blob_lookup_qpc;
    u64 vs_compile_qpc;
    u64 ps_compile_qpc;
    u64 create_pso_qpc;
    u64 flush_qpc;
    u64 fence_wait_qpc;
    u64 flush_reason[LD_FLUSH_REASON_COUNT];
    u64 flush_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 fence_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 input_vertices;
    u64 expanded_vertices;
    u64 used_attribute_draws;
    u64 used_attribute_sum;
    u64 legacy_vertex_upload_bytes;
    u64 vertex_upload_bytes;
    u64 vertex_fetch_pack_qpc;
    u64 sink_end_qpc;
    u64 draw_phase_qpc[LD_CPU_PHASE_COUNT];
    u64 draw_phase_samples[LD_CPU_PHASE_COUNT];
    u64 texture_upload_bytes;
    u64 texture_decode_calls;
    u64 texture_decode_qpc;
    u64 texture_hash_calls,texture_hash_span_bytes,texture_hash_read_fail,texture_hash_qpc;
    u64 texture_cache_lookups,texture_cache_probes,sampler_lookups,sampler_probes;
    u64 ps_constant_allocations;
    u64 ps_constant_upload_bytes;
    u64 ps_constant_upload_qpc;
    u64 ps_constant_capacity_failures;
} ld_profile_counts;

typedef struct {
    ld_profile_counts total;
    ld_profile_counts previous;
    u64 vs_hashes[MAX_SHADER_BLOBS];
    u64 ps_hashes[MAX_SHADER_BLOBS];
    u64 ps_raw_exact_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_raw_constant_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_exact_source_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_constant_specialized_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_canonical_hashes[LD_VARIANT_SET_CAPACITY];
    u32 n_ps_raw_exact_hashes;
    u32 n_ps_raw_constant_hashes;
    u32 n_vs_hashes;
    u32 n_ps_hashes;
    u32 n_ps_exact_source_hashes;
    u32 n_ps_constant_specialized_hashes;
    u32 n_ps_canonical_hashes;
    u64 ps_variant_tracking_overflow;
    u32 frame_upload_high;
    u32 frame_vb_high;
    u32 frame_retired_high;
    u32 total_upload_high;
    u32 total_vb_high;
    u32 total_retired_high;
    u32 frame_used_attribute_max;
    u32 total_used_attribute_max;
    u64 measured_frames;
    u64 compile_free_frames;
    double total_frame_ms;
    double compile_free_frame_ms;
    u64 previous_packets;
    u64 previous_groups;
    u64 previous_executed;
    u64 previous_evictions;
    u64 previous_refreshes;
    LONGLONG previous_present_qpc;
    LONGLONG qpc_frequency;
    IDXGIAdapter3* adapter;
    u64 rejected_pso_keys[MAX_REJECTED_PSO_KEYS];
    u8 rejected_pso_class_masks[MAX_REJECTED_PSO_KEYS];
    u32 n_rejected_pso_keys;
    u64 rejected_pso_requests[LD_REJECT_CLASS_COUNT];
    u32 rejected_pso_unique[LD_REJECT_CLASS_COUNT];
    u32 first_pso_full_frame;
} ld_profile_state;

static ld_profile_state g_ld_profile;

static LONGLONG ld_profile_qpc(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static INIT_ONCE g_ld_cpu_phases_once=INIT_ONCE_STATIC_INIT;
static int g_ld_cpu_phases_enabled;
static _Thread_local rsx_cpu_draw_phase_scope g_ld_cpu_phase_scope;
static BOOL CALLBACK ld_cpu_phases_init(PINIT_ONCE once,PVOID arg,PVOID* context)
{
    (void)once;(void)arg;(void)context;
    const char* value=getenv("LD_CPU_DRAW_PHASES");
    g_ld_cpu_phases_enabled=value&&!strcmp(value,"1");
    if(g_ld_cpu_phases_enabled)
        fprintf(stderr,"[draw-phase-config] enabled=1 disjoint_wall=1 exclusive_thread_cpu=0 buckets=pso,texture,descriptors,constants,state,draw\n");
    return TRUE;
}
/* Hash/probe diagnostics belong to the existing texture phase only. No
 * clocks outside profile+LD_CPU_DRAW_PHASES=1 or per candidate lookup. */
static int ld_texture_profile_active(void){
    return g_ld_cpu_phases_enabled&&g_ld_cpu_phase_scope.depth==1u&&g_ld_cpu_phase_scope.active==LD_CPU_PHASE_TEXTURE;
}
static void ld_texture_hash_profile_end(LONGLONG begin,u32 span,int readable){
    if(!begin)return;
    g_ld_profile.total.texture_hash_calls++;
    if(readable)g_ld_profile.total.texture_hash_span_bytes+=span;
    else g_ld_profile.total.texture_hash_read_fail++;
    g_ld_profile.total.texture_hash_qpc+=(u64)(ld_profile_qpc()-begin);
}
static u64 ld_cpu_phase_clock(void){return(u64)ld_profile_qpc();}
static void ld_cpu_draw_phase_begin(void)
{
    InitOnceExecuteOnce(&g_ld_cpu_phases_once,ld_cpu_phases_init,NULL,NULL);
    rsx_cpu_draw_phase_begin(&g_ld_cpu_phase_scope,g_ld_cpu_phases_enabled);
}
static int ld_cpu_draw_phase_switch(int phase)
{
    return rsx_cpu_draw_phase_switch(&g_ld_cpu_phase_scope,phase,ld_cpu_phase_clock,
        g_ld_profile.total.draw_phase_qpc,g_ld_profile.total.draw_phase_samples);
}
static void ld_cpu_draw_phase_end(void)
{
    rsx_cpu_draw_phase_end(&g_ld_cpu_phase_scope,ld_cpu_phase_clock,
        g_ld_profile.total.draw_phase_qpc,g_ld_profile.total.draw_phase_samples);
}

static void ld_profile_note_ring_highwater(void)
{
    if (g.upload_used > g_ld_profile.frame_upload_high)
        g_ld_profile.frame_upload_high = g.upload_used;
    if (g.upload_used > g_ld_profile.total_upload_high)
        g_ld_profile.total_upload_high = g.upload_used;
    if (g.vb_used > g_ld_profile.frame_vb_high)
        g_ld_profile.frame_vb_high = g.vb_used;
    if (g.vb_used > g_ld_profile.total_vb_high)
        g_ld_profile.total_vb_high = g.vb_used;
    if (g.n_retired_textures > g_ld_profile.frame_retired_high)
        g_ld_profile.frame_retired_high = g.n_retired_textures;
    if (g.n_retired_textures > g_ld_profile.total_retired_high)
        g_ld_profile.total_retired_high = g.n_retired_textures;
}
#else
static void ld_profile_note_ring_highwater(void) {}
static void ld_cpu_draw_phase_begin(void) {}
static int ld_cpu_draw_phase_switch(int phase){(void)phase;return LD_CPU_PHASE_NONE;}
static void ld_cpu_draw_phase_end(void) {}
#endif

static volatile LONG g_ld_a010_probe_active = 0;
static u32 g_ld_a010_probe_start_frame = 0;
static u32 g_ld_a010_probe_sample = 0;
static u64 g_ld_a010_probe_touched = 0;
static volatile LONG g_ld_a010_camera_ready = 0;
static volatile LONG g_ld_a010_world_ready = 0;
static u32 g_ld_a010_camera_bits[16];

static void ld_movement_camera_snapshot(LONG phase)
{
    fprintf(stderr,
            "[movement-frontier-rsx] phase=%ld frame=%u root=%ld "
            "reference_active=%ld camera_ready=%ld\n",
            phase, g_ld_frames,
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0),
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0),
            InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0));
    for (u32 row = 0; row < 4u; ++row) {
        fprintf(stderr,
                "[movement-frontier-rsx-camera] phase=%ld row=%u "
                "constant=%08X/%08X/%08X/%08X "
                "fallback=%08X/%08X/%08X/%08X\n",
                phase, row,
                g.rsx.constants[108u + row][0],
                g.rsx.constants[108u + row][1],
                g.rsx.constants[108u + row][2],
                g.rsx.constants[108u + row][3],
                g_ld_a010_camera_bits[row * 4u + 0u],
                g_ld_a010_camera_bits[row * 4u + 1u],
                g_ld_a010_camera_bits[row * 4u + 2u],
                g_ld_a010_camera_bits[row * 4u + 3u]);
    }
    fflush(stderr);
}
/* Host movie presentation and the FIFO consumer live on different threads but
 * share one D3D12 command list. Normal gameplay remains single-producer and
 * bypasses this lock; the active-reader handshake closes the movie-mode
 * transition race without putting every ordinary RSX method behind an SRW
 * lock. Host-frame priority prevents a flood of guest caption commands from
 * starving Present when the window is backgrounded. */
static SRWLOCK g_ld_access_lock = SRWLOCK_INIT;
static volatile LONG g_ld_guest_active = 0;
static volatile LONG g_ld_host_waiting = 0;

#define LD_PSO_INDEX 16384u   /* power of two, 2x MAX_PSOS */
static u32 g_ld_pso_index[LD_PSO_INDEX];   /* key -> g.psos index + 1 */
static u32 g_ld_force_cull_pending, g_ld_force_cull_active;   /* native Edge culling */
static const u8* guest_ptr(u32 location, u32 offset, u32 min_bytes)
{
    if (!g.guest_ptr) return NULL;
    return g.guest_ptr(g.guest_user, location, offset, min_bytes);
}
static u64 fnv1a(const void* data, u32 n, u64 h);
static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface);
static u64 ld_dump_resource_ppm(const char* path, const surface_t* surface,
                                D3D12_RESOURCE_STATES input_state);
static int g_ld_canonical_surface_rebind;
static u64 live_legacy_pso_key(void);
static void ld_vertex_diag_emit(const char* reason, int dump_surface);

/* ---------------------------------------------------------------------------
 * enable gate
 * -----------------------------------------------------------------------*/
int rsx_live_draw_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        /* Opt-in in this tree. Upstream (caner's Yakuza fork) defaults this ON
         * because that runner has no other renderer; here rsx_d3d12_backend.c
         * is still the default for every other title, so the live engine has
         * to be asked for. (The YZ_RSX_DRAW alias his scripts used went with
         * the title-hack drain; RSX_LIVE_DRAW is the one spelling.) */
        const char* e = getenv("RSX_LIVE_DRAW");
        cached = (e && e[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* ---------------------------------------------------------------------------
 * B1 render/sampler state decode (identical facts to replay_main.c)
 * -----------------------------------------------------------------------*/
#define M_BLEND_ENABLE       0x0310
#define M_ALPHA_TEST_ENABLE  0x0304
#define M_ALPHA_FUNC         0x0308
#define M_ALPHA_REF          0x030C
#define M_BLEND_SFACTOR      0x0314
#define M_BLEND_DFACTOR      0x0318
#define M_BLEND_EQUATION     0x0320
#define M_DEPTH_FUNC         0x0A6C
#define M_DEPTH_WRITE        0x0A70
#define M_DEPTH_TEST_ENABLE  0x0A74
#define M_POLY_OFFSET_FILL   0x0A68
#define M_POLY_OFFSET_SCALE  0x0A78
#define M_POLY_OFFSET_BIAS   0x0A7C
#define M_CULL_FACE          0x1830
#define M_FRONT_FACE         0x1834
#define M_CULL_FACE_ENABLE   0x183C
#define M_COLOR_MASK         0x0324
/* Stencil + scissor (raw NV4097 offsets, same convention as above; RPCS3
 * gcm_enums.h consulted as a read-only numbering oracle). */
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_ZSTENCIL_CLEAR      0x1D8C

static D3D12_COMPARISON_FUNC gcm_cmp(u32 f)
{
    switch (f) {
    case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default:     return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}
static D3D12_BLEND gcm_blend_factor(u32 f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static D3D12_BLEND_OP gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

/* SURFACE_COLOR_TARGET selector -> number of colour targets written, A first.
 * 0x02 (B alone) and anything unknown draw to A only, as before. */
static u32 ld_rt_count(u32 sel)
{
    switch (sel) {
    case 0x13: return 2;
    case 0x17: return 3;
    case 0x1F: return 4;
    default:   return 1;
    }
}

static DXGI_FORMAT ld_surface_format(u32 raw)
{
    switch (raw & 31u) {
    case 9: return DXGI_FORMAT_R8_UNORM;
    case 11: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 12: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 13: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
static u32 ld_surface_texel_bytes(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}
static u32 ld_surface_write_mask(u32 raw, u32 mask)
{
    if ((raw & 31u) == 9u) return (mask & 0xFFu) ? 1u : 0u;
    if ((raw & 31u) == 13u) return (mask & 0x00FF0000u) ? 1u : 0u;
    return ((mask & 0x00FF0000u) ? 1u : 0u) |
           ((mask & 0x0000FF00u) ? 2u : 0u) |
           ((mask & 0x000000FFu) ? 4u : 0u) |
           ((mask & 0xFF000000u) ? 8u : 0u);
}

typedef struct {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    /* TM: this title renders HDR into FP16 (SURFACE_FORMAT 0xB) targets;
     * the RTV format is PSO state, so it belongs in the key. */
    u32 rt_color_format;
    /* Colour targets the draw writes (SURFACE_COLOR_TARGET: A, AB, ABC,
     * ABCD). GH3's venue pass writes A and B; the post pass then samples B.
     * With only A bound, B stayed empty and that pass painted the scene
     * black. NumRenderTargets is PSO state, so it is keyed too. */
    u32 rt_count;
    /* Stencil is PSO state in D3D12, so it lives here and feeds the PSO key
     * like every other field. The reference value is dynamic
     * (OMSetStencilRef) and deliberately NOT part of this struct. */
    u32 stencil_enable, stencil_two_sided;
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
    /* glPolygonOffset: float bits of factor/units, zero when disabled.
     * Rasterizer state is PSO state. */
    u32 poly_offset_scale, poly_offset_bias;
} render_state_t;

static void decode_render_state(render_state_t* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(&g.rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func = rsx_dsp_reg(&g.rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(&g.rsx, M_ALPHA_REF);
    rsx_dsp_surface alpha_surface;
    rsx_dsp_get_surface(&g.rsx, &alpha_surface);
    rs->alpha_ref_format = alpha_surface.color_format;
    rs->rt_color_format = alpha_surface.color_format;
    rs->rt_count = ld_rt_count(alpha_surface.color_target);
    rs->blend_enable = rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1;
    const u32 sf = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
    const u32 df = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
    const u32 eq = rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sf & 0xFFFF; rs->sf_a = sf >> 16;
    rs->df_rgb = df & 0xFFFF; rs->df_a = df >> 16;
    rs->eq_rgb = eq & 0xFFFF; rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1;
    /* LD_NO_DEPTH=1: disable the depth test on every draw. A probe for draws
     * that vanish -- Tornado Outbreak's UI text fails its depth test here (not
     * yet understood), and this is how its menus are readable meanwhile. */
    { static int nd = -1; if (nd < 0) nd = getenv("LD_NO_DEPTH") ? 1 : 0; if (nd) rs->depth_test = 0; }
    rs->depth_write = rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(&g.rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(&g.rsx, M_CULL_FACE_ENABLE) & 1;
    rs->cull_face   = rsx_dsp_reg(&g.rsx, M_CULL_FACE);
    rs->front_face  = rsx_dsp_reg(&g.rsx, M_FRONT_FACE);
    /* Native Edge culling (spurs_job.c edge_geom1_native): the job's draw
     * relies on the SPU having culled back faces; the GPU does it instead,
     * for this draw group only (no guest-visible state changes). */
    int edge_none = 0;
    if (g_ld_force_cull_active == 2 || g_ld_force_cull_active == 3) {
        /* Edge mode 2 keeps the faces the RSX calls front with front=CW (F),
         * mode 3 the others (B). The guest's own RSX culling still applies
         * to Edge's survivors, so drop what it removes too; when that is
         * everything Edge kept, the draw leaves nothing. */
        const int keep_f = g_ld_force_cull_active == 2;
        int game_f = 0, game_b = 0;   /* faces the guest state removes, in front=CW terms */
        if (rs->cull_enable) {
            const int ccw = rs->front_face == 0x901;
            if (rs->cull_face == 0x404 || rs->cull_face == 0x408) { if (ccw) game_b = 1; else game_f = 1; }
            if (rs->cull_face == 0x405 || rs->cull_face == 0x408) { if (ccw) game_f = 1; else game_b = 1; }
        }
        static int logged;
        if ((keep_f ? game_f : game_b) && logged++ < 4)
            fprintf(stderr, "[edge-cull] mode=%u guest culls the kept faces (face=%03X front=%03X): draw writes nothing\n",
                    g_ld_force_cull_active, rs->cull_face, rs->front_face);
        rs->cull_enable = 1; rs->cull_face = keep_f ? 0x405 : 0x404; rs->front_face = 0x900;
        edge_none = keep_f ? game_f : game_b;
    }
    { static int nc = -1; if (nc < 0) nc = getenv("LD_NO_CULL") ? 1 : 0; if (nc) rs->cull_enable = 0; }
    /* s31 (scratch/s31_blue_emitter.md): honor the RAW register — 0 is a
     * legitimate game-written "write no color channels" (the character
     * shadow-mask depth-prime pass). rsx_dispatch_init seeds the nv40
     * reset default (0x01010101), so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
    rs->stencil_enable    = rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(&g.rsx, M_TWO_SIDED_STENCIL) & 1;
    { static int ns = -1; if (ns < 0) ns = getenv("LD_NO_STENCIL") ? 1 : 0; if (ns) rs->stencil_enable = rs->stencil_two_sided = 0; }
    rs->s_func       = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZPASS);
    /* LD_NO_POLY_OFFSET=1: ignore the game's depth bias (A/B comparison). */
    /* Runtime A/B: a file named no_poly_offset in the working directory
     * disables it too, checked about once a second. */
    { static int np = -1; static int np_file = 0; static ULONGLONG np_next = 0;
      if (np < 0) np = getenv("LD_NO_POLY_OFFSET") ? 1 : 0;
      const ULONGLONG np_now = GetTickCount64();
      if (np_now >= np_next) {
          np_next = np_now + 1000;
          const int was = np_file;
          np_file = GetFileAttributesA("no_poly_offset") != INVALID_FILE_ATTRIBUTES;
          if (was != np_file) fprintf(stderr, "[live-draw] polygon offset %s (no_poly_offset file)\n", np_file ? "OFF" : "ON");
      }
      if (!np && !np_file && (rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_FILL) & 1)) {
          rs->poly_offset_scale = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_SCALE);
          rs->poly_offset_bias  = rsx_dsp_reg(&g.rsx, M_POLY_OFFSET_BIAS);
      } }
    if (edge_none) { rs->color_mask = 0; rs->depth_write = 0; rs->stencil_enable = 0; rs->stencil_two_sided = 0; }
}

/* GL stencil-op enums (the NV4097 payload) -> D3D12. Zero (never seeded by
 * the game before first use) and GL_KEEP both map to KEEP. */
static D3D12_STENCIL_OP gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;      /* GL_ZERO */
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;  /* GL_INCR (clamped) */
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;  /* GL_DECR (clamped) */
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;      /* GL_INCR_WRAP */
    case 0x8508: return D3D12_STENCIL_OP_DECR;      /* GL_DECR_WRAP */
    case 0x1E00: default: return D3D12_STENCIL_OP_KEEP;
    }
}

static void apply_render_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                               const render_state_t* rs)
{
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    /* nv40_3d COLOR_MASK byte layout (Mesa/nouveau nv40_3d.xml.h, MIT/X11):
     * B=[0:7] G=[8:15] R=[16:23] A=[24:31]; any nonzero byte = channel on.
     * (s31: was hardcoded ENABLE_ALL — the blue-character class' live twin.) */
    b->RenderTargetWriteMask = (UINT8)ld_surface_write_mask(
        rs->rt_color_format, rs->color_mask);
    /* B8's memory write mask is the guest BLUE byte, although its fragment
     * export is the first scalar and the host attachment is R8. */
    if (rs->blend_enable) {
        b->BlendEnable   = TRUE;
        b->SrcBlend      = gcm_blend_factor(rs->sf_rgb, 0);
        b->DestBlend     = gcm_blend_factor(rs->df_rgb, 0);
        b->BlendOp       = gcm_blend_op(rs->eq_rgb);
        b->SrcBlendAlpha = gcm_blend_factor(rs->sf_a, 1);
        b->DestBlendAlpha= gcm_blend_factor(rs->df_a, 1);
        b->BlendOpAlpha  = gcm_blend_op(rs->eq_a);
    }
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (rs->cull_enable && rs->cull_face) {
        const u32 f = rs->cull_face;
        pd->RasterizerState.CullMode = (f == 0x0404) ? D3D12_CULL_MODE_FRONT
                                     : (f == 0x0405) ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE;
    } else {
        pd->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    /* Y-negating viewport epilogue mirrors winding; take the front sense
     * straight from the guest value (validated in B1). */
    pd->RasterizerState.FrontCounterClockwise = (rs->front_face == 0x0901);
    if (rs->poly_offset_scale || rs->poly_offset_bias) {
        /* glPolygonOffset(factor, units): factor is the slope scale; units
         * count minimum resolvable steps of the guest's 24-bit depth. */
        float factor, units; memcpy(&factor, &rs->poly_offset_scale, 4); memcpy(&units, &rs->poly_offset_bias, 4);
        pd->RasterizerState.SlopeScaledDepthBias = factor;
        pd->RasterizerState.DepthBias = (INT)units;
        static int logged;
        if (PS3_LOG_FIRST(logged, 8))
            fprintf(stderr, "[live-draw] polygon offset factor=%g units=%g\n", factor, units);
    }
    if (g.depth) {
        pd->DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd->DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
        pd->DepthStencilState.DepthWriteMask =
            rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd->DepthStencilState.DepthFunc = gcm_cmp(rs->depth_func);
        if (rs->stencil_enable) {
            pd->DepthStencilState.StencilEnable    = TRUE;
            pd->DepthStencilState.StencilReadMask  = (UINT8)rs->s_func_mask;
            pd->DepthStencilState.StencilWriteMask = (UINT8)rs->s_write_mask;
            pd->DepthStencilState.FrontFace.StencilFunc = gcm_cmp(rs->s_func);
            pd->DepthStencilState.FrontFace.StencilFailOp = gcm_stencil_op(rs->s_fail);
            pd->DepthStencilState.FrontFace.StencilDepthFailOp = gcm_stencil_op(rs->s_zfail);
            pd->DepthStencilState.FrontFace.StencilPassOp = gcm_stencil_op(rs->s_zpass);
            /* Two-sided off: nv40 applies the front state to both faces. */
            if (rs->stencil_two_sided) {
                pd->DepthStencilState.BackFace.StencilFunc = gcm_cmp(rs->bs_func);
                pd->DepthStencilState.BackFace.StencilFailOp = gcm_stencil_op(rs->bs_fail);
                pd->DepthStencilState.BackFace.StencilDepthFailOp = gcm_stencil_op(rs->bs_zfail);
                pd->DepthStencilState.BackFace.StencilPassOp = gcm_stencil_op(rs->bs_zpass);
            } else {
                pd->DepthStencilState.BackFace = pd->DepthStencilState.FrontFace;
            }
        } else {
            pd->DepthStencilState.StencilEnable = FALSE;
        }
    }
}

static D3D12_TEXTURE_ADDRESS_MODE gcm_wrap(u32 w)
{
    switch (w & 0xF) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 6:
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static int ld_is_depth_tex(const rsx_dsp_texture* t)
{
    /* Preserve the format's 0x80 bit, just as the FP comparison decoder does.
     * Linear/unnormalized flags are outside TEX_FMT_BASE_MASK. */
    const u32 f = t->format & TEX_FMT_BASE_MASK;
    return f >= TEX_FMT_DEPTH24_D8 && f <= 0x93u;
}
static int ld_depth_border_black(void)
{
    static int v = -1; static DWORD last;
    const DWORD now = GetTickCount();
    if (v < 0 || now - last > 1000u) {
        const int was = v;
        last = now;
        v = GetFileAttributesA("depth_border_black") != INVALID_FILE_ATTRIBUTES;
        if (was >= 0 && was != v)
            fprintf(stderr, "[live-draw] depth border %s (depth_border_black file)\n", v ? "BLACK" : "WHITE");
    }
    return v;
}
/* LOD bias of a texture unit in 1/256 steps: SET_TEXTURE_FILTER bits 0-12,
 * signed with 8 fraction bits (RPCS3 fragment_texture::bias). This title sets
 * -1.0 (0x1F00) on nearly every texture -- sharper mips than the default --
 * and the renderer has always ignored it; applying it sharpens (and
 * shimmers) distant textures further at the 1.5x internal scale, so it stays
 * off by default. lod_bias_game file: apply the title's bias. lod_scale_bias
 * file: add log2(internal render scale), so mips are chosen as at 720p. */
#include <math.h>
static int ld_sampler_bias_256(const rsx_dsp_texture* t)
{
    int bias = LD_FILE_FLAG("lod_bias_game") ? (int)((int16_t)((t->filter & 0x1FFFu) << 3) >> 3) : 0;
    if (LD_FILE_FLAG("lod_scale_bias") && g_rs_den && g_rs_num > g_rs_den)
        bias += (int)(256.0 * log((double)g_rs_num / (double)g_rs_den) / 0.69314718055994531 + 0.5);
    return bias;
}
static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t)
{
    D3D12_SAMPLER_DESC sd = {0};
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    const int min_linear = (minf == 2 || minf == 4 || minf == 6);
    const int mag_linear = (magf == 2);
    const int mip_linear = (minf == 5 || minf == 6);
    const int mip_present = (minf >= 3);
    D3D12_FILTER_TYPE mnf = min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mgf = mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mpf = mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mnf, mgf, mpf, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.AddressU = gcm_wrap(t->wrap);
    sd.AddressV = gcm_wrap(t->wrap >> 8);
    sd.AddressW = gcm_wrap(t->wrap >> 16);
    const u32 max_lod_fx = (t->control0 >> 7)  & 0xFFF;
    const u32 min_lod_fx = (t->control0 >> 19) & 0xFFF;
    sd.MinLOD = (float)min_lod_fx / 256.0f;
    sd.MaxLOD = mip_present ? (float)max_lod_fx / 256.0f : 0.0f;
    if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
    sd.MipLODBias = (float)ld_sampler_bias_256(t) / 256.0f;
    if (LD_FILE_FLAG("mip_base_only")) sd.MaxLOD = sd.MinLOD;   /* diagnostic: are the small mips wrong? */
    sd.MaxAnisotropy = 1;
    /* TEXTURE_BORDER_COLOR is ARGB8. It was never copied, so every BORDER
     * address mode (4, and mirror-once-border 7) sampled transparent black.
     * A shadow map read off its edge then returned depth 0, which compares as
     * occluded: the unlit region outside the light's frustum ended in a hard
     * dark edge. A depth texture whose border is left at 0 reads as the far
     * plane (lit), which is what the light map coverage intends; the
     * depth_border_black file in the working directory restores 0 for A/B. */
    {
        const u32 bc = t->border_color;
        sd.BorderColor[0] = (float)((bc >> 16) & 0xFF) / 255.0f;
        sd.BorderColor[1] = (float)((bc >> 8) & 0xFF) / 255.0f;
        sd.BorderColor[2] = (float)(bc & 0xFF) / 255.0f;
        sd.BorderColor[3] = (float)((bc >> 24) & 0xFF) / 255.0f;
        if (ld_is_depth_tex(t) && bc == 0 && !ld_depth_border_black())
            sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    }
    return sd;
}
/* Cache key of a sampler: exactly the fields decode_sampler reads, so two states share a descriptor only when
 * they decode alike. The previous key kept wrap bits 0-11 (U and V, none of W at bits 16-19) and shifted the 21-bit
 * LOD field of control0 left by 18, which drops its top seven bits: min LOD (control0 bits 19-30) above bit 20
 * and the W address mode were lost, so states differing only there reused another state's descriptor. */
static u64 sampler_key(const rsx_dsp_texture* t)
{
    const u64 minf    = (t->filter >> 16) & 0x7;
    const u64 magf    = (t->filter >> 24) & 0x7;
    const u64 wrap_u  = t->wrap & 0xF;
    const u64 wrap_v  = (t->wrap >> 8) & 0xF;
    const u64 wrap_w  = (t->wrap >> 16) & 0xF;
    const u64 max_lod = (t->control0 >> 7) & 0xFFF;
    const u64 min_lod = (t->control0 >> 19) & 0xFFF;
    /* Border color, 5 bits per channel, plus the depth-white substitution. */
    const u32 bc = t->border_color;
    const u64 bkey = (u64)(((bc >> 3) & 0x1Fu) | ((bc >> 6) & 0x3E0u) | ((bc >> 9) & 0x7C00u) | ((bc >> 12) & 0xF8000u));
    const u64 dw = (ld_is_depth_tex(t) && bc == 0 && !ld_depth_border_black()) ? 1u : 0u;
    const u64 exact = minf | (magf << 3) | (wrap_u << 6) | (wrap_v << 10) | (wrap_w << 14) | (max_lod << 18) |
                      (min_lod << 30) | (bkey << 42) | (dw << 62);
    /* The bias does not fit the 63 exact bits; mixed in (bias 0 keeps the old key). */
    return exact ^ ((u64)(u32)ld_sampler_bias_256(t) * 0x9E3779B97F4A7C15ull) ^ (LD_FILE_FLAG("mip_base_only") ? 0x5bd1e995ull : 0);
}

/* ---------------------------------------------------------------------------
 * descriptor heap helpers
 * -----------------------------------------------------------------------*/
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_cpu_heap, &h);
    h.ptr += (size_t)slot * g.srv_step;
    return h;
}
static void srv_write(u32 slot, ID3D12Resource* tex)
{
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, NULL, srv_cpu(slot));
}
static void srv_write_surface(u32 slot, ID3D12Resource* tex, DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (format == DXGI_FORMAT_R8_UNORM)
        sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0,0,0,5);
    else if (format == DXGI_FORMAT_R32_FLOAT)
        sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0,0,0,0);
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, &sd, srv_cpu(slot));
}
static void srv_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* The replay-proven depth snapshot broadcasts depth to RGB. Mirror that
     * mapping directly in the native SRV and force alpha to one. */
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void srv_write_depth_source(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void uav_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateUnorderedAccessView(
        g.dev, tex, NULL, &ud, srv_cpu(slot));
}
static u32 ld_depth_remap_mapping(u32 remap, int packed)
{
    static const u32 argb[4] = {3, 0, 1, 2};
    static const u32 out2comp[4] = {1, 2, 3, 0};
    u32 mapping = 1u << 12;
    for (u32 out = 0; out < 4; out++) {
        const u32 comp = out2comp[out];
        const u32 op = (remap >> (8 + comp * 2)) & 3;
        const u32 sel = (remap >> (comp * 2)) & 3;
        const u32 source = packed ? argb[sel] : 0;
        mapping |= (op == 0 ? 4 : op == 1 ? 5 : source) << (out * 3);
    }
    return mapping;
}

static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE], int guest_remap)
{
    if (g.srv_ring_used >= SRV_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SRV_TABLE_SIZE];
    const UINT dst_size = SRV_TABLE_SIZE;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_heap, &dst);
    dst.ptr += (size_t)base * g.srv_step;
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++)
        src[i] = srv_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SRV_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (guest_remap) for (u32 unit = 0; unit < SRV_TABLE_SIZE; unit++) {
        const int packed = slots[unit] >= SRV_ZPACK_BASE &&
                           slots[unit] < SRV_ZPACK_BASE + g.n_zdepths;
        const int normalized = slots[unit] >= SRV_ZDEPTH_BASE &&
                               slots[unit] < SRV_ZDEPTH_BASE + g.n_zdepths;
        if (!packed && !normalized) continue;
        const u32 index = slots[unit] - (packed ? SRV_ZPACK_BASE : SRV_ZDEPTH_BASE);
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = packed ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = ld_depth_remap_mapping(texture.remap, packed);
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE unit_dst = dst;
        unit_dst.ptr += (size_t)unit * g.srv_step;
        g.dev->lpVtbl->CreateShaderResourceView(g.dev,
            packed ? g.zdepths[index].packed_snapshot : g.zdepths[index].snapshot,
            &sd, unit_dst);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.srv_heap, &h);
    h.ptr += (u64)base * g.srv_step;
    return h;
}
static int depth_snapshot_descriptors(
    u32 zindex, D3D12_GPU_DESCRIPTOR_HANDLE* out_source,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_destination)
{
    if (zindex >= MAX_SURFACES || g.srv_ring_used >= SRV_RING_TABLES)
        return 0;
    const u32 base = g.srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        g.srv_heap, &cpu);
    cpu.ptr += (size_t)base * g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(SRV_DEPTH_SOURCE_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(UAV_ZDEPTH_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        g.srv_heap, &gpu);
    gpu.ptr += (u64)base * g.srv_step;
    *out_source = gpu;
    gpu.ptr += g.srv_step;
    *out_destination = gpu;
    return 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}
static u32 sampler_slot(const rsx_dsp_texture* t, u64 key)
{
#if defined(YZ_PERF_PROFILE)
    const int profile_lookup=ld_texture_profile_active();
    if(profile_lookup)g_ld_profile.total.sampler_lookups++;
#endif
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key) {
#if defined(YZ_PERF_PROFILE)
            if(profile_lookup)g_ld_profile.total.sampler_probes+=i+1u;
#endif
            return 1+i;
        }
#if defined(YZ_PERF_PROFILE)
    if(profile_lookup)g_ld_profile.total.sampler_probes+=g.n_samplers;
#endif
    if (g.n_samplers >= SMP_CACHE_SLOTS) return SMP_DEFAULT;
    D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    for (u32 i = 0; i < g.smp_ring_used; i++) {
        if (memcmp(g.smp_ring_slots[i], slots, sizeof(g.smp_ring_slots[i])) == 0) {
            D3D12_GPU_DESCRIPTOR_HANDLE cached;
            g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                g.smp_heap, &cached);
            cached.ptr += (u64)(i * SMP_TABLE_SIZE) * g.smp_step;
            return cached;
        }
    }
    /* The caller preflights capacity before recording render-state commands,
     * because a command-list flush here would discard that state. */
    if (g.smp_ring_used >= SMP_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.smp_ring_used++ * SMP_TABLE_SIZE;
    memcpy(g.smp_ring_slots[base / SMP_TABLE_SIZE], slots,
           sizeof(g.smp_ring_slots[0]));
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SMP_TABLE_SIZE];
    const UINT dst_size = SMP_TABLE_SIZE;
    g.smp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_heap, &dst);
    dst.ptr += (size_t)base * g.smp_step;
    for (u32 i = 0; i < SMP_TABLE_SIZE; i++)
        src[i] = smp_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SMP_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.smp_heap, &h);
    h.ptr += (u64)base * g.smp_step;
    return h;
}

/* ---------------------------------------------------------------------------
 * command list submit/wait (simple synchronous model, like the harness)
 * -----------------------------------------------------------------------*/
/* Hardware batch timing diagnostic. Never creates waits, signals or flushes. */
#define LD_GT_SLOTS 64u
typedef struct {u64 fence,seq;u32 first_frame,last_frame,reason;LONGLONG submit,wait;int pending;}ld_gt_batch;
static struct {int on,active,armed;u32 cursor,count,read,dropped,invalid,cap;u64 hz,next_gate;LONGLONG qhz;ID3D12QueryHeap*heap;ID3D12Resource*buffer;u64*data;FILE*file;char gate[MAX_PATH];ld_gt_batch slots[LD_GT_SLOTS];}g_gt;
static LONGLONG ld_gt_qpc(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
static void ld_gt_fail(const char*where,HRESULT hr){fprintf(stderr,"[gpu-timing] disabled where=%s hr=%08lX\n",where,(unsigned long)hr);g_gt.on=0;g_gt.active=-1;}
static void ld_gt_collect(void){
 if(!g_gt.on)return;u64 done=g.fence->lpVtbl->GetCompletedValue(g.fence);if(done==UINT64_MAX){ld_gt_fail("device_removed",E_FAIL);return;}
 for(u32 i=0;i<LD_GT_SLOTS;i++){ld_gt_batch*b=&g_gt.slots[i];if(!b->pending||done<b->fence)continue;u64 a=g_gt.data[i*2],z=g_gt.data[i*2+1];int valid=z>=a;if(!valid)g_gt.invalid++;
 fprintf(g_gt.file,"%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%d,%.9f,%lld,%.9f\n",(unsigned long long)b->seq,b->first_frame,b->last_frame,b->reason,(unsigned long long)b->fence,(unsigned long long)a,(unsigned long long)z,(unsigned long long)g_gt.hz,valid,valid?(double)(z-a)*1000.0/g_gt.hz:-1.0,(long long)b->submit,(double)b->wait*1000.0/g_gt.qhz);b->pending=0;g_gt.read++;}
}
static void ld_gt_begin(void){
 if(!g_gt.on||g_gt.count>=g_gt.cap)return;
 if(!g_gt.armed){u64 now=GetTickCount64();if(now<g_gt.next_gate)return;g_gt.next_gate=now+250;DWORD a=GetFileAttributesA(g_gt.gate);if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_DIRECTORY))return;g_gt.armed=1;}
 for(u32 n=0;n<LD_GT_SLOTS;n++){u32 i=(g_gt.cursor+n)%LD_GT_SLOTS;if(g_gt.slots[i].pending)continue;memset(&g_gt.slots[i],0,sizeof(ld_gt_batch));g_gt.slots[i].first_frame=g_ld_frames;g_gt.active=(int)i;g_gt.cursor=(i+1)%LD_GT_SLOTS;g.list->lpVtbl->EndQuery(g.list,g_gt.heap,D3D12_QUERY_TYPE_TIMESTAMP,i*2);return;}g_gt.dropped++;
}
static int ld_gt_end(ld_flush_reason reason){
 if(!g_gt.on||g_gt.active<0)return -1;u32 i=(u32)g_gt.active;g_gt.slots[i].last_frame=g_ld_frames;g_gt.slots[i].reason=(u32)reason;
 g.list->lpVtbl->EndQuery(g.list,g_gt.heap,D3D12_QUERY_TYPE_TIMESTAMP,i*2+1);g.list->lpVtbl->ResolveQueryData(g.list,g_gt.heap,D3D12_QUERY_TYPE_TIMESTAMP,i*2,2,g_gt.buffer,(u64)i*16);return(int)i;
}
static void ld_gt_init(void){
 const char*on=getenv("LD_GPU_TIMING");if(!on||strcmp(on,"1"))return;memset(&g_gt,0,sizeof g_gt);g_gt.active=-1;g_gt.cap=256;
 const char*cap=getenv("LD_GPU_TIMING_MAX_BATCHES");if(cap&&*cap){char*e=NULL;unsigned long n=strtoul(cap,&e,10);if(e!=cap&&!*e&&n>=1&&n<=4096)g_gt.cap=(u32)n;}
 const char*gate=getenv("LD_GPU_TIMING_GATE");if(gate&&*gate){if(strlen(gate)>=sizeof g_gt.gate){ld_gt_fail("gate_length",E_INVALIDARG);return;}strcpy(g_gt.gate,gate);}else g_gt.armed=1;
 HRESULT hr=g.queue->lpVtbl->GetTimestampFrequency(g.queue,&g_gt.hz);if(FAILED(hr)||!g_gt.hz){ld_gt_fail("frequency",FAILED(hr)?hr:E_FAIL);return;}
 LARGE_INTEGER q;QueryPerformanceFrequency(&q);g_gt.qhz=q.QuadPart;
 D3D12_QUERY_HEAP_DESC qd={0};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=LD_GT_SLOTS*2;hr=g.dev->lpVtbl->CreateQueryHeap(g.dev,&qd,&IID_ID3D12QueryHeap,(void**)&g_gt.heap);if(FAILED(hr)){ld_gt_fail("heap",hr);return;}
 D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=LD_GT_SLOTS*16;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 hr=g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&g_gt.buffer);if(FAILED(hr)){ld_gt_fail("readback",hr);return;}
 D3D12_RANGE range={0,LD_GT_SLOTS*16};hr=g_gt.buffer->lpVtbl->Map(g_gt.buffer,0,&range,(void**)&g_gt.data);if(FAILED(hr)){ld_gt_fail("map",hr);return;}
 const char*out=getenv("LD_GPU_TIMING_OUTPUT");g_gt.file=fopen(out&&*out?out:"gpu-timing.csv","wb");if(!g_gt.file){ld_gt_fail("output",E_FAIL);return;}
 fprintf(g_gt.file,"batch,open_frame,close_frame,flush_reason,fence,start_tick,end_tick,frequency_hz,valid,gpu_ms,submit_qpc,cpu_fence_wait_ms\n");g_gt.on=1;fprintf(stderr,"[gpu-timing] enabled frequency=%llu slots=64 max_batches=%u gate=%s no_extra_wait=1\n",(unsigned long long)g_gt.hz,g_gt.cap,g_gt.armed?"none":g_gt.gate);ld_gt_begin();
}
static void ld_gt_shutdown(void){
 ld_gt_collect();if(g_gt.file){fprintf(stderr,"[gpu-timing] summary submitted=%u collected=%u dropped=%u invalid=%u\n",g_gt.count,g_gt.read,g_gt.dropped,g_gt.invalid);fclose(g_gt.file);g_gt.file=NULL;}
 if(g_gt.data){D3D12_RANGE w={0,0};g_gt.buffer->lpVtbl->Unmap(g_gt.buffer,0,&w);g_gt.data=NULL;}if(g_gt.buffer)g_gt.buffer->lpVtbl->Release(g_gt.buffer);if(g_gt.heap)g_gt.heap->lpVtbl->Release(g_gt.heap);g_gt.buffer=NULL;g_gt.heap=NULL;g_gt.on=0;
}

#include "rsx_live_async_frames.h"

static void ld_wb_poll(void);
void pp_tl(const char* tag, unsigned a, unsigned b);
static void ld_zeta_import_color(u32 zslot);
static int ld_wb_blit_enabled(void);
static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot);
static void ld_wb_record(u32 di, u32 dx, u32 dy, u32 w, u32 h, u32 dst_loc, u32 dst_abs, u32 dst_pitch);
static void ld_flush(ld_flush_reason reason)
{
   
    if(reason==LD_FLUSH_PRESENT && ld_af_submit_present())return;
    if(!ld_af_drain_reason((u32)reason))return;
#if defined(YZ_PERF_PROFILE)
    const LONGLONG flush_begin = ld_profile_qpc();
    g_ld_profile.total.flush_reason[reason]++;
    ld_profile_note_ring_highwater();
#else
    (void)reason;
#endif
    const int gt_slot=ld_gt_end(reason);
    const HRESULT close_hr = g.list->lpVtbl->Close(g.list);
    if (FAILED(close_hr)) {
        fprintf(stderr,
                "[d3d-fail] command-list Close hr=0x%08lX frame=%u\n",
                (unsigned long)close_hr, g_ld_frames);
        if(g_gt.on)ld_gt_fail("Close",close_hr);
        ld_dump_dred("Close", close_hr);
        g.ready = 0;
        return;
    }
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    const LONGLONG gt_submit=gt_slot>=0?ld_gt_qpc():0;
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    if(g_af.on && (u32)reason<LD_AF_FLUSH_REASONS)g_af_counts.legacy_submits[reason]++;
    const u64 v = ++g.fence_value;
    const HRESULT signal_hr =
        g.queue->lpVtbl->Signal(g.queue, g.fence, v);
    if (FAILED(signal_hr)) {
        fprintf(stderr,
                "[d3d-fail] queue Signal hr=0x%08lX frame=%u\n",
                (unsigned long)signal_hr, g_ld_frames);
        if(g_gt.on)ld_gt_fail("Signal",signal_hr);
        ld_dump_dred("Signal", signal_hr);
        g.ready = 0;
        return;
    }
    ld_af_assign_labels(v);
    if(g_af.on&&g_af.poisoned)return;
    if(gt_slot>=0){ld_gt_batch*b=&g_gt.slots[gt_slot];b->fence=v;b->submit=gt_submit;b->pending=1;b->seq=++g_gt.count;g_gt.active=-1;}
    if (g.fence->lpVtbl->GetCompletedValue(g.fence) < v) {
        if(g_af.on && (u32)reason<LD_AF_FLUSH_REASONS)g_af_counts.legacy_waits[reason]++;
#if defined(YZ_PERF_PROFILE)
        const LONGLONG wait_begin = ld_profile_qpc();
#endif
        const HRESULT event_hr =
            g.fence->lpVtbl->SetEventOnCompletion(
                g.fence, v, g.fence_event);
        if (FAILED(event_hr)) {
            fprintf(stderr,
                    "[d3d-fail] fence SetEventOnCompletion "
                    "hr=0x%08lX frame=%u\n",
                    (unsigned long)event_hr, g_ld_frames);
            ld_dump_dred("SetEventOnCompletion", event_hr);
            g.ready = 0;
            return;
        }
        /* CPU fence wait only: this is not a GPU timestamp query. Keep the
         * existing synchronization unchanged while making D3D12 stalls
         * visible in the same critical-path timeline as FIFO drains. */
        const uint64_t ft_wait_begin = ps3_ft_on() ? ps3_ft_now() : 0;
        const LONGLONG gt_wait=gt_slot>=0?ld_gt_qpc():0;
        WaitForSingleObject(g.fence_event, INFINITE);
        if(gt_slot>=0)g_gt.slots[gt_slot].wait=ld_gt_qpc()-gt_wait;
        if (ft_wait_begin)
            ps3_ft_span(PS3_FT_GPU_WAIT, 0, 0, (uint32_t)reason,
                        ft_wait_begin, ps3_ft_now());
#if defined(YZ_PERF_PROFILE)
        const u64 wait_ticks = (u64)(ld_profile_qpc() - wait_begin);
        g_ld_profile.total.fence_wait_qpc += wait_ticks;
        g_ld_profile.total.fence_reason_qpc[reason] += wait_ticks;
#endif
    }
    if(g_af.on){ld_af_poll();if(g_af.poisoned)return;}
    ld_gt_collect();
    ld_drain_info_queue("flush");
    /* Dynamic guest textures can replace a cached D3D resource while an
     * earlier draw in this command list still references the old one.  The
     * fence above is the first safe point at which those old resources may be
     * released. */
    for (u32 i = 0; i < g.n_retired_textures; i++)
        g.retired_textures[i]->lpVtbl->Release(g.retired_textures[i]);
    g.n_retired_textures = 0;
    g.alloc->lpVtbl->Reset(g.alloc);
    const HRESULT gt_reset_hr=g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
    if(g_gt.on&&FAILED(gt_reset_hr))ld_gt_fail("list_reset",gt_reset_hr);
    if(reason!=LD_FLUSH_SHUTDOWN)ld_gt_begin();
    g.upload_used = 0;
    ld_wb_poll();
#if defined(YZ_PERF_PROFILE)
    const u64 flush_ticks = (u64)(ld_profile_qpc() - flush_begin);
    g_ld_profile.total.flush_qpc += flush_ticks;
    g_ld_profile.total.flush_reason_qpc[reason] += flush_ticks;
#endif
}

/* DXGI pacing is independent of the guest VBlank gate. Preserve the original
 * interval by default. This diagnostic never changes swap-chain flags, guest
 * flip completion, or the success/error handling at the two call sites. */
static INIT_ONCE g_ld_present_config_once = INIT_ONCE_STATIC_INIT;
static UINT g_ld_present_interval = 1u;
static int g_ld_present_timing;
static volatile LONG64 g_ld_present_timing_calls;
static BOOL CALLBACK ld_present_config_init(PINIT_ONCE once, PVOID arg, PVOID* context)
{
    (void)once;(void)arg;(void)context;
    const char* value=getenv("LD_PRESENT_INTERVAL");
    g_ld_present_interval=value&&!strcmp(value,"0")?0u:1u;
    const char* timing=getenv("LD_PRESENT_TIMING");
    g_ld_present_timing=timing&&!strcmp(timing,"1");
    if(value || g_ld_present_timing)
        fprintf(stderr,"[dxgi-present-config] requested=%s interval=%u flags=0 timing=%d default=1\n",value?value:"unset",g_ld_present_interval,g_ld_present_timing);
    return TRUE;
}
/* Options window (rsx_null_backend.c): vsync on/off at run time. */
void rsx_live_draw_set_vsync(int on)
{
    InitOnceExecuteOnce(&g_ld_present_config_once,ld_present_config_init,NULL,NULL);
    g_ld_present_interval = on ? 1u : 0u;
}
static HRESULT ld_swap_present(u32 frame, u32 movie)
{
    InitOnceExecuteOnce(&g_ld_present_config_once,ld_present_config_init,NULL,NULL);
    const int trace=ps3_ft_on();
    const int timed=trace||g_ld_present_timing;
    const uint64_t begin=timed?ps3_ft_now():0;
    const HRESULT hr=g.swap->lpVtbl->Present(g.swap,g_ld_present_interval,0);
    const uint64_t end=timed?ps3_ft_now():0;
    if(trace)ps3_ft_span(PS3_FT_PRESENT,0,movie,g_ld_present_interval,begin,end);
    if(g_ld_present_timing){
        const LONG64 call=InterlockedIncrement64(&g_ld_present_timing_calls);
        if(call<=4 || (call%600==0 && call<=36000))
            fprintf(stderr,"[dxgi-present] call=%llu frame=%u path=%s interval=%u flags=0 wall_ms=%.6f hr=%08lX\n",(unsigned long long)call,frame,movie?"movie":"game",g_ld_present_interval,(double)(end-begin)/1e6,(unsigned long)hr);
    }
    if(!movie)ld_af_post_present(hr);
    return hr;
}

/* A reached flip before the first guest RT still needs real GPU work and a
   successful Present. Clearing the actual swapchain BB consumes that request
   through precisely the ordinary frame-fence completion path. */
static void ld_af_blank_present(void) {
    if(!g_af.on||!g_af.request_valid||!g.ready)return;
    u32 bb=g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    if(!ld_af_claim(bb))return;
    ld_af_record_blank(g.backbuf[bb],rtv_handle(bb));
    ld_flush(LD_FLUSH_PRESENT);
    if(!g.ready)return;
    HRESULT hr=ld_swap_present(g_ld_frames+1u,0u);
    if(FAILED(hr)){if(!g_af.poisoned)ld_af_poison("blank-present",hr);return;}
    if(!g.ready)return;
    ++g_ld_frames;g.depth_cleared=0;
}

/* Public wrapper for the RSX SET_REFERENCE / sync fence: block until the GPU
 * has finished all queued draws (mirrors RPCS3 nv406e::set_reference's sync(),
 * RSXThread.cpp), so the game's REF poll advances only after the GPU has really
 * caught up. Without it our async consumer writes REF instantly and races ahead
 * of real GPU time (measured: ours skips the fence wait RPCS3 performs). Gated
 * at the call site by RSX_FENCE_SYNC. */
void rsx_live_draw_flush(void)
{
    if (g.ready) ld_flush(LD_FLUSH_GUEST_REFERENCE);
}

static int ld_af_label(void *user,u32 ea,u32 value) {
    (void)user;
    return ld_af_defer_label(ea,value);
}

int rsx_live_draw_async_enabled(void){return g_af.on;}
void* rsx_live_draw_gpu_completion_event(void){return g_af.on?(void*)g_af.gpu_completion_event:NULL;}
void rsx_live_draw_set_present_request(u32 seq){
    if(!g_af.on)return;
    if(g_af.request_valid){ld_af_poison("duplicate-guest-request",E_UNEXPECTED);return;}
    g_af.request_seq=seq;g_af.request_valid=1;
}
int rsx_live_draw_take_gpu_completion(u32 *seq){
    if(!g_af.on)return 0;
    if(!g.ready&&!g_af.poisoned)ld_af_poison("renderer-not-ready",E_FAIL);
    ld_af_poll();if(g_af.poisoned)return -1;
    if(g_af.complete_read==g_af.complete_write)return 0;
    if(!seq)return -1;
    *seq=g_af.complete_seq[g_af.complete_read++%LD_AF_COMPLETIONS];return 1;
}

static void retire_texture(ID3D12Resource* tex)
{
    if (!tex) return;
    if (g.n_retired_textures >= MAX_RETIRED_TEXTURES)
        ld_flush(LD_FLUSH_RETIRE_QUEUE);
    g.retired_textures[g.n_retired_textures++] = tex;
    ld_profile_note_ring_highwater();
}

/* ---------------------------------------------------------------------------
 * texture upload (single-level + mip)
 * -----------------------------------------------------------------------*/
static u32 log2_u32(u32 v) { u32 n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static u32 morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}
static u8 remap_comp(const u8 s[4], u32 remap, u32 comp)
{
    const u32 op  = (remap >> (8 + comp * 2)) & 3;
    const u32 sel = (remap >> (comp * 2)) & 3;
    if (op == 0) return 0;
    if (op == 1) return 255;
    return s[sel];
}
static void decode_texel(u32 base_fmt, const u8* p, u32 remap, u8 d[4])
{
    u8 s[4];
    switch (base_fmt) {
    /* B8 is one channel. Broadcasting it to alpha as well is arguably more
     * faithful (the YUV movie combine shader samples each plane's .w), but
     * DO NOT do it until the USM decode actually fills those planes: while
     * they are empty it only makes the garbage VISIBLE, putting green noise
     * behind the title screen and every other movie-backed UI. Tried
     * 2026-09-01, reverted the same session. See ydkj-live-draw-rebuild. */
    case TEX_FMT_B8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_A4R4G4B4: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    }
    case TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        /* LD_FORCE_A1_OPAQUE=1: experiment, not a fix. A PS1 15-bit pixel uses
         * the top bit as a semi-transparency FLAG, and it is clear for ordinary
         * opaque pixels -- so decoding it as A1R5G5B5 alpha gives alpha=0 for
         * essentially the whole framebuffer. If the pipeline then blends on
         * source alpha, every pixel is discarded and the surface reads black,
         * which is exactly what LD_SURF_DUMP reports for all nine surfaces.
         * This flips alpha to opaque so that theory can be tested in one run
         * instead of argued about. */
        { static int fo = -1;
          if (fo < 0) fo = getenv("LD_FORCE_A1_OPAQUE") ? 1 : 0;
          s[0] = fo ? 255 : ((v & 0x8000) ? 255 : 0); }
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_R5G6B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_G8B8:
        s[0] = 255;
        s[1] = s[2] = p[0];
        s[3] = p[1];
        break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: {
        /* Four big-endian halves, X Y Z W -> R G B A. Values are linear and
         * may exceed 1.0 (these are render targets), so clamp rather than
         * wrap. Decoded to 8-bit because the cache is RGBA8 throughout. */
        for (int c = 0; c < 4; c++) {
            const u16 hbits = (u16)((p[c * 2] << 8) | p[c * 2 + 1]);
            const int  exp  = (hbits >> 10) & 0x1F;
            const int  man  = hbits & 0x3FF;
            float f;
            if (exp == 0)        f = (float)man * (1.0f / 16384.0f) * (1.0f / 64.0f);
            else if (exp == 31)  f = 1.0f;
            else {
                float m = 1.0f + (float)man / 1024.0f;
                int   e = exp - 15;
                while (e > 0) { m *= 2.0f; e--; }
                while (e < 0) { m *= 0.5f; e++; }
                f = m;
            }
            if (hbits & 0x8000) f = 0.0f;   /* negative -> clamp to zero */
            if (f > 1.0f) f = 1.0f;
            /* s[] is A,R,G,B; the halves arrive X,Y,Z,W = R,G,B,A. */
            const u8 v = (u8)(f * 255.0f + 0.5f);
            if (c == 3) s[0] = v; else s[c + 1] = v;
        }
        break;
    }
    case TEX_FMT_DEPTH24_D8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    default: s[0] = p[0]; s[1] = p[1]; s[2] = p[2]; s[3] = p[3]; break;
    }
    d[0] = remap_comp(s, remap, 1);
    d[1] = remap_comp(s, remap, 2);
    d[2] = remap_comp(s, remap, 3);
    d[3] = remap_comp(s, remap, 0);
}

typedef struct { u32 w, h; const u8* data; u32 row_bytes, rows; } tex_level_t;

static ID3D12Resource* create_texture_mipped(DXGI_FORMAT fmt, const tex_level_t* lv, u32 n)
{
    if (n == 0) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)n; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    const HRESULT texture_hr = g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex);
    if (FAILED(texture_hr)) {
        static u32 texture_resource_fail_logs;
        if (texture_resource_fail_logs++ < 8)
            fprintf(stderr, "[texture-resource] create failed fmt=%u %ux%u mips=%u hr=0x%08lX\n",
                    (u32)fmt, lv[0].w, lv[0].h, n, (unsigned long)texture_hr);
        return NULL;
    }
    for (u32 m = 0; m < n; m++) {
        const u32 pitch = (lv[m].row_bytes + 255) & ~255u;
        const u32 start = (g.upload_used + 511) & ~511u;
        if ((u64)start + (u64)pitch * lv[m].rows > UPLOAD_SIZE) {
            static volatile LONG overflow_logs;
            if (InterlockedIncrement(&overflow_logs) <= 16)
                fprintf(stderr, "[texture-upload] staging full: used=%u need=%u %ux%u mip=%u -- level skipped\n",
                        g.upload_used, pitch * lv[m].rows, lv[m].w, lv[m].h, m);
            break;
        }
        for (u32 y = 0; y < lv[m].rows; y++)
            memcpy(g.upload_mapped + start + (size_t)y * pitch,
                   lv[m].data + (size_t)y * lv[m].row_bytes, lv[m].row_bytes);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = g.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = start;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = lv[m].w;
        src.PlacedFootprint.Footprint.Height = lv[m].h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        g.upload_used = start + pitch * lv[m].rows;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.texture_upload_bytes +=
            (u64)pitch * lv[m].rows;
#endif
        ld_profile_note_ring_highwater();
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

/* Cubemap faces are stored face-major in guest memory. D3D12 subresources use
 * that same order: face * mip_count + mip. */
static ID3D12Resource* create_texture_cube(
    DXGI_FORMAT fmt, const tex_level_t* lv, u32 n_mips)
{
    if (!n_mips) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 6;
    rd.MipLevels = (u16)n_mips; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 face = 0; face < 6; face++) {
        for (u32 mip = 0; mip < n_mips; mip++) {
            const tex_level_t* level = &lv[face * n_mips + mip];
            const u32 pitch = (level->row_bytes + 255) & ~255u;
            const u32 start = (g.upload_used + 511) & ~511u;
            if ((u64)start + (u64)pitch * level->rows > UPLOAD_SIZE) {
                tex->lpVtbl->Release(tex);
                return NULL;
            }
            for (u32 y = 0; y < level->rows; y++)
                memcpy(g.upload_mapped + start + (size_t)y * pitch,
                       level->data + (size_t)y * level->row_bytes,
                       level->row_bytes);
            D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
            src.pResource = g.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = start;
            src.PlacedFootprint.Footprint.Format = fmt;
            src.PlacedFootprint.Footprint.Width = level->w;
            src.PlacedFootprint.Footprint.Height = level->h;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = face * n_mips + mip;
            g.list->lpVtbl->CopyTextureRegion(
                g.list, &dst, 0, 0, 0, &src, NULL);
            g.upload_used = start + pitch * level->rows;
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.texture_upload_bytes +=
                (u64)pitch * level->rows;
#endif
            ld_profile_note_ring_highwater();
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return tex;
}

static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h)
{
    tex_level_t lv = { w, h, rgba, w * 4, h };
    return create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, &lv, 1);
}

static u32 texture_source_span(const rsx_dsp_texture* t)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    u32 texel_size = 0, block_size = 0;
    switch (base_fmt) {
    case TEX_FMT_DXT1:  block_size = 8; break;
    case TEX_FMT_DXT23:
    case TEX_FMT_DXT45: block_size = 16; break;
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8:
    case TEX_FMT_X16: texel_size = 2; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    default: return 0;
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096 ||
        (t->dimension != 2 && !(t->dimension == 1 && t->height == 1 && !t->cubemap)))
        return 0;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;
    if (t->cubemap && block_size) {
        n_mips = 1;
        for (u32 d = (t->width < t->height ? t->width : t->height) / 4;
             d > 1; d >>= 1)
            n_mips++;
        if (t->mipmaps && n_mips > t->mipmaps)
            n_mips = t->mipmaps;
        if (n_mips > 14) n_mips = 14;
    }
    u32 mw = t->width, mh = t->height, span = 0;
    for (u32 m = 0; m < n_mips; m++) {
        if (block_size)
            span += ((mw + 3) / 4) * block_size * ((mh + 3) / 4);
        else {
            const u32 pitch = (m == 0 && linear && t->pitch)
                ? t->pitch : mw * texel_size;
            span += pitch * mh;
        }
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    return t->cubemap ? span * 6 : span;
}

/* Process-lifetime algorithm selection: cache creation and refresh both call
 * texture_content_hash and cannot disagree or switch after hashes are stored. */
static INIT_ONCE g_ld_texture_hash_once=INIT_ONCE_STATIC_INIT;
static int g_ld_texture_hash_mode=RSX_TEXTURE_HASH_FNV;
static BOOL CALLBACK ld_texture_hash_config_init(PINIT_ONCE once,PVOID arg,PVOID* context){
    (void)once;(void)arg;(void)context;
    const char* value=getenv("LD_TEXTURE_HASH");
    g_ld_texture_hash_mode=rsx_texture_hash_parse_mode(value);
    fprintf(stderr,"[texture-hash-config] requested=%s selected=%s full_span=1 vector=x64-sse2 default=fnv\n",value?value:"unset",g_ld_texture_hash_mode==RSX_TEXTURE_HASH_XXH3?"xxh3":"fnv");
    return TRUE;
}
static int ld_texture_hash_mode(void){
    InitOnceExecuteOnce(&g_ld_texture_hash_once,ld_texture_hash_config_init,NULL,NULL);
    return g_ld_texture_hash_mode;
}

/* Guest page write tracking (MEM_WRITE_WATCH on the guest reservation).
 * A cached texture asks the OS, for its own pages only, which were written
 * since they were last reset; written pages get a fresh epoch stamp and are
 * reset, so the stamps keep the history for other textures sharing a page. A
 * texture whose pages are all stamped no later than its last hash is
 * unchanged and its full-span hash can be skipped. (A single query over the
 * whole 4 GB reservation per present cost ~3.7 ms a frame -- as much as the
 * hashing it saved.) Writes the watch cannot see are covered by the
 * verification in texture_srv_slot. */
extern uint8_t* vm_base;
#define LD_WW_SPAN 0x100000000ull
static u32* g_ww_page;          /* epoch each 4 KB page was last seen written */
static int g_ww_state = -1;     /* -1 untried, 0 unavailable, 1 active */
static u32 g_ww_epoch = 1;
static void ld_ww_update(void)
{
    if (g_ww_state >= 0 || !vm_base) return;
    /* LD_NO_WRITE_WATCH=1: off. */
    if (GetEnvironmentVariableA("LD_NO_WRITE_WATCH", NULL, 0)) { g_ww_state = 0; return; }
    g_ww_page = (u32*)calloc((size_t)(LD_WW_SPAN >> 12), sizeof(u32));
    PVOID probe; ULONG_PTR n = 1; DWORD gran = 0;
    if (!g_ww_page || GetWriteWatch(0, vm_base, 0x10000, &probe, &n, &gran) != 0) {
        fprintf(stderr, "[write-watch] unavailable (error %lu): textures hashed every frame\n", GetLastError());
        g_ww_state = 0; return;
    }
    fprintf(stderr, "[write-watch] active granularity=%lu (per-texture queries)\n", (unsigned long)gran);
    g_ww_state = 1;
}
/* 1 if [p, p+len) may have been written after epoch `since`. */
static int ld_ww_dirty_since(const u8* p, u32 len, u32 since)
{
    if (g_ww_state != 1 || !p || !len) return 1;
    const uintptr_t off = (uintptr_t)p - (uintptr_t)vm_base;
    if (off >= LD_WW_SPAN || (u64)off + len > LD_WW_SPAN) return 1;
    const u64 first = off >> 12, last = ((u64)off + len - 1) >> 12;
    uint8_t* lo = vm_base + (first << 12);
    const SIZE_T sz = (SIZE_T)((last - first + 1) << 12);
    enum { CAP = 4096 };
    static PVOID addrs[CAP];
    ULONG_PTR n = CAP; DWORD gran;
    if (GetWriteWatch(0, lo, sz, addrs, &n, &gran) != 0) return 1;
    if (n) {
        const u32 e = ++g_ww_epoch;
        if (n == CAP) { for (u64 pg = first; pg <= last; pg++) g_ww_page[pg] = e; }
        else for (ULONG_PTR i = 0; i < n; i++) g_ww_page[((uintptr_t)addrs[i] - (uintptr_t)vm_base) >> 12] = e;
        if (ResetWriteWatch(lo, sz) != 0) return 1;
    }
    for (u64 pg = first; pg <= last; pg++)
        if (g_ww_page[pg] > since) return 1;
    return 0;
}

/* Guest data newer than a render surface at the same address (F031).
 *
 * A texture whose (location, offset) equals a surface's samples that surface. When the texture
 * cannot be a sub-rect of it (different size and pitch, see surface_window) the whole surface was
 * bound anyway. Macross 30's controller-type screen draws its menu background blur pyramid into
 * 0:0x0738FE00.. (surface 640x360 pitch 5120 at 0x07713E00), and the game later loads the label
 * texture opt_text01 (256x256 A8R8G8B8, pitch 1024) into the same memory: the guest bytes there
 * were byte-identical to the DDS payload, but every label quad showed the stale blurred sky.
 *
 * The guest reservation is MEM_WRITE_WATCH (PPU stores, SPU DMA, m2mf copies, file reads all
 * register). At a surface's first GPU write in a frame its guest pages are drained (pending
 * writes stamped, epoch remembered). A later bind with an incompatible layout asks the watch
 * for the texture's pages: if at least half of the pages wholly inside the texture's span were
 * written after that epoch, the guest wrote this memory after the GPU last did, so the texture
 * comes from guest memory (texture_srv_slot), as the RSX would read it. Exact-size aliases and
 * sub-rect windows are untouched.
 *
 * Off: LD_ALIAS_GUEST=0 (also stops the drains) or a no_alias_guest file in the working
 * directory (checked once a second; A/B at run time). Log: [alias-guest]. */
static int ld_gw_env_off(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("LD_ALIAS_GUEST"); v = (e && e[0] == '0') ? 1 : 0;
                 if (v) fprintf(stderr, "[alias-guest] disabled (LD_ALIAS_GUEST=0)\n"); }
    return v;
}
static int ld_gw_disabled(void)
{
    static int v = -1; static DWORD last;
    if (ld_gw_env_off()) return 1;
    const DWORD now = GetTickCount();
    if (v < 0 || now - last > 1000u) {
        const int was = v; last = now;
        v = GetFileAttributesA("no_alias_guest") != INVALID_FILE_ATTRIBUTES;
        if (was != v) fprintf(stderr, "[alias-guest] %s\n", v ? "OFF (no_alias_guest)" : "ON");
    }
    return v;
}
/* Stamp pending writes in [p, p+len) without judging them (chunks the watch can report whole). */
static void ld_gw_drain(const u8* p, u64 len)
{
    const u64 chunk = 4096ull << 12;
    for (u64 at = 0; at < len; at += chunk)
        (void)ld_ww_dirty_since(p + at, (u32)(len - at < chunk ? len - at : chunk), UINT32_MAX - 1u);
}
static void ld_gw_surface_written(u32 target)
{
    if (target >= MAX_SURFACES || g_ww_state != 1 || ld_gw_env_off()) return;
    surface_t* s = &g.surfaces[target];
    if (s->gw_frame == g_ld_frames + 1u) return;     /* once per frame: later writes are this frame's too */
    const u32 bpp = ld_surface_texel_bytes(s->fmt);
    u64 row = (u64)s->w * (bpp ? bpp : 4u);
    if (s->pitch > row) row = s->pitch;
    u64 len = row * s->h;
    if (len > (64ull << 20)) len = 64ull << 20;
    const u8* p = (len && s->location <= 1u) ? guest_ptr(s->location, s->offset, (u32)len) : NULL;
    if (!p) { s->gw_frame = 0; return; }
    ld_gw_drain(p, len);
    s->gw_epoch = g_ww_epoch;
    s->gw_frame = g_ld_frames + 1u;
}
static u64 g_ld_gw_guest_binds, g_ld_gw_checks;
/* 1: bind the guest texture instead of surface si (whose layout the texture cannot share). */
static int ld_alias_guest_newer(u32 si, const rsx_dsp_texture* t)
{
    surface_t* s = &g.surfaces[si];
    if (g_ww_state != 1 || !s->gw_frame || ld_gw_disabled()) return 0;
    if (t->width == s->w && t->height == s->h) return 0;   /* exact-size alias: the surface, as before */
    if (t->cubemap || t->dimension != 2u || t->location != s->location) return 0;
    const u32 span = texture_source_span(t);
    const u8* p = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (!p) return 0;
    if (s->gw_chk_frame != g_ld_frames + 1u || s->gw_chk_offset != t->offset || s->gw_chk_span != span) {
        g_ld_gw_checks++;
        ld_gw_drain(p, span);
        const u64 off = (u64)((uintptr_t)p - (uintptr_t)vm_base);
        const u64 first = (off + 0xFFFu) >> 12, end = (off + span) >> 12;   /* pages wholly inside */
        u32 newer = 0, full = 0;
        for (u64 pg = first; pg < end; pg++) { full++; if (g_ww_page[pg] > s->gw_epoch) newer++; }
        s->gw_chk_frame = g_ld_frames + 1u; s->gw_chk_offset = t->offset; s->gw_chk_span = span;
        s->gw_chk_newer = newer; s->gw_chk_full = full;
    }
    const int guest = s->gw_chk_full && s->gw_chk_newer * 2u >= s->gw_chk_full;
    if (guest) {
        static u32 n_log;
        if (n_log++ < 8 || (g_ld_gw_guest_binds & 0xFFFu) == 0)
            fprintf(stderr, "[alias-guest] tex %u:0x%08X fmt=0x%02X %ux%u pitch=%u -> guest upload "
                    "(surface[%u] %ux%u pitch=%u drawn before the guest write: %u/%u pages newer, "
                    "epoch %u) binds=%llu checks=%llu\n", t->location, t->offset, t->format, t->width,
                    t->height, t->pitch, si, s->w, s->h, s->pitch, s->gw_chk_newer, s->gw_chk_full,
                    s->gw_epoch, (unsigned long long)g_ld_gw_guest_binds + 1u,
                    (unsigned long long)g_ld_gw_checks);
        g_ld_gw_guest_binds++;
    }
    return guest;
}

static u64 texture_content_hash(const rsx_dsp_texture* t, int* readable)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG hash_profile_begin=ld_texture_profile_active()?ld_profile_qpc():0;
#endif
    const u32 span = texture_source_span(t);
    const u8* src = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (!src) {
        *readable = 0;
#if defined(YZ_PERF_PROFILE)
        ld_texture_hash_profile_end(hash_profile_begin,span,0);
#endif
        return 0;
    }
    /* LD_TEXSRC_DBG=1: is the guest data there AT ALL?
     *
     * "Draws execute, forced green fills the screen, the sampled texture is
     * black" keeps arriving as the same question, and the decoded-RGBA dump
     * cannot answer it: that dump lives on the uncompressed path, so a DXT
     * texture -- which is most of a real game's atlas -- never reaches it.
     * Counting non-zero SOURCE bytes covers every format and separates "the
     * upload never happened" from "decode ate it". */
    { static int ts = -1; static int tsn = 0;
      if (ts < 0) ts = getenv("LD_TEXSRC_DBG") ? 1 : 0;
      if (ts && tsn < 16) {
          u32 nz = 0;
          for (u32 q = 0; q < span; q++) if (src[q]) nz++;
          char head[80]; int hn = 0;
          for (u32 q = 0; q < 16 && q < span; q++)
              hn += snprintf(head + hn, sizeof head - hn, "%02X", src[q]);
          fprintf(stderr, "[tex-src] loc=%u off=0x%08X %ux%u fmt=0x%02X mips=%u"
                          " remap=0x%08X filter=0x%08X head=%s"
                          " span=%u nonzero=%u\n",
                  t->location, t->offset, t->width, t->height,
                  t->format & 0xFF, t->mipmaps, t->remap, t->filter, head,
                  span, nz);
          fflush(stderr);
          tsn++;
      } }

    /* Same complete readable source span and once-per-entry/frame policy.
     * Only the 64-bit mutation hash implementation is selectable. */
    const u64 hash=rsx_texture_hash_bytes(src,span,ld_texture_hash_mode());
    *readable = 1;
#if defined(YZ_PERF_PROFILE)
    ld_texture_hash_profile_end(hash_profile_begin,span,1);
#endif
    return hash;
}

static int ld_texrgba_dump_offset(const rsx_dsp_texture* t)
{
    static int initialized, filtered;
    static u32 offset;
    if (!initialized) {
        initialized = 1;
        const char* e = getenv("LD_TEXRGBA_DUMP_OFFSET");
        filtered = e && e[0];
        offset = filtered ? (u32)strtoul(e, NULL, 16) : 0;
    }
    return !filtered || t->offset == offset;
}
#include "aircraft_hd_runtime.h"
static ID3D12Resource* decode_guest_texture(const rsx_dsp_texture* t, u32 remap)
{
    { ID3D12Resource* hd=aircraft_hd_texture(t); if(hd)return hd; }
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    const u32 w = t->width, h = t->height;
    if (!w || !h || w > 4096 || h > 4096 ||
        (t->dimension != 2 && !(t->dimension == 1 && h == 1 && !t->cubemap)))
        return NULL;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;

    if (base_fmt == TEX_FMT_X16) {
        const u32 span = texture_source_span(t);
        const u8* source = span ? guest_ptr(t->location, t->offset, span) : NULL;
        if (!source || (!linear && ((w & (w - 1)) || (h & (h - 1)))))
            return NULL;
        u32 levels_per_face = 1;
        for (u32 mw = w, mh = h; mw > 1 || mh > 1; levels_per_face++) {
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        if (n_mips > levels_per_face) n_mips = levels_per_face;
        const u32 faces = t->cubemap ? 6 : 1;
        tex_level_t levels[6 * 14];
        u16* pixels[6 * 14] = {0};
        u32 count = 0;
        int failed = 0;
        for (u32 face = 0; face < faces && !failed; face++) {
            u32 mw = w, mh = h, off = face * (span / faces);
            for (u32 mip = 0; mip < n_mips; mip++) {
                const u32 pitch = mip == 0 && linear && t->pitch ? t->pitch : mw * 2;
                if (pitch < mw * 2 || (u64)off + (u64)pitch * mh > span) {
                    failed = 1; break;
                }
                u16* data = (u16*)malloc((size_t)mw * mh * 2);
                if (!data) { failed = 1; break; }
                u32 lw = 0, lh = 0;
                for (u32 v = mw; v > 1; v >>= 1) lw++;
                for (u32 v = mh; v > 1; v >>= 1) lh++;
                for (u32 y = 0; y < mh; y++)
                    for (u32 x = 0; x < mw; x++) {
                        const u8* p = source + off + (linear
                            ? (size_t)y * pitch + (size_t)x * 2
                            : (size_t)morton_index(x, y, lw, lh) * 2);
                        data[(size_t)y * mw + x] = (u16)(((u32)p[0] << 8) | p[1]);
                    }
                pixels[count] = data;
                levels[count++] = (tex_level_t){mw, mh, (const u8*)data, mw * 2, mh};
                off += pitch * mh;
                mw = mw > 1 ? mw >> 1 : 1;
                mh = mh > 1 ? mh >> 1 : 1;
            }
        }
        /* One line per X16 decode: Macross 30's lighting LUT is X16, and an
         * all-zero LUT removes specular and shading together. */
        if (!failed && count) {
            static u32 logged;
            if (logged++ < 24) {
                const u16* d0 = (const u16*)levels[0].data;
                u32 nz = 0; u16 mx = 0;
                for (u32 q = 0; q < levels[0].w * levels[0].h; q++) { if (d0[q]) nz++; if (d0[q] > mx) mx = d0[q]; }
                fprintf(stderr, "[x16] %u:0x%08X %ux%u linear=%d nonzero=%u/%u max=%u samples=%u,%u,%u,%u\n",
                        t->location, t->offset, w, h, linear, nz, levels[0].w * levels[0].h, mx,
                        d0[0], d0[w / 2], d0[(h / 2) * w + w / 2], d0[w * h - 1]);
            }
        }
        ID3D12Resource* result = failed ? NULL : t->cubemap
            ? create_texture_cube(DXGI_FORMAT_R16_UNORM, levels, n_mips)
            : create_texture_mipped(DXGI_FORMAT_R16_UNORM, levels, n_mips);
        for (u32 i = 0; i < count; i++) free(pixels[i]);
        return result;
    }
    if (base_fmt == TEX_FMT_DXT1 || base_fmt == TEX_FMT_DXT23 ||
        base_fmt == TEX_FMT_DXT45) {
        const DXGI_FORMAT dxgi = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                               : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                           : DXGI_FORMAT_BC3_UNORM;
        const u32 block = base_fmt == TEX_FMT_DXT1 ? 8 : 16;
        const u32 total = texture_source_span(t);
        const u8* src = guest_ptr(t->location, t->offset, total);
        if (!src) return NULL;
        if (t->cubemap) {
            n_mips = 1;
            for (u32 d = (w < h ? w : h) / 4; d > 1; d >>= 1)
                n_mips++;
            if (t->mipmaps && n_mips > t->mipmaps)
                n_mips = t->mipmaps;
            if (n_mips > 14) n_mips = 14;
            const u32 face_span = total / 6;
            tex_level_t cube_levels[6 * 14];
            u32 level = 0;
            for (u32 face = 0; face < 6; face++) {
                u32 mw = w, mh = h, off = 0;
                for (u32 mip = 0; mip < n_mips; mip++) {
                    const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                    cube_levels[level].w = (mw + 3) & ~3u;
                    cube_levels[level].h = (mh + 3) & ~3u;
                    cube_levels[level].data =
                        src + (size_t)face * face_span + off;
                    cube_levels[level].row_bytes = bw * block;
                    cube_levels[level].rows = bh;
                    level++;
                    off += bw * block * bh;
                    mw = mw > 1 ? mw >> 1 : 1;
                    mh = mh > 1 ? mh >> 1 : 1;
                }
            }
            return create_texture_cube(dxgi, cube_levels, n_mips);
        }
        tex_level_t levels[14];
        u32 mw = w, mh = h, off = 0, n = 0;
        for (u32 m = 0; m < n_mips; m++) {
            const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            levels[n].w = (mw + 3) & ~3u;
            levels[n].h = (mh + 3) & ~3u;
            levels[n].data = src + off;
            levels[n].row_bytes = bw * block;
            levels[n].rows = bh;
            n++;
            off += bw * block * bh;
            if (mw == 1 && mh == 1) break;
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        return create_texture_mipped(dxgi, levels, n);
    }

    u32 texel_size;
    switch (base_fmt) {
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    default: return NULL;
    }
    if (!linear && ((w & (w - 1)) || (h & (h - 1)))) return NULL;
    const u32 span = texture_source_span(t);
    const u8* src = guest_ptr(t->location, t->offset, span);
    if (!src) return NULL;

    if (t->cubemap) {
        u32 available_mips = 1;
        for (u32 mw = w, mh = h; mw > 1 || mh > 1; available_mips++) {
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        if (n_mips > available_mips) n_mips = available_mips;
        const u32 face_span = span / 6;
        u8* rgba[6 * 14] = {0};
        tex_level_t cube_levels[6 * 14];
        u32 level = 0;
        int oom = 0;
        for (u32 face = 0; face < 6 && !oom; face++) {
            u32 mw = w, mh = h, off = 0;
            for (u32 mip = 0; mip < n_mips; mip++) {
                const u32 pitch = (mip == 0 && linear && t->pitch)
                    ? t->pitch : mw * texel_size;
                rgba[level] = (u8*)malloc((size_t)mw * mh * 4);
                if (!rgba[level]) {
                    oom = 1;
                    break;
                }
                const u32 lw = log2_u32(mw), lh = log2_u32(mh);
                const u8* level_src =
                    src + (size_t)face * face_span + off;
                for (u32 y = 0; y < mh; y++)
                    for (u32 x = 0; x < mw; x++) {
                        const u8* pixel = linear
                            ? level_src + (size_t)y * pitch +
                                (size_t)x * texel_size
                            : level_src +
                                (size_t)morton_index(x, y, lw, lh) *
                                    texel_size;
                        decode_texel(
                            base_fmt, pixel, remap,
                            rgba[level] + ((size_t)y * mw + x) * 4);
                    }
                cube_levels[level].w = mw;
                cube_levels[level].h = mh;
                cube_levels[level].data = rgba[level];
                cube_levels[level].row_bytes = mw * 4;
                cube_levels[level].rows = mh;
                level++;
                off += pitch * mh;
                if (mw == 1 && mh == 1) break;
                mw = mw > 1 ? mw >> 1 : 1;
                mh = mh > 1 ? mh >> 1 : 1;
            }
        }
        ID3D12Resource* resource = (!oom && level == 6 * n_mips)
            ? create_texture_cube(
                DXGI_FORMAT_R8G8B8A8_UNORM, cube_levels, n_mips)
            : NULL;
        for (u32 i = 0; i < level; i++) free(rgba[i]);
        return resource;
    }

    u8* rgba[14] = {0};
    tex_level_t levels[14];
    u32 mw = w, mh = h, off = 0, n = 0;
    int oom = 0;
    for (u32 m = 0; m < n_mips; m++) {
        const u32 pitch = (m == 0 && linear && t->pitch)
            ? t->pitch : mw * texel_size;
        rgba[n] = (u8*)malloc((size_t)mw * mh * 4);
        if (!rgba[n]) { oom = 1; break; }
        const u32 lw = log2_u32(mw), lh = log2_u32(mh);
        const u8* level_src = src + off;
        /* LD_PLANE_DUMP=1: write the first few big single-channel (B8) planes
         * straight out as PGM, exactly as the guest laid them down. Settles
         * "is the video decode producing an image?" without guessing from the
         * composited frame. ponytail: one-shot debug, costs nothing when off. */
        { static int pd = -1; static int pdn = 0;
          if (pd < 0) { const char* e = getenv("LD_PLANE_DUMP"); pd = e ? atoi(e) : 0; }
          if (pd && (int)g_ld_frames >= pd && m == 0 && base_fmt == TEX_FMT_B8 && mw >= 320 && pdn < 6) {
              char fn[128];
              snprintf(fn, sizeof fn, "scratch/plane_%02d_%ux%u_p%u.pgm", pdn, mw, mh, pitch);
              FILE* pf = fopen(fn, "wb");
              if (pf) {
                  fprintf(pf, "P5\n%u %u\n255\n", mw, mh);
                  for (u32 yy = 0; yy < mh; yy++)
                      fwrite(level_src + (size_t)yy * pitch, 1, mw, pf);
                  fclose(pf);
                  fprintf(stderr, "[plane-dump] %s linear=%d\n", fn, linear);
              }
              pdn++;
          } }
        for (u32 y = 0; y < mh; y++)
            for (u32 x = 0; x < mw; x++) {
                const u8* pixel = linear
                    ? level_src + (size_t)y * pitch + (size_t)x * texel_size
                    : level_src + (size_t)morton_index(x, y, lw, lh) * texel_size;
                decode_texel(base_fmt, pixel, remap,
                             rgba[n] + ((size_t)y * mw + x) * 4);
            }
        /* LD_TEXRGBA_DUMP=1: the DECODED level-0 RGBA, with a non-black count
         * and the remap that produced it.
         *
         * Forcing the fragment shader to green fills every surface completely
         * (368,640 = 720x512 and 921,600 = 1280x720), so the draws reach the
         * GPU and write every pixel -- the only thing left is that the sampled
         * texture comes back black. The guest MEMORY has content (PS1 VRAM
         * carries the BIOS boot screen), so the question is whether decode
         * preserves it. remap is applied here, and a remap that zeroes channels
         * would produce exactly this. */
        { static int td = -1; static int tdn = 0; static u32 tdf = 0;
          static const char* td_dir;
          /* LD_TEXRGBA_DUMP=<hex fmt>: only that base format -- the 12-dump
           * cap otherwise fills with whatever decodes first. */
          if (td < 0) { const char* e = getenv("LD_TEXRGBA_DUMP");
                        td = e ? 1 : 0; tdf = e ? (u32)strtoul(e, 0, 16) : 0;
                        td_dir = getenv("LD_TEXRGBA_DUMP_DIR"); }
          const int td_this = td && !(tdf > 1 && base_fmt != tdf) && ld_texrgba_dump_offset(t);
          /* No size floor: the floor was picked for one title's full-screen
           * textures and silently skipped every UI atlas, which is the size
           * a 2D layer is made of. The dump is opt-in; dump what was asked. */
          if (td_this && m == 0 && tdn < 12 && mw && mh) {
              u64 nb = 0, na = 0;
              for (u32 q = 0; q < mw * mh; q++) {
                  const u8* px = rgba[n] + (size_t)q * 4;
                  if (px[0] || px[1] || px[2]) nb++;
                  if (px[3] > 0x1A) na++;
              }
              char fn[MAX_PATH * 2];
              if (td_dir && td_dir[0])
                  snprintf(fn, sizeof fn, "%s/tex_%02d_%u_%08X_%ux%u_f%02X.ppm",
                           td_dir, tdn, t->location, t->offset, mw, mh, base_fmt);
              else
                  snprintf(fn, sizeof fn, "scratch/tex_%02d_%ux%u_f%02X.ppm",
                           tdn, mw, mh, base_fmt);
              FILE* tf = fopen(fn, "wb");
              if (tf) {
                  fprintf(tf, "P6\n%u %u\n255\n", mw, mh);
                  for (u32 q = 0; q < mw * mh; q++)
                      fwrite(rgba[n] + (size_t)q * 4, 1, 3, tf);
                  fclose(tf);
              }
              /* Preserve alpha and exact uploaded RGBA; RGB PPM alone cannot
               * distinguish valid transparent atlas pixels from corruption. */
              if (td_dir && td_dir[0]) {
                  char sidecar[MAX_PATH * 2 + 32];
                  snprintf(sidecar, sizeof sidecar, "%s.rgba", fn);
                  tf = fopen(sidecar, "wb");
                  if (tf) { fwrite(rgba[n], 4, (size_t)mw * mh, tf); fclose(tf); }
                  const u32 source_bytes = span <= 16u * 1024u * 1024u ? span : 0;
                  snprintf(sidecar, sizeof sidecar, "%s.source.bin", fn);
                  tf = source_bytes ? fopen(sidecar, "wb") : NULL;
                  if (tf) { fwrite(src, 1, source_bytes, tf); fclose(tf); }
                  snprintf(sidecar, sizeof sidecar, "%s.json", fn);
                  tf = fopen(sidecar, "wb");
                  if (tf) {
                      fprintf(tf, "{\"frame\":%u,\"location\":%u,\"offset\":%u,\"format\":%u,"
                                  "\"base_format\":%u,\"width\":%u,\"height\":%u,\"pitch\":%u,"
                                  "\"linear\":%d,\"dimension\":%u,\"mips\":%u,\"remap\":%u,"
                                  "\"source_bytes\":%u,\"decoded_rgba_bytes\":%llu,"
                                  "\"source_provenance\":\"guest reread after decode; RGBA is actual upload payload\","
                                  "\"nonblack\":%llu,\"alpha_over_26\":%llu}\n",
                              g_ld_frames, t->location, t->offset, t->format, base_fmt, mw, mh,
                              pitch, linear, t->dimension, t->mipmaps, remap, source_bytes,
                              (unsigned long long)((u64)mw * mh * 4),
                              (unsigned long long)nb, (unsigned long long)na);
                      fclose(tf);
                  }
              }
              fprintf(stderr, "[tex-rgba] %s fmt=0x%02X remap=0x%04X linear=%d"
                              " pitch=%u nonblack=%llu/%u alpha>0x1A=%llu\n",
                      fn, base_fmt, remap, linear, pitch,
                      (unsigned long long)nb, mw * mh, (unsigned long long)na);
              tdn++;
          } }
        levels[n].w = mw;
        levels[n].h = mh;
        levels[n].data = rgba[n];
        levels[n].row_bytes = mw * 4;
        levels[n].rows = mh;
        n++;
        off += pitch * mh;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    /* A pitch-linear texture is read at level 0 only (see n_mips above), but a
     * sampler asking for mipmapped minification then reads that one level at
     * every distance. Macross 30's animated water normal map (512x512 LN,
     * mipmaps=10, LINEAR_MIPMAP_LINEAR) sparkled into per-pixel noise. Build
     * the chain the sampler expects by 2x2 box filtering level 0. */
    if (!oom && linear && n == 1 && t->mipmaps > 1 && w == h && !(w & (w - 1))) {
        u32 lw2 = w, lh2 = h;
        const u32 want = t->mipmaps < 14 ? t->mipmaps : 14;
        while (n < want && (lw2 > 1 || lh2 > 1)) {
            const u32 nw = lw2 > 1 ? lw2 >> 1 : 1, nh = lh2 > 1 ? lh2 >> 1 : 1;
            u8* dst = (u8*)malloc((size_t)nw * nh * 4);
            if (!dst) break;
            const u8* srcl = rgba[n - 1];
            for (u32 y = 0; y < nh; y++)
                for (u32 x = 0; x < nw; x++) {
                    const u32 x0 = x * 2, y0 = y * 2;
                    const u32 x1 = x0 + 1 < lw2 ? x0 + 1 : x0, y1 = y0 + 1 < lh2 ? y0 + 1 : y0;
                    for (u32 c = 0; c < 4; c++) {
                        const u32 sum = srcl[((size_t)y0 * lw2 + x0) * 4 + c] + srcl[((size_t)y0 * lw2 + x1) * 4 + c] +
                                        srcl[((size_t)y1 * lw2 + x0) * 4 + c] + srcl[((size_t)y1 * lw2 + x1) * 4 + c];
                        dst[((size_t)y * nw + x) * 4 + c] = (u8)((sum + 2) / 4);
                    }
                }
            rgba[n] = dst;
            levels[n].w = nw; levels[n].h = nh; levels[n].data = dst;
            levels[n].row_bytes = nw * 4; levels[n].rows = nh;
            n++; lw2 = nw; lh2 = nh;
        }
    }
    ID3D12Resource* resource = (!oom && n)
        ? create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, levels, n) : NULL;
    for (u32 m = 0; m < n; m++) free(rgba[m]);
    return resource;
}

static ID3D12Resource* decode_guest_texture_profiled(
    const rsx_dsp_texture* texture, u32 remap)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
    g_ld_profile.total.texture_decode_calls++;
#endif
    ID3D12Resource* result = decode_guest_texture(texture, remap);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.texture_decode_qpc +=
        (u64)(ld_profile_qpc() - begin);
#endif
    return result;
}

static void write_texture_srv(u32 index, const texcache_t* entry)
{
    const u32 base_fmt = entry->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    if (base_fmt == TEX_FMT_X16) {
        /* RSX base ARGB = (X,1,X,1), then apply the guest remap. Keep the
         * source as R16_UNORM so lookup-table precision survives filtering. */
        static const u32 source[4] = {0, 5, 0, 5};
        static const u32 out2comp[4] = {1, 2, 3, 0};
        u32 mapping = 1u << 12;
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping |= (op == 0 ? 4 : op == 1 ? 5 : source[sel]) << (out * 3);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = DXGI_FORMAT_R16_UNORM;
        desc.ViewDimension = entry->cubemap ? D3D12_SRV_DIMENSION_TEXTURECUBE
                                         : D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = mapping;
        if (entry->cubemap) desc.TextureCube.MipLevels = (UINT)-1;
        else desc.Texture2D.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
        return;
    }
    const int compressed = base_fmt == TEX_FMT_DXT1 ||
                           base_fmt == TEX_FMT_DXT23 ||
                           base_fmt == TEX_FMT_DXT45;
    if (entry->cubemap) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = !compressed ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.Shader4ComponentMapping =
            !compressed || entry->remap == 0xAAE4
            ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            : mapping[0] | (mapping[1] << 3) | (mapping[2] << 6) |
              (mapping[3] << 9) | (1u << 12);
        desc.TextureCube.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else if (compressed && entry->remap != 0xAAE4) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = mapping[0] | (mapping[1] << 3) |
                                       (mapping[2] << 6) | (mapping[3] << 9) |
                                       (1u << 12);
        desc.Texture2D.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else {
        srv_write(SRV_TEXTURE_BASE + index, entry->tex);
    }
}

/* Decode a guest texture descriptor into a cached SRV slot.  Unlike the old
 * descriptor-only cache, re-hash the source once per frame and refresh the
 * D3D resource when the game rewrites the same guest address. */
static u32 texture_srv_slot(const rsx_dsp_texture* t)
{
#if defined(YZ_PERF_PROFILE)
    const int profile_lookup=ld_texture_profile_active();
    if(profile_lookup)g_ld_profile.total.texture_cache_lookups++;
#endif
    const u32 remap = t->remap & 0xFFFF;
    static int refresh_enabled = -1;
    if (refresh_enabled < 0)
        refresh_enabled = getenv("RSX_NO_TEX_REFRESH") ? 0 : 1;

    for (u32 i = 0; i < g.n_textures; i++) {
        texcache_t* entry = &g.textures[i];
        if (entry->location != t->location || entry->offset != t->offset ||
            entry->format != t->format || entry->width != t->width ||
            entry->height != t->height || entry->pitch != t->pitch ||
            entry->remap != remap || entry->cubemap != t->cubemap ||
            entry->mipmaps != t->mipmaps || entry->dimension != t->dimension)
            continue;
#if defined(YZ_PERF_PROFILE)
        if(profile_lookup)g_ld_profile.total.texture_cache_probes+=i+1u;
#endif
        if (refresh_enabled &&
            entry->last_hash_frame != g_ld_frames) {
            /* Unwritten since the last hash: the content is what was hashed. */
            if (entry->content_readable) {
                const u32 wspan = texture_source_span(t);
                const u8* wsrc = wspan ? guest_ptr(t->location, t->offset, wspan) : NULL;
                if (wsrc && !entry->ww_bad && !ld_ww_dirty_since(wsrc, wspan, entry->ww_epoch)) {
                    /* Some writes never reach the watch (seen: the movie's
                     * chroma planes in RSX local memory). Trust the verdict
                     * only after it was confirmed 4 times, re-confirm every
                     * 16th frame, and hash this texture every frame for good
                     * once a verdict proves wrong. */
                    const int confirm = entry->ww_ok < 4 || ((g_ld_frames + i) & 15u) == 0;
                    if (!confirm) {
                        entry->last_hash_frame = g_ld_frames;
                        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
                    }
                    int rd = 0; const u64 h = texture_content_hash(t, &rd);
                    if (rd && h == entry->content_hash) {
                        if (entry->ww_ok < 255) entry->ww_ok++;
                        entry->ww_epoch = g_ww_epoch;
                        entry->last_hash_frame = g_ld_frames;
                        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
                    }
                    entry->ww_bad = 1;   /* fall through: hash and refresh */
                }
            }
            int readable = 0;
            const u64 hash = texture_content_hash(t, &readable);
            /* LD_TEX_RACE=1: hash the SAME source twice, back to back. The two
             * reads are microseconds apart, so they can only disagree if a
             * guest thread is writing this texture right now -- i.e. we are
             * decoding a half-written image. Pure observation; it changes
             * nothing. Counts per source so one torn texture is attributable. */
            { static int race = -1;
              if (race < 0) race = getenv("LD_TEX_RACE") ? 1 : 0;
              if (race && readable) {
                  int r2 = 0;
                  const u64 again = texture_content_hash(t, &r2);
                  static unsigned long long checks = 0, torn = 0;
                  checks++;
                  if (r2 && again != hash) torn++;
                  if ((checks % 256) == 0)
                      fprintf(stderr,
                              "[tex-race] src=%u:0x%08X %ux%u  torn %llu/%llu reads"
                              " (%.1f%%)\n",
                              t->location, t->offset, t->width, t->height,
                              (unsigned long long)torn,
                              (unsigned long long)checks,
                              100.0 * (double)torn / (double)checks);
              } }
            /* LD_FBDBG=1: account for the PS1 framebuffer specifically
             * (location 1, offset 0x400000, the 1024x512 fmt 0xE2 texture the
             * PS3 side composites). Its guest memory demonstrably fills up --
             * PS1 VRAM goes from 0 to 75,527 non-zero words -- yet
             * [tex-refresh] fires only 6 times in 90 seconds. Three different
             * things produce that, and they need different fixes: the texture
             * is rarely BOUND, the span is unreadable so `readable` is 0, or
             * the hash genuinely does not change. Count all three. */
            { static int fbd = -1;
              if (fbd < 0) fbd = getenv("LD_FBDBG") ? 1 : 0;
              if (fbd && t->location == 1u && t->offset == 0x400000u) {
                  static unsigned long long checks = 0, unread = 0, changed = 0;
                  checks++;
                  if (!readable) unread++;
                  if (readable && hash != entry->content_hash) changed++;
                  if ((checks % 64) == 0 || checks < 4)
                      fprintf(stderr, "[fbdbg] ps1fb checks=%llu unreadable=%llu"
                                      " changed=%llu span=%u %ux%u pitch=%u\n",
                              checks, unread, changed,
                              texture_source_span(t), t->width, t->height,
                              t->pitch);
              } }
            entry->last_hash_frame = g_ld_frames;
            entry->ww_epoch = g_ww_epoch;
            if (readable && (!entry->content_readable || hash != entry->content_hash)) {
                /* Remember failed attempts as well: retry only after source
                 * bytes change or previously unreadable data becomes mapped. */
                entry->content_readable = 1;
                entry->content_hash = hash;
                ID3D12Resource* replacement =
                    decode_guest_texture_profiled(t, remap);
                if (replacement) {
                    ID3D12Resource* old = entry->tex;
                    entry->tex = replacement;
                    entry->content_hash = hash;
                    write_texture_srv(i, entry);
                    if (old) retire_texture(old);
                    static u32 refresh_count = 0;
                    refresh_count++;
                    if (refresh_count <= 64 || (refresh_count & 255) == 0)
                        fprintf(stderr,
                                "[tex-refresh] n=%u frame=%u unit-src=%u:0x%08X "
                                "fmt=0x%02X %ux%u\n",
                                refresh_count, g_ld_frames, t->location, t->offset,
                                t->format, t->width, t->height);
                }
            }
        }
        entry->last_use_serial = ++g_ld_texture_use_serial;
        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
    }

#if defined(YZ_PERF_PROFILE)
    if(profile_lookup)g_ld_profile.total.texture_cache_probes+=g.n_textures;
#endif
    u32 index;
    ID3D12Resource* evicted = NULL;
    const int cache_was_full = g.n_textures >= MAX_TEXTURES;
    if (!cache_was_full) {
        index = g.n_textures++;
    } else {
        /*
         * A full boot plus a010 binds far more than 128 distinct guest
         * texture descriptors. Returning SRV_WHITE after the fixed cache
         * filled made the recovered orphanage render as flat green/black
         * geometry. Reuse the least-recently-used descriptor while keeping
         * its resource alive until the open command list reaches a fence.
         * The shader-visible draw table already contains a descriptor copy,
         * so rewriting this CPU cache slot cannot alter earlier draws.
         */
        g_ld_texture_cache_full++;
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial <
                g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].tex;
        g_ld_texture_cache_evictions++;
        if (g_ld_texture_cache_full <= 16 ||
            (g_ld_texture_cache_full & (g_ld_texture_cache_full - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] FULL cap=%u lru-evictions=%llu\n",
                    MAX_TEXTURES,
                    (unsigned long long)g_ld_texture_cache_evictions);
    }

    texcache_t replacement;
    memset(&replacement, 0, sizeof(replacement));
    replacement.location = t->location;
    replacement.offset = t->offset;
    replacement.format = t->format;
    replacement.width = t->width;
    replacement.height = t->height;
    replacement.pitch = t->pitch;
    replacement.remap = remap;
    replacement.cubemap = t->cubemap;
    replacement.mipmaps = t->mipmaps;
    replacement.dimension = t->dimension;
    replacement.last_hash_frame = g_ld_frames;
    replacement.ww_epoch = g_ww_epoch;
    replacement.last_use_serial = ++g_ld_texture_use_serial;
    {
        int readable = 0;
        replacement.content_hash = texture_content_hash(t, &readable);
        replacement.content_readable = readable;
    }
    replacement.tex = decode_guest_texture_profiled(t, remap);
    if (replacement.tex) {
        texcache_t* entry = &g.textures[index];
        if (evicted)
            retire_texture(evicted);
        *entry = replacement;
        write_texture_srv(index, entry);
    } else {
        if (!cache_was_full)
            g.textures[index] = replacement;
        g_ld_texture_decode_fail++;
        if (g_ld_texture_decode_fail <= 16 ||
            (g_ld_texture_decode_fail & (g_ld_texture_decode_fail - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] decode failed n=%llu src=%u:0x%08X "
                    "fmt=0x%02X %ux%u pitch=%u dim=%u cube=%u mips=%u "
                    "remap=0x%04X span=%u readable=%d\n",
                    (unsigned long long)g_ld_texture_decode_fail,
                    t->location, t->offset, t->format, t->width, t->height,
                    t->pitch, t->dimension, t->cubemap, t->mipmaps,
                    remap, texture_source_span(t), replacement.content_readable);
    }
    return replacement.tex ? SRV_TEXTURE_BASE + index : SRV_WHITE;
}

typedef struct {
    DXGI_FORMAT dxgi;
    u32 bytes_per_texel;
    u32 component_bytes;
} vertex_texture_format_t;

static int vertex_texture_format(
    const rsx_dsp_vertex_texture* vt, vertex_texture_format_t* out)
{
    if (!vt || vt->dimension != 2 || vt->cubemap)
        return 0;
    vertex_texture_format_t format = {DXGI_FORMAT_UNKNOWN, 0, 0};
    switch (vt->format & TEX_FMT_BASE_MASK) {
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        format.bytes_per_texel = 8;
        format.component_bytes = 2;
        break;
    case RSX_TEX_FMT_W32Z32Y32X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT;
        format.bytes_per_texel = 16;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 2;
        break;
    default:
        return 0;
    }
    if (out)
        *out = format;
    return 1;
}

static int vertex_texture_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RSX_VTEX_TRACE") ? 1 : 0;
    return enabled;
}

static u32 vertex_texture_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (!vt.enabled) continue;
        g_ld_vtex_enabled++;
        if (vertex_texture_format(&vt, NULL)) {
            mask |= 1u << u;
        } else {
            g_ld_vtex_unsupported++;
            static u32 warned = 0;
            if (PS3_LOG_FIRST(warned, 16) && vertex_texture_trace_enabled())
                fprintf(stderr,
                        "[vtex] enabled but unsupported unit=%u off=0x%08X "
                        "fmt=0x%02X dim=%u cube=%u %ux%u pitch=%u ctl=0x%08X\n",
                        u, vt.offset, vt.format, vt.dimension, vt.cubemap,
                        vt.width, vt.height, vt.pitch, vt.control0);
        }
    }
    return mask;
}

static u64 vertex_texture_hash(const rsx_dsp_vertex_texture* vt,
                               const u8** out_src, u32* out_pitch)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return 0;
    const u64 row_bytes64 = (u64)vt->width * format.bytes_per_texel;
    const u64 pitch64 = vt->pitch ? vt->pitch : row_bytes64;
    const u64 span64 = pitch64 * vt->height;
    if (row_bytes64 > UINT32_MAX || pitch64 < row_bytes64 ||
        pitch64 > UINT32_MAX || span64 > UINT32_MAX)
        return 0;
    const u32 pitch = (u32)pitch64;
    const u32 span = (u32)span64;
    const u8* src = span ? guest_ptr(vt->location, vt->offset, span) : NULL;
    if (out_src) *out_src = src;
    if (out_pitch) *out_pitch = pitch;
    return src ? fnv1a(src, span, 1469598103934665603ull) : 0;
}

static ID3D12Resource* decode_vertex_texture(
    const rsx_dsp_vertex_texture* vt, u64* out_hash)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return NULL;
    const u8* src = NULL;
    u32 pitch = 0;
    const u64 hash = vertex_texture_hash(vt, &src, &pitch);
    if (!src) return NULL;
    const u32 row_bytes = vt->width * format.bytes_per_texel;
    if (pitch < row_bytes)
        return NULL;
    u8* staging = (u8*)malloc((size_t)row_bytes * vt->height);
    if (!staging) return NULL;
    for (u32 y = 0; y < vt->height; y++) {
        const u8* srow = src + (size_t)y * pitch;
        u8* drow = staging + (size_t)y * row_bytes;
        const u32 components = row_bytes / format.component_bytes;
        for (u32 c = 0; c < components; c++) {
            const u8* source =
                srow + (size_t)c * format.component_bytes;
            u8* destination =
                drow + (size_t)c * format.component_bytes;
            for (u32 byte = 0; byte < format.component_bytes; byte++)
                destination[byte] =
                    source[format.component_bytes - 1u - byte];
        }
    }
    tex_level_t level = {
        vt->width, vt->height, staging, row_bytes, vt->height
    };
    ID3D12Resource* tex = create_texture_mipped(format.dxgi, &level, 1);
    free(staging);
    if (tex) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        if (out_hash) *out_hash = hash;
    }
    return tex;
}

static void write_vertex_texture_srv(u32 index, ID3D12Resource* tex)
{
    rsx_dsp_vertex_texture descriptor = {0};
    vertex_texture_format_t format;
    if (index >= g.n_vtex)
        return;
    descriptor.format = g.vtex[index].format;
    descriptor.dimension = 2;
    if (!vertex_texture_format(&descriptor, &format))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = format.dxgi;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(SRV_VTEX_BASE + index));
}

static u32 vertex_texture_srv_slot(const rsx_dsp_vertex_texture* vt)
{
    for (u32 i = 0; i < g.n_vtex; i++) {
        vtexcache_t* e = &g.vtex[i];
        if (e->location != vt->location || e->offset != vt->offset ||
            e->format != vt->format || e->width != vt->width ||
            e->height != vt->height || e->pitch != vt->pitch)
            continue;
        if (e->tex && e->last_hash_frame != g_ld_frames) {
            const u64 hash = vertex_texture_hash(vt, NULL, NULL);
            e->last_hash_frame = g_ld_frames;
            if (hash && hash != e->content_hash) {
                u64 replacement_hash = 0;
                ID3D12Resource* replacement =
                    decode_vertex_texture(vt, &replacement_hash);
                if (replacement) {
                    ID3D12Resource* old = e->tex;
                    e->tex = replacement;
                    e->content_hash = replacement_hash;
                    write_vertex_texture_srv(i, e->tex);
                    retire_texture(old);
                    g_ld_vtex_refreshes++;
                }
            }
        }
        return e->tex ? SRV_VTEX_BASE + i : SRV_WHITE;
    }
    if (g.n_vtex >= MAX_VTEX) return SRV_WHITE;
    const u32 index = g.n_vtex++;
    vtexcache_t* e = &g.vtex[index];
    memset(e, 0, sizeof(*e));
    e->location = vt->location;
    e->offset = vt->offset;
    e->format = vt->format;
    e->width = vt->width;
    e->height = vt->height;
    e->pitch = vt->pitch;
    e->last_hash_frame = g_ld_frames;
    e->tex = decode_vertex_texture(vt, &e->content_hash);
    if (e->tex) {
        write_vertex_texture_srv(index, e->tex);
        g_ld_vtex_uploads++;
        if (vertex_texture_trace_enabled())
            fprintf(stderr,
                    "[vtex] upload unit-data %u:0x%08X fmt=0x%02X %ux%u pitch=%u\n",
                    vt->location, vt->offset, vt->format,
                    vt->width, vt->height, vt->pitch);
    }
    return e->tex ? SRV_VTEX_BASE + index : SRV_WHITE;
}

/* ---------------------------------------------------------------------------
 * surfaces (color RTs keyed by location/offset), rendered into then presented
 * -----------------------------------------------------------------------*/
/* fp constants are stored as raw bits; the shader reads them as floats. */
static float ld_c2f(u32 bits) { float f; memcpy(&f, &bits, sizeof f); return f; }

typedef struct {
    u32 location, offset, w, h;
    DXGI_FORMAT fmt;
} ld_surface_demand_key;
static ld_surface_demand_key g_ld_surface_demand[MAX_SURFACE_DEMAND_KEYS];
static u32 g_ld_surface_demand_count;
static u64 g_ld_surface_demand_untracked, g_ld_surface_capacity_rejects;

/* Census is separate from allocations: failed/table-full keys still count.
 * Beyond the bounded census we report unknown requests, never a fake exact
 * unique count. One line per distinct key survives a bounded process kill. */
static void ld_surface_note_demand(u32 location, u32 offset, u32 w, u32 h,
                                   DXGI_FORMAT fmt)
{
    for (u32 i = 0; i < g_ld_surface_demand_count; ++i) {
        const ld_surface_demand_key* k = &g_ld_surface_demand[i];
        if (k->location == location && k->offset == offset &&
            k->w == w && k->h == h && k->fmt == fmt) return;
    }
    if (g_ld_surface_demand_count == MAX_SURFACE_DEMAND_KEYS) {
        if (++g_ld_surface_demand_untracked == 1)
            fprintf(stderr, "[surface-demand] census saturated at %u; unique count is a lower bound\n",
                    MAX_SURFACE_DEMAND_KEYS);
        return;
    }
    ld_surface_demand_key* k =
        &g_ld_surface_demand[g_ld_surface_demand_count++];
    k->location = location; k->offset = offset;
    k->w = w; k->h = h; k->fmt = fmt;
    fprintf(stderr,
            "[surface-demand] unique=%u allocated=%u capacity=%u key=%u:0x%08X %ux%u fmt=%u\n",
            g_ld_surface_demand_count, g.n_surfaces, MAX_SURFACES,
            location, offset, w, h, (unsigned)fmt);
}

static u32 surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                       DXGI_FORMAT want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    /* Key on the DIMENSIONS as well as location/offset.
     *
     * Keying on location/offset alone meant a redeclaration of the same offset
     * at a different size STOLE the slot and recreated the D3D12 resource,
     * throwing the old contents away. ps1_netemu renders two passes at two
     * sizes -- the PS1 composite at 720x512 and the PS3 output at 1280x720 --
     * and it alternates between them, so that path fired 9,573 times in a
     * 90-second run:
     *
     *     [surfsz] live surface 0x0 redeclared 720x512 -> 1280x720 (content dropped)
     *     [surfsz] live surface 0x1401C00 redeclared 1280x720 -> 720x512 (content dropped)
     *
     * Every frame of both passes was being destroyed by the other. Giving each
     * (offset, size) its own surface lets both keep their contents.
     *
     * ponytail: two surfaces over the same guest memory do not alias, so a pass
     * that renders at one size and samples at the other will not see the
     * other's pixels. That is strictly better than the old behaviour, which saw
     * NOBODY's pixels, and it is the smaller change. If a title ever needs true
     * aliasing, the fix is one surface plus a copy on redeclare -- not going
     * back to destroying content. */
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset &&
            g.surfaces[i].w == want_w && g.surfaces[i].h == want_h &&
            g.surfaces[i].fmt == want_fmt)
            return i;
    /* Never destroy a usable render target because a malformed live command
     * briefly decoded a guest pointer as clip dimensions.  The known-good
     * orphanage stream never exceeds 1280x1024; D3D12 rejects the observed
     * 1280x16452 declaration and the old path then dereferenced a null
     * resource.  Preserve the prior surface so diagnostics can capture the
     * actual world pass while the bad command link is isolated. */
    if (want_w > 8192u || want_h > 8192u) {
        static u32 invalid_surface_logs = 0;
        if (PS3_LOG_FIRST(invalid_surface_logs, 32u))
            fprintf(stderr,
                    "[surface-guard] rejected implausible surface 0x%X "
                    "%ux%u; preserving slot=%s\n",
                    offset, want_w, want_h,
                    slot < MAX_SURFACES ? "existing" : "none");
        return slot < MAX_SURFACES ? slot : LD_INVALID_SURFACE;
    }
    ld_surface_note_demand(location, offset, want_w, want_h, want_fmt);
    if (g.n_surfaces >= MAX_SURFACES) {
        g_ld_surface_capacity_rejects++;
        /* Out of slots is now the only way a surface can be lost, so say so
         * once rather than silently returning an invalid handle. */
        static u32 full_logs = 0;
        if (PS3_LOG_FIRST(full_logs, 8))
            fprintf(stderr, "[surfsz] surface table full (%u); dropping "
                            "%u:0x%X %ux%u\n",
                    (unsigned)MAX_SURFACES, location, offset, want_w, want_h);
        return LD_INVALID_SURFACE;
    }
    slot = g.n_surfaces;
    { static u32 new_logs = 0;
      if (PS3_LOG_FIRST(new_logs, MAX_SURFACES))
          fprintf(stderr, "[surfsz] new live surface %u:0x%X %ux%u (slot %u)\n",
                  location, offset, want_w, want_h, slot); }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ld_rs_config();
    /* Render scale: only canvas-size targets in local memory (scene, its
     * copies and the display buffers); everything the guest can read back
     * or that is a tiny LUT/luminance target stays at the guest size. */
    const u32 want_scaled = (u32)ld_rs_wants(location, want_w, want_h);
    const u32 host_w = want_scaled ? ld_rs_up(want_w) : want_w;
    const u32 host_h = want_scaled ? ld_rs_up(want_h) : want_h;
    rd.Width = host_w; rd.Height = host_h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = want_fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = rd.Format;
    surface_t* s = &g.surfaces[slot];
    ID3D12Resource* replacement = NULL;
    const HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&replacement);
    if (FAILED(create_hr)) {
        static u32 surface_fail_logs = 0;
        if (PS3_LOG_FIRST(surface_fail_logs, 32)) {
            const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
            fprintf(stderr,
                    "[surface-fail] color %u:0x%X %ux%u hr=0x%08lX "
                    "removed=0x%08lX slot=%u count=%u\n",
                    location, offset, want_w, want_h,
                    (unsigned long)create_hr, (unsigned long)removed,
                    slot, g.n_surfaces);
        }
        /* A resize failure must not poison an existing usable slot. */
        return slot < g.n_surfaces && s->tex ? slot : LD_INVALID_SURFACE;
    }
    /* Draws already recorded in the open command list can still reference
     * the old render target.  Release it only after ld_flush fences that list,
     * just like dynamic texture and zeta replacements. */
    if (s->tex)
        retire_texture(s->tex);
    if (s->fb_tex) {                 /* sized for the old resource */
        retire_texture(s->fb_tex);
        s->fb_tex = NULL;
    }
    for (u32 j = 0; j < LD_SWIN_PER; j++)   /* windows are sized and typed for the old resource */
        if (s->swin[j].tex) { retire_texture(s->swin[j].tex); s->swin[j].tex = NULL; }
    s->tex = replacement;
    s->location = location; s->offset = offset; s->w = want_w; s->h = want_h;
    ld_cube_neg_forget(location, offset, slot);
    s->hw = host_w; s->hh = host_h; s->scaled = want_scaled;
    s->fmt = want_fmt;
#if !defined(YZ_PERF_CLEAN)
    s->resource_serial = ++g_ld_surface_resource_serial;
#endif
    ld_surface_note_write(slot, LD_SURFACE_WRITE_CREATE);
    /* RTVs for surfaces live above the swap-chain backbuffer RTVs */
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
        NULL, rtv_handle(LD_SWAP_BUFFERS + slot));
    srv_write_surface(SRV_SURFACE_BASE + slot, s->tex, s->fmt);
    if (slot == g.n_surfaces) g.n_surfaces++;
    return slot;
}

/* A texture smaller than the colour surface it aliases reads the surface's memory texel for
 * sample, i.e. the top-left tex_w x tex_h of the surface, not the whole surface scaled into it. a title's scene
 * renders into 0x01FB0000 with SURFACE_FORMAT 0xA0B5148 (A8R8G8B8, antialias 5 = 2x2 samples), raw clip 2560x1440,
 * pitch 10240, viewport 1280x720. rsx_dsp_get_surface multiplies the clip by the sample grid (PR #281), so
 * the host surface is 5120x2880, but the game draws only its viewport, 2560x1440 samples, which is all the guest
 * memory holds (pitch 10240 = 2560 x 4 bytes; tile 7 is 0xE10000 = 10240 x 1440 bytes). The downsample then binds
 * 0x01FB0000 as a 2560x1440 A8R8G8B8 linear texture; bound to the whole 5120x2880 surface, its normalised
 * coordinates took the scene from the top-left quarter only, and every later pass (640x360 bloom chain, final
 * composite) showed the scene in a corner. On the RSX texel (x,y) of a linear texture is the byte at
 * y*pitch + x*bpp, which for an equal pitch and texel size is sample (x,y) of the surface. A 2D SRV cannot select a
 * sub-rectangle, so, the top-left w x h of the surface is copied into a w x h
 * texture of the surface's format (kept per surface and size, recopied only after the surface was written), and
 * clamp, filtering and texel size then see the texture the guest declared.
 * Returns the SRV slot, or -1 to bind the whole surface as before: equal size, a texture larger than the surface,
 * not a linear 2D texture, unknown or different texel size, or a texture pitch that differs from the surface's
 * guest pitch (the texture then does not walk the surface's rows; the old behaviour is kept, unproven either way).
 * LD_SWIN=0 = old behaviour. LD_ALIAS_DBG=1 logs each sub-rect mapping ([alias-subrect]). */
static int ld_swin_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_SWIN"); on = (e && e[0] == '0') ? 0 : 1;
                  fprintf(stderr, "[swin] %s (LD_SWIN)\n", on ? "on" : "off"); }
    return on;
}
/* Guest bytes per texel of a texture format (modifier bits ignored); 0 = compressed or not listed. */
static u32 ld_tex_texel_bytes(u32 format)
{
    switch (format & TEX_FMT_BASE_MASK) {
    case RSX_TEX_FMT_B8: return 1u;
    case RSX_TEX_FMT_A1R5G5B5: case RSX_TEX_FMT_A4R4G4B4: case RSX_TEX_FMT_R5G6B5: case RSX_TEX_FMT_G8B8: return 2u;
    case RSX_TEX_FMT_A8R8G8B8: case RSX_TEX_FMT_DEPTH24_D8: case RSX_TEX_FMT_X32_FLOAT: case RSX_TEX_FMT_Y16X16_FLOAT:
        return 4u;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: return 8u;
    case RSX_TEX_FMT_W32Z32Y32X32_FLOAT: return 16u;
    default: return 0u;
    }
}
static u64 g_ld_swin_copies;
/* A cube sampled from GPU-owned targets is six distinct guest surfaces, not
 * a 2D alias of face zero. Faces retain the game's ordering/orientation.
 * Return -1 only when no face is GPU-owned (normal guest-memory upload);
 * -2 means an owned but incomplete/unsupported cube and must reject the draw. */
static int owned_cube_srv(const rsx_dsp_texture* t, u64 pinned, u32* out_slot)
{
    const u32 levels = t->mipmaps ? t->mipmaps : 1;
    const u32 stride = rsx_texture_cube_face_stride(t->format, t->width,
                                                   t->height, levels, t->pitch);
    u32 face_slots[6] = {0}, generations[6] = {0};
    u32 found = 0;
    int any_owned = 0;
    /* Almost every cube is a plain guest texture: remember that no surface
     * sits at any of its face addresses until a surface address changes
     * (the 6 x surfaces scan ran for every cube unit of every draw). */
    const u32 nk = ((t->offset >> 7) ^ (t->offset >> 17) ^ stride ^ t->location) & (LD_CUBE_NEG_SLOTS - 1);
    const int cached = g_ld_cube_neg[nk].valid && g_ld_cube_neg[nk].location == t->location &&
        g_ld_cube_neg[nk].offset == t->offset && g_ld_cube_neg[nk].stride == stride;
    int cacheable = !cached;
    u8 cn[6] = {0}; u16 cc[6][LD_CUBE_CANDS];
    if (cached) g_ld_cube_neg_hits++; else g_ld_cube_scans++;
    for (u32 face = 0; face < 6; face++) {
        const u64 address = (u64)t->offset + (u64)face * stride;
        int best = -1;
        const u32 n_iter = cached ? g_ld_cube_neg[nk].n[face] : g.n_surfaces;
        for (u32 j = 0; j < n_iter; j++) {
            const u32 i = cached ? g_ld_cube_neg[nk].cand[face][j] : j;
            const surface_t* s = &g.surfaces[i];
            if (address > 0xFFFFFFFFull || s->location != t->location || s->offset != (u32)address)
                continue;
            any_owned = 1;
            if (cacheable) {
                if (cn[face] < LD_CUBE_CANDS) cc[face][cn[face]++] = (u16)i; else cacheable = 0;
            }
            if (!s->tex || s->last_write_generation == s->last_create_generation)
                continue;
            if (best < 0 || s->last_write_generation > g.surfaces[best].last_write_generation)
                best = (int)i;
        }
        if (best >= 0) {
            const surface_t* s = &g.surfaces[best];
            /* A newer incompatible view owns these bytes. Never resurrect an
             * older matching alias merely because its dimensions fit. */
            const int linear = (t->format & TEX_FMT_LINEAR) != 0;
            const int compatible_layout = linear
                ? s->raster_type == 1 && s->pitch == t->width * 4 &&
                  (!t->pitch || t->pitch == s->pitch)
                : s->raster_type == 2;
            if (s->fmt != DXGI_FORMAT_R8G8B8A8_UNORM || s->w != t->width ||
                s->h != t->height || !compatible_layout) continue;
            face_slots[face] = (u32)best;
            generations[face] = g.surfaces[best].last_write_generation;
            found++;
        }
    }
    if (any_owned) g_ld_cube_owned++;
    if (cacheable) {
        g_ld_cube_neg[nk].location = t->location; g_ld_cube_neg[nk].offset = t->offset;
        g_ld_cube_neg[nk].stride = stride; g_ld_cube_neg[nk].valid = 1;
        memcpy(g_ld_cube_neg[nk].n, cn, sizeof cn); memcpy(g_ld_cube_neg[nk].cand, cc, sizeof cc);
    }
    if (!any_owned) return -1;
    if (!stride || levels != 1 || t->dimension != 2 || !t->width || t->width != t->height ||
        ((t->format & TEX_FMT_LINEAR) && t->pitch && t->pitch != t->width * 4) ||
        (t->format & 0x9Fu) != TEX_FMT_A8R8G8B8 || found != 6) {
        static u32 rejected;
        if (rejected++ < 16)
            fprintf(stderr, "[owned-cube] rejected %u:0x%08X fmt=%02X %ux%u levels=%u stride=%u faces=%u/6\n",
                    t->location, t->offset, t->format, t->width, t->height, levels, stride, found);
        return -2;
    }
    int slot = -1;
    for (u32 i = 0; i < MAX_OWNED_CUBES; i++) {
        const owned_cube_t* c = &g.owned_cubes[i];
        if (c->tex && c->location == t->location && c->offset == t->offset &&
            c->w == t->width && c->h == t->height && c->format == t->format && c->remap == (t->remap & 0xFFFFu)) {
            slot = (int)i; break;
        }
    }
    if (slot < 0) {
        for (u32 i = 0; i < MAX_OWNED_CUBES; i++) {
            if (pinned & (1ull << i)) continue;
            if (!g.owned_cubes[i].tex) { slot = (int)i; break; }
            if (slot < 0 || g.owned_cubes[i].last_use < g.owned_cubes[slot].last_use) slot = (int)i;
        }
        if (slot < 0) return -2;
        owned_cube_t* c = &g.owned_cubes[slot];
        /* Retirement can flush: do it before recording barriers or keeping any
         * shader-visible table handles for this draw. Earlier draws copied
         * their own descriptors and retain the retired resource to its fence. */
        if (c->tex) {
            if (g.n_retired_textures >= MAX_RETIRED_TEXTURES) return -2;
            retire_texture(c->tex);
        }
        memset(c, 0, sizeof *c);
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = t->width; rd.Height = t->height; rd.DepthOrArraySize = 6;
        rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&c->tex))) return -2;
        c->location = t->location; c->offset = t->offset; c->w = t->width; c->h = t->height;
        c->format = t->format; c->remap = t->remap & 0xFFFFu;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = rd.Format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.Shader4ComponentMapping = ld_depth_remap_mapping(c->remap, 1);
        sd.TextureCube.MipLevels = 1;
        g.dev->lpVtbl->CreateShaderResourceView(g.dev, c->tex, &sd, srv_cpu(SRV_OWNED_CUBE_BASE + slot));
    }
    owned_cube_t* c = &g.owned_cubes[slot];
    if (memcmp(c->face_slot, face_slots, sizeof face_slots) ||
        memcmp(c->face_generation, generations, sizeof generations)) {
        D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = c->tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        for (u32 face = 0; face < 6; face++) {
            ID3D12Resource* source = g.surfaces[face_slots[face]].tex;
            b.Transition.pResource = source;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
            D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
            src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = c->tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = face;
            g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        }
        b.Transition.pResource = c->tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
        memcpy(c->face_slot, face_slots, sizeof face_slots);
        memcpy(c->face_generation, generations, sizeof generations);
        static u32 copied;
        if (copied++ < 16)
            fprintf(stderr, "[owned-cube] copied %u:0x%08X stride=0x%X size=%u faces=%u,%u,%u,%u,%u,%u slot=%d\n",
                    t->location, t->offset, stride, t->width, face_slots[0], face_slots[1],
                    face_slots[2], face_slots[3], face_slots[4], face_slots[5], slot);
    }
    c->last_use = ++g.owned_cube_use;
    *out_slot = (u32)slot;
    return (int)(SRV_OWNED_CUBE_BASE + slot);
}

static int surface_window(u32 si, const rsx_dsp_texture* t, int dbg)
{
    surface_t* s = &g.surfaces[si];
    const u32 w = t->width, h = t->height;
    if (!ld_swin_on() || !s->tex || !w || !h) return -1;
    if (w == s->w && h == s->h) return -1;           /* exact-size alias: the whole surface, unchanged */
    if (w > s->w || h > s->h) return -1;
    const u32 sb = ld_surface_texel_bytes(s->fmt);
    const u32 tb = ld_tex_texel_bytes(t->format);
    if (t->dimension != 2u || t->cubemap || !(t->format & TEX_FMT_LINEAR) || !s->pitch ||
        t->pitch != s->pitch || tb != sb) {
        if (dbg) {
            static u32 n_keep;
            if (n_keep++ < 24)
                fprintf(stderr, "[alias-subrect] keep whole surface: tex %u:0x%08X fmt=0x%02X %ux%u pitch=%u "
                        "(%u B/texel) vs surface[%u] %ux%u pitch=%u (%u B/texel)\n", t->location, t->offset,
                        t->format, w, h, t->pitch, tb, si, s->w, s->h, s->pitch, sb);
        }
        return -1;
    }
    static u32 use_clock;
    u32 k = LD_SWIN_PER;
    for (u32 j = 0; j < LD_SWIN_PER; j++)
        if (s->swin[j].tex && s->swin[j].src == s->tex && s->swin[j].w == w && s->swin[j].h == h) { k = j; break; }
    if (k == LD_SWIN_PER) {
        k = 0;
        for (u32 j = 1; j < LD_SWIN_PER; j++)
            if (!s->swin[j].tex || (s->swin[k].tex && s->swin[j].used < s->swin[k].used)) k = j;
        if (s->swin[k].tex) retire_texture(s->swin[k].tex);   /* the open command list may still read it */
        s->swin[k].tex = NULL;
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);              /* the surface's format and sample count */
        rd.Width = ld_sx(s, w); rd.Height = ld_sx(s, h); rd.Alignment = 0;   /* host scale of the surface */
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* wt = NULL;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&wt))) {
            static u32 n_fail;
            if (n_fail++ < 8)
                fprintf(stderr, "[swin] create failed %u:0x%X %ux%u: whole surface bound\n", s->location, s->offset,
                        w, h);
            return -1;
        }
        s->swin[k].tex = wt; s->swin[k].src = s->tex; s->swin[k].w = w; s->swin[k].h = h;
        s->swin[k].wgen = s->swin_wgen - 1u;
        srv_write_surface(SRV_SURF_WIN_BASE + si * LD_SWIN_PER + k, wt, s->fmt);
        static u32 n_log;
        if (n_log++ < 16)
            fprintf(stderr, "[swin] %u:0x%X surface %ux%u pitch=%u -> window %ux%u (slot %u.%u)\n",
                    s->location, s->offset, s->w, s->h, s->pitch, w, h, si, k);
    }
    if (dbg) {
        static u32 n_hit;
        if (n_hit++ < 24)
            fprintf(stderr, "[alias-subrect] tex %u:0x%08X fmt=0x%02X %ux%u pitch=%u -> surface[%u] %ux%u "
                    "texels [0,%u)x[0,%u) (window %u)\n", t->location, t->offset, t->format, w, h, t->pitch, si,
                    s->w, s->h, w, h, k);
    }
    s->swin[k].used = ++use_clock;
    if (s->swin[k].wgen != s->swin_wgen) {
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = s->tex;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = s->swin[k].tex;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s->swin[k].tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s->tex;         src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = { 0, 0, 0, ld_sx(s, w), ld_sx(s, h), 1 };
        g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &box);
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
        s->swin[k].wgen = s->swin_wgen;
        if ((++g_ld_swin_copies & 0x3FFFu) == 1u)
            fprintf(stderr, "[swin] copies=%llu frame=%u\n", (unsigned long long)g_ld_swin_copies, g_ld_frames);
    }
    return (int)(SRV_SURF_WIN_BASE + si * LD_SWIN_PER + k);
}

/* A draw that samples its own render target. RSX reads and writes the same
 * surface in one pass (GH3's post pass reads the scene at 0x200000 while
 * drawing into 0x200000); D3D12 cannot, and the lookup used to fall back to
 * guest VRAM -- empty, so the pass painted the scene black. Snapshot the
 * target into a per-surface copy and sample that. Returns the SRV slot, or
 * SRV_WHITE if the copy cannot be made. */
static u32 surface_feedback_srv(u32 slot)
{
    surface_t* s = &g.surfaces[slot];
    if (!s->tex) return SRV_WHITE;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    int fresh = 0;
    if (!s->fb_tex) {
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
                g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&s->fb_tex))) {
            s->fb_tex = NULL;
            return SRV_WHITE;
        }
        srv_write_surface(SRV_FEEDBACK_BASE + slot, s->fb_tex, s->fmt);
        fresh = 1;
    }
    b[0].Transition.pResource = s->tex;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = s->fb_tex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, fresh ? 1 : 2, b);
    g.list->lpVtbl->CopyResource(g.list, s->fb_tex, s->tex);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    return SRV_FEEDBACK_BASE + slot;
}

/* Find the live surface whose guest rows (at `pitch`) contain byte `abs`, with
 * room for a w x h rect there. Returns the slot and the rect's origin. */
static int surface_containing(u32 location, u32 abs, u32 pitch, u32 w, u32 h,
                              u32* ox, u32* oy)
{
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != location || abs < s->offset) continue;
        const u32 bpp = ld_surface_texel_bytes(s->fmt);
        if (!pitch || (s->pitch && s->pitch != pitch)) continue;
        const u32 d = abs - s->offset, y = d / pitch, x = (d % pitch) / bpp;
        if (x + w <= s->w && y + h <= s->h) { *ox = x; *oy = y; return (int)i; }
    }
    return -1;
}

/* NV3089 scaled-image blit whose SOURCE is a live surface: the pixels exist
 * only on the GPU, so the guest-memory copy in cellGcmSys moves black. GH3
 * copies its 1040x592 scene out to 0x1651500 every frame, runs its post pass
 * sampling that copy, and copies it back -- all black, so attract mode showed
 * only its overlay. Do the copy on the GPU, creating a live surface for the
 * destination so later texture lookups at that address find it. 1:1 only.
 * Returns 1 when handled, <= 0 when not (reason codes in ld_blit_gpu). */
static FILE* s_csv_file;       /* defined with live_draw_csv_emit */
static int   s_csv_gate_open;
static int   g_bt_on;   /* bind_trace frame in progress */
int rsx_live_draw_trace_on(void) { return g_bt_on; }

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h);
int rsx_live_draw_blit(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    const int r = ld_blit_gpu(src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h);
    if (g_bt_on)
        fprintf(stderr, "[bind-trace] blit %u:0x%08X/%u -> %u:0x%08X/%u %ux%u gpu=%d\n",
                src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h, r);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#blit,%u,%u:0x%X/%u,%u:0x%X/%u,%ux%u,gpu=%d\n", g_ld_frames,
                src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h, r);
        fflush(s_csv_file);
    }
    /* LD_BLIT_DUMP=<dir>: while the CSV gate is open, read back the first four
     * blits' destination surfaces right after the copy (one frame's worth). */
    { static const char* dir = (const char*)-1; static u32 n;
      if (dir == (const char*)-1) dir = getenv("LD_BLIT_DUMP");
      if (!(s_csv_file && s_csv_gate_open)) n = 0;
      else if (dir && r == 1 && n < 4) {
          u32 dx, dy;
          const int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
          if (di >= 0) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\blit_f%06u_%u_dst%08X.ppm", dir, g_ld_frames, n,
                       g.surfaces[di].offset);
              ld_dump_surface_ppm(path, &g.surfaces[di]);
          }
          n++;
      } }
    /* 2: copied on the GPU out of a local surface this renderer drew. Local
     * surfaces are never written back, so the guest bytes there are stale and
     * the CPU's guest-memory copy of them (cellGcmSys nv3089_blit, up to 3.6 MB
     * per blit) moves nothing anyone reads. */
    if (r == 1 && src_loc == 0u) {
        u32 sx, sy;
        const int si = surface_containing(src_loc, src_abs, src_pitch, w, h, &sx, &sy);
        if (si >= 0 && g.surfaces[si].location == 0u && g.surfaces[si].ct_draw_gen) return 2;
    }
    return r;
}

/* A SCALED NV3089 blit out of a live surface cannot be a CopyTextureRegion.
 * The one that matters is a supersampled scene resolved into a display buffer
 * (Tornado Outbreak renders 2x2 and downsamples into its scanout in 512x512
 * tiles): the guest-memory copy moves black and the flip presented whatever
 * surface happened to be current. When such a blit lands on a display buffer's
 * origin, remember the source surface; the flip presents it (at the host
 * surface's size) when the buffer has no live surface of its own.
 * ponytail: scanout alias, not a real scaled copy -- a title that samples the
 * resolved buffer as a texture still sees black; add a draw-based scaled blit
 * then. */
/* A k x k box-filter downsample from one live surface into another, drawn as
 * a full-viewport triangle over the destination rect: a resolve cannot be a
 * CopyTextureRegion once the source holds its samples (an antialiased target
 * is kept at its sample resolution, rsx_dispatch.h). Pipelines are made on
 * first use, one per render-target format. Returns 1 when drawn. */
static int ld_downsample(u32 si, u32 sx, u32 sy, u32 k,
                         u32 di, u32 dx, u32 dy, u32 w, u32 h)
{
   
    const DXGI_FORMAT fmt = g.surfaces[di].fmt;
    ID3D12PipelineState* pso = NULL;
    u32 n = 0;
    for (; n < 4 && g.downsample_pso[n]; n++)
        if (g.downsample_fmt[n] == fmt) { pso = g.downsample_pso[n]; break; }
    if (!pso) {
        if (n == 4) return 0;
        static const char src[] =
            "Texture2D<float4> s : register(t0);\n"
            "cbuffer c : register(b0) { int2 so; int2 dorg; int k; };\n"
            "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
            "    float2 t = float2((id << 1) & 2, id & 2);\n"
            "    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "}\n"
            "float4 ps(float4 p : SV_Position) : SV_Target {\n"
            "    int2 b = so + (int2(p.xy) - dorg) * k;\n"
            "    float4 a = 0;\n"
            "    for (int y = 0; y < k; y++)\n"
            "        for (int x = 0; x < k; x++) a += s.Load(int3(b + int2(x, y), 0));\n"
            "    return a / (k * k);\n"
            "}\n";
        ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
        HRESULT hr = ld_builtin_compile(src, sizeof(src) - 1u, "ld_downsample", NULL, NULL,
                                "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
        if (err) { err->lpVtbl->Release(err); err = NULL; }
        if (SUCCEEDED(hr))
            hr = ld_builtin_compile(src, sizeof(src) - 1u, "ld_downsample", NULL, NULL,
                            "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
        if (err) { err->lpVtbl->Release(err); err = NULL; }
        if (SUCCEEDED(hr) && !g.downsample_rootsig) {
            D3D12_DESCRIPTOR_RANGE range = {0};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER rp[2] = {0};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants.Num32BitValues = 5;
            rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 1;
            rp[1].DescriptorTable.pDescriptorRanges = &range;
            rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd = {0};
            rsd.NumParameters = 2; rsd.pParameters = rp;
            ID3DBlob* sig = NULL;
            hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
            if (err) err->lpVtbl->Release(err);
            if (SUCCEEDED(hr)) {
                hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                        sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
                        (void**)&g.downsample_rootsig);
                sig->lpVtbl->Release(sig);
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
            pd.pRootSignature = g.downsample_rootsig;
            pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
            pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
            pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb);
            pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = 0xFFFFFFFFu;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = fmt;
            pd.SampleDesc.Count = 1;
            hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd,
                    &IID_ID3D12PipelineState, (void**)&pso);
        }
        if (vsb) vsb->lpVtbl->Release(vsb);
        if (psb) psb->lpVtbl->Release(psb);
        if (FAILED(hr) || !pso) {
            static u32 fail_logs = 0;
            if (PS3_LOG_FIRST(fail_logs, 8))
                fprintf(stderr, "[live-draw] downsample pipeline for format %u failed hr=0x%08lX\n",
                        (unsigned)fmt, (unsigned long)hr);
            return 0;
        }
        g.downsample_pso[n] = pso; g.downsample_fmt[n] = fmt;
    }
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots, 0);
    if (!table.ptr) return 0;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.surfaces[si].tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + di);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.downsample_rootsig);
    const u32 c[5] = {sx, sy, dx, dy, k};
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 5, c, 0);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    const D3D12_VIEWPORT vp = {(float)dx, (float)dy, (float)w, (float)h, 0.0f, 1.0f};
    const D3D12_RECT sc = {(LONG)dx, (LONG)dy, (LONG)(dx + w), (LONG)(dy + h)};
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    ld_maa_copy(si,(float)sx,(float)sy,(float)(w*k),(float)(h*k),di,dx,dy,w,h,"downsample_draw_recorded",0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return 1;
}

/* The copy half of a scaled resolve, done on the GPU. The host surface holds
 * the guest's grid 1:1 -- an antialiased target is kept at its sample
 * resolution (Tornado Outbreak: pitch 10240 = 2560 guest pixels, host surface
 * 2560x1440) -- so the guest position is the host position and a ds x dt
 * resolve is a k x k box downsample. Each tile lands in a live surface at the display buffer it writes
 * into, so the flip presents a snapshot taken at resolve time -- presenting
 * the source live (the alias below) showed whatever the title rendered into
 * it next, and the menu flickered to black on two frames in three.
 * Returns 1 when copied. */
/* Scaled resolve as a draw. NV3089 scales with independent 20.12 factors per
 * axis; Twisted Metal resolves its 1280x704 scene into a 642x432 rect at
 * (39,24) of a 720x480 display buffer (ds ~1.99, dt ~1.63), which no
 * CopyTextureRegion can express. Draw one triangle covering the destination
 * rect and sample the source surface with a linear clamp sampler.
 * One root signature, one PSO per destination format (cached). */
static ID3D12RootSignature* g_sblit_rs;
static struct { DXGI_FORMAT fmt; int display; ID3D12PipelineState* pso; } g_sblit_pso[8];

static ID3D12PipelineState* ld_scaled_blit_pso_ex(DXGI_FORMAT fmt, int display)
{
    for (int i = 0; i < 8; i++)
        if (g_sblit_pso[i].pso && g_sblit_pso[i].fmt == fmt &&
            g_sblit_pso[i].display == display) return g_sblit_pso[i].pso;
    static const char src[] =
        "cbuffer C : register(b0) { float4 r; float display_gamma; };\n"
        "Texture2D t0 : register(t0);\n"
        "SamplerState s0 : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "    V o; float2 q = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    o.uv = r.xy + (r.zw - r.xy) * q; return o; }\n"
        "float4 ps(V i) : SV_Target { return t0.SampleLevel(s0, i.uv, 0); }\n"
        "float4 ps_display(V i) : SV_Target { float4 c = ps(i);\n"
        "    c.rgb = pow(saturate(c.rgb), display_gamma); return c; }\n";
    if (!g_sblit_rs) {
        D3D12_DESCRIPTOR_RANGE range = {0};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER p[2] = {0};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.Num32BitValues = 5;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &range;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp = {0};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 2; rsd.pParameters = p;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &smp;
        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            if (err) err->lpVtbl->Release(err);
            return NULL;
        }
        HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                         sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&g_sblit_rs);
        sig->lpVtbl->Release(sig);
        if (FAILED(hr)) { g_sblit_rs = NULL; return NULL; }
    }
    ID3DBlob* vs = NULL; ID3DBlob* ps = NULL; ID3DBlob* err = NULL;
    if (FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "scaled_blit", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "scaled_blit", NULL, NULL, display ? "ps_display" : "ps", "ps_5_0", 0, 0, &ps, &err))) {
        fprintf(stderr, "[scaled-blit] shader compile failed: %s\n",
                err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vs) vs->lpVtbl->Release(vs);
        return NULL;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g_sblit_rs;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    if (FAILED(hr)) { fprintf(stderr, "[scaled-blit] PSO failed hr=0x%08lX\n", (unsigned long)hr); return NULL; }
    for (int i = 0; i < 8; i++)
        if (!g_sblit_pso[i].pso) { g_sblit_pso[i].fmt = fmt; g_sblit_pso[i].display = display; g_sblit_pso[i].pso = pso; break; }
    return pso;
}

static ID3D12PipelineState* ld_scaled_blit_pso(DXGI_FORMAT fmt)
{
    return ld_scaled_blit_pso_ex(fmt, 0);
}

/* Draw source surface rect (sx,sy,sw,sh, guest pixels) scaled into the
 * destination surface rect (dx,dy,dw,dh, guest pixels). The source is read
 * normalised and the destination is converted to its host scale, so either
 * side may be render-scaled. Returns 1 when recorded. */
static int ld_scaled_blit(u32 si, float sx, float sy, float sw, float sh,
                          u32 di, u32 dx, u32 dy, u32 dw, u32 dh)
{
   
    surface_t* s = &g.surfaces[si];
    surface_t* d = &g.surfaces[di];
    if (!s->tex || !d->tex || !s->w || !s->h || !dw || !dh) return 0;
    /* Scale the end points, not start and size apart: floor(x)+floor(w)
     * left 1-pixel gaps/overlaps between neighbouring rects at odd x. */
    if (d->scaled) {
        const u32 ex = ld_rs_up(dx + dw), ey = ld_rs_up(dy + dh);
        dx = ld_rs_up(dx); dy = ld_rs_up(dy); dw = ex - dx; dh = ey - dy;
        if (!dw || !dh) return 0;
    }
    ID3D12PipelineState* pso = ld_scaled_blit_pso(d->fmt);
    if (!pso) return 0;
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots, 0);
    if (!table.ptr) return 0;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s->tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + di);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_sblit_rs);
    const float r[4] = { sx / (float)s->w, sy / (float)s->h,
                         (sx + sw) / (float)s->w, (sy + sh) / (float)s->h };
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 4, r, 0);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    D3D12_VIEWPORT vp = { (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f };
    D3D12_RECT sc = { (LONG)dx, (LONG)dy, (LONG)(dx + dw), (LONG)(dy + dh) };
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    ld_maa_copy(si,sx,sy,sw,sh,di,dx,dy,dw,dh,"scaled_draw_recorded",1);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    ld_surface_note_write(di, LD_SURFACE_WRITE_COPY);
    ld_consumer_surface_event("scaled_resolve_complete",di,si);
    { static int n = 0; if (n++ < 4)
        fprintf(stderr, "[scaled-blit] surf %u (%.0f,%.0f %.0fx%.0f) -> surf %u (%u,%u %ux%u)\n",
                si, sx, sy, sw, sh, di, dx, dy, dw, dh); }
    return 1;
}

static void ld_nv3089_signed_axis(u32 origin, u32 increment, u32 extent,
                                 float* start, float* span)
{
    const float step = (float)(int32_t)increment / 1048576.0f;
    *start = (float)origin + (step < 0 ? 0.5f - 0.5f * step : 0.0f);
    *span = (float)extent * step;
}

/* Re-upload a main-memory (location 1) R8G8B8A8 surface from guest bytes
 * (A,R,G,B per texel), once per frame per surface. Used for the scene copy the
 * SPU post-processes in place; see rsx_live_draw_resolve_blit. */
static void ld_surface_upload_from_guest(u32 slot)
{
    static ID3D12Resource* up; static u64 up_size;
    static u32 last_frame[MAX_SURFACES];
    surface_t* s = &g.surfaces[slot];
    if (!s->tex || s->location != 1u || s->fmt != DXGI_FORMAT_R8G8B8A8_UNORM || !s->w || !s->h) return;
    if (s->scaled) {   /* a guest-size image cannot replace the scaled host copy */
        static int said;
        if (!said++) fprintf(stderr, "[render-scale] guest upload into a scaled surface skipped\n");
        return;
    }
    if (last_frame[slot] == g_ld_frames + 1u) return;
    const u32 gpitch = s->pitch ? s->pitch : s->w * 4u;
    const u8* gp = guest_ptr(1u, s->offset, gpitch * s->h);
    if (!gp) return;
    const u32 rpitch = (s->w * 4u + 255u) & ~255u;
    const u64 need = (u64)rpitch * s->h;
    if (!up || up_size < need) {
        if (up) up->lpVtbl->Release(up);   /* flush already waited for any prior use */
        up = NULL;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = need;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&up))) { up = NULL; return; }
        up_size = need;
    }
    u8* dst = NULL; D3D12_RANGE none = {0, 0};
    if (FAILED(up->lpVtbl->Map(up, 0, &none, (void**)&dst))) return;
    for (u32 y = 0; y < s->h; y++) {
        const u8* src = gp + (size_t)y * gpitch;
        u8* d = dst + (size_t)y * rpitch;
        /* A,R,G,B -> R,G,B,A is a byte rotate; whole words, because the
         * upload heap is write-combined and byte stores crawl. */
        const u32* sw = (const u32*)src; u32* dw = (u32*)d;
        for (u32 x = 0; x < s->w; x++) { const u32 v = sw[x]; dw[x] = (v >> 8) | (v << 24); }
    }
    up->lpVtbl->Unmap(up, 0, NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s->tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dl = {0};
    src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = s->fmt;
    src.PlacedFootprint.Footprint.Width = s->w;
    src.PlacedFootprint.Footprint.Height = s->h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rpitch;
    dl.pResource = s->tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dl, 0, 0, 0, &src, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    last_frame[slot] = g_ld_frames + 1u;
    static u32 logged;
    if (logged++ < 3)
        fprintf(stderr, "[guest-upload] surface %u 1:0x%08X %ux%u refreshed from guest (SPU post-process)\n",
                slot, s->offset, s->w, s->h);
}

/* GPU stand-in for the last two SPU post-process stages (posttask family11
 * then family10): out = LUT_c[ min(1, scene + 2x-upsampled bloom) ]. The scene
 * is the host copy of the main-memory scene (no write-back race), the bloom is
 * the 640x360 buffer the SPU bloom chain leaves at 1:0x017C0000, and the LUT
 * is the one the SPU itself was about to apply (captured from LS by
 * spu_posttask_native/guard.c). Runs once per frame at the first resolve.
 * pp_gpu_off file in the working directory disables it. */
unsigned char g_pp_lut[768];
volatile long g_pp_lut_seq;
volatile unsigned g_pp_scale9, g_pp_scale10;
volatile unsigned long g_pp_gpu_last_ms;
unsigned char g_pp_bloom_snap[640 * 360 * 4];   /* bloom buffer copied when family11 starts */
volatile long g_pp_bloom_seq;   /* GetTickCount of the last applied pass */   /* family11 scene/bloom gains (float bits) */
static struct {
    int state;                    /* 0 untried, 1 ready, -1 unavailable */
    ID3D12RootSignature* rs;
    ID3D12PipelineState* pso;
    ID3D12DescriptorHeap* heap;   /* 3 shader-visible SRVs */
    ID3D12Resource* bloom; ID3D12Resource* lut; ID3D12Resource* out;
    u32 bloom_w, bloom_h;         /* bloom texture size = the glow mask's host size */
    ID3D12Resource* up; u64 up_size;
    u32 out_w, out_h; u32 last_frame;
    u32 applied_slot, applied_wgen;   /* scene write already post-processed */
    /* Bloom pyramid (ld_ppg_pyramid): levels 1/2, 1/4, 1/8 of the glow mask,
     * each a separable Gaussian (pyr[k][1] horizontal, pyr[k][0] vertical). */
    ID3D12PipelineState* blur_pso;
    ID3D12Resource* pyr[3][2]; u32 pyr_w[3], pyr_h[3];
    u32 pyr_mask_w, pyr_mask_h;       /* glow-mask size the pyramid was made for */
    ID3D12DescriptorHeap* pyr_rtvh;   /* 6 RTVs: level k at 2k (A) and 2k+1 (B) */
} g_ppg;
static int ld_ppg_disabled(void)
{
    static int v = -1; static DWORD last;
    const DWORD now = GetTickCount();
    if (v < 0 || now - last > 1000u) {
        const int was = v; last = now;
        /* The scene needs the same final exposure/tone stage at every scale.
         * At native scale the old opt-in also left guest upload disabled
         * (blit_writeback is opt-in), presenting the pre-SPU scene instead.
         * The LUT surface copy and applied_wgen guard below address the old
         * zero-LUT/double-application flicker. Keep explicit diagnostic opt-out. */
        v = GetFileAttributesA("pp_gpu_off") != INVALID_FILE_ATTRIBUTES;
        if (was >= 0 && was != v) fprintf(stderr, "[pp-gpu] %s\n", v ? "OFF" : "ON");
    }
    return v;
}
static ID3D12Resource* ld_ppg_tex(u32 w, u32 h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Flags = flags;
    ID3D12Resource* r = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                                      &IID_ID3D12Resource, (void**)&r))) return NULL;
    return r;
}
static int ld_ppg_build(u32 w, u32 h)
{
    if (g_ppg.state) return g_ppg.state > 0 && g_ppg.out_w == w && g_ppg.out_h == h;
    g_ppg.state = -1;
    static const char src[] =
        "Texture2D scene : register(t0);\n"
        "Texture2D bloom : register(t1);\n"
        "Texture2D lut   : register(t2);\n"
        "Texture2D pyr1  : register(t3);\n"
        "Texture2D pyr2  : register(t4);\n"
        "Texture2D pyr3  : register(t5);\n"
        "cbuffer G : register(b0) { float4 gain; };\n"
        "SamplerState lin : register(s0);\n"
        "struct V { float4 p : SV_Position; };\n"
        "V vs(uint id : SV_VertexID) { V o; float2 q = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target {\n"
        "    uint w, h; scene.GetDimensions(w, h);\n"
        "    int2 p = int2(i.p.xy);\n"
        "    float4 s = scene.Load(int3(p, 0));\n"
        "    float2 uv = (float2(p) + 0.5) / float2(w, h);\n"
        "    float4 b = bloom.SampleLevel(lin, uv, 0);\n"
        /* gain.z > 0: blur the 640x360 glow mask here (7x7 Gaussian, tap
         * spacing gain.z mask texels) -- the SPU bloom chain's job, whose
         * input arrived after it ran (see ld_ppg_apply). */
        /* gain.w > 0: the bloom pyramid (ld_ppg_pyramid). Weights sum to 1,
         * so a broad glow keeps its brightness while a small hot core
         * spreads out instead of burning white (the SPU chain also blurs
         * downsampled copies, not the full-size mask). */
        "    if (gain.w > 0.0) {\n"
        "        b.rgb = pyr1.SampleLevel(lin, uv, 0).rgb * 0.40 + pyr2.SampleLevel(lin, uv, 0).rgb * 0.35 +\n"
        "                pyr3.SampleLevel(lin, uv, 0).rgb * 0.25; }\n"
        "    else if (gain.z > 0.0) {\n"
        "        uint bw, bh; bloom.GetDimensions(bw, bh);\n"
        "        float2 bt = gain.z / float2(bw, bh); float3 acc = 0; float ws = 0;\n"
        "        [unroll] for (int y = -3; y <= 3; y++) [unroll] for (int x = -3; x <= 3; x++) {\n"
        "            float wt = exp(-(x * x + y * y) / 4.5);\n"
        "            acc += bloom.SampleLevel(lin, uv + float2(x, y) * bt, 0).rgb * wt; ws += wt; }\n"
        "        b.rgb = acc / ws; }\n"
        "    float3 v = saturate(s.rgb * gain.x + b.rgb * gain.y);\n"
        "    uint3 k = (uint3)round(v * 255.0);\n"
        "    return float4(lut.Load(int3(k.r, 0, 0)).r, lut.Load(int3(k.g, 1, 0)).r,\n"
        "                  lut.Load(int3(k.b, 2, 0)).r, s.a); }\n"
        /* One pyramid pass: 9-tap Gaussian (sigma 2 taps) of t0 along gain.xy
         * (uv per tap), output pixel size gain.zw (uv). Reading a 2x larger
         * source at this level's pixel centres box-filters it on the way down. */
        "float4 blur(V i) : SV_Target {\n"
        "    float2 uv = i.p.xy * gain.zw; float3 acc = 0; float ws = 0;\n"
        "    [unroll] for (int k = -4; k <= 4; k++) { float wt = exp(-(k * k) / 8.0);\n"
        "        acc += scene.SampleLevel(lin, uv + gain.xy * k, 0).rgb * wt; ws += wt; }\n"
        "    return float4(acc / ws, 1); }\n";
    ID3DBlob* vs = NULL; ID3DBlob* ps = NULL; ID3DBlob* bps = NULL; ID3DBlob* err = NULL;
    if (FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "pp_gpu", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "pp_gpu", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err)) ||
        FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "pp_gpu", NULL, NULL, "blur", "ps_5_0", 0, 0, &bps, &err))) {
        fprintf(stderr, "[pp-gpu] compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vs) vs->lpVtbl->Release(vs);
        if (ps) ps->lpVtbl->Release(ps);
        return 0;
    }
    /* Six SRVs per table: the composite reads scene, mask, LUT and the three
     * pyramid levels; a pyramid pass reads only t0 (see the heap layout in
     * ld_pp_gpu_apply). */
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 6;
    D3D12_ROOT_PARAMETER rps[2] = {0};
    rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rps[0].DescriptorTable.NumDescriptorRanges = 1; rps[0].DescriptorTable.pDescriptorRanges = &range;
    rps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rps[1].Constants.Num32BitValues = 4;
    rps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {0};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 2; rsd.pParameters = rps; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &smp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
               sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&g_ppg.rs))) {
        fprintf(stderr, "[pp-gpu] root signature failed\n");
        if (sig) sig->lpVtbl->Release(sig);
        vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); bps->lpVtbl->Release(bps);
        return 0;
    }
    sig->lpVtbl->Release(sig);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g_ppg.rs;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState, (void**)&g_ppg.pso);
    if (SUCCEEDED(hr)) {   /* pyramid passes: half-float levels (no banding in faint glow) */
        pd.PS.pShaderBytecode = bps->lpVtbl->GetBufferPointer(bps); pd.PS.BytecodeLength = bps->lpVtbl->GetBufferSize(bps);
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                                              (void**)&g_ppg.blur_pso))) {
            g_ppg.blur_pso = NULL;
            fprintf(stderr, "[pp-gpu] bloom pyramid PSO failed; single blur only\n");
        }
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); bps->lpVtbl->Release(bps);
    if (FAILED(hr)) { fprintf(stderr, "[pp-gpu] PSO failed hr=0x%08lX\n", (unsigned long)hr); return 0; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 12;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_ppg.heap))) return 0;
    {   /* Null SRVs until the pyramid exists: every table slot stays valid. */
        D3D12_CPU_DESCRIPTOR_HANDLE h0;
        g_ppg.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ppg.heap, &h0);
        const UINT st = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = 0; i < 12; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE hi = h0; hi.ptr += (SIZE_T)i * st;
            g.dev->lpVtbl->CreateShaderResourceView(g.dev, NULL, &sv, hi);
        }
    }
    g_ppg.bloom = ld_ppg_tex(w / 2, h / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_ppg.bloom_w = w / 2; g_ppg.bloom_h = h / 2;
    g_ppg.lut = ld_ppg_tex(256, 3, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_ppg.out = ld_ppg_tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!g_ppg.bloom || !g_ppg.lut || !g_ppg.out) { fprintf(stderr, "[pp-gpu] texture creation failed\n"); return 0; }
    g_ppg.out_w = w; g_ppg.out_h = h;
    g_ppg.state = 1;
    fprintf(stderr, "[pp-gpu] ready %ux%u\n", w, h);
    return 1;
}
static void ld_ppg_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &br);
}
/* pp_gpu heap layout. 0..5: pyramid pass inputs (mask, B1, A1, B2, A2, B3);
 * 6..11: the composite's table (scene, mask, LUT, A1, A2, A3). A pass binds
 * the table at its input, so it starts at most at 5 and ends inside the heap. */
#define LD_PPG_DESC_COMPOSITE 6u
static void ld_ppg_srv(UINT index, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g_ppg.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ppg.heap, &h);
    h.ptr += (SIZE_T)index * g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = f; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, r, &sv, h);
}
/* Bloom pyramid for a mask_w x mask_h glow mask: three levels at 1/2, 1/4,
 * 1/8, each blurred horizontally then vertically (6 small passes, the largest
 * 320x180 for the 640x360 mask). Made on first use and when the mask size
 * changes. Returns 1 when the levels are ready to draw. */
static int ld_ppg_pyramid(u32 mask_w, u32 mask_h)
{
    if (!g_ppg.blur_pso) return 0;
    if (g_ppg.pyr[2][0] && g_ppg.pyr_mask_w == mask_w && g_ppg.pyr_mask_h == mask_h) return 1;
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 2; j++)
            if (g_ppg.pyr[k][j]) { retire_texture(g_ppg.pyr[k][j]); g_ppg.pyr[k][j] = NULL; }
    if (!g_ppg.pyr_rtvh) {
        D3D12_DESCRIPTOR_HEAP_DESC rd = {0}; rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 6;
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &rd, &IID_ID3D12DescriptorHeap,
                                                      (void**)&g_ppg.pyr_rtvh))) { g_ppg.pyr_rtvh = NULL; return 0; }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0;
    g_ppg.pyr_rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ppg.pyr_rtvh, &rtv0);
    const UINT rstep = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    u32 w = mask_w, h = mask_h;
    for (int k = 0; k < 3; k++) {
        w = w > 1u ? w / 2u : 1u; h = h > 1u ? h / 2u : 1u;
        g_ppg.pyr_w[k] = w; g_ppg.pyr_h[k] = h;
        for (int j = 0; j < 2; j++) {
            g_ppg.pyr[k][j] = ld_ppg_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (!g_ppg.pyr[k][j]) { fprintf(stderr, "[pp-gpu] bloom pyramid texture failed\n"); return 0; }
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0; rtv.ptr += (SIZE_T)(2 * k + j) * rstep;
            g.dev->lpVtbl->CreateRenderTargetView(g.dev, g_ppg.pyr[k][j], NULL, rtv);
        }
        /* Inputs: B_k feeds the vertical pass, A_k the next level. */
        ld_ppg_srv(1u + 2u * (u32)k, g_ppg.pyr[k][1], DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (k < 2) ld_ppg_srv(2u + 2u * (u32)k, g_ppg.pyr[k][0], DXGI_FORMAT_R16G16B16A16_FLOAT);
        ld_ppg_srv(LD_PPG_DESC_COMPOSITE + 3u + (u32)k, g_ppg.pyr[k][0], DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    g_ppg.pyr_mask_w = mask_w; g_ppg.pyr_mask_h = mask_h;
    fprintf(stderr, "[pp-gpu] bloom pyramid %ux%u -> %ux%u, %ux%u, %ux%u\n", mask_w, mask_h,
            g_ppg.pyr_w[0], g_ppg.pyr_h[0], g_ppg.pyr_w[1], g_ppg.pyr_h[1], g_ppg.pyr_w[2], g_ppg.pyr_h[2]);
    return 1;
}
/* Record the six pyramid passes. The mask (descriptor 0) must already be a
 * shader resource; the levels end as shader resources for the composite.
 * spread: tap spacing in level texels (2 with pp_bloom_wide). */
static void ld_ppg_pyramid_draw(float spread)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0;
    g_ppg.pyr_rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ppg.pyr_rtvh, &rtv0);
    const UINT rstep = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT sstep = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE gh0; g_ppg.heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g_ppg.heap, &gh0);
    ID3D12DescriptorHeap* heaps[] = { g_ppg.heap };
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 1, heaps);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_ppg.rs);
    g.list->lpVtbl->SetPipelineState(g.list, g_ppg.blur_pso);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (u32 k = 0; k < 3; k++) {
        const float iw = 1.0f / (float)g_ppg.pyr_w[k], ih = 1.0f / (float)g_ppg.pyr_h[k];
        D3D12_VIEWPORT vp = { 0, 0, (float)g_ppg.pyr_w[k], (float)g_ppg.pyr_h[k], 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)g_ppg.pyr_w[k], (LONG)g_ppg.pyr_h[k] };
        g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
        g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
        for (u32 j = 0; j < 2; j++) {   /* j 0: horizontal into B_k, 1: vertical into A_k */
            ID3D12Resource* dst = g_ppg.pyr[k][j ? 0 : 1];
            ld_ppg_barrier(dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0; rtv.ptr += (SIZE_T)(2u * k + (j ? 0u : 1u)) * rstep;
            g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
            D3D12_GPU_DESCRIPTOR_HANDLE gh = gh0; gh.ptr += (UINT64)(2u * k + j) * sstep;
            g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 0, gh);
            const float c[4] = { j ? 0.0f : spread * iw, j ? spread * ih : 0.0f, iw, ih };
            g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 1, 4, c, 0);
            g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
            ld_ppg_barrier(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
}
/* Returns 1 when the scene surface now holds the post-processed image. */
static int ld_pp_gpu_apply(u32 slot)
{
   
    surface_t* s = &g.surfaces[slot];
    if (!g_pp_lut_seq || ld_ppg_disabled() || !s->tex || s->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return 0;
    /* A resolve with no new scene copy since the last pass (the guest presents
     * more often than it renders) would process the image twice: brighter
     * every other flip. Only process a scene the blit has rewritten. */
    static int pp_tr; static DWORD pp_tr_chk; { const DWORD now = GetTickCount();
      if (now - pp_tr_chk > 500u) { pp_tr_chk = now; pp_tr = GetFileAttributesA("pp_trace") != INVALID_FILE_ATTRIBUTES; } }
    if (pp_tr) fprintf(stderr, "[pp-trace] s9=%08X lutR32=%u lutseq=%ld ", g_pp_scale9, g_pp_lut[32], (long)g_pp_lut_seq);
    if (pp_tr) fprintf(stderr, "[pp-trace] frame=%u resolve slot=%u wgen=%u applied_wgen=%u %s\n", g_ld_frames, slot,
                       s->swin_wgen, g_ppg.applied_wgen,
                       (g_ppg.applied_slot == slot + 1u && g_ppg.applied_wgen == s->swin_wgen) ? "skip-same" : "apply");
    if (g_ppg.applied_slot == slot + 1u && g_ppg.applied_wgen == s->swin_wgen) return 1;
    if (!ld_ppg_build(s->hw ? s->hw : s->w, s->hh ? s->hh : s->h)) return 0;
    const u32 bw = s->w / 2, bh = s->h / 2;
    const u8* bloom_guest = (g_pp_bloom_seq && bw == 640u && bh == 360u)
        ? g_pp_bloom_snap : guest_ptr(1u, 0x017C0000u, bw * 4u * bh);
    if (!bloom_guest) return 0;
    if (pp_tr) {   /* bloom buffer content: mean/max of each byte lane */
        unsigned long long sum[4] = {0}; unsigned mx[4] = {0};
        for (u32 i = 0; i < bw * bh * 4u; i += 4u * 7u)
            for (int c = 0; c < 4; ++c) { sum[c] += bloom_guest[i + c]; if (bloom_guest[i + c] > mx[c]) mx[c] = bloom_guest[i + c]; }
        const unsigned long long cnt = (bw * bh + 6u) / 7u;
        fprintf(stderr, "[pp-trace] bloom mean=%llu/%llu/%llu/%llu max=%u/%u/%u/%u snapseq=%ld\n",
                sum[0] / cnt, sum[1] / cnt, sum[2] / cnt, sum[3] / cnt, mx[0], mx[1], mx[2], mx[3], (long)g_pp_bloom_seq);
    }
    /* Upload: bloom (ARGB -> RGBA rows) and the 3x256 LUT. */
    const u32 bpitch = (bw * 4u + 255u) & ~255u, lpitch = 256u;
    const u64 need = (u64)bpitch * bh + (u64)lpitch * 3u;
    /* Stage through the renderer's upload ring: it is per frame context, so
     * with frames in flight the next frame cannot overwrite this frame's
     * staging before the GPU copies it (one shared buffer could). */
    const u32 pp_base = (g.upload_used + 511u) & ~511u;
    if ((u64)pp_base + need > UPLOAD_SIZE) return 0;
    u8* m = g.upload_mapped + pp_base;
    g.upload_used = pp_base + (u32)need;
    for (u32 y = 0; y < bh; y++) {
        const u32* sw = (const u32*)(bloom_guest + (size_t)y * bw * 4u);
        u32* dw = (u32*)(m + (size_t)y * bpitch);
        for (u32 x = 0; x < bw; x++) { const u32 v = sw[x]; dw[x] = (v >> 8) | (v << 24); }
    }
    memcpy(m + (size_t)bpitch * bh, g_pp_lut, 768);
    /* The glow mask source: this frame's host surface when there is one. */
    int bs = -1;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* bsf = &g.surfaces[i];
        if (!bsf->tex || bsf->w != bw || bsf->h != bh || bsf->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) continue;
        if (bsf->location == 1u && bsf->offset == 0x017C0000u) { bs = (int)i; break; }
        if (bsf->location == 0u && bsf->offset == 0x045B0000u && bs < 0) bs = (int)i;
    }
    if (LD_FILE_FLAG("pp_bloom_guest")) bs = -1;
    /* The bloom texture must be exactly the mask's size. At internal scale
     * 1.5 the scene is 1920x1080 but the 640x360 mask stays at guest size
     * (ld_rs_wants scales canvas-size targets only); a 960x540 texture got the
     * mask in its top-left two thirds, and the composite then drew every glow
     * 1.5x too far from the top-left corner -- panel lights floating off the
     * panels. Size it to the source and resample in the shader (normalised uv). */
    const u32 src_w = bs >= 0 && g.surfaces[bs].hw ? g.surfaces[bs].hw : bw;
    const u32 src_h = bs >= 0 && g.surfaces[bs].hh ? g.surfaces[bs].hh : bh;
    int bloom_fresh = 0;
    if (src_w != g_ppg.bloom_w || src_h != g_ppg.bloom_h) {
        ID3D12Resource* nb = ld_ppg_tex(src_w, src_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        if (!nb) return 0;
        if (g_ppg.bloom) retire_texture(g_ppg.bloom);
        g_ppg.bloom = nb; g_ppg.bloom_w = src_w; g_ppg.bloom_h = src_h; bloom_fresh = 1;
        fprintf(stderr, "[pp-gpu] bloom texture %ux%u (glow mask %s)\n", src_w, src_h, bs >= 0 ? "host surface" : "guest copy");
    }
    if (g_ppg.last_frame) {   /* textures were left as shader resources last frame */
        if (!bloom_fresh)
            ld_ppg_barrier(g_ppg.bloom, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        ld_ppg_barrier(g_ppg.lut, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = g.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = pp_base;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = bw; src.PlacedFootprint.Footprint.Height = bh;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = bpitch;
    dst.pResource = g_ppg.bloom; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    /* The bloom is drawn by the GPU (0:0x045B0000) and blitted to main
     * 0x017C0000 for the SPU composite. That blit lands in a host surface
     * only; guest memory there stays zero, so the bloom read from it was
     * black. Copy the host surface instead (no CPU round trip). */
    {
        /* The host surface is this frame's glow mask; the shader blurs it.
         * The guest copy the SPU chain reads lands ~8 ms after that chain
         * ran (wb-store after spu-get in the pp_timeline), so its snapshot
         * was empty nearly every frame and the unblurred mask was added raw:
         * hard-edged green bands. */
        if (bs >= 0) {
            ID3D12Resource* br = g.surfaces[bs].tex;
            ld_ppg_barrier(br, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION bsrc = {0};
            bsrc.pResource = br; bsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &bsrc, NULL);
            ld_ppg_barrier(br, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (pp_tr) fprintf(stderr, "[pp-trace] bloom from host surface %d %u:0x%08X\n", bs,
                               g.surfaces[bs].location, g.surfaces[bs].offset);
        } else {
            g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        }
    }
    src.PlacedFootprint.Offset = (u64)pp_base + (u64)bpitch * bh;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src.PlacedFootprint.Footprint.Width = 256; src.PlacedFootprint.Footprint.Height = 3;
    src.PlacedFootprint.Footprint.RowPitch = lpitch;
    dst.pResource = g_ppg.lut;
    /* Prefer the three 256x1 R8 tone curves the GPU itself draws each frame
     * at main 0x00A00C00/0D00/0E00 (identical to the table the SPU applies).
     * The copy captured from SPU LS was sometimes all zero -- the table is
     * not yet resident at that call on every worker -- and a zero LUT turns
     * the scene black: that was the flicker, and later a black screen. */
    {
        int lut_surf[3] = {-1, -1, -1};
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const surface_t* ls = &g.surfaces[i];
            if (!ls->tex || ls->location != 1u || ls->w != 256u || ls->h != 1u || ls->fmt != DXGI_FORMAT_R8_UNORM) continue;
            if (ls->offset == 0x00A00C00u) lut_surf[0] = (int)i;
            if (ls->offset == 0x00A00D00u) lut_surf[1] = (int)i;
            if (ls->offset == 0x00A00E00u) lut_surf[2] = (int)i;
        }
        if (lut_surf[0] >= 0 && lut_surf[1] >= 0 && lut_surf[2] >= 0) {
            for (int c = 0; c < 3; c++) {
                ID3D12Resource* lr = g.surfaces[lut_surf[c]].tex;
                ld_ppg_barrier(lr, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION ls_src = {0};
                ls_src.pResource = lr; ls_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX box = {0, 0, 0, 256, 1, 1};
                g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, (UINT)c, 0, &ls_src, &box);
                ld_ppg_barrier(lr, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        } else {
            g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);
        }
    }
    ld_ppg_barrier(g_ppg.bloom, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ld_ppg_barrier(g_ppg.lut, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ld_ppg_barrier(s->tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ld_ppg_barrier(g_ppg.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    /* Descriptors: scene, bloom, lut (composite table), and the mask again as
     * the first pyramid pass input. */
    ld_ppg_srv(LD_PPG_DESC_COMPOSITE + 0u, s->tex, DXGI_FORMAT_R8G8B8A8_UNORM);
    ld_ppg_srv(LD_PPG_DESC_COMPOSITE + 1u, g_ppg.bloom, DXGI_FORMAT_R8G8B8A8_UNORM);
    ld_ppg_srv(LD_PPG_DESC_COMPOSITE + 2u, g_ppg.lut, DXGI_FORMAT_R8_UNORM);
    ld_ppg_srv(0u, g_ppg.bloom, DXGI_FORMAT_R8G8B8A8_UNORM);
    /* Bloom: pyramid by default; pp_bloom_single: the single 7x7 blur of the
     * full mask (previous behaviour, A/B); pp_bloom_noblur: the raw mask;
     * pp_bloom_wide: twice the spread in either mode. */
    const int bloom_noblur = LD_FILE_FLAG("pp_bloom_noblur");
    const int bloom_wide = LD_FILE_FLAG("pp_bloom_wide");
    const int bloom_pyr = !bloom_noblur && !LD_FILE_FLAG("pp_bloom_single") &&
                          ld_ppg_pyramid(g_ppg.bloom_w, g_ppg.bloom_h);
    if (bloom_pyr) ld_ppg_pyramid_draw(bloom_wide ? 2.0f : 1.0f);
    /* Render into the temp target through a transient RTV in slot 0 of the
     * renderer's own RTV heap is not available, so use a small private heap. */
    static ID3D12DescriptorHeap* rtvh;
    if (!rtvh) {
        D3D12_DESCRIPTOR_HEAP_DESC rd = {0}; rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 1;
        if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &rd, &IID_ID3D12DescriptorHeap, (void**)&rtvh))) return 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, g_ppg.out, NULL, rtv);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
    ID3D12DescriptorHeap* heaps[] = { g_ppg.heap };
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 1, heaps);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_ppg.rs);
    g.list->lpVtbl->SetPipelineState(g.list, g_ppg.pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; g_ppg.heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g_ppg.heap, &gh);
    gh.ptr += (UINT64)LD_PPG_DESC_COMPOSITE *
              g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 0, gh);
    {   float gains[4] = {1, 1, 0, 0};
        const unsigned s9 = g_pp_scale9, s10 = g_pp_scale10;
        if (s9) memcpy(&gains[0], &s9, 4);
        if (s10) memcpy(&gains[1], &s10, 4);
        /* pp_nobloom file: drop the bloom term (A/B for flicker). */
        if (LD_FILE_FLAG("pp_nobloom")) gains[1] = 0.0f;
        /* Single-blur radius in mask texels (pp_bloom_single); 0 = the raw
         * mask (pp_bloom_noblur). gains[3]: read the pyramid instead. */
        gains[2] = bloom_noblur ? 0.0f : bloom_wide ? 4.0f : 2.0f;
        gains[3] = bloom_pyr ? 1.0f : 0.0f;
        g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 1, 4, gains, 0); }
    D3D12_VIEWPORT vp = { 0, 0, (float)ld_sx(s, s->w), (float)ld_sx(s, s->h), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)ld_sx(s, s->w), (LONG)ld_sx(s, s->h) };
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    /* Copy the result back over the scene surface the resolve reads. */
    ld_ppg_barrier(g_ppg.out, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ld_ppg_barrier(s->tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->lpVtbl->CopyResource(g.list, s->tex, g_ppg.out);
    ld_ppg_barrier(s->tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_ppg.last_frame = g_ld_frames + 1u;
    g_ppg.applied_slot = slot + 1u; g_ppg.applied_wgen = s->swin_wgen;
    g_pp_gpu_last_ms = GetTickCount();
    static u32 logged;
    if (logged++ < 6 || (logged % 300) == 0)
        fprintf(stderr, "[pp-gpu] applied to surface %u (lut R[32]=%u G[32]=%u B[32]=%u seq=%ld scale9=%08X scale10=%08X)\n",
                slot, g_pp_lut[32], g_pp_lut[256 + 32], g_pp_lut[512 + 32], (long)g_pp_lut_seq,
                g_pp_scale9, g_pp_scale10);
    return 1;
}

int rsx_live_draw_resolve_blit(u32 src_loc, u32 src_abs, u32 src_pitch, u32 ds, u32 dt,
                               u32 dst_loc, u32 dst_abs, u32 dst_pitch,
                               u32 out_x, u32 out_y, u32 out_w, u32 out_h)
{
    if (g_bt_on)
        fprintf(stderr, "[bind-trace] resolve-blit %u:0x%08X/%u ds=0x%X dt=0x%X -> %u:0x%08X/%u out=%u,%u %ux%u\n",
                src_loc, src_abs, src_pitch, ds, dt, dst_loc, dst_abs, dst_pitch, out_x, out_y, out_w, out_h);
    if (!g.ready || !src_pitch || !dst_pitch || !ds || !dt) return 0;
    /* Diagnostic: brightness of the GUEST bytes this blit reads (the SPU may
     * have post-processed them in main memory), once every ~60 calls. */
    /* dump_guest_scene file: write the guest 1280x720 image at 1:0x01400000
     * as two PPMs (bytes 1,2,3 and 3,2,1) to see what the SPU left there. */
    static ULONGLONG dgs_checked;   /* polled at most once a second (was once per blit) */
    if (src_loc == 1u && GetTickCount64() - dgs_checked > 1000 && (dgs_checked = GetTickCount64(), 1) &&
        GetFileAttributesA("dump_guest_scene") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("dump_guest_scene");
        const u8* gp = guest_ptr(1u, 0x01400000u, 5120u * 720u);
        if (gp) {
            for (int v = 0; v < 2; v++) {
                char path[64]; snprintf(path, sizeof path, "guest_scene_%d.ppm", v);
                FILE* f = fopen(path, "wb");
                if (!f) continue;
                fprintf(f, "P6\n1280 720\n255\n");
                for (u32 y = 0; y < 720; y++)
                    for (u32 x = 0; x < 1280; x++) {
                        const u8* p = gp + (size_t)y * 5120 + x * 4;
                        u8 rgb[3] = { v ? p[3] : p[1], p[2], v ? p[1] : p[3] };
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
            }
            fprintf(stderr, "[resolve-src] dumped guest_scene_0/1.ppm\n");
            /* Raw inputs of the same frame for an offline fit of the SPU
             * post-process: host scene (pre-SPU), host 640x360 glow surface,
             * and the guest glow copy (SPU writes into it too). Raw bytes. */
            for (u32 i = 0; i < g.n_surfaces; i++) {
                const surface_t* sv = &g.surfaces[i];
                if ((sv->location == 1u && sv->offset == 0x01400000u) ||
                    (sv->location == 1u && sv->offset >= 0x00A00C00u && sv->offset <= 0x00A11100u) ||
                    (sv->location == 0u && sv->offset == 0x045B0000u) ||
                    (sv->location == 1u && sv->offset == 0x017C0000u)) {
                    char path[64];
                    snprintf(path, sizeof path, "host_%u_%08X.ppm", sv->location, sv->offset);
                    ld_dump_surface_ppm(path, sv);
                }
            }
            { const u8* gg = guest_ptr(1u, 0x017C0000u, 2560u * 360u);
              FILE* f = gg ? fopen("guest_glow.raw", "wb") : NULL;
              if (f) { fwrite(gg, 1, 2560u * 360u, f); fclose(f); } }
            /* What the SPU bloom chain leaves after the glow mask: its PUTs
             * land from 1:0x018A1000 on (320x180 and smaller levels). */
            { const u8* gb = guest_ptr(1u, 0x018A1000u, 0x100000u);
              FILE* f = gb ? fopen("guest_bloom_chain.raw", "wb") : NULL;
              if (f) { fwrite(gb, 1, 0x100000u, f); fclose(f); } }
            { FILE* f = fopen("guest_scene.raw", "wb");
              if (f) { fwrite(gp, 1, 5120u * 720u, f); fclose(f); } }
            /* Find the post-process parameter block: three big-endian floats
             * in a row that look like per-channel gains (~2.0..2.6). */
            { extern uint8_t* vm_base; int hits = 0;
              for (u32 a = 0x00010000u; a < 0x10000000u - 64u && hits < 60; a += 4u) {
                  float f3[3];
                  for (int k = 0; k < 3; k++) {
                      const u8* q = vm_base + a + k * 4u;
                      const u32 bits = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                      memcpy(&f3[k], &bits, 4);
                  }
                  if (!(f3[0] > 1.69f && f3[0] < 1.74f)) continue;
                  hits++;
                  fprintf(stderr, "[pp-param] ea=0x%08X", a - 16u);
                  for (int k = -4; k < 12; k++) {
                      const u8* q = vm_base + a + k * 4;
                      const u32 bits = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                      float fv; memcpy(&fv, &bits, 4);
                      fprintf(stderr, " %g", fv);
                  }
                  fprintf(stderr, "\n");
              } }
        }
    }
    if (src_loc == 1u) {
        static u32 n_probe;
        if ((n_probe++ % 60u) == 0u) {
            const u32 rows = 64u;
            const u8* gp = guest_ptr(src_loc, src_abs - (src_pitch * (rows - 1u)) * ((int32_t)dt < 0), src_pitch * rows);
            if (gp) {
                u64 s[4] = {0,0,0,0}; u32 n = 0;
                for (u32 y = 0; y < rows; y += 4)
                    for (u32 x = 0; x + 4 <= src_pitch; x += 64) {
                        const u8* p = gp + (size_t)y * src_pitch + x;
                        s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; s[3] += p[3]; n++;
                    }
                fprintf(stderr, "[resolve-src] %u:0x%08X guest bytes mean=(%.1f %.1f %.1f %.1f)\n",
                        src_loc, src_abs, n ? (double)s[0] / n : 0, n ? (double)s[1] / n : 0,
                        n ? (double)s[2] / n : 0, n ? (double)s[3] / n : 0);
            }
        }
    }
    /* Uneven or fractional factors cannot be a copy; they are drawn. */
    const int32_t signed_ds = (int32_t)ds, signed_dt = (int32_t)dt;
    const int scaled = signed_ds < 0 || signed_dt < 0 ||
        ds != dt || ds < 0x100000u || (ds & 0xFFFFFu) != 0;
    /* Source surface and the rect's guest position inside it. */
    /* The CLOSEST base at or below the address: surfaces' guest ranges
     * overlap (a stale surface at 0x0 "contains" 0x12804 too), and taking the
     * first match copied every 0x10000 tile out of that black surface. */
    int si = -1; u32 gx = 0, gy = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != src_loc || src_abs < s->offset) continue;
        const u32 d = src_abs - s->offset;
        if (d / src_pitch >= 2u * s->h) continue;
        if (si >= 0 && g.surfaces[si].offset > s->offset) continue;
        if (s->pitch && s->pitch != src_pitch) continue;
        si = (int)i; gx = (d % src_pitch) / ld_surface_texel_bytes(s->fmt); gy = d / src_pitch;
    }
    if (si < 0) return 0;
    /* The source is the scene copy in MAIN memory, which the SPU post-process
     * (bloom, glow, tone) rewrites in place before this resolve. Show those
     * guest bytes, not the pre-SPU host copy: refresh the surface from guest
     * memory once per frame (needs the blit write-back so the SPU had the
     * real scene to work on). */
    pp_tl("resolve", src_loc, src_abs);
    if (src_loc == 1u && !ld_pp_gpu_apply((u32)si) && ld_wb_blit_enabled())
        ld_surface_upload_from_guest((u32)si);
    /* Destination: the display buffer this tile writes into. */
    int bi = -1;
    for (u32 b = 0; b < 8; b++) {
        const display_buffer_t* d = &g.display_buffers[b];
        if (d->valid && d->location == dst_loc && dst_abs >= d->offset &&
            dst_abs < d->offset + d->pitch * d->height) { bi = (int)b; break; }
    }
    if (bi < 0) return 0;
    const display_buffer_t* db = &g.display_buffers[bi];
    const u32 dx = (dst_abs - db->offset) % db->pitch /
        ld_surface_texel_bytes(g.surfaces[si].fmt) + out_x;
    const u32 dy = (dst_abs - db->offset) / db->pitch + out_y;
    const u32 slot = surface_get(db->location, db->offset, db->width, db->height, g.surfaces[si].fmt);
    if (slot == LD_INVALID_SURFACE || (int)slot == si) return 0;
    surface_t* dst = &g.surfaces[slot];
    if (scaled) {
        /* Source extent in the blit's source units, which match host pixels
         * for a surface the host renders at its guest size. */
        u32 w = out_w, h = out_h;
        if (dx >= dst->w || dy >= dst->h) return 0;
        if (dx + w > dst->w) w = dst->w - dx;
        if (dy + h > dst->h) h = dst->h - dy;
        /* NV3089 increments are signed 12.20. A -1 Y increment from row
         * 719 must sample 719,718,...,0, not a huge positive extent. The
         * fullscreen triangle interpolates at pixel centres; move a reflected
         * edge so its first sample still lands at the requested texel centre.
         * Use the clipped destination extent: keeping out_w/out_h here would
         * stretch the entire source interval into the surviving clipped rect. */
        float sx, sy, sw, sh;
        ld_nv3089_signed_axis(gx, ds, w, &sx, &sw);
        ld_nv3089_signed_axis(gy, dt, h, &sy, &sh);
        return ld_scaled_blit((u32)si, sx, sy, sw, sh, slot, dx, dy, w, h);
    }
    if (dst->fmt != g.surfaces[si].fmt) return 0;
    const u32 k = ds >> 20;                       /* 20.12 fixed -> integer factor */
    const u32 sx = gx, sy = gy;
    u32 w = out_w, h = out_h;
    if (dx >= dst->w || dy >= dst->h || sx >= g.surfaces[si].w || sy >= g.surfaces[si].h) return 0;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (sx + w * k > g.surfaces[si].w) w = (g.surfaces[si].w - sx) / k;
    if (sy + h * k > g.surfaces[si].h) h = (g.surfaces[si].h - sy) / k;
    if (!w || !h) return 0;
    /* Render scale: a copy between surfaces of different host scale is a
     * draw; between equally scaled ones it runs in host pixels. */
    if (g.surfaces[si].scaled != dst->scaled ||
        (k == 1 && !ld_copy_span_matches(&g.surfaces[si], dst, sx, sy, dx, dy, w, h)))
        return ld_scaled_blit((u32)si, (float)sx, (float)sy, (float)(w * k), (float)(h * k), slot, dx, dy, w, h);
    const u32 hsx = ld_sx(dst, sx), hsy = ld_sx(dst, sy), hdx = ld_sx(dst, dx), hdy = ld_sx(dst, dy);
    /* Extent from the scaled destination end point (see ld_scaled_blit),
     * kept inside both host resources. */
    u32 hw = ld_sx(dst, dx + w) - hdx, hh = ld_sx(dst, dy + h) - hdy;
    if (hsx + hw > ld_host_w(&g.surfaces[si])) hw = ld_host_w(&g.surfaces[si]) > hsx ? ld_host_w(&g.surfaces[si]) - hsx : 0;
    if (hsy + hh > ld_host_h(&g.surfaces[si])) hh = ld_host_h(&g.surfaces[si]) > hsy ? ld_host_h(&g.surfaces[si]) - hsy : 0;
    if (hdx + hw > ld_host_w(dst)) hw = ld_host_w(dst) > hdx ? ld_host_w(dst) - hdx : 0;
    if (hdy + hh > ld_host_h(dst)) hh = ld_host_h(dst) > hdy ? ld_host_h(dst) - hdy : 0;
    if (!hw || !hh) return 0;
    if (k > 1) {
        if (!ld_downsample((u32)si, hsx, hsy, k, slot, hdx, hdy, hw, hh)) return 0;
        ld_surface_note_write(slot, LD_SURFACE_WRITE_COPY);
        return 1;
    }
    ID3D12Resource* s = g.surfaces[si].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int q = 0; q < 2; q++) {
        b[q].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[q].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[q].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = s;        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst->tex; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    D3D12_TEXTURE_COPY_LOCATION sl = {0}, dl = {0};
    sl.pResource = s;        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.pResource = dst->tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { hsx, hsy, 0, hsx + hw, hsy + hh, 1 };
    g.list->lpVtbl->CopyTextureRegion(g.list, &dl, hdx, hdy, 0, &sl, &box);
    ld_maa_copy((u32)si,(float)sx,(float)sy,(float)w,(float)h,slot,dx,dy,w,h,"resolve_copy_recorded",0);
    for (int q = 0; q < 2; q++) {
        b[q].Transition.StateBefore = b[q].Transition.StateAfter;
        b[q].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    ld_surface_note_write(slot, LD_SURFACE_WRITE_COPY);
    ld_consumer_surface_event("resolve_copy_complete",slot,(u32)si);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#resolve,%u,%u:0x%X,%u:0x%X,%u,%u,%ux%u\n", g_ld_frames,
                src_loc, src_abs, dst_loc, dst_abs, sx, sy, w, h);
        fflush(s_csv_file);
    }
    return 1;
}

void rsx_live_draw_note_resolve(u32 src_loc, u32 src_abs, u32 dst_loc, u32 dst_abs)
{
    if (!g.ready) return;
    for (u32 b = 0; b < 8; b++) {
        display_buffer_t* d = &g.display_buffers[b];
        if (!d->valid || d->location != dst_loc || d->offset != dst_abs) continue;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const surface_t* s = &g.surfaces[i];
            if (s->tex && s->location == src_loc && s->offset == src_abs) {
                if (d->resolve_src != (int)i + 1)
                    fprintf(stderr, "[live-draw] display buffer %u resolves from surface %u (0x%X)\n",
                            b, i, s->offset);
                d->resolve_src = (int)i + 1;
                return;
            }
        }
    }
}

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    if (!g.ready || !src_pitch || !dst_pitch || !w || !h) return 0;
    u32 sx, sy, dx = 0, dy = 0;
    const int si = surface_containing(src_loc, src_abs, src_pitch, w, h, &sx, &sy);
    if (si < 0) return -1;            /* source is not a live surface */
    int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
    if (di < 0) {
        const u32 slot = surface_get(dst_loc, dst_abs,
                                     dst_pitch / ld_surface_texel_bytes(g.surfaces[si].fmt), h,
                                     g.surfaces[si].fmt);
        if (slot == LD_INVALID_SURFACE) return -2;
        di = (int)slot;
        if (w > g.surfaces[di].w || h > g.surfaces[di].h) return -3;
    }
    if (di == si) return -4;
    if (g.surfaces[di].fmt != g.surfaces[si].fmt) return -5;
    /* Render scale: different host scales need a draw; equal ones copy in
     * host pixels. The write-back below takes guest coordinates. */
    if (g.surfaces[si].scaled != g.surfaces[di].scaled ||
        !ld_copy_span_matches(&g.surfaces[si], &g.surfaces[di], sx, sy, dx, dy, w, h)) {
        if (!ld_scaled_blit((u32)si, (float)sx, (float)sy, (float)w, (float)h, (u32)di, dx, dy, w, h)) return -6;
        ld_wb_record((u32)di, dx, dy, w, h, dst_loc, dst_abs, dst_pitch);
        return 1;
    }
    const u32 hsx = ld_sx(&g.surfaces[si], sx), hsy = ld_sx(&g.surfaces[si], sy);
    const u32 hdx = ld_sx(&g.surfaces[di], dx), hdy = ld_sx(&g.surfaces[di], dy);
    /* Extent from the scaled destination end point (see ld_scaled_blit),
     * kept inside both host resources. */
    u32 hw = ld_sx(&g.surfaces[di], dx + w) - hdx, hh = ld_sx(&g.surfaces[di], dy + h) - hdy;
    if (hsx + hw > ld_host_w(&g.surfaces[si])) hw = ld_host_w(&g.surfaces[si]) > hsx ? ld_host_w(&g.surfaces[si]) - hsx : 0;
    if (hsy + hh > ld_host_h(&g.surfaces[si])) hh = ld_host_h(&g.surfaces[si]) > hsy ? ld_host_h(&g.surfaces[si]) - hsy : 0;
    if (hdx + hw > ld_host_w(&g.surfaces[di])) hw = ld_host_w(&g.surfaces[di]) > hdx ? ld_host_w(&g.surfaces[di]) - hdx : 0;
    if (hdy + hh > ld_host_h(&g.surfaces[di])) hh = ld_host_h(&g.surfaces[di]) > hdy ? ld_host_h(&g.surfaces[di]) - hdy : 0;
    if (!hw || !hh) return -7;
    ID3D12Resource* src = g.surfaces[si].tex;
    ID3D12Resource* dst = g.surfaces[di].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = src; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    D3D12_TEXTURE_COPY_LOCATION s = {0}, d = {0};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { hsx, hsy, 0, hsx + hw, hsy + hh, 1 };
    g.list->lpVtbl->CopyTextureRegion(g.list, &d, hdx, hdy, 0, &s, &box);
    ld_maa_copy((u32)si,(float)sx,(float)sy,(float)w,(float)h,(u32)di,dx,dy,w,h,"blit_copy_recorded",0);
    for (int k = 0; k < 2; k++) {
        b[k].Transition.StateBefore = b[k].Transition.StateAfter;
        b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, 2, b);
    ld_surface_note_write((u32)di, LD_SURFACE_WRITE_COPY);
    ld_wb_record((u32)di, dx, dy, w, h, dst_loc, dst_abs, dst_pitch);
    return 1;
}

/* Blit write-back. An NV3089 blit from a render target into MAIN memory is
 * done as a GPU copy into a host surface, so the guest bytes at the
 * destination never change. Macross 30 copies its scene there for the SPUs
 * (env cube source, reflection blur): they read black, and every metal and
 * water reflection built from it was black. Each such blit also copies the
 * destination into a readback buffer; once the fence for that command list
 * has passed (checked after every flush, which already waits), the texels are
 * stored to guest memory as A8R8G8B8. No extra GPU wait; the guest sees the
 * data at the next flush. A no_blit_writeback file in the working directory
 * turns it off (A/B). */
#define LD_WB_SLOTS 12
static struct {
    ID3D12Resource* rb; u64 rb_size;
    u32 loc, off, dpitch, w, h, rbpitch, texel;
    DXGI_FORMAT fmt;
    u64 fence; int pending;
} g_wb[LD_WB_SLOTS];
static unsigned long long g_wb_done, g_wb_skipped;

/* Runtime A/B switches, files in the working directory, checked ~1/s. */
static int ld_wb_file(const char* name, int* cache, DWORD* last)
{
    const DWORD now = GetTickCount();
    if (*cache < 0 || now - *last > 1000u) {
        const int was = *cache;
        *last = now;
        *cache = GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
        if (was >= 0 && was != *cache)
            fprintf(stderr, "[guest-wb] %s %s\n", name, *cache ? "present" : "removed");
    }
    return *cache;
}
/* Blit write-back is on unless a no_blit_writeback file exists: the SPU
 * post-process (bloom, glow, tone) reads the scene copy it produces. Main-memory render-target write-back
 * is on unless no_surface_writeback exists. */
static int ld_wb_blit_enabled(void)
{
    static int v = -1; static DWORD last;
    return ld_wb_file("blit_writeback", &v, &last);   /* opt-in until the GPU port: tiles race */
}
static int ld_wb_surface_disabled(void)
{
    static int v = -1; static DWORD last;
    return ld_wb_file("no_surface_writeback", &v, &last);
}

static u32 ld_wb_texel(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return 4;
    case DXGI_FORMAT_R32_FLOAT:          return 4;
    case DXGI_FORMAT_R8_UNORM:           return 1;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default:                             return 0;
    }
}

/* Render scale: what the guest reads back must be guest-sized. Draw the guest
 * rect (gx,gy,w,h) of render-scaled surface si into a private guest-size
 * target and leave it in COPY_SOURCE at (0,0); ld_rs_tmp_release returns it
 * to RENDER_TARGET after the caller's copy. NULL when unavailable. */
static struct { ID3D12Resource* tex; DXGI_FORMAT fmt; u32 w, h; ID3D12DescriptorHeap* rtvh; } g_rs_tmp[4];
static ID3D12Resource* ld_rs_downscale(u32 si, u32 gx, u32 gy, u32 w, u32 h)
{
   
    const surface_t* s = &g.surfaces[si];
    ID3D12PipelineState* pso = ld_scaled_blit_pso(s->fmt);
    if (!pso || !w || !h) return NULL;
    int k = -1;
    for (int i = 0; i < 4; i++) if (g_rs_tmp[i].tex && g_rs_tmp[i].fmt == s->fmt) { k = i; break; }
    if (k < 0) for (int i = 0; i < 4; i++) if (!g_rs_tmp[i].tex) { k = i; break; }
    if (k < 0) return NULL;
    if (!g_rs_tmp[k].tex || g_rs_tmp[k].w < w || g_rs_tmp[k].h < h) {
        if (g_rs_tmp[k].tex) retire_texture(g_rs_tmp[k].tex);
        g_rs_tmp[k].tex = NULL;
        const u32 nw = w > g_rs_tmp[k].w ? w : g_rs_tmp[k].w, nh = h > g_rs_tmp[k].h ? h : g_rs_tmp[k].h;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = nw; rd.Height = nh;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = s->fmt; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void**)&g_rs_tmp[k].tex))) {
            g_rs_tmp[k].tex = NULL; return NULL;
        }
        g_rs_tmp[k].fmt = s->fmt; g_rs_tmp[k].w = nw; g_rs_tmp[k].h = nh;
        if (!g_rs_tmp[k].rtvh) {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
            if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap,
                    (void**)&g_rs_tmp[k].rtvh))) { g_rs_tmp[k].rtvh = NULL; return NULL; }
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    g_rs_tmp[k].rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_rs_tmp[k].rtvh, &rtv);
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, g_rs_tmp[k].tex, NULL, rtv);
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots, 0);
    if (!table.ptr) return NULL;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = s->tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_sblit_rs);
    const float r[4] = { gx / (float)s->w, gy / (float)s->h, (gx + w) / (float)s->w, (gy + h) / (float)s->h };
    g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 4, r, 0);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    b.Transition.pResource = g_rs_tmp[k].tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    return g_rs_tmp[k].tex;
}
static void ld_rs_tmp_release(ID3D12Resource* t)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = t;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
}
static void ld_wb_record_any(u32 di, u32 dx, u32 dy, u32 w, u32 h, u32 dst_loc, u32 dst_abs, u32 dst_pitch)
{
    const surface_t* sf = &g.surfaces[di];
    const u32 texel = ld_wb_texel(sf->fmt);
    if (!texel || !sf->tex || !w || !h || dst_pitch < w * texel) { g_wb_skipped++; return; }
    int k = -1;
    for (int i = 0; i < LD_WB_SLOTS; i++)
        if (g_wb[i].rb && g_wb[i].loc == dst_loc && g_wb[i].off == dst_abs) { k = i; break; }
    if (k < 0) for (int i = 0; i < LD_WB_SLOTS; i++) if (!g_wb[i].pending) { k = i; break; }
    if (k < 0) { g_wb_skipped++; return; }
    if (g_wb[k].pending) { g_wb_skipped++; return; }   /* previous copy not stored yet */
    const u32 rbpitch = (w * texel + 255u) & ~255u;
    const u64 need = (u64)rbpitch * h;
    if (!g_wb[k].rb || g_wb[k].rb_size < need) {
        if (g_wb[k].rb) g_wb[k].rb->lpVtbl->Release(g_wb[k].rb);
        g_wb[k].rb = NULL;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = need;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&g_wb[k].rb))) {
            g_wb[k].rb = NULL; g_wb_skipped++; return;
        }
        g_wb[k].rb_size = need;
    }
    /* Render scale: read a guest-size downscale of a scaled surface. */
    ID3D12Resource* const down = sf->scaled ? ld_rs_downscale(di, dx, dy, w, h) : NULL;
    if (sf->scaled && !down) { g_wb_skipped++; return; }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = sf->tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!down) g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = down ? down : sf->tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g_wb[k].rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = sf->fmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rbpitch;
    D3D12_BOX box = { down ? 0 : dx, down ? 0 : dy, 0, (down ? 0 : dx) + w, (down ? 0 : dy) + h, 1 };
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, &box);
    if (down) ld_rs_tmp_release(down);
    else {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    }
    g_wb[k].loc = dst_loc; g_wb[k].off = dst_abs; g_wb[k].dpitch = dst_pitch;
    g_wb[k].w = w; g_wb[k].h = h; g_wb[k].rbpitch = rbpitch;
    g_wb[k].texel = texel; g_wb[k].fmt = sf->fmt;
    g_wb[k].fence = g.fence_value + 1;   /* signalled by the flush that submits this list */
    g_wb[k].pending = 1;
    pp_tl("wb-record", dst_loc, dst_abs);
}

/* NV3089 blit into main memory (opt-in, see above). */
static void ld_wb_record(u32 di, u32 dx, u32 dy, u32 w, u32 h, u32 dst_loc, u32 dst_abs, u32 dst_pitch)
{
    if (dst_loc != 1u) return;
    /* The bloom mask blit (main 0x017C0000, 640x360) is always stored: the
     * SPU bloom chain blurs it in place and the final composite reads the
     * result. Without it the chain saw zeros and the composite fell back to
     * the unblurred mask (bright objects burned out). no_bloom_writeback: off. */
    /* With the GPU post-process the host surface feeds the bloom directly
     * and the SPU chain's result is never shown: skip the readback then. */
    static int nbw = -1; static DWORD nbw_last;
    const int bloom = dst_abs == 0x017C0000u && ld_ppg_disabled() &&
                      !ld_wb_file("no_bloom_writeback", &nbw, &nbw_last);
    if (!bloom && !ld_wb_blit_enabled()) return;
    ld_wb_record_any(di, dx, dy, w, h, dst_loc, dst_abs, dst_pitch);
}

/* Render target in MAIN memory. RSX writes such a target where the CPU can
 * read it; here the pixels only exist in the host surface. Macross 30 reduces
 * the scene to a 1x1 R32F average luminance at 1:0x00A11000 and the game's
 * CPU side reads it to drive auto-exposure -- it read whatever the guest
 * memory held, and the scene came out dull and dark. Called when a pass
 * leaves a main-memory target; the store lands at the next flush. */
static void ld_wb_surface_pass_done(u32 slot)
{
    if (slot >= g.n_surfaces) return;
    const surface_t* sf = &g.surfaces[slot];
    if (sf->location != 1u || ld_wb_surface_disabled()) return;
    const u32 texel = ld_wb_texel(sf->fmt);
    if (!texel) { g_wb_skipped++; return; }
    const u32 pitch = sf->pitch ? sf->pitch : sf->w * texel;
    ld_wb_record_any(slot, 0, 0, sf->w, sf->h, sf->location, sf->offset, pitch);
}

/* pp_timeline file in the working directory: log the next 600 post-process
 * ordering events (blit write-back, label, SPU tile I/O, resolve) with a
 * microsecond clock. Diagnostic for the SPU post-process round trip. */
void pp_tl(const char* tag, unsigned a, unsigned b)
{
    static volatile LONG armed, left; static LARGE_INTEGER t0, fq; static DWORD chk;
    if (!armed) {
        const DWORD now = GetTickCount();
        if (now - chk < 300u) return;
        chk = now;
        if (GetFileAttributesA("pp_timeline") == INVALID_FILE_ATTRIBUTES) return;
        DeleteFileA("pp_timeline"); QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
        left = 600; armed = 1;
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    /* SPU tile traffic is thousands of events: print a burst start, and the
     * last SPU event before any other event. */
    static LONGLONG spu_last; static unsigned spu_n; static char spu_tag[16];
    const double us = (double)(t.QuadPart - t0.QuadPart) * 1e6 / (double)fq.QuadPart;
    if (tag[0] == 's' && tag[1] == 'p' && tag[2] == 'u') {
        const int burst = !spu_last || (t.QuadPart - spu_last) * 1000000 / fq.QuadPart > 300;
        spu_last = t.QuadPart; spu_n++; snprintf(spu_tag, sizeof spu_tag, "%s", tag);
        if (!burst) return;
        spu_n = 1;
    } else if (spu_n > 1) {
        fprintf(stderr, "[pp-tl] %10.1f us %-12s n=%u\n", (double)(spu_last - t0.QuadPart) * 1e6 / (double)fq.QuadPart, "spu-last", spu_n);
        spu_n = 0;
    }
    (void)us;
    if (InterlockedDecrement(&left) < 0) return;
    fprintf(stderr, "[pp-tl] %10.1f us %-12s %08X %08X tid=%lu\n",
            (double)(t.QuadPart - t0.QuadPart) * 1e6 / (double)fq.QuadPart, tag, a, b, GetCurrentThreadId());
}
static void ld_wb_poll(void)
{
    if (!g.fence) return;
    const u64 done = g.fence->lpVtbl->GetCompletedValue(g.fence);
    for (int k = 0; k < LD_WB_SLOTS; k++) {
        if (!g_wb[k].pending || done < g_wb[k].fence) continue;
        g_wb[k].pending = 0;
        const u32 texel = g_wb[k].texel;
        const u64 span = (u64)g_wb[k].dpitch * (g_wb[k].h - 1) + (u64)g_wb[k].w * texel;
        u8* gp = (u8*)guest_ptr(g_wb[k].loc, g_wb[k].off, (u32)span);
        if (!gp) { g_wb_skipped++; continue; }
        u8* px = NULL; D3D12_RANGE rr = {0, (SIZE_T)((u64)g_wb[k].rbpitch * g_wb[k].h)};
        if (FAILED(g_wb[k].rb->lpVtbl->Map(g_wb[k].rb, 0, &rr, (void**)&px))) { g_wb_skipped++; continue; }
        for (u32 y = 0; y < g_wb[k].h; y++) {
            const u8* s = px + (size_t)y * g_wb[k].rbpitch;
            u8* d = gp + (size_t)y * g_wb[k].dpitch;
            switch (g_wb[k].fmt) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: {  /* RGBA -> A,R,G,B: byte rotate */
                const u32* sw = (const u32*)s; u32* dw = (u32*)d;
                for (u32 x = 0; x < g_wb[k].w; x++) { const u32 v = sw[x]; dw[x] = (v << 8) | (v >> 24); }
                break;
            }
            case DXGI_FORMAT_R32_FLOAT:         /* little -> big endian */
                for (u32 x = 0; x < g_wb[k].w; x++, s += 4, d += 4) {
                    d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
                }
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: /* each half big endian, RGBA order */
                for (u32 x = 0; x < g_wb[k].w * 4u; x++, s += 2, d += 2) {
                    d[0] = s[1]; d[1] = s[0];
                }
                break;
            default:                            /* R8 */
                memcpy(d, s, g_wb[k].w);
                break;
            }
        }
        D3D12_RANGE wr = {0, 0};
        g_wb[k].rb->lpVtbl->Unmap(g_wb[k].rb, 0, &wr);
        rsx_live_draw_note_guest_write(g_wb[k].loc, g_wb[k].off, (u32)span);
        pp_tl("wb-store", g_wb[k].loc, g_wb[k].off);
        if (g_wb_done++ < 12) {
            fprintf(stderr, "[guest-wb] stored %u:0x%08X %ux%u fmt=%d pitch=%u first:",
                    g_wb[k].loc, g_wb[k].off, g_wb[k].w, g_wb[k].h, (int)g_wb[k].fmt, g_wb[k].dpitch);
            for (u32 i = 0; i < 8 && i < g_wb[k].w * texel; i++) fprintf(stderr, " %02X", gp[i]);
            fprintf(stderr, "\n");
        }
    }
}
/* Called before the RSX publishes a label/semaphore. A guest that waits on
 * the label may then read the main-memory output of an earlier blit (the SPU
 * post-process reads the scene copy right after it): make that write-back
 * land first, as the RSX had finished the copy before writing the label. */
void rsx_live_draw_before_label(void)
{
    if (!g.ready) return;
    /* The 1x1 luminance (1:0x00A11000) feeds exposure, which tolerates it a
     * frame late; waiting for it drained the GPU at every label -- 221 of 256
     * frames stalled the renderer (async-frame-counts reason 7). Flush only
     * for the other write-backs (HUD surfaces sampled in the same frame).
     * wb_flush_all file: flush for the luminance too (previous behaviour). */
    int pending = 0, urgent = 0;
    for (int k = 0; k < LD_WB_SLOTS; k++)
        if (g_wb[k].pending) {
            pending = 1;
            if (!(g_wb[k].loc == 1u && g_wb[k].off == 0x00A11000u)) urgent = 1;
        }
    if (!pending) return;
    if (g_af.on && !urgent) { ld_wb_poll(); return; }
    /* With frames in flight a mid-frame flush splits the frame (its present
     * pairing broke: HUD/logo flicker). Deliver finished readbacks only; the
     * luminance reaches the guest a frame later, which exposure tolerates. */
    /* With frames in flight this still flushes: the HUD draws text, the radar
     * map and gauge icons into main-memory surfaces and samples them back as
     * textures in the same frame, so the write-back must land before the
     * label (deferred, they rendered empty). LD_ASYNC_WB_FLUSH=0: poll only. */
    { static int wbf = -1; if (wbf < 0) { const char* e = getenv("LD_ASYNC_WB_FLUSH"); wbf = e ? atoi(e) : 1; }
      if (g_af.on && !wbf) { ld_wb_poll(); return; } }
    pp_tl("label-flush", 0, 0);
    ld_flush(LD_FLUSH_READBACK);
    ld_wb_poll();
}

/* Save-icon capture. Macross 30's field save draws a 320x176 thumbnail into a
 * LOCAL target (0:0x04D30000), ends that pass with user command 0x3A7B1C00,
 * and once the command's handler opens its gate the CPU copies the rows
 * straight out of local memory (func_000EC26C: local base + the texture's
 * offset, pitch from the texture) into the buffer it hands cellPngEnc. Local
 * surfaces are never written back, so the icon came out black (mean=0). On
 * that command, store the latest small colour target to its guest bytes and
 * wait for it: the FIFO walker calls this before it queues the command, so
 * the handler cannot run first. Once per save. no_capture_writeback: off. */
void rsx_live_draw_user_command(u32 cmd)
{
    if (!g.ready || cmd != 0x3A7B1C00u) return;
    if (LD_FILE_FLAG("no_capture_writeback")) return;
    const u32 si = g_ld_last_ct.slot;
    if (si >= g.n_surfaces) return;
    const surface_t* sf = &g.surfaces[si];
    if (!sf->tex || sf->location != g_ld_last_ct.location || sf->offset != g_ld_last_ct.offset ||
        sf->w > 640u || sf->h > 360u) {
        fprintf(stderr, "[capture-wb] cmd %08X: no small colour target (slot %u)\n", cmd, si);
        return;
    }
    const u32 texel = ld_wb_texel(sf->fmt);
    if (!texel) return;
    const u32 pitch = sf->pitch ? sf->pitch : sf->w * texel;
    const unsigned long long done0 = g_wb_done;
    ld_wb_record_any(si, 0, 0, sf->w, sf->h, sf->location, sf->offset, pitch);
    ld_flush(LD_FLUSH_READBACK);
    ld_wb_poll();
    fprintf(stderr, "[capture-wb] cmd %08X: stored %u:0x%08X %ux%u pitch=%u scaled=%u %s\n", cmd,
            sf->location, sf->offset, sf->w, sf->h, pitch, sf->scaled,
            g_wb_done > done0 ? "ok" : "FAILED");
}

/* RSX keeps colour and zeta in the same memory, and Macross 30 relies on it:
 * before its water-reflection pass it DRAWS A8R8G8B8 colour into the 640x360
 * buffer it then binds as the Z24S8 zeta. Those bytes become the depth (A,R,G)
 * and the stencil (B) that the reflection draws test (stencil EQUAL 0xFF). Here
 * colour and zeta are separate resources, so the stencil stayed 0 and every
 * reflection pixel failed -- the water reflected nothing. When a zeta is bound
 * and a colour surface at the same address was really drawn after the zeta's
 * last depth/stencil write, reinterpret that colour into depth+stencil with a
 * full-screen pass. A no_zeta_import file in the working directory disables it. */
/* Without PS stencil-ref support (SV_StencilRef) the stencil byte is written
 * one bit plane per pass: pass k uses StencilWriteMask = 1<<k, REPLACE with
 * reference 0xFF, and discards pixels whose bit k is clear. Plane 0 also
 * writes depth and starts from a stencil cleared to 0 by the caller. */
static ID3D12PipelineState* g_zimp_pso[8];
static ID3D12PipelineState* g_zimp_depth_pso;
static int g_zimp_state;   /* 0 untried, 1 ready, -1 unavailable */
static int ld_zimp_disabled(void)
{
    static int v = -1; static DWORD last;
    const DWORD now = GetTickCount();
    if (v < 0 || now - last > 1000u) {
        const int was = v; last = now;
        v = GetFileAttributesA("no_zeta_import") != INVALID_FILE_ATTRIBUTES;
        if (was >= 0 && was != v) fprintf(stderr, "[zeta-import] %s\n", v ? "OFF" : "ON");
    }
    return v;
}
static int ld_zimp_build(void)
{
    if (g_zimp_state) return g_zimp_state > 0;
    g_zimp_state = -1;
    if (!ld_scaled_blit_pso(DXGI_FORMAT_R8G8B8A8_UNORM)) return 0;   /* builds g_sblit_rs */
    static const char src[] =
        "cbuffer C : register(b0) { float4 r; };\n"
        "Texture2D t0 : register(t0);\n"
        "struct V { float4 p : SV_Position; nointerpolation float bit : TEXCOORD0; nointerpolation float mode : TEXCOORD1; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "    V o; float2 q = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    o.bit = r.x; o.mode = r.y; return o; }\n"
        "float ps(V i) : SV_Depth {\n"
        "    float4 c = t0.Load(int3(int2(i.p.xy), 0));\n"
        "    uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        "    uint st = i.mode > 0.5 ? b.a : b.b;\n"
        "    if (((st >> (uint)i.bit) & 1u) == 0u) discard;\n"
        "    uint d = i.mode > 0.5 ? ((b.r << 16) | (b.g << 8) | b.b) : ((b.a << 16) | (b.r << 8) | b.g);\n"
        "    return (float)d / 16777215.0; }\n"
        "float ps_depth(V i) : SV_Depth {\n"
        "    float4 c = t0.Load(int3(int2(i.p.xy), 0));\n"
        "    uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        "    uint d = i.mode > 0.5 ? ((b.r << 16) | (b.g << 8) | b.b) : ((b.a << 16) | (b.r << 8) | b.g);\n"
        "    return (float)d / 16777215.0; }\n";
    ID3DBlob* vs = NULL; ID3DBlob* ps = NULL; ID3DBlob* psd = NULL; ID3DBlob* err = NULL;
    if (FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "zeta_import", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "zeta_import", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err)) ||
        FAILED(ld_builtin_compile(src, sizeof(src) - 1u, "zeta_import", NULL, NULL, "ps_depth", "ps_5_0", 0, 0, &psd, &err))) {
        fprintf(stderr, "[zeta-import] compile failed: %s\n",
                err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vs) vs->lpVtbl->Release(vs);
        if (ps) ps->lpVtbl->Release(ps);
        return 0;
    }
    int ok = 1;
    /* slot 0..7: stencil bit planes (no depth write); the depth pass is index 8 below. */
    for (int k = 0; k < 8 && ok; k++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = g_sblit_rs;
        pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = 0xFF;
        pd.DepthStencilState.StencilWriteMask = (UINT8)(1u << k);
        pd.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        pd.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pd.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_REPLACE;
        pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.SampleDesc.Count = 1;
        if (FAILED(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                                              (void**)&g_zimp_pso[k]))) ok = 0;
    }
    if (ok) {
        static ID3D12PipelineState* depth_pso;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = g_sblit_rs;
        pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode = psd->lpVtbl->GetBufferPointer(psd); pd.PS.BytecodeLength = psd->lpVtbl->GetBufferSize(psd);
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.SampleDesc.Count = 1;
        if (FAILED(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState,
                                                              (void**)&depth_pso))) ok = 0;
        g_zimp_depth_pso = depth_pso;
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); psd->lpVtbl->Release(psd);
    if (!ok) { fprintf(stderr, "[zeta-import] PSO creation failed\n"); return 0; }
    g_zimp_state = 1;
    return 1;
}
static void ld_zeta_import_color(u32 zslot)
{
    if (!zslot || zslot > g.n_zdepths || ld_zimp_disabled()) return;
    zdepth_t* z = &g.zdepths[zslot - 1];
    int si = -1;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* c = &g.surfaces[i];
        if (!c->tex || c->location != z->location || c->offset != z->offset ||
            c->fmt != DXGI_FORMAT_R8G8B8A8_UNORM || !c->ct_draw_gen) continue;
        if (si < 0 || c->ct_draw_gen > g.surfaces[si].ct_draw_gen) si = (int)i;
    }
    if (si < 0) return;
    const surface_t* c = &g.surfaces[si];
    if (c->ct_draw_gen <= z->color_import_gen || c->ct_draw_gen <= z->ds_write_gen) return;
    z->color_import_gen = c->ct_draw_gen;
    if (!ld_zimp_build()) return;
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + (u32)si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots, 0);
    if (!table.ptr) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = c->tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
    /* The shader Loads the colour at SV_Position, i.e. host pixels: cover the
     * colour's HOST extent (guest w/h imported only the top-left 1/scale of
     * a render-scaled colour). */
    const u32 cw = ld_sx(c, c->w), ch = ld_sx(c, c->h);
    D3D12_RECT sc = { 0, 0, (LONG)cw, (LONG)ch };
    g.list->lpVtbl->ClearDepthStencilView(g.list, dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &sc);
    g.list->lpVtbl->OMSetRenderTargets(g.list, 0, NULL, FALSE, &dsv);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_sblit_rs);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    D3D12_VIEWPORT vp = { 0, 0, (float)cw, (float)ch, 0.0f, 1.0f };
    g.list->lpVtbl->RSSetViewports(g.list, 1, &vp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->lpVtbl->OMSetStencilRef(g.list, 0xFF);
    /* zimp_alt file: read the zeta bytes as S8D24 (stencil = A, depth = RGB)
     * instead of D24S8 (depth = ARG, stencil = B). */
    static int zalt; static DWORD zalt_chk;
    { const DWORD now = GetTickCount();
      if (now - zalt_chk > 500u) { zalt_chk = now; const int was = zalt;
        zalt = GetFileAttributesA("zimp_alt") != INVALID_FILE_ATTRIBUTES;
        if (was != zalt) fprintf(stderr, "[zeta-import] layout %s\n", zalt ? "S8D24 (stencil=A)" : "D24S8 (stencil=B)"); } }
    { const float r0[4] = {0, zalt ? 1.0f : 0.0f, 0, 0};
      g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 4, r0, 0); }
    g.list->lpVtbl->SetPipelineState(g.list, g_zimp_depth_pso);
    g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    for (u32 k = 0; k < 8; k++) {
        const float r[4] = {(float)k, zalt ? 1.0f : 0.0f, 0, 0};
        g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 4, r, 0);
        g.list->lpVtbl->SetPipelineState(g.list, g_zimp_pso[k]);
        g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    z->had_write = 1;
    z->packed_generation = UINT64_MAX;
    static u32 logged;
    if (logged++ < 4)
        fprintf(stderr, "[zeta-import] colour surface %d %u:0x%08X %ux%u -> zeta #%u depth+stencil\n",
                si, c->location, c->offset, c->w, c->h, zslot);
}

static u32 current_surface(void)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    const u32 slot = surface_get(sf.color_location[0], sf.color_offset[0],
                                 sf.clip_w, sf.clip_h,
                                 ld_surface_format(sf.color_format));
    if (slot != LD_INVALID_SURFACE) {
        g.surfaces[slot].pitch = sf.color_pitch[0];
        g.surfaces[slot].raster_type = sf.raster_type;
    }
    /* A pass into a main-memory target just ended: store it for the CPU. */
    { static u32 last = LD_INVALID_SURFACE;
      if (last != slot && last != LD_INVALID_SURFACE) ld_wb_surface_pass_done(last);
      last = slot; }
    return slot;
}

/* On RSX each zeta (depth) address is distinct memory. The offline renderer's
 * s31 fix proved that sharing one D3D depth resource across all of this game's
 * shadow, scene and post-processing passes cross-contaminates later depth
 * tests. Keep the legacy shared target only as a bounded allocation fallback. */
static int honor_zeta_track(void)
{
    static int initialized = 0;
    static int enabled = 1;
    if (!initialized) {
        initialized = 1;
        enabled = getenv("RSX_NO_ZETA_TRACK") ? 0 : 1;
        fprintf(stderr, "[zetatrack] live per-zeta depth %s\n",
                enabled ? "ON" : "OFF (legacy shared target)");
    }
    return enabled;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* Snapshot dimensions describe the latest producer, not the largest DSV
 * allocation. The same guest address alternates between scaled scene depth
 * and unscaled square shadow maps. Read-only binds must retain the producer's
 * dimensions; otherwise normalized shadow UVs sample padded/old pixels. */
static void ld_depth_snapshot_extent(u32 backing_w, u32 backing_h,
                                     u32 rt_w, u32 rt_h, u32* w, u32* h)
{
    *w = rt_w && rt_w < backing_w ? rt_w : backing_w;
    *h = rt_h && rt_h < backing_h ? rt_h : backing_h;
}
static void zdepth_producer_snapshot(u32 i, u32 rt_w, u32 rt_h, u32 scaled)
{
    zdepth_t* z = &g.zdepths[i];
    if (!z->snapshot) return;
    u32 nw, nh;
    ld_depth_snapshot_extent(z->w, z->h, rt_w, rt_h, &nw, &nh);
    if (nw == z->snapshot_w && nh == z->snapshot_h) {
        z->scaled = scaled;
        return;
    }
    int spare = -1;
    for (u32 k = 0; k < 3; ++k)
        if (z->snapshot_spare[k] && z->snapshot_spare_w[k] == nw &&
            z->snapshot_spare_h[k] == nh) { spare = (int)k; break; }
    ID3D12Resource* snap = spare >= 0 ? z->snapshot_spare[spare] : NULL;
    D3D12_RESOURCE_STATES state = spare >= 0 ? z->snapshot_spare_state[spare]
                                                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!snap) {
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = nw;
    rd.Height = nh;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&snap))) {
        fprintf(stderr, "[zetatrack] snapshot resize failed %u:0x%X %ux%u\n",
                z->location, z->offset, nw, nh);
        return;
    }
    }
    static u32 resize_logs;
    if (resize_logs++ < 24) fprintf(stderr, "[zetatrack] snapshot %u:0x%X resized %ux%u -> %ux%u\n",
            z->location, z->offset, z->snapshot_w, z->snapshot_h, nw, nh);
    /* Keep inactive allocations, but never their content-validity. The
     * current depth resource is authoritative and every swap resolves anew.
     * Retire evictions only after the open GPU command list has completed. */
    const u32 keep = spare >= 0 ? (u32)spare : z->snapshot_spare_next++ % 3u;
    if (spare < 0 && z->snapshot_spare[keep]) retire_texture(z->snapshot_spare[keep]);
    z->snapshot_spare[keep] = z->snapshot;
    z->snapshot_spare_state[keep] = z->snapshot_state;
    z->snapshot_spare_w[keep] = z->snapshot_w;
    z->snapshot_spare_h[keep] = z->snapshot_h;
    z->snapshot = snap;
    z->snapshot_w = nw;
    z->snapshot_h = nh;
    z->scaled = scaled;
    z->packed_generation = UINT64_MAX;
    z->snapshot_state = state;
    srv_write_zdepth(SRV_ZDEPTH_BASE + i, z->snapshot);
    uav_write_zdepth(UAV_ZDEPTH_BASE + i, z->snapshot);
    /* The depth resource still holds what the old snapshot held (a clear
     * would have invalidated both): resolve it again at the next read. */
    if (z->snapshot_valid) z->had_write = 1;
    z->snapshot_valid = 0;
}

/* Returns DSV slot 1+i. Slot 0 is the legacy shared depth fallback. */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h, int producer)
{
    if (!honor_zeta_track()) return 0;
    /* The DSV must cover the complete live framebuffer/viewport it is bound
     * with.  Some early passes declare a smaller clip than the active canvas;
     * allocating only that clip causes the D3D device to reject later
     * surface/PSO creation.  Sampling a declared-size window from this padded
     * backing remains a separate SRV concern. */
    rsx_dsp_surface declared_depth;
    rsx_dsp_get_surface(&g.rsx, &declared_depth);
    /* Render scale: a zeta bound with a scaled colour target is scaled too
     * (same predicate as surface_get). */
    const int zscaled = ld_rs_wants_clip(declared_depth.color_location[0], declared_depth.clip_w, declared_depth.clip_h);
    if (zscaled) { rt_w = ld_rs_up(rt_w); rt_h = ld_rs_up(rt_h); }
    const u32 canvas_w = zscaled ? ld_rs_up(g.width) : g.width;
    const u32 canvas_h = zscaled ? ld_rs_up(g.height) : g.height;
    u32 want_w = rt_w > canvas_w ? rt_w : canvas_w;
    u32 want_h = rt_h > canvas_h ? rt_h : canvas_h;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        zdepth_t* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset &&
            z->depth_format == declared_depth.depth_format) {
            if (z->w >= want_w && z->h >= want_h) {
                if (g_rs_num != g_rs_den && z->w > want_w && !zscaled) {
                    static int mixed;
                    if (mixed++ < 8)
                        fprintf(stderr, "[render-scale] zeta %u:0x%X (%ux%u) bound with an unscaled %ux%u target\n",
                                location, offset, z->w, z->h, rt_w, rt_h);
                }
                if (producer) zdepth_producer_snapshot(i, rt_w, rt_h, (u32)zscaled);
                return 1 + i;
            }
            slot = i;
            break;
        }
    }
    if (slot == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            fprintf(stderr, "[zetatrack] live cache full; shared fallback\n");
            return 0;
        }
        slot = g.n_zdepths;
    } else {
        zdepth_t* old = &g.zdepths[slot];
        if (want_w < old->w) want_w = old->w;
        if (want_h < old->h) want_h = old->h;
        fprintf(stderr,
                "[zetatrack] live zeta %u:0x%X outgrown %ux%u -> %ux%u\n",
                location, offset, old->w, old->h, want_w, want_h);
        if (old->tex) {
            /* Draws already recorded in the open command list may still
             * reference this DSV.  D3D12 command lists do not retain resource
             * lifetimes for the application, so release it only after the
             * next submit/fence completes. */
            retire_texture(old->tex);
            old->tex = NULL;
        }
        if (old->snapshot) {
            retire_texture(old->snapshot);
            old->snapshot = NULL;
        }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w;
    rd.Height = want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    /* Optimized clear values must use the typed DSV format even though the
     * resource itself is typeless so it can also be exposed through an SRV. */
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[slot];
    z->depth_format = declared_depth.depth_format;
    HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->tex);
    if (FAILED(create_hr)) {
        fprintf(stderr,
                "[zetatrack] live create failed %u:0x%X hr=0x%08lX; shared fallback\n",
                location, offset, (unsigned long)create_hr);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->w = want_w;
    z->h = want_h;
    z->scaled = (u32)zscaled;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_w = rt_w ? rt_w : want_w;
    z->snapshot_h = rt_h ? rt_h : want_h;
    if (z->snapshot_w > want_w) z->snapshot_w = want_w;
    if (z->snapshot_h > want_h) z->snapshot_h = want_h;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {0};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateDepthStencilView(
        g.dev, z->tex, &dsvd, dsv_handle(1 + slot));

    /* D3D12 forbids a partial CopyTextureRegion from a depth/stencil
     * resource.  Keep the replay-proven logical dimensions and resolve the
     * depth plane with a compute shader instead; that is a normal SRV read,
     * not a depth-resource copy operation. */
    D3D12_RESOURCE_DESC snapshot_rd = {0};
    snapshot_rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    snapshot_rd.Width = z->snapshot_w;
    snapshot_rd.Height = z->snapshot_h;
    snapshot_rd.DepthOrArraySize = 1;
    snapshot_rd.MipLevels = 1;
    snapshot_rd.Format = DXGI_FORMAT_R32_FLOAT;
    snapshot_rd.SampleDesc.Count = 1;
    snapshot_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT snapshot_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &snapshot_rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&z->snapshot);
    if (FAILED(snapshot_hr)) {
        fprintf(stderr,
                "[zetatrack] live snapshot create failed %u:0x%X "
                "%ux%u hr=0x%08lX; guest fallback\n",
                location, offset, z->snapshot_w, z->snapshot_h,
                (unsigned long)snapshot_hr);
        z->snapshot = NULL;
    } else {
        z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        srv_write_depth_source(SRV_DEPTH_SOURCE_BASE + slot, z->tex);
        srv_write_zdepth(SRV_ZDEPTH_BASE + slot, z->snapshot);
        uav_write_zdepth(UAV_ZDEPTH_BASE + slot, z->snapshot);
    }
    g.list->lpVtbl->ClearDepthStencilView(
        g.list, dsv_handle(1 + slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);
    z->cleared = 1;
    fprintf(stderr,
            "[zetatrack] live new #%u %u:0x%X backing=%ux%u "
            "clip=%ux%u canvas=%ux%u\n",
            slot, location, offset, want_w, want_h,
            rt_w, rt_h, g.width, g.height);
    if (slot == g.n_zdepths) g.n_zdepths++;
    return 1 + slot;
}

/* Preserve the last completed depth-writing pass before the guest clears or
 * reuses its live zeta.  The replay renderer gets these bytes from its
 * captured cross-frame memory image; live rendering must explicitly retain
 * the equivalent GPU result because the guest never reads it back to CPU
 * VRAM. */
static int zdepth_snapshot(u32 slot)
{
   
    if (!slot) return 0;
    zdepth_t* z = &g.zdepths[slot - 1];
    if (!z->snapshot || !g.depth_snapshot_rootsig ||
        !g.depth_snapshot_pso)
        return 0;
    if (!z->had_write) return z->snapshot_valid;

    D3D12_GPU_DESCRIPTOR_HANDLE source_table, destination_table;
    if (!depth_snapshot_descriptors(
            slot - 1u, &source_table, &destination_table)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot descriptor ring exhausted "
                "at frame %u\n",
                g_ld_frames);
        return z->snapshot_valid;
    }

    D3D12_RESOURCE_BARRIER bars[2] = {0};
    u32 nbar = 0;
    bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[nbar].Transition.pResource = z->tex;
    bars[nbar].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[nbar].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bars[nbar].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    nbar++;
    if (z->snapshot_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[nbar].Transition.pResource = z->snapshot;
        bars[nbar].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[nbar].Transition.StateBefore = z->snapshot_state;
        bars[nbar].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        nbar++;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, nbar, bars);

    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, g.depth_snapshot_pso);
    g.list->lpVtbl->SetComputeRootSignature(
        g.list, g.depth_snapshot_rootsig);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 0, source_table);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 1, destination_table);
    g.list->lpVtbl->Dispatch(
        g.list, (z->snapshot_w + 7u) / 8u,
        (z->snapshot_h + 7u) / 8u, 1);

    D3D12_RESOURCE_BARRIER uav_done = {0};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = z->snapshot;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &uav_done);

    D3D12_RESOURCE_BARRIER done[2] = {0};
    done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[0].Transition.pResource = z->tex;
    done[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    done[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[1].Transition.pResource = z->snapshot;
    done[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    done[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, done);
    z->snapshot_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    z->snapshot_valid = 1;
    z->snapshot_generation++;
    z->had_write = 0;
    return 1;
}

/* A colour view of guest D24S8 memory is distinct from normalized depth.
 * Guest big-endian word DDDDDDSS decodes as ARGB={Dhi,Dmid,Dlo,S}. */
static void ld_pack_d24s8_rgba(u32 depth24, u32 stencil, u8 rgba[4])
{
    rgba[0] = (u8)(depth24 >> 8);
    rgba[1] = (u8)depth24;
    rgba[2] = (u8)stencil;
    rgba[3] = (u8)(depth24 >> 16);
}

static int make_depth_pack_pipeline(void)
{
    if (g.depth_pack_pso) return 1;
    static const char source[] =
        "Texture2D<float> z : register(t0);\n"
        "Texture2D<uint> s : register(t1);\n"
        "RWTexture2D<float4> dst : register(u0);\n"
        "[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
        " uint w,h; dst.GetDimensions(w,h); if(id.x>=w || id.y>=h)return;\n"
        " uint d=(uint)round(saturate(z.Load(int3(id.xy,0)))*16777215.0);\n"
        " uint st=s.Load(int3(id.xy,0)) & 255u;\n"
        " dst[id.xy]=float4((d>>8)&255u,d&255u,st,(d>>16)&255u)/255.0;\n"
        "}\n";
    ID3DBlob *shader = NULL, *errors = NULL;
    HRESULT hr = ld_builtin_compile(source, sizeof(source)-1, "depth_color_reinterpret",
        NULL,NULL,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors);
    if (FAILED(hr)) {
        fprintf(stderr,"[zpack] shader compile failed: %s\n",errors ?
            (const char*)errors->lpVtbl->GetBufferPointer(errors) : "no diagnostics");
        if (errors) errors->lpVtbl->Release(errors);
        return 0;
    }
    if (errors) errors->lpVtbl->Release(errors);
    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors=2;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors=1;
    D3D12_ROOT_PARAMETER params[2]={0};
    for(u32 i=0;i<2;++i) {
        params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges=1;
        params[i].DescriptorTable.pDescriptorRanges=&ranges[i];
        params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc={0};desc.NumParameters=2;desc.pParameters=params;
    ID3DBlob *serialized=NULL,*sig_errors=NULL;
    hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&sig_errors);
    if(sig_errors) sig_errors->lpVtbl->Release(sig_errors);
    if(SUCCEEDED(hr)) {
        hr=g.dev->lpVtbl->CreateRootSignature(g.dev,0,serialized->lpVtbl->GetBufferPointer(serialized),
            serialized->lpVtbl->GetBufferSize(serialized),&IID_ID3D12RootSignature,(void**)&g.depth_pack_rootsig);
        serialized->lpVtbl->Release(serialized);
    }
    if(SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=g.depth_pack_rootsig;
        pd.CS.pShaderBytecode=shader->lpVtbl->GetBufferPointer(shader);
        pd.CS.BytecodeLength=shader->lpVtbl->GetBufferSize(shader);
        hr=g.dev->lpVtbl->CreateComputePipelineState(g.dev,&pd,&IID_ID3D12PipelineState,(void**)&g.depth_pack_pso);
    }
    shader->lpVtbl->Release(shader);
    if(FAILED(hr)) fprintf(stderr,"[zpack] pipeline failed hr=%08lX\n",(unsigned long)hr);
    return SUCCEEDED(hr);
}

static int zdepth_packed_snapshot(u32 slot)
{
   
    if (!slot || slot > g.n_zdepths) return 0;
    zdepth_t* z=&g.zdepths[slot-1];
    if(z->depth_format != 2u || !zdepth_snapshot(slot) || !make_depth_pack_pipeline()) return 0;
    if(z->packed_snapshot && z->packed_w==z->snapshot_w && z->packed_h==z->snapshot_h &&
       z->packed_generation==z->snapshot_generation) return 1;
    if(g.srv_ring_used>=SRV_RING_TABLES) return 0;
    if(!z->packed_snapshot || z->packed_w!=z->snapshot_w || z->packed_h!=z->snapshot_h) {
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width=z->snapshot_w;rd.Height=z->snapshot_h;rd.DepthOrArraySize=1;
        rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;
        rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* replacement=NULL;
        if(FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,NULL,&IID_ID3D12Resource,(void**)&replacement))) return 0;
        if(z->packed_snapshot) retire_texture(z->packed_snapshot);
        z->packed_snapshot=replacement;z->packed_w=z->snapshot_w;z->packed_h=z->snapshot_h;
        z->packed_state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        srv_write(SRV_ZPACK_BASE+slot-1,z->packed_snapshot);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud={0};ud.Format=rd.Format;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->lpVtbl->CreateUnorderedAccessView(g.dev,z->packed_snapshot,NULL,&ud,srv_cpu(UAV_ZPACK_BASE+slot-1));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC stencil={0};stencil.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    stencil.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;stencil.Texture2D.MipLevels=1;
    stencil.Texture2D.PlaneSlice=1;
    /* X32_G8 exposes stencil in G; the scalar compute input reads R. */
    stencil.Shader4ComponentMapping=D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);
    g.dev->lpVtbl->CreateShaderResourceView(g.dev,z->tex,&stencil,srv_cpu(SRV_STENCIL_SOURCE_BASE+slot-1));
    const u32 table=g.srv_ring_used++*SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    g.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_heap,&cpu);
    cpu.ptr+=(size_t)table*g.srv_step;
    const u32 descriptors[3]={SRV_ZDEPTH_BASE+slot-1,SRV_STENCIL_SOURCE_BASE+slot-1,UAV_ZPACK_BASE+slot-1};
    for(u32 i=0;i<3;++i) {g.dev->lpVtbl->CopyDescriptorsSimple(g.dev,1,cpu,srv_cpu(descriptors[i]),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=g.srv_step;}
    D3D12_RESOURCE_BARRIER bars[3]={0};
    ID3D12Resource* resources[3]={z->snapshot,z->tex,z->packed_snapshot};
    D3D12_RESOURCE_STATES before[3]={z->snapshot_state,D3D12_RESOURCE_STATE_DEPTH_WRITE,z->packed_state};
    D3D12_RESOURCE_STATES during[3]={D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    u32 nbar=0;
    for(u32 i=0;i<3;++i) if(before[i]!=during[i]) {bars[nbar].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;bars[nbar].Transition.pResource=resources[i];bars[nbar].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;bars[nbar].Transition.StateBefore=before[i];bars[nbar++].Transition.StateAfter=during[i];}
    if(nbar) g.list->lpVtbl->ResourceBarrier(g.list,nbar,bars);
    ID3D12DescriptorHeap* heaps[]={g.srv_heap,g.smp_heap};g.list->lpVtbl->SetDescriptorHeaps(g.list,2,heaps);
    g.list->lpVtbl->SetPipelineState(g.list,g.depth_pack_pso);g.list->lpVtbl->SetComputeRootSignature(g.list,g.depth_pack_rootsig);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;g.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.srv_heap,&gpu);gpu.ptr+=(u64)table*g.srv_step;
    g.list->lpVtbl->SetComputeRootDescriptorTable(g.list,0,gpu);gpu.ptr+=2u*g.srv_step;g.list->lpVtbl->SetComputeRootDescriptorTable(g.list,1,gpu);
    g.list->lpVtbl->Dispatch(g.list,(z->packed_w+7u)/8u,(z->packed_h+7u)/8u,1);
    for(u32 i=0;i<3;++i) {bars[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;bars[i].Transition.pResource=resources[i];bars[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;bars[i].Transition.StateBefore=during[i];bars[i].Transition.StateAfter=i==2 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : before[i];}
    g.list->lpVtbl->ResourceBarrier(g.list,3,bars);
    z->packed_state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;z->packed_generation=z->snapshot_generation;
    {static u32 logs=0;if(logs++<16)fprintf(stderr,"[zpack] D24S8 %u:0x%08X %ux%u generation=%llu\n",z->location,z->offset,z->packed_w,z->packed_h,(unsigned long long)z->packed_generation);}
    return 1;
}

/* ---------------------------------------------------------------------------
 * PSO cache (VP+FP+render-state keyed)
 * -----------------------------------------------------------------------*/
static u64 fnv1a(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int sampler_table_needs_flush(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.smp_ring_used < SMP_RING_TABLES) return 0;
    for (u32 i = 0; i < g.smp_ring_used; i++)
        if (memcmp(g.smp_ring_slots[i], slots,
                   sizeof(g.smp_ring_slots[i])) == 0)
            return 0;
    return 1;
}

static u32 shader_disk_cache_stage_index(u32 stage)
{
    return stage == 'V' ? 0u : 1u;
}

static void shader_disk_cache_progress(u64 value)
{
    if (value > 4u && (value & (value - 1u)) != 0u)
        return;
    fprintf(stderr,
            "[shader-disk-cache] hits{vs=%llu ps=%llu} "
            "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
            "rejects=%llu\n",
            (unsigned long long)g_ld_shader_disk_hits[0],
            (unsigned long long)g_ld_shader_disk_hits[1],
            (unsigned long long)g_ld_shader_disk_misses[0],
            (unsigned long long)g_ld_shader_disk_misses[1],
            (unsigned long long)g_ld_shader_disk_writes[0],
            (unsigned long long)g_ld_shader_disk_writes[1],
            (unsigned long long)g_ld_shader_disk_rejects);
}

static int shader_disk_cache_prepare(void)
{
    if (g_ld_shader_disk_ready >= 0)
        return g_ld_shader_disk_ready;
    g_ld_shader_disk_ready = 0;
    if (getenv("RSX_NO_SHADER_DISK_CACHE")) {
        fprintf(stderr, "[shader-disk-cache] disabled\n");
        return 0;
    }

    const char* override_dir = getenv("RSX_SHADER_CACHE_DIR");
    if (override_dir && override_dir[0]) {
        if (snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                     "%s", override_dir) < 0 ||
            strlen(g_ld_shader_disk_dir) >= sizeof(g_ld_shader_disk_dir) - 1u)
            return 0;
    } else {
        CreateDirectoryA("scratch", NULL);
        snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                 "scratch\\rsx_shader_cache_v%u",
                 SHADER_DISK_CACHE_VERSION);
    }

    if (!CreateDirectoryA(g_ld_shader_disk_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr,
                "[shader-disk-cache] cannot create '%s' error=%lu\n",
                g_ld_shader_disk_dir, (unsigned long)GetLastError());
        return 0;
    }
    const DWORD attributes = GetFileAttributesA(g_ld_shader_disk_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;

    g_ld_shader_disk_ready = 1;
    fprintf(stderr, "[shader-disk-cache] enabled dir='%s' version=%u\n",
            g_ld_shader_disk_dir, SHADER_DISK_CACHE_VERSION);
    return 1;
}

static int shader_disk_cache_path(
    char path[MAX_PATH], u32 stage, u64 hash, u32 source_length)
{
    if (!shader_disk_cache_prepare())
        return 0;
    const int length = snprintf(
        path, MAX_PATH, "%s\\%c_%016llX_%08X.bin",
        g_ld_shader_disk_dir, (char)stage,
        (unsigned long long)hash, source_length);
    return length > 0 && length < MAX_PATH;
}

static u64 g_ld_shader_seed_hits;

/* Reads one cache-format file; exact generated source, DXBC container and
 * header fields must all match. *reject_kind says why it did not. */
static int shader_disk_cache_read(
    FILE* file, u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob** out, int* reject_kind)
{
    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    *out = NULL;
    *reject_kind = RSP_REJECT_FORMAT;
    int valid = fread(&header, sizeof(header), 1, file) == 1 &&
        header.magic == SHADER_DISK_CACHE_MAGIC &&
        header.version == SHADER_DISK_CACHE_VERSION &&
        header.stage == stage && header.source_hash == hash &&
        header.source_length == source_length && source_length <= 256u * 1024u &&
        header.blob_length > 0 &&
        header.blob_length <= SHADER_DISK_CACHE_MAX_BLOB;
    char* stored_source = NULL;
    ID3DBlob* blob = NULL;
    if (valid) {
        stored_source = (char*)malloc(source_length ? source_length : 1u);
        valid = stored_source != NULL &&
            fread(stored_source, 1, source_length, file) == source_length;
        if (valid && memcmp(stored_source, source, source_length) != 0) {
            valid = 0;
            *reject_kind = RSP_REJECT_SOURCE;
        }
    }
    if (valid) {
        valid = SUCCEEDED(D3DCreateBlob(header.blob_length, &blob)) && blob &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1,
                  header.blob_length, file) == header.blob_length;
    }
    if (valid && !rsp_dxbc_valid(blob->lpVtbl->GetBufferPointer(blob),
                                 blob->lpVtbl->GetBufferSize(blob))) {
        valid = 0;
        *reject_kind = RSP_REJECT_DXBC;
    }
    free(stored_source);
    if (!valid) {
        if (blob)
            blob->lpVtbl->Release(blob);
        return 0;
    }
    *out = blob;
    return 1;
}

/* User cache first (it has precedence), then the shipped seed next to the
 * executable. A bad user entry is deleted; the seed is never modified.
 * Driver PSO data is never part of either. */
static ID3DBlob* shader_disk_cache_load(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    char path[MAX_PATH];
    const u32 stage_index = shader_disk_cache_stage_index(stage);
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return NULL;

    ID3DBlob* blob = NULL;
    int reject_kind = RSP_REJECT_FORMAT;
    FILE* file = fopen(path, "rb");
    if (file) {
        const int ok = shader_disk_cache_read(
            file, stage, source, source_length, hash, &blob, &reject_kind);
        fclose(file);
        if (ok) {
            const u64 value = ++g_ld_shader_disk_hits[stage_index];
            shader_disk_cache_progress(value);
            rsp_rt_note_load(stage, 0, 1, -1);
            return blob;
        }
        DeleteFileA(path);
        g_ld_shader_disk_rejects++;
        rsp_rt_note_load(stage, 0, 0, reject_kind);
    }
    file = rsp_seed_open(SHADER_DISK_CACHE_VERSION, stage, hash, source_length);
    if (file) {
        const int ok = shader_disk_cache_read(
            file, stage, source, source_length, hash, &blob, &reject_kind);
        fclose(file);
        if (ok) {
            const u64 value = ++g_ld_shader_disk_hits[stage_index];
            shader_disk_cache_progress(value);
            rsp_rt_note_load(stage, 1, 1, -1);
            ++g_ld_shader_seed_hits;
            if (g_ld_shader_seed_hits <= 4 ||
                (g_ld_shader_seed_hits & (g_ld_shader_seed_hits - 1)) == 0)
                fprintf(stderr, "[shader-cache-seed] hit=%llu stage=%c source=%016llX bytes=%u exact_source=1\n",
                    (unsigned long long)g_ld_shader_seed_hits, (char)stage,
                    (unsigned long long)hash, source_length);
            return blob;
        }
        g_ld_shader_disk_rejects++;
        rsp_rt_note_load(stage, 1, 0, reject_kind);
    }
    const u64 value = ++g_ld_shader_disk_misses[stage_index];
    shader_disk_cache_progress(value);
    return NULL;
}

static void shader_disk_cache_store(
    u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob* blob)
{
    if (!blob || source_length > 256u * 1024u)
        return;
    const size_t blob_length = blob->lpVtbl->GetBufferSize(blob);
    if (!blob_length || blob_length > SHADER_DISK_CACHE_MAX_BLOB)
        return;

    char path[MAX_PATH], temporary[MAX_PATH];
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return;
    const int temporary_length = snprintf(
        temporary, sizeof(temporary), "%s.tmp.%lu", path,
        (unsigned long)GetCurrentProcessId());
    if (temporary_length <= 0 || temporary_length >= sizeof(temporary))
        return;

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    header.magic = SHADER_DISK_CACHE_MAGIC;
    header.version = SHADER_DISK_CACHE_VERSION;
    header.stage = stage;
    header.source_length = source_length;
    header.blob_length = (u32)blob_length;
    header.source_hash = hash;

    FILE* file = fopen(temporary, "wb");
    if (!file)
        return;
    const int wrote = fwrite(&header, sizeof(header), 1, file) == 1 &&
        fwrite(source, 1, source_length, file) == source_length &&
        fwrite(blob->lpVtbl->GetBufferPointer(blob), 1,
               blob_length, file) == blob_length &&
        fflush(file) == 0;
    fclose(file);
    if (!wrote || !MoveFileExA(
            temporary, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary);
        return;
    }

    const u32 stage_index = shader_disk_cache_stage_index(stage);
    const u64 value = ++g_ld_shader_disk_writes[stage_index];
    shader_disk_cache_progress(value);
}

static const char* ld_shader_cache_dir_or_null(void)
{
    return g_ld_shader_disk_ready > 0 ? g_ld_shader_disk_dir : NULL;
}

/* Fixed renderer shaders (blits, post-process, original bloom kernels): the
 * same cache/seed path as generated shaders under stage 'B'. The identity
 * text is name/entry/target/flags followed by the complete source, so any
 * change to either compiles normally. Defines/includes bypass the cache. */
static HRESULT ld_builtin_compile(LPCVOID source, SIZE_T bytes, LPCSTR name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors)
{
    uint32_t identity_length = 0;
    char* identity = defines || include ? NULL : rsp_builtin_identity(
        source, bytes, name, entry, target, flags1, flags2, &identity_length);
    if (!identity)
        return D3DCompile(source, bytes, name, defines, include, entry, target,
                          flags1, flags2, code, errors);
    const u64 hash = fnv1a(identity, identity_length, 1469598103934665603ull);
    ID3DBlob* blob = shader_disk_cache_load('B', identity, identity_length, hash);
    if (blob) {
        *code = blob;
        if (errors)
            *errors = NULL;
        free(identity);
        return S_OK;
    }
    const double begin = rsp_now_ms();
    const HRESULT hr = D3DCompile(source, bytes, name, defines, include, entry,
                                  target, flags1, flags2, code, errors);
    rsp_rt_note_compile('B', hash, identity_length, rsp_now_ms() - begin,
                        g_ld_frames, hr, ld_shader_cache_dir_or_null());
    if (SUCCEEDED(hr) && *code)
        shader_disk_cache_store('B', identity, identity_length, hash, *code);
    free(identity);
    return hr;
}

static u64 ld_hash_structural_render_state(
    const render_state_t* state, u64 hash)
{
#define LD_HASH_RENDER_FIELD(name) \
    hash = fnv1a(&state->name, sizeof(state->name), hash)
    /* Alpha payload is deliberately absent. Enable and compare mode still
     * select distinct shader/PSO behavior. Every other D3D12 render-state
     * field is listed explicitly so struct padding can never become identity. */
    LD_HASH_RENDER_FIELD(alpha_test_enable);
    LD_HASH_RENDER_FIELD(alpha_func);
    LD_HASH_RENDER_FIELD(blend_enable);
    LD_HASH_RENDER_FIELD(sf_rgb);
    LD_HASH_RENDER_FIELD(df_rgb);
    LD_HASH_RENDER_FIELD(sf_a);
    LD_HASH_RENDER_FIELD(df_a);
    LD_HASH_RENDER_FIELD(eq_rgb);
    LD_HASH_RENDER_FIELD(eq_a);
    LD_HASH_RENDER_FIELD(depth_test);
    LD_HASH_RENDER_FIELD(depth_write);
    LD_HASH_RENDER_FIELD(depth_func);
    LD_HASH_RENDER_FIELD(cull_enable);
    LD_HASH_RENDER_FIELD(cull_face);
    LD_HASH_RENDER_FIELD(front_face);
    LD_HASH_RENDER_FIELD(color_mask);
    LD_HASH_RENDER_FIELD(rt_color_format);
    LD_HASH_RENDER_FIELD(rt_count);
    LD_HASH_RENDER_FIELD(poly_offset_scale);
    LD_HASH_RENDER_FIELD(poly_offset_bias);
    LD_HASH_RENDER_FIELD(stencil_enable);
    LD_HASH_RENDER_FIELD(stencil_two_sided);
    LD_HASH_RENDER_FIELD(s_func);
    LD_HASH_RENDER_FIELD(s_func_mask);
    LD_HASH_RENDER_FIELD(s_write_mask);
    LD_HASH_RENDER_FIELD(s_fail);
    LD_HASH_RENDER_FIELD(s_zfail);
    LD_HASH_RENDER_FIELD(s_zpass);
    LD_HASH_RENDER_FIELD(bs_func);
    LD_HASH_RENDER_FIELD(bs_fail);
    LD_HASH_RENDER_FIELD(bs_zfail);
    LD_HASH_RENDER_FIELD(bs_zpass);
#undef LD_HASH_RENDER_FIELD
    return hash;
}

/*
 * PSO identity includes render state, but D3DCompile only consumes the final
 * generated shader text.  Keep those cache boundaries separate: one exact
 * HLSL program is compiled once and its bytecode is reused by every PSO state
 * variant that references it.  The source bytes are retained for collision-
 * safe equality; the 64-bit hash is only an index accelerator.
 */
static ID3DBlob* shader_blob_cache_find(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, int* post_boundary_hit, int* hash_seen)
{
    if (post_boundary_hit)
        *post_boundary_hit = 0;
    if (hash_seen)
        *hash_seen = 0;
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->hash != hash)
            continue;
        if (hash_seen)
            *hash_seen = 1;
        if (
            entry->source_length != source_length ||
            memcmp(entry->source, source, source_length) != 0)
            continue;
        if (post_boundary_hit && i >= FORMER_MAX_SHADER_BLOBS)
            *post_boundary_hit = 1;
        entry->blob->lpVtbl->AddRef(entry->blob);
        return entry->blob;
    }
    return NULL;
}

typedef enum {
    SHADER_BLOB_INSERT_SKIPPED = 0,
    SHADER_BLOB_INSERTED,
    SHADER_BLOB_INSERT_FULL
} shader_blob_insert_result;

static shader_blob_insert_result shader_blob_cache_insert(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob)
{
    if (!blob)
        return SHADER_BLOB_INSERT_SKIPPED;
    if (cache->count >= MAX_SHADER_BLOBS)
        return SHADER_BLOB_INSERT_FULL;
    char* source_copy = (char*)malloc((size_t)source_length + 1u);
    if (!source_copy)
        return SHADER_BLOB_INSERT_SKIPPED;
    memcpy(source_copy, source, source_length);
    source_copy[source_length] = '\0';
    shader_blob_t* entry = &cache->entries[cache->count++];
    entry->hash = hash;
    entry->source_length = source_length;
    entry->source = source_copy;
    entry->blob = blob;
    blob->lpVtbl->AddRef(blob);
    cache->retained_source_bytes += source_length;
    cache->retained_blob_bytes += blob->lpVtbl->GetBufferSize(blob);
    return SHADER_BLOB_INSERTED;
}

static void shader_blob_cache_insert_accounted(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob, u32 stage, int hash_seen)
{
    const u32 count_before = cache->count;
    const shader_blob_insert_result result = shader_blob_cache_insert(
        cache, source, source_length, hash, blob);
#if defined(YZ_PERF_PROFILE)
    if (stage == 'V') {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.vs_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.vs_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.vs_blob_full_rejects++;
        }
    } else {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.ps_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.ps_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.ps_blob_full_rejects++;
        }
    }
#else
    (void)count_before;
    (void)result;
    (void)stage;
    (void)hash_seen;
#endif
}

static void shader_blob_cache_release(shader_blob_cache_t* cache)
{
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->blob)
            entry->blob->lpVtbl->Release(entry->blob);
        free(entry->source);
    }
    memset(cache, 0, sizeof(*cache));
}

#if defined(YZ_PERF_PROFILE)
static INIT_ONCE g_ld_fp_variant_once=INIT_ONCE_STATIC_INIT;
static int g_ld_fp_variant_enabled,g_ld_fp_variant_truncated;
static BOOL CALLBACK ld_fp_variant_init(PINIT_ONCE once,PVOID arg,PVOID* context)
{
    (void)once;(void)arg;(void)context;
    const char* value=getenv("LD_FP_VARIANT_PROFILE");
    g_ld_fp_variant_enabled=value&&!strcmp(value,"1");
    if(g_ld_fp_variant_enabled)
        fprintf(stderr,"[fp-variant-profile] enabled=1 literal_formatting=1 bounded_capacity=%u\n",LD_VARIANT_SET_CAPACITY);
    return TRUE;
}
static int ld_fp_variant_profile_enabled(void)
{
    InitOnceExecuteOnce(&g_ld_fp_variant_once,ld_fp_variant_init,NULL,NULL);
    return g_ld_fp_variant_enabled;
}
static const char* ld_fp_variant_profile_status(void)
{
    return !ld_fp_variant_profile_enabled()?"disabled":g_ld_fp_variant_truncated?"truncated":"enabled";
}
static void ld_fp_variant_truncate(void)
{
    if(!g_ld_fp_variant_truncated){
        g_ld_fp_variant_truncated=1;g_ld_profile.ps_variant_tracking_overflow++;
        fprintf(stderr,"[fp-variant-profile] truncated=1 capacity=%u further_literal_work=0\n",LD_VARIANT_SET_CAPACITY);
    }
}
static void ld_profile_note_hlsl(
    const char* hlsl, u64 hashes[MAX_SHADER_BLOBS], u32* count, u64* unique)
{
    const size_t length = strlen(hlsl);
    const u64 hash = fnv1a(
        hlsl, (u32)length, 1469598103934665603ull);
    for (u32 i = 0; i < *count; i++)
        if (hashes[i] == hash)
            return;
    if (*count < MAX_SHADER_BLOBS)
        hashes[(*count)++] = hash;
    (*unique)++;
}

static int ld_profile_note_hash(
    u64 hash, u64 hashes[LD_VARIANT_SET_CAPACITY], u32* count)
{
    if (!hash)
        hash = 1;
    u32 slot = (u32)hash & (LD_VARIANT_SET_CAPACITY - 1u);
    for (u32 probe = 0; probe < LD_VARIANT_SET_CAPACITY; ++probe) {
        if (hashes[slot] == hash)
            return 0;
        if (hashes[slot] == 0) {
            hashes[slot] = hash;
            (*count)++;
            return 1;
        }
        slot = (slot + 1u) & (LD_VARIANT_SET_CAPACITY - 1u);
    }
    ld_fp_variant_truncate();
    return -1;
}

static void ld_profile_note_fp_variants(
    const u8* fp_uc, u32 fp_size, u32 fp_ctrl_key, u32 cube_mask,
    const render_state_t* state)
{
    if(!ld_fp_variant_profile_enabled() || g_ld_fp_variant_truncated)return;
    if(g_ld_profile.n_ps_raw_constant_hashes>=LD_VARIANT_SET_CAPACITY ||
       g_ld_profile.n_ps_raw_exact_hashes>=LD_VARIANT_SET_CAPACITY ||
       g_ld_profile.n_ps_exact_source_hashes>=LD_VARIANT_SET_CAPACITY ||
       g_ld_profile.n_ps_constant_specialized_hashes>=LD_VARIANT_SET_CAPACITY ||
       g_ld_profile.n_ps_canonical_hashes>=LD_VARIANT_SET_CAPACITY){ld_fp_variant_truncate();return;}
    const u64 seed = 1469598103934665603ull;
    u64 raw_constant = fnv1a(fp_uc, fp_size, seed);
    u64 raw_exact = raw_constant;
    u64 canonical = rsx_fp_structural_hash(fp_uc, fp_size, seed);
    if (!canonical)
        return;

    const u32 alpha_enable = state->alpha_test_enable ? 1u : 0u;
    const u32 alpha_func = alpha_enable ? state->alpha_func : 0u;
#define LD_HASH_FP_SOURCE_FIELD(value) do { \
    const u32 _field = (value); \
    raw_exact = fnv1a(&_field, sizeof(_field), raw_exact); \
    raw_constant = fnv1a( \
        &_field, sizeof(_field), raw_constant); \
    canonical = fnv1a(&_field, sizeof(_field), canonical); \
} while (0)
    LD_HASH_FP_SOURCE_FIELD(fp_ctrl_key);
    LD_HASH_FP_SOURCE_FIELD(cube_mask);
    LD_HASH_FP_SOURCE_FIELD(alpha_enable);
    LD_HASH_FP_SOURCE_FIELD(alpha_func);
#undef LD_HASH_FP_SOURCE_FIELD

    float alpha_ref = 0.0f;
    if (alpha_enable && alpha_func != 0x0207u) {
        alpha_ref = rsx_fp_alpha_ref(
            state->alpha_ref_raw, state->alpha_ref_format);
        raw_exact = fnv1a(
            &alpha_ref, sizeof(alpha_ref), raw_exact);
    }

    const int new_raw_constant = ld_profile_note_hash(
        raw_constant, g_ld_profile.ps_raw_constant_hashes,
        &g_ld_profile.n_ps_raw_constant_hashes);
    const int new_raw_exact = ld_profile_note_hash(
        raw_exact, g_ld_profile.ps_raw_exact_hashes,
        &g_ld_profile.n_ps_raw_exact_hashes);
    if(new_raw_constant<0 || new_raw_exact<0)return;
    if (new_raw_constant || new_raw_exact) {
        u64 literal = rsx_fp_literal_source_hash(
            fp_uc, fp_size, seed);
        literal = fnv1a(
            &fp_ctrl_key, sizeof(fp_ctrl_key), literal);
        literal = fnv1a(
            &cube_mask, sizeof(cube_mask), literal);
        literal = fnv1a(
            &alpha_enable, sizeof(alpha_enable), literal);
        literal = fnv1a(
            &alpha_func, sizeof(alpha_func), literal);
        if (new_raw_constant)
            ld_profile_note_hash(
                literal,
                g_ld_profile.ps_constant_specialized_hashes,
                &g_ld_profile.n_ps_constant_specialized_hashes);
        if (new_raw_exact) {
            u64 exact_source = literal;
            if (alpha_enable && alpha_func != 0x0207u) {
                char alpha_literal[64];
                const int length = snprintf(
                    alpha_literal, sizeof(alpha_literal), "%.9g",
                    (double)alpha_ref);
                if (length > 0 &&
                    (u32)length < sizeof(alpha_literal))
                    exact_source = fnv1a(
                        alpha_literal, (u32)length, exact_source);
            }
            ld_profile_note_hash(
                exact_source, g_ld_profile.ps_exact_source_hashes,
                &g_ld_profile.n_ps_exact_source_hashes);
        }
    }
    ld_profile_note_hash(
        canonical, g_ld_profile.ps_canonical_hashes,
        &g_ld_profile.n_ps_canonical_hashes);
}
#endif

static u32 vp_txl_unit_mask(const u8* ucode, u32 instrs)
{
    u32 mask = 0;
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

/* Render-thread owned, bounded immutable analysis. Guest FP payloads are
 * snapshotted for this draw; cache entries never replace mutable constants. */
typedef struct ld_shader_draw_metadata {
    rsx_shader_metadata* vp;
    rsx_shader_metadata* fp;
    const u8* fp_source;
} ld_shader_draw_metadata;
static INIT_ONCE g_ld_smc_once=INIT_ONCE_STATIC_INIT;
static rsx_shader_metadata_cache g_ld_smc;
static u8 g_ld_smc_fp_snapshot[RSX_SMC_MAX_BYTES];
/* Entry whose code the snapshot currently holds (constant slots aside). */
static const rsx_shader_metadata* g_ld_smc_snap_entry; static u64 g_ld_smc_snap_serial;
static int g_ld_smc_enabled,g_ld_smc_verify,g_ld_smc_failed,g_ld_smc_busy;
static u32 g_ld_smc_calls,g_ld_smc_verified;
static struct {u64 vp,fp;u32 samples;} g_ld_smc_verify_pairs[128];
static u32 g_ld_smc_verify_pair_count;
static BOOL CALLBACK ld_shader_metadata_init(PINIT_ONCE once,PVOID arg,PVOID* context)
{
    (void)once;(void)arg;(void)context;
    const char* value=getenv("LD_SHADER_METADATA_CACHE");
    g_ld_smc_enabled=value&&!strcmp(value,"1");
    value=getenv("LD_SHADER_METADATA_VERIFY");
    g_ld_smc_verify=value&&!strcmp(value,"1");
    if(g_ld_smc_enabled){
        rsx_smc_init(&g_ld_smc,RSX_SMC_DEFAULT_BUDGET);
        fprintf(stderr,"[shader-metadata] enabled=1 buffered_only=1 verify=%d verify_limit=256 per_program_pair=2 verify_reexecutes_legacy_sanitizer_and_diagnostics=%d entries=%u budget=%u\n",g_ld_smc_verify,g_ld_smc_verify,RSX_SMC_ENTRIES,RSX_SMC_DEFAULT_BUDGET);
    }
    return TRUE;
}
static int ld_shader_metadata_enabled(void)
{
    InitOnceExecuteOnce(&g_ld_smc_once,ld_shader_metadata_init,NULL,NULL);
    return g_ld_smc_enabled&&!g_ld_smc_failed&&ld_fp_constants_buffered();
}
static rsx_shader_metadata* ld_shader_metadata_acquire(u32 stage,u64 address,
    const u8* source,u32 available)
{
    rsx_shader_metadata* hint=rsx_smc_hint(&g_ld_smc,stage,address);
    if(rsx_smc_matches(hint,source,available)){
        g_ld_smc.hint_hits++;return rsx_smc_pin(&g_ld_smc,hint);
    }
    /* Slot0 VP uploads and FP rings can alternate addresses/programs. Reuse
     * exact existing content before paying parser/reflection miss work. */
    for(u32 i=0;i<RSX_SMC_ENTRIES;i++){
        rsx_shader_metadata* e=&g_ld_smc.entries[i];
        if(e!=hint && e->stage==stage && rsx_smc_matches(e,source,available)){
            g_ld_smc.content_hits++;rsx_smc_set_hint(&g_ld_smc,address,e);
            return rsx_smc_pin(&g_ld_smc,e);
        }
    }
    return rsx_smc_acquire(&g_ld_smc,stage,address,source,available,vp_txl_unit_mask);
}
static void ld_shader_metadata_prepare(ld_shader_draw_metadata* draw)
{
    memset(draw,0,sizeof(*draw));
    const u32 start=rsx_dsp_vp_start(&g.rsx);
    if(start<RSX_DSP_VP_INSTR)
        draw->vp=ld_shader_metadata_acquire(RSX_SMC_VP,start,
            (const u8*)(g.rsx.vp+start*4),(RSX_DSP_VP_INSTR-start)*16);
    u32 location=0;const u32 offset=rsx_dsp_fragment_program(&g.rsx,&location);
    const u64 address=((u64)location<<32)|offset;
    rsx_shader_metadata* hint=rsx_smc_hint(&g_ld_smc,RSX_SMC_FP,address);
    u32 available=0;const u8* source=NULL;
    /* A size hint is never an address-only hit. Every compared byte is in a
     * newly validated span; copy only the matched program, not a 64KB bound. */
    for(u32 n=0;n<=RSX_SMC_ENTRIES && !draw->fp;n++){
        rsx_shader_metadata* entry=n==0?hint:&g_ld_smc.entries[n-1u];
        if(!entry || !entry->code || entry->stage!=RSX_SMC_FP || (n && entry==hint))continue;
        if(entry->bytes>available){
            const u8* larger=guest_ptr(location,offset,entry->bytes);
            if(!larger)continue;source=larger;available=entry->bytes;
        }
        if(!rsx_smc_matches(entry,source,available))continue;
        /* The snapshot already holds this entry's code (same entry and serial
         * as the last full copy, verified then): refresh only the inline
         * constant slots, which change per draw. */
        if(entry==g_ld_smc_snap_entry && entry->serial==g_ld_smc_snap_serial){
            for(u32 k=0;k<entry->constant_count;k++)
                memcpy(g_ld_smc_fp_snapshot+entry->constant_offsets[k],source+entry->constant_offsets[k],16);
        } else {
            memcpy(g_ld_smc_fp_snapshot,source,entry->bytes);
            if(!rsx_smc_matches(entry,g_ld_smc_fp_snapshot,entry->bytes)){g_ld_smc_snap_entry=NULL;break;}
            g_ld_smc_snap_entry=entry; g_ld_smc_snap_serial=entry->serial;
        }
        draw->fp=rsx_smc_pin(&g_ld_smc,entry);
        if(draw->fp){
            if(entry==hint)g_ld_smc.hint_hits++;else g_ld_smc.content_hits++;
            rsx_smc_set_hint(&g_ld_smc,address,entry);
        }
    }
    if(!draw->fp){
        source=guest_ptr(location,offset,RSX_SMC_MAX_BYTES);
        if(source){
            const u32 bytes=rsx_fp_program_size(source,RSX_SMC_MAX_BYTES);
            if(bytes){
                memcpy(g_ld_smc_fp_snapshot,source,bytes); g_ld_smc_snap_entry=NULL;
                draw->fp=ld_shader_metadata_acquire(RSX_SMC_FP,address,g_ld_smc_fp_snapshot,bytes);
            }
        }
    }
    if(draw->fp)draw->fp_source=g_ld_smc_fp_snapshot;
}
static void ld_shader_metadata_release(ld_shader_draw_metadata* draw)
{
    if(draw->fp)rsx_smc_release(&g_ld_smc,draw->fp);
    if(draw->vp)rsx_smc_release(&g_ld_smc,draw->vp);
    memset(draw,0,sizeof(*draw));
}
static int ld_shader_metadata_should_verify(const ld_shader_draw_metadata* draw)
{
    if(!g_ld_smc_verify || g_ld_smc_verified>=256u || !draw->vp || !draw->fp)return 0;
    u32 i=0;
    for(;i<g_ld_smc_verify_pair_count;i++)
        if(g_ld_smc_verify_pairs[i].vp==draw->vp->serial && g_ld_smc_verify_pairs[i].fp==draw->fp->serial)break;
    if(i==g_ld_smc_verify_pair_count){
        if(i>=128u)return 0;
        g_ld_smc_verify_pairs[i].vp=draw->vp->serial;
        g_ld_smc_verify_pairs[i].fp=draw->fp->serial;
        g_ld_smc_verify_pair_count++;
    }
    if(g_ld_smc_verify_pairs[i].samples>=2u)return 0;
    g_ld_smc_verify_pairs[i].samples++;return 1;
}

/* LINES is RSX primitive2 (rsx_commands.h); retain old triangle keys so
 * legacy diagnostic/capture identities remain comparable. */
static u64 ld_topology_pso_key(u64 key, u32 primitive)
{
    if (primitive == 2u) {
        const u32 line_tag = 0x454E494Cu; /* LINE */
        return fnv1a(&line_tag, sizeof line_tag, key);
    }
    return key;
}
static ID3D12PipelineState* build_pso(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets, u32 primitive, int raw28)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    static u32 compile_fail_logs = 0;
    static u32 create_fail_logs = 0;
    const u32 vs_length = (u32)strlen(vs_hlsl);
    const u32 ps_length = (u32)strlen(ps_hlsl);
    const u64 vs_hash = fnv1a(
        vs_hlsl, vs_length, 1469598103934665603ull);
    const u64 ps_hash = fnv1a(
        ps_hlsl, ps_length, 1469598103934665603ull);
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_hlsl(
        vs_hlsl, g_ld_profile.vs_hashes, &g_ld_profile.n_vs_hashes,
        &g_ld_profile.total.vs_unique);
    ld_profile_note_hlsl(
        ps_hlsl, g_ld_profile.ps_hashes, &g_ld_profile.n_ps_hashes,
        &g_ld_profile.total.ps_unique);
    g_ld_profile.total.vs_blob_lookups++;
    const LONGLONG vs_lookup_begin = ld_profile_qpc();
#endif
    int vs_post_boundary_hit = 0;
    int vs_hash_seen = 0;
    vs = shader_blob_cache_find(
        &g.vs_blobs, vs_hlsl, vs_length, vs_hash,
        &vs_post_boundary_hit, &vs_hash_seen);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vs_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - vs_lookup_begin);
#endif
    HRESULT vs_hr = S_OK;
    if (vs) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_hits++;
        if (vs_post_boundary_hit)
            g_ld_profile.total.vs_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_misses++;
#endif
        vs = shader_disk_cache_load('V', vs_hlsl, vs_length, vs_hash);
        if (vs) {
            shader_blob_cache_insert_accounted(
                &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                'V', vs_hash_seen);
        } else {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            const double vs_compile_begin = rsp_now_ms();
            vs_hr = D3DCompile(
                vs_hlsl, vs_length, "xvs", NULL, NULL, "main",
                "vs_5_0", 0, 0, &vs, &err);
            rsp_rt_note_compile('V', vs_hash, vs_length,
                rsp_now_ms() - vs_compile_begin, g_ld_frames, vs_hr,
                ld_shader_cache_dir_or_null());
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(vs_hr)) {
                shader_disk_cache_store(
                    'V', vs_hlsl, vs_length, vs_hash, vs);
                shader_blob_cache_insert_accounted(
                    &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                    'V', vs_hash_seen);
            }
        }
    }
    if (FAILED(vs_hr)) {
        if (PS3_LOG_FIRST(compile_fail_logs, 32)) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] vertex compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookups++;
    const LONGLONG ps_lookup_begin = ld_profile_qpc();
#endif
    int ps_post_boundary_hit = 0;
    int ps_hash_seen = 0;
    ps = shader_blob_cache_find(
        &g.ps_blobs, ps_hlsl, ps_length, ps_hash,
        &ps_post_boundary_hit, &ps_hash_seen);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - ps_lookup_begin);
#endif
    HRESULT ps_hr = S_OK;
    if (ps) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_hits++;
        if (ps_post_boundary_hit)
            g_ld_profile.total.ps_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_misses++;
#endif
        ps = shader_disk_cache_load('P', ps_hlsl, ps_length, ps_hash);
        if (ps) {
            shader_blob_cache_insert_accounted(
                &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                'P', ps_hash_seen);
        } else {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            const double ps_compile_begin = rsp_now_ms();
            ps_hr = D3DCompile(
                ps_hlsl, ps_length, "xps", NULL, NULL, "main",
                "ps_5_0", 0, 0, &ps, &err);
            rsp_rt_note_compile('P', ps_hash, ps_length,
                rsp_now_ms() - ps_compile_begin, g_ld_frames, ps_hr,
                ld_shader_cache_dir_or_null());
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(ps_hr)) {
                shader_disk_cache_store(
                    'P', ps_hlsl, ps_length, ps_hash, ps);
                shader_blob_cache_insert_accounted(
                    &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                    'P', ps_hash_seen);
            }
        }
    }
    if (FAILED(ps_hr)) {
        if (PS3_LOG_FIRST(compile_fail_logs, 32)) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] pixel compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    const u32 input_count =
        masked_layout ? masked_layout->count : RSX_DSP_NUM_VERTEX_ATTR;
    for (u32 slot = 0; slot < input_count; slot++) {
        const u32 attr =
            masked_layout ? masked_layout->attrs[slot] : slot;
        D3D12_INPUT_ELEMENT_DESC e = {
            "ATTR", attr, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
            masked_layout && !packed_offsets ? attr * 16u : slot * 16u,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        };
        il[slot] = e;
    }
    if(raw28==2 && g_ld_rawn_cur){
        static const DXGI_FORMAT uf[5]={DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R32G32_UINT,
                                        DXGI_FORMAT_R32G32B32_UINT,DXGI_FORMAT_R32G32B32A32_UINT};
        for(u32 i=0;i<g_ld_rawn_cur->count;i++){
            const rsx_rawn_attr* ra=&g_ld_rawn_cur->a[i];
            D3D12_INPUT_ELEMENT_DESC e={"ATTR",ra->attr,
                ra->type==RSX_RAWN_CONST?DXGI_FORMAT_R32G32B32A32_FLOAT:uf[ra->words],
                0,ra->offset,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
            il[i]=e;
        }
    }
    if(raw28==1){
        for(u32 i=0;i<5;i++){
            D3D12_INPUT_ELEMENT_DESC e={"ATTR",rsx_raw28_ia[i].attr,
                rsx_raw28_ia[i].components==3?DXGI_FORMAT_R32G32B32_UINT:DXGI_FORMAT_R32_UINT,
                0,rsx_raw28_ia[i].offset,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
            il[i]=e;
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig_x;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = input_count ? il : NULL;
    pd.InputLayout.NumElements = raw28==1 ? 5u : raw28==2 && g_ld_rawn_cur ? g_ld_rawn_cur->count : input_count;
    apply_render_state(&pd, rs);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = primitive == 2u ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                             : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rs->rt_count ? rs->rt_count : 1;
    for (u32 r = 0; r < pd.NumRenderTargets; r++)
        pd.RTVFormats[r] = ld_surface_format(rs->rt_color_format);
    pd.SampleDesc.Count = 1;
    /* Local pipeline library (built on this PC for this adapter/driver)
     * first; a miss creates the PSO exactly as before and records the
     * descriptor for the next preparation run. */
    rsp_rt_query pso_query;
    ID3D12PipelineState* pso = rsp_rt_pso_lookup(
        &pso_query, g.dev, ld_shader_cache_dir_or_null(), &pd,
        vs_hash, vs_length, ps_hash, ps_length);
    HRESULT hr = S_OK;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_calls++;
    const LONGLONG create_begin = ld_profile_qpc();
#endif
    if (!pso) {
        hr = g.dev->lpVtbl->CreateGraphicsPipelineState(
            g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
        rsp_rt_pso_created(&pso_query, hr, g_ld_frames);
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_qpc +=
        (u64)(ld_profile_qpc() - create_begin);
#endif
    if (FAILED(hr)) {
        const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        if (PS3_LOG_FIRST(create_fail_logs, 32))
            fprintf(stderr,
                    "[pso-fail] create hr=0x%08lX removed=0x%08lX "
                    "depth{test=%u write=%u func=%u} blend=%u "
                    "cull{enable=%u face=%u}\n",
                    (unsigned long)hr, (unsigned long)removed,
                    rs->depth_test, rs->depth_write, rs->depth_func,
                    rs->blend_enable, rs->cull_enable, rs->cull_face);
        if (removed != S_OK)
            ld_dump_dred("CreateGraphicsPipelineState", hr);
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    return SUCCEEDED(hr) ? pso : NULL;
}

static int ld_current_vertex_layout(rsx_vertex_layout_plan* layout)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) {
        ld_layout_plan_get(0xFFFFu, layout);
        return 0;
    }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    if(ld_shader_metadata_enabled()&&!g_ld_smc_busy){
        g_ld_smc_busy=1;
        rsx_shader_metadata* meta=ld_shader_metadata_acquire(RSX_SMC_VP,start,
            vp_uc,(RSX_DSP_VP_INSTR-start)*16);
        if(meta){
            const u32 mask=meta->inputs.exact?meta->inputs.input_mask:0xffffu;
            ld_layout_plan_get(mask,layout);rsx_smc_release(&g_ld_smc,meta);
            g_ld_smc_busy=0;return 1;
        }
        g_ld_smc_busy=0;
    }
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (!vp_instrs ||
        rsx_vp_analyze_inputs(
            vp_uc, vp_instrs * 16, &analysis) != (int)vp_instrs ||
        !analysis.exact)
        analysis.input_mask = 0xFFFFu;
    ld_layout_plan_get(analysis.input_mask, layout);
    return vp_instrs != 0;
}

/* LD_PSO_DBG=1: name the first bail-out of each kind in get_pso(). Every exit
 * there returns NULL silently and the caller only counts drop{pso=N}, so a
 * title whose draws ALL die in this function has no way to say which gate
 * rejected them. One line per distinct reason, so a stuck title prints a
 * handful of lines rather than one per draw. */
static void ld_pso_bail(int* said, const char* reason)
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("LD_PSO_DBG") ? 1 : 0;
    if (!dbg || *said) return;
    *said = 1;
    fprintf(stderr, "[pso-bail] %s\n", reason);
    fflush(stderr);
}
#define LD_PSO_BAIL(reason) \
    do { static int _said_ = 0; ld_pso_bail(&_said_, (reason)); } while (0)

/* Represent RSX 1D textures as width-by-one Texture2D resources. Collapsing
 * the second coordinate here is essential: its wrap/border and derivatives
 * must not affect either the sampled value or automatic mip selection. */
static int ld_fp_apply_1d_rows(char* hlsl, u32 capacity, u32 mask,
                             u32 unnorm_mask, const u32 widths[16])
{
    if (!hlsl || !capacity || strlen(hlsl) >= capacity) return -1;
    int patched = 0;
    static const char* operations[] = {"Sample", "SampleBias", "SampleLevel"};
    for (u32 unit = 0; unit < 16; unit++) {
        if (!(mask & (1u << unit))) continue;
        if ((unnorm_mask & (1u << unit)) && !widths[unit]) return -1;
        for (u32 separated = 0; separated < 2; separated++)
            for (u32 op = 0; op < 3; op++) {
                char needle[80];
                snprintf(needle, sizeof needle, separated ? "rsx_tex%u.%s(" : "rsx_tex[%u].%s(",
                         unit, operations[op]);
                char* search = hlsl;
                char* hit;
                while ((hit = strstr(search, needle)) != NULL) {
                    /* Find the sampler/coordinate comma, then the end of the
                     * balanced coordinate expression (before optional LOD). */
                    char* begin = strchr(hit + strlen(needle), ',');
                    if (!begin) return -1;
                    begin++;
                    char* end = begin;
                    int depth = 0;
                    for (; *end; end++) {
                        if (*end == '(') depth++;
                        else if (*end == ')') { if (!depth) break; depth--; }
                        else if (*end == ',' && !depth) break;
                    }
                    if (!*end || depth) return -1;
                    const size_t n = (size_t)(end - begin);
                    const size_t length = strlen(hlsl);
                    char suffix[80];
                    snprintf(suffix, sizeof suffix, (unnorm_mask & (1u << unit))
                             ? ").x / %u.0, 0.5)" : ").x, 0.5)", widths[unit]);
                    const size_t prefix_n = strlen(" float2((");
                    const size_t suffix_n = strlen(suffix);
                    const size_t replacement_n = prefix_n + n + suffix_n;
                    if (length + replacement_n - n >= capacity) return -1;
                    char* replacement = (char*)malloc(replacement_n + 1);
                    if (!replacement) return -1;
                    memcpy(replacement, " float2((", prefix_n);
                    memcpy(replacement + prefix_n, begin, n);
                    memcpy(replacement + prefix_n + n, suffix, suffix_n + 1);
                    memmove(begin + replacement_n, end, strlen(end) + 1);
                    memcpy(begin, replacement, replacement_n);
                    free(replacement);
                    search = begin + replacement_n;
                    patched++;
                }
            }
    }
    return patched;
}

static ID3D12PipelineState* get_pso_impl(
    const rsx_vertex_layout_plan* masked_layout, int packed_payload, u32 primitive, int raw28,
    const ld_shader_draw_metadata* metadata)
{
    memset(&g_ld_current_pso, 0, sizeof(g_ld_current_pso));
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) { LD_PSO_BAIL("no vertex program uploaded (vp start out of range)"); return NULL; }
    const u8* vp_uc = metadata&&metadata->vp?metadata->vp->code:(const u8*)(g.rsx.vp + start * 4);
    const u32 vp_instrs = metadata&&metadata->vp?metadata->vp->bytes/16:rsx_vp_program_size_instrs(vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs) { LD_PSO_BAIL("vertex program has zero instructions"); return NULL; }

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = metadata&&metadata->fp?metadata->fp_source:guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc) { LD_PSO_BAIL("fragment program address does not resolve to guest memory"); return NULL; }
    const u32 fp_size = metadata&&metadata->fp?metadata->fp->bytes:rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) {
        /* No END-bit instruction in 64 KB: the bytes at (location, offset) are
         * not a fragment program. Almost always the LOCATION is wrong, not the
         * offset -- the same class of bug caner hit in Yakuza with textures --
         * so report the register the guest actually wrote and the head of what
         * we read, which is what says local-vs-main. */
        static int said = 0;
        if (!said && getenv("LD_PSO_DBG")) {
            said = 1;
            fprintf(stderr,
                    "[pso-bail] fragment program size reads as zero: "
                    "reg=0x%08X -> loc=%u off=0x%08X, first words "
                    "%08X %08X %08X %08X\n",
                    rsx_dsp_reg(&g.rsx, 0x08E4 /* M_FP_ACTIVE_PROGRAM */), fp_loc, fp_off,
                    rsx_fp_read_word(fp_uc + 0), rsx_fp_read_word(fp_uc + 4),
                    rsx_fp_read_word(fp_uc + 8), rsx_fp_read_word(fp_uc + 12));
            /* All-zero here has two very different causes: the offset resolved
             * into the wrong memory, or nothing ever wrote the buffer. Scan
             * forward for the first non-zero byte -- a hit a few hundred bytes
             * on means the region IS populated and the offset is wrong; no hit
             * across 64 KB means the producer never ran. */
            {
                const u8* q = guest_ptr(fp_loc, fp_off, 0x10000);
                u32 i = 0;
                if (q) { while (i < 0x10000u && !q[i]) i++; }
                if (!q)
                    fprintf(stderr, "[pso-bail]   (64 KB window not mapped)\n");
                else if (i == 0x10000u)
                    fprintf(stderr, "[pso-bail]   64 KB from here is entirely "
                                    "zero -- nothing ever wrote this buffer\n");
                else
                    fprintf(stderr, "[pso-bail]   first non-zero byte at "
                                    "+0x%X -- the region is populated, the "
                                    "offset is wrong\n", i);
            }
            fflush(stderr);
        }
        return NULL;
    }
    /* re-resolve with the true size to validate the whole program is mapped */
    fp_uc = metadata&&metadata->fp?metadata->fp_source:guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc) { LD_PSO_BAIL("fragment program is not fully mapped in guest memory"); return NULL; }

    /* Fragment output register mode (fp16 h0 vs fp32 r0) is driven by the
     * SHADER_CONTROL word bit 0x40 (same fix as the replay harness — the
     * AUTO heuristic returned stale fp32 scratch for h0-writing materials);
     * fold the deciding bit into the cache key so a program reused under a
     * different export mode gets its own PSO. Kill-switch RSX_FP_CTRL_AUTO=1
     * restores the old heuristic for the A/B. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("RSX_FP_CTRL_AUTO") ? 1 : 0;
    const u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO : rsx_dsp_shader_control(&g.rsx);
    u32 cube_mask = 0, row1d_mask = 0, row1d_width[16] = {0};
    for (u32 unit = 0; unit < RSX_DSP_NUM_TEXTURES; unit++) {
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        if (texture.enabled && texture.cubemap)
            cube_mask |= 1u << unit;
        if (texture.enabled && texture.dimension == 1 &&
            texture.height == 1 && !texture.cubemap) {
            row1d_mask |= 1u << unit;
            row1d_width[unit] = texture.width;
        }
    }
    rsx_fp_shadow_desc shadow[16] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = metadata&&metadata->fp?metadata->fp->sampled_units:rsx_fp_texture_units(fp_uc, fp_size);
    for (u32 unit = 0; unit < 16; unit++) {
        if (!(sampled_units & (1u << unit))) continue;
        rsx_dsp_texture texture;
        rsx_dsp_get_texture(&g.rsx, unit, &texture);
        rsx_fp_shadow_desc_from_texture(&shadow[unit], texture.format,
            texture.dimension, texture.cubemap, texture.mipmaps,
            texture.filter, texture.wrap, texture.control0);
        if (shadow[unit].enabled) shadow_mask |= 1u << unit;
    }
    /* sRGB: the RSX linearizes gamma-flagged texture fetches and re-encodes
     * the colour output (same rules as rsx_draw_engine). The live path did
     * neither, so lit scenes came out much too dark. LD_NO_GAMMA=1 or a
     * no_gamma file in the working directory (checked ~1/s) turns it off. */
    static int gamma_off_env = -1, gamma_off_file = 0;
    static ULONGLONG gamma_next = 0;
    if (gamma_off_env < 0) gamma_off_env = getenv("LD_NO_GAMMA") ? 1 : 0;
    { const ULONGLONG now = GetTickCount64();
      if (now >= gamma_next) {
          gamma_next = now + 1000;
          const int was = gamma_off_file;
          gamma_off_file = GetFileAttributesA("no_gamma") != INVALID_FILE_ATTRIBUTES;
          if (was != gamma_off_file)
              fprintf(stderr, "[live-draw] sRGB gamma %s (no_gamma file)\n", gamma_off_file ? "OFF" : "ON");
      } }
    u8 gamma_masks[16] = {0};
    u32 gamma_units = 0, srgb_output = 0;
    if (!gamma_off_env && !gamma_off_file) {
        for (u32 gu = 0; gu < 16; ++gu) {
            rsx_dsp_texture gt; rsx_dsp_get_texture(&g.rsx, gu, &gt);
            if (gt.enabled) gamma_masks[gu] = rsx_texture_gamma_mask(gt.format, gt.remap, gt.filter, gt.wrap);
        }
        gamma_units = rsx_fp_texture_gamma_units(fp_uc, fp_size, gamma_masks);
        for (u32 gu = 0; gu < 16; ++gu) if (!(gamma_units & (1u << gu))) gamma_masks[gu] = 0;
        srgb_output = rsx_fp_srgb_output_enabled(rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl,
                                                 rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu) ? 1u : 0u;
    }
    const u32 vtex_mask = vertex_texture_mask();
    const u32 txl_mask = metadata&&metadata->vp?metadata->vp->txl_mask:vp_txl_unit_mask(vp_uc, vp_instrs);
    if (txl_mask && !(txl_mask & vtex_mask)) {
        g_ld_vtex_missing_for_txl++;
        static u32 warned = 0;
        if (PS3_LOG_FIRST(warned, 32) && vertex_texture_trace_enabled()) {
            fprintf(stderr,
                    "[vtex] TXL shader has no supported binding txl=0x%X "
                    "bound=0x%X start=%u instrs=%u\n",
                    txl_mask, vtex_mask, start, vp_instrs);
            for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
                if (!((txl_mask >> u) & 1u)) continue;
                const u32 base = 0x0900 + u * 0x20;
                fprintf(stderr,
                        "[vtex] raw u%u off=%08X fmt=%08X wrap=%08X "
                        "ctl0=%08X ctl3=%08X filter=%08X rect=%08X\n",
                        u, rsx_dsp_reg(&g.rsx, base),
                        rsx_dsp_reg(&g.rsx, base + 4),
                        rsx_dsp_reg(&g.rsx, base + 8),
                        rsx_dsp_reg(&g.rsx, base + 12),
                        rsx_dsp_reg(&g.rsx, base + 16),
                        rsx_dsp_reg(&g.rsx, base + 20),
                        rsx_dsp_reg(&g.rsx, base + 24));
            }
        }
    }

    render_state_t rs;
    decode_render_state(&rs);
    if ((metadata&&metadata->fp?rsx_smc_collect_constants(metadata->fp,fp_uc,fp_size,&g.fp_constants):
         rsx_fp_collect_constants(fp_uc, fp_size, &g.fp_constants)) < 0) {
        LD_PSO_BAIL("fragment program constant collection failed");
        return NULL;
    }
    g.fp_alpha_ref = rsx_fp_alpha_ref(
        rs.alpha_ref_raw, rs.alpha_ref_format);

    /* The vertex program only changes when words are uploaded: hash it once
     * per (start, size, upload generation) instead of twice per draw. */
    u64 vp_hash_once;
    if (metadata && metadata->vp) vp_hash_once = metadata->vp->content_hash;
    else {
        extern volatile unsigned g_rsx_vp_upload_gen;
        static u32 c_start = 0xFFFFFFFFu, c_instrs, c_gen; static u64 c_hash;
        const u32 gen = g_rsx_vp_upload_gen;
        if (c_start != start || c_instrs != vp_instrs || c_gen != gen) {
            c_hash = fnv1a(vp_uc, vp_instrs * 16, 1469598103934665603ull);
            c_start = start; c_instrs = vp_instrs; c_gen = gen;
        }
        vp_hash_once = c_hash;
    }
    u64 key = vp_hash_once;
    if (ld_fp_constants_buffered())
        key = metadata&&metadata->fp?rsx_smc_fp_hash(metadata->fp,key):rsx_fp_structural_hash(fp_uc, fp_size, key);
    else
        key = fnv1a(fp_uc, fp_size, key);
    if (!key) {
        LD_PSO_BAIL("shader hash collapsed to zero");
        return NULL;
    }
    const u32 fp_ctrl_key = fp_ctrl & 0x4Eu;   /* 32-bit exports, depth export */
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    key = fnv1a(&cube_mask, sizeof(cube_mask), key);
    key = fnv1a(&row1d_mask, sizeof(row1d_mask), key);
    if (row1d_mask) key = fnv1a(row1d_width, sizeof(row1d_width), key);
    if (shadow_mask) {
        key = fnv1a(&shadow_mask, sizeof(shadow_mask), key);
        key = fnv1a(shadow, sizeof(shadow), key);
    }
    if (srgb_output) key = fnv1a(&srgb_output, sizeof(srgb_output), key);
    if (gamma_units) key = fnv1a(gamma_masks, sizeof(gamma_masks), key);
    key = fnv1a(&vtex_mask, sizeof(vtex_mask), key);
    /* Texel-addressed (RSX_TEX_FMT_UNNORM) units need their coordinates divided
     * by the texture size in the shader -- D3D12 has no unnormalised addressing
     * mode. The divisor is baked into the HLSL, so the sizes are part of the
     * PSO identity and must be in the key. */
    u32 unnorm_mask = 0, shadow_scaled = 0;
    u32 unnorm_dim[16][2] = {{0}};
    for (u32 uu = 0; uu < 16; uu++) {
        rsx_dsp_texture ut; rsx_dsp_get_texture(&g.rsx, uu, &ut);
        if (!ut.enabled || !(ut.format & TEX_FMT_UNNORM)) continue;
        if (!ut.width || !ut.height) continue;
        unnorm_mask |= 1u << uu;
        unnorm_dim[uu][0] = ut.width;
        unnorm_dim[uu][1] = ut.height;
        /* A depth texture bound to a zeta's snapshot reads that host
         * resource, which is as large as the largest pass that used the zeta
         * (bounded by the zeta), not as large as the texture. Texel
         * coordinates address the resource, so divide by its size: a texture
         * smaller than the zeta then reads its top-left corner, as it does in
         * VRAM, instead of a stretched copy of the whole depth buffer. */
        if ((ut.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) ==
                TEX_FMT_DEPTH24_D8) {
            for (u32 zi = 0; zi < g.n_zdepths; zi++) {
                const zdepth_t* z = &g.zdepths[zi];
                if (z->location != ut.location || z->offset != ut.offset)
                    continue;
                /* Only when the bind will take the snapshot (it has, or
                 * will copy, a depth-writing pass); a zeta that was only
                 * cleared is read from guest memory at texture size. */
                /* Render scale: the snapshot is host-sized but the shader's
                 * texel coordinates are guest texels; divide by the guest
                 * size of the snapshot (dividing by the host size read only
                 * the top-left 1/scale of the depth). */
                const u32 gsw = z->scaled ? (u32)(((u64)z->snapshot_w * g_rs_den + g_rs_num / 2) / g_rs_num) : z->snapshot_w;
                const u32 gsh = z->scaled ? (u32)(((u64)z->snapshot_h * g_rs_den + g_rs_num / 2) / g_rs_num) : z->snapshot_h;
                if (z->snapshot && (z->had_write || z->snapshot_valid) &&
                    gsw >= ut.width && gsh >= ut.height) {
                    unnorm_dim[uu][0] = gsw;
                    unnorm_dim[uu][1] = gsh;
                    /* The depth-compare helper divides by GetDimensions()
                     * (host size) itself; give it the guest size instead. */
                    if (z->scaled && shadow[uu].enabled && shadow[uu].unnormalized) {
                        shadow[uu].unnorm_w = (u16)gsw;
                        shadow[uu].unnorm_h = (u16)gsh;
                        shadow_scaled = 1;
                    }
                }
                break;
            }
        }
    }
    key = fnv1a(&unnorm_mask, sizeof(unnorm_mask), key);
    if (unnorm_mask) key = fnv1a(unnorm_dim, sizeof(unnorm_dim), key);
    if (shadow_scaled) key = fnv1a(shadow, sizeof(shadow), key);   /* guest divisors set above */
    if (masked_layout) {
        static const u32 masked_layout_tag = 0x314B534Du; /* "MSK1" */
        const u32 payload_stride =
            packed_payload ? masked_layout->stride : VERT_STRIDE;
        key = fnv1a(
            &masked_layout_tag, sizeof(masked_layout_tag), key);
        key = fnv1a(
            &masked_layout->mask, sizeof(masked_layout->mask), key);
        key = fnv1a(
            &payload_stride, sizeof(payload_stride), key);
        key = fnv1a(
            &packed_payload, sizeof(packed_payload), key);
    }
    if (ld_fp_constants_buffered())
        key = ld_hash_structural_render_state(&rs, key);
    else
        key = fnv1a(&rs, sizeof(rs), key);
    key = ld_topology_pso_key(key, primitive);
    if(raw28==1){const u32 tag=0x31383252u;key=fnv1a(&tag,sizeof tag,key);} /* R281 */
    if(raw28==2){if(!g_ld_rawn_cur){LD_PSO_BAIL("rawn without layout");return NULL;}
                 const u32 tag=0x4E574152u;key=fnv1a(&tag,sizeof tag,key);
                 key=fnv1a(&g_ld_rawn_cur->sig,sizeof g_ld_rawn_cur->sig,key);} /* RAWN */
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_fp_variants(
        fp_uc, fp_size, fp_ctrl_key, cube_mask, &rs);
#endif
    g_ld_current_pso.valid = 1;
    g_ld_current_pso.key = key;
    g_ld_current_pso.vp_hash = vp_hash_once;
    g_ld_current_pso.vp_start = start;
    g_ld_current_pso.vp_instrs = vp_instrs;
    g_ld_current_pso.fp_location = fp_loc;
    g_ld_current_pso.fp_offset = fp_off;
    g_ld_current_pso.fp_size = fp_size;
    g_ld_current_pso.fp_control = fp_ctrl;
    g_ld_current_pso.cube_mask = cube_mask;
    g_ld_current_pso.vtex_mask = vtex_mask;
    g_ld_current_pso.txl_mask = txl_mask;
    g_ld_current_pso.input_mask =
        masked_layout ? masked_layout->mask : 0xFFFFu;
    g_ld_current_pso.input_stride =
        raw28==1 ? 28u : raw28==2 ? g_ld_rawn_cur->stride : masked_layout && packed_payload
            ? masked_layout->stride : VERT_STRIDE;
    g_ld_current_pso.packed_offsets =
        masked_layout && packed_payload;
    if (ld_canonical_fp_enabled()) {
        g_ld_capture_shadow_mask = shadow_mask;
        g_ld_capture_row1d_mask = row1d_mask;
        g_ld_capture_unnorm_mask = unnorm_mask;
        memcpy(g_ld_capture_shadow, shadow, sizeof shadow);
        memcpy(g_ld_capture_unnorm_dim, unnorm_dim, sizeof unnorm_dim);
        memcpy(g_ld_capture_row1d_width, row1d_width, sizeof row1d_width);
    }

#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_lookups++;
#endif
    /* Observation only: output encoding is not inferred from dark pixels.
     * Include packer in the diagnostic identity because the current PSO key
     * does not yet consume it. Gate late scenes so startup cannot fill cap. */
    {
        static int enabled = -1;
        static u32 from_frame;
        static struct { u64 key; u32 packer, ctrl, format; } seen[64];
        static u32 count;
        if (enabled < 0) {
            const char* value = getenv("LD_SRGB_DIAG");
            enabled = value && atoi(value) != 0;
            value = getenv("LD_SRGB_DIAG_FROM");
            from_frame = value ? (u32)strtoul(value, NULL, 10) : 0;
        }
        if (enabled && g_ld_frames >= from_frame && count < 64) {
            const u32 packer = rsx_dsp_reg(&g.rsx, 0x1FECu);
            const u32 ctrl = rsx_dsp_shader_control(&g.rsx);
            const u32 format = rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu;
            u32 i = 0;
            for (; i < count; i++)
                if (seen[i].key == key && seen[i].packer == packer &&
                    seen[i].ctrl == ctrl && seen[i].format == format) break;
            if (i == count) {
                seen[count].key = key; seen[count].packer = packer;
                seen[count].ctrl = ctrl; seen[count].format = format;
                count++;
                fprintf(stderr, "[srgb-state] frame=%u pso=%016llX packer=%08X fpctrl=%08X colorfmt=%u effective=%d n=%u/64\n",
                        g_ld_frames, (unsigned long long)key, packer, ctrl, format,
                        rsx_fp_srgb_output_enabled(packer, ctrl, format), count);
            }
        }
    }
    /* Hash index over g.psos (was a linear scan of up to 8192 per draw). */
    for (u32 h = (u32)(key ^ (key >> 29)) & (LD_PSO_INDEX - 1u), n = 0; n < LD_PSO_INDEX; n++, h = (h + 1u) & (LD_PSO_INDEX - 1u)) {
        const u32 slot = g_ld_pso_index[h];
        if (!slot) break;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.pso_probes++;
#endif
        if (g.psos[slot - 1u].key == key) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.pso_hits++;
#endif
            return g.psos[slot - 1u].pso;
        }
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_misses++;
#endif
    if (g.n_psos >= MAX_PSOS) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.pso_full++;
#endif
        LD_PSO_BAIL("PSO cache full (MAX_PSOS)");
        return NULL;
    }

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
#if defined(YZ_PERF_PROFILE)
    const LONGLONG decompile_begin = ld_profile_qpc();
#endif
    rsx_vp_set_branch_base(rsx_dsp_vp_start(&g.rsx));
    const int vi = masked_layout
        ? rsx_vp_decompile_compact_ex(
            vp_uc, vp_instrs * 16, vtex_mask, masked_layout->mask,
            vs_hlsl, sizeof(vs_hlsl))
        : rsx_vp_decompile_ex(
            vp_uc, vp_instrs * 16, vtex_mask, vs_hlsl, sizeof(vs_hlsl));
    if(raw28==1 && (vi<=0 || !rsx_raw28_shader(vs_hlsl,sizeof vs_hlsl))){
        LD_PSO_BAIL("raw28 shader adapter rejected output");return NULL;
    }
    if(raw28==2 && (vi<=0 || !g_ld_rawn_cur || !rsx_rawn_shader(vs_hlsl,sizeof vs_hlsl,g_ld_rawn_cur))){
        LD_PSO_BAIL("rawn shader adapter rejected output");return NULL;
    }
    u32 decompiled_constant_count = 0;
    int fi;
    if (ld_fp_constants_buffered()) {
        fi = rsx_fp_decompile_buffered_shadow_color_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask, shadow, gamma_masks, ps_hlsl,
            sizeof(ps_hlsl), &decompiled_constant_count);
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(ps_hlsl, sizeof(ps_hlsl)) < 0)
            fi = -1;
        if (fi > 0 &&
            decompiled_constant_count != g.fp_constants.count)
            fi = -1;
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func) < 0)
            fi = -1;
        if (fi > 0) {
            const int np = rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask & ~row1d_mask & ~shadow_mask, unnorm_dim, cube_mask);
            { static int n = 0;
              if (PS3_LOG_FIRST(n, 8))
                  fprintf(stderr, "[unnorm] mask=0x%X cube=0x%X dim0=%ux%u"
                                  " patched=%d\n",
                          unnorm_mask, cube_mask, unnorm_dim[0][0],
                          unnorm_dim[0][1], np); }
            if (np < 0) fi = -1;
        }
    } else {
        if (shadow_mask) {
            LD_PSO_BAIL("depth comparison requires buffered fragment constants");
            return NULL;
        }
        fi = rsx_fp_decompile_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask,
            ps_hlsl, sizeof(ps_hlsl));
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func,
                g.fp_alpha_ref) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask & ~row1d_mask,
                unnorm_dim, cube_mask) < 0)
            fi = -1;
    }
    if (fi > 0 && ld_fp_apply_1d_rows(ps_hlsl, sizeof(ps_hlsl),
                                    row1d_mask, unnorm_mask, row1d_width) < 0)
        fi = -1;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.decompile_qpc +=
        (u64)(ld_profile_qpc() - decompile_begin);
#endif
    /* LD_FORCE_GREEN=1: make every fragment program return solid green.
     *
     * The same trick as LD_CLEAR_TEST, applied one stage later. Clears are
     * visible on these surfaces and draws are not, while the engine reports
     * 18,452 groups executed with zero drops and the GPU state cannot discard
     * anything (viewport 0,0 720x512, scissor full, depth test off, colour mask
     * all channels, blend off, cull off). Two possibilities remain and they need
     * opposite fixes: the draws reach the GPU and the SAMPLED DATA is black, or
     * they never reach it and the accounting is wrong. Green separates them. */
    { static int fg = -1;
      /* LD_FORCE_GREEN=<pso key hex> limits it to one PSO. */
      static u64 gk = 0;
      if (fg < 0) { const char* e = getenv("LD_FORCE_GREEN"); fg = e ? 1 : 0;
                    gk = e ? _strtoui64(e, 0, 16) : 0; if (gk == 1) gk = 0; }
      if (fg && fi > 0 && (!gk || gk == live_legacy_pso_key())) {
          /* The emulated alpha test discards before the return, so replacing
           * only the return still showed nothing for a failing alpha. */
          for (char* d = strstr(ps_hlsl, "discard;"); d; d = strstr(d, "discard;"))
              memcpy(d, "        ", 8);
          char* r = strstr(ps_hlsl, "    return ");
          if (r) {
              char* semi = strchr(r, ';');
              if (semi) {
                  const char rep[] = "    return float4(0,1,0,1);";
                  const size_t rl = sizeof(rep) - 1;
                  const size_t old = (size_t)(semi + 1 - r);
                  const size_t used = strlen(ps_hlsl);
                  if (used - old + rl + 1 <= sizeof(ps_hlsl)) {
                      memmove(r + rl, semi + 1, used - (size_t)(semi + 1 - ps_hlsl) + 1);
                      memcpy(r, rep, rl);
                  }
              }
          }
      } }
    /* LD_FORCE_COL0=1: every vertex program outputs white COLOR0 -- separates
     * "the VS computes a wrong colour" from everything downstream of it. */
    /* LD_FORCE_COL0=<pso key hex> limits it to one PSO. */
    { static int fc = -1; static u64 fk = 0;
      if (fc < 0) { const char* e = getenv("LD_FORCE_COL0"); fc = e ? 1 : 0;
                    fk = e ? _strtoui64(e, 0, 16) : 0; if (fk == 1) fk = 0; }
      char* c0 = fc && vi > 0 && (!fk || fk == live_legacy_pso_key())
                 ? strstr(vs_hlsl, "Out.col0 = o[1];") : NULL;
      if (c0) memcpy(c0, "Out.col0=1+0*_p;", 16); }   /* same length, in place */
    /* LD_HLSL_DUMP=<dir>: files named by the PSO key the draw CSV prints. */
    { static const char* hd = (const char*)-1;
      if (hd == (const char*)-1) hd = getenv("LD_HLSL_DUMP");
      if (hd) {
        char fn[MAX_PATH]; FILE* f;
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.vs.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fputs(vi > 0 ? vs_hlsl : "<vp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.ps.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fprintf(f, "// fp_structural_hash=%016llx\n", (unsigned long long)rsx_fp_structural_hash(fp_uc, fp_size, 1469598103934665603ull)); fputs(fi > 0 ? ps_hlsl : "<fp fail>", f); fclose(f); }
      } }
    if (vi > 0 && fi > 0)
        pso = build_pso(
            vs_hlsl, ps_hlsl, &rs, masked_layout, packed_payload, primitive, raw28);

    g.psos[g.n_psos].key = key;
    g.psos[g.n_psos].pso = pso;
    for (u32 h = (u32)(key ^ (key >> 29)) & (LD_PSO_INDEX - 1u);; h = (h + 1u) & (LD_PSO_INDEX - 1u))
        if (!g_ld_pso_index[h]) { g_ld_pso_index[h] = g.n_psos + 1u; break; }
    if (pso && fi > 0 && ld_canonical_fp_enabled()) {
        const size_t bytes = strlen(ps_hlsl) + 1;
        if (bytes <= 16u * 1024u * 1024u - g_ld_capture_ps_source_bytes) {
            char* source = (char*)malloc(bytes);
            if (source) {
                memcpy(source, ps_hlsl, bytes);
                g.psos[g.n_psos].capture_ps_hlsl = source;
                g_ld_capture_ps_source_bytes += bytes;
            }
        }
    }
    g.n_psos++;
    return pso;
}

static ID3D12PipelineState* get_pso(
    const rsx_vertex_layout_plan* masked_layout,int packed_payload,u32 primitive,int raw28)
{
    const int previous=ld_cpu_draw_phase_switch(LD_CPU_PHASE_PSO);
    ld_shader_draw_metadata metadata={0};
    const int active=ld_shader_metadata_enabled()&&!g_ld_smc_busy;
    if(active){g_ld_smc_busy=1;ld_shader_metadata_prepare(&metadata);}
    ID3D12PipelineState* result=get_pso_impl(masked_layout,packed_payload,primitive,raw28,active?&metadata:NULL);
    if(active && ld_shader_metadata_should_verify(&metadata)){
        g_ld_smc_verified++;
        const ld_pso_metadata observed=g_ld_current_pso;
        rsx_fp_constant_block* constants=(rsx_fp_constant_block*)malloc(sizeof(*constants));
        if(constants)memcpy(constants,&g.fp_constants,sizeof(*constants));
        ID3D12PipelineState* legacy=get_pso_impl(masked_layout,packed_payload,primitive,raw28,NULL);
        const int equal=constants&&memcmp(&observed,&g_ld_current_pso,sizeof observed)==0&&
            memcmp(constants,&g.fp_constants,sizeof(*constants))==0&&result==legacy;
        free(constants);result=legacy;
        if(!equal){
            g_ld_smc_failed=1;
            fprintf(stderr,"[shader-metadata] verify_mismatch=%u fallback=legacy cache_disabled=1\n",g_ld_smc_verified);
        }else if(g_ld_smc_verified<=4u || g_ld_smc_verified%32u==0)
            fprintf(stderr,"[shader-metadata] verify=%u key_masks_constants_equal=1 legacy_result=1\n",g_ld_smc_verified);
    }
    if(active){
        g_ld_smc_calls++;
        if(g_ld_smc_calls<=4u || (g_ld_smc_calls%2048u==0 && g_ld_smc_calls<=32768u))
            fprintf(stderr,"[shader-metadata] draws=%u vp=%d fp=%d hint_hits=%llu content_hits=%llu misses=%llu owned_bytes=%llu\n",g_ld_smc_calls,metadata.vp!=NULL,metadata.fp!=NULL,(unsigned long long)g_ld_smc.hint_hits,(unsigned long long)g_ld_smc.content_hits,(unsigned long long)g_ld_smc.misses,(unsigned long long)g_ld_smc.owned_bytes);
        ld_shader_metadata_release(&metadata);g_ld_smc_busy=0;
    }
    ld_cpu_draw_phase_switch(previous);
    return result;
}

static int ld_upload_pixel_constants(u32* out_offset)
{
    if (out_offset)
        *out_offset = 0;
    if (!ld_fp_constants_buffered())
        return 1;
    if (!g.ps_cb || !g.ps_cb_mapped)
        return 0;

    /* HLSL uses a one-element placeholder array for programs with no inline
     * constants so fp_alpha retains a valid 16-byte slot; fp_window follows. */
    const u32 value_slots =
        g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 data_bytes = (value_slots + 2u) * 16u;
    u32 offset = 0;
    u32 allocation_bytes = 0;
    int plan = rsx_fp_constant_ring_plan(
        g.ps_cb_used, PS_CB_RING_BYTES, data_bytes,
        &offset, &allocation_bytes);
    if (plan < 0) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_constant_capacity_failures++;
#endif
        return 0;
    }
    if (plan == 0) {
        ld_flush(LD_FLUSH_PIXEL_CONSTANT_RING);
        if (!g.ready)
            return 0;
        g.ps_cb_used = 0;
        plan = rsx_fp_constant_ring_plan(
            g.ps_cb_used, PS_CB_RING_BYTES, data_bytes,
            &offset, &allocation_bytes);
        if (plan != 1) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_constant_capacity_failures++;
#endif
            return 0;
        }
    }

#if defined(YZ_PERF_PROFILE)
    const LONGLONG upload_begin = ld_profile_qpc();
#endif
    u8* destination = g.ps_cb_mapped + offset;
    memset(destination, 0, data_bytes);
    if (g.fp_constants.count)
        memcpy(
            destination, g.fp_constants.values,
            (size_t)g.fp_constants.count * 16u);
    rsx_fp_pack_alpha_fog((float*)(destination + value_slots * 16u), g.fp_alpha_ref,
        rsx_dsp_reg(&g.rsx, 0x08CCu), rsx_dsp_reg(&g.rsx, 0x08D0u),
        rsx_dsp_reg(&g.rsx, 0x08D4u));
    rsx_fp_pack_shader_window((float*)(destination + (value_slots + 1u) * 16u),
        rsx_dsp_reg(&g.rsx, 0x1D88u));
    if (g_rs_num != g_rs_den) {
        /* Render scale: WPOS is in guest pixels; the raster runs at host scale. */
        rsx_dsp_surface rsf;
        rsx_dsp_get_surface(&g.rsx, &rsf);
        if (ld_rs_wants_clip(rsf.color_location[0], rsf.clip_w, rsf.clip_h)) {
            float* win = (float*)(destination + (value_slots + 1u) * 16u);
            win[0] *= (float)g_rs_den / (float)g_rs_num;
            win[1] *= (float)g_rs_den / (float)g_rs_num;
        }
    }
    g.ps_cb_used += allocation_bytes;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_constant_allocations++;
    g_ld_profile.total.ps_constant_upload_bytes += data_bytes;
    g_ld_profile.total.ps_constant_upload_qpc +=
        (u64)(ld_profile_qpc() - upload_begin);
#endif
    if (out_offset)
        *out_offset = offset;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Draw accumulation sink (mirrors the harness sink)
 * -----------------------------------------------------------------------*/
typedef struct { float a[16][4]; } vtx_t;
typedef struct { u32 first, count; } batch_t;

typedef struct {
    u64 key;
    ID3D12Resource* buf;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    u8* shadow;          /* CPU copy, for the verification compares */
    u32 bytes, epoch, uses, last_frame;
    u8 bad;
} ld_vcache_entry;
typedef struct {
    u64 key;
    ID3D12Resource* buf;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    u8* raw;                   /* guest index bytes the entry was built from */
    u32 raw_bytes, lo, range, n_source, n_out, last_frame;
    u8 bad, changes;
} ld_icache_entry;
typedef struct {
    /* INLINE_ARRAY source for this primitive, or NULL. The stream is the
     * vertices themselves, in guest byte order, so the fetch path reads it
     * with the same big-endian decoders it uses for a vertex array -- only
     * the base pointer differs. inl_off[i] is attribute i's byte offset
     * within one inline vertex; the attributes are packed in ascending
     * index order, which is what makes the offsets derivable at all (an
     * inline stream carries no per-array offset registers). */
    const u8* inl; u32 inl_bytes, inl_stride; u32 inl_off[16];
    batch_t arr[256]; u32 n_arr;
    batch_t idx[256]; u32 n_idx;
    u32     n_packets;
    vtx_t*  verts; u32 n_verts, cap_verts;
    rsx_vertex_ref* refs; u32 n_refs, cap_refs;
    u32     n_source_refs;
    int     refs_remapped;
    rsx_vertex_remap ref_remap;
    u8* compact_verts; u64 compact_capacity;
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int raw28;
    int rawn;                  /* compact_verts holds rawn_layout raw vertices */
    ld_vcache_entry* vcache;   /* rawn vertices come from this cache entry (no copy) */
    u64 vcache_key;            /* store/verify the scratch copy under this key */
    ld_icache_entry* icache;   /* topology indices come from this cache entry */
    u64 icache_key;            /* build an entry for this draw's indices */
    u32 icache_raw_bytes;      /* guest index bytes stashed in g_icache_pending */
    rsx_rawn_layout rawn_layout;
    int     fetch_ok;
    /* Primitive-restart cut points (s25 port of the replay-harness fix):
     * a guest index equal to the RSX restart sentinel is a cut marker, not
     * a vertex reference (RPCS3 RSXThread.cpp:398); fetch_batches records
     * the n_verts position at each one and the STRIP/FAN expansion must
     * not bridge across a cut. */
    u32*    cuts; u32 n_cuts, cap_cuts;
} draw_ctx;

static draw_ctx dc;

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_verts = dc.n_refs = 0;
    dc.n_source_refs = 0;
    dc.refs_remapped = 0;
    dc.raw28 = 0;
    dc.rawn = 0;
    dc.vcache = NULL;
    dc.vcache_key = 0;
    dc.icache = NULL;
    dc.icache_key = 0;
    dc.n_packets = 0;
    dc.n_cuts = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL; dc.inl_bytes = dc.inl_stride = 0;
}
static void push_vert(const vtx_t* v)
{
    if (dc.n_verts >= dc.cap_verts) {
        u32 nc = dc.cap_verts ? dc.cap_verts * 2 : 4096;
        vtx_t* nv = (vtx_t*)realloc(dc.verts, (size_t)nc * sizeof(vtx_t));
        if (!nv) { dc.fetch_ok = 0; return; }
        dc.verts = nv; dc.cap_verts = nc;
    }
    dc.verts[dc.n_verts++] = *v;
}

static void push_compact_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next_capacity = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* refs = (rsx_vertex_ref*)realloc(
            dc.refs, (size_t)next_capacity * sizeof(*refs));
        if (!refs) {
            dc.fetch_ok = 0;
            return;
        }
        dc.refs = refs;
        dc.cap_refs = next_capacity;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
}


/* ---------------------------------------------------------------------------
 * Static vertex cache (RAWN draws over one dense element range).
 *
 * Most of this title's geometry is static: the same guest vertex bytes are
 * copied every frame into scratch and again into the write-combined upload
 * ring (tens of MB a frame at the save point, where the renderer thread is
 * saturated). A cache entry keeps those raw vertices in a buffer of its own,
 * keyed by layout signature, source ranges and element range. It is reused
 * while guest write-watch reports no write to the source pages since it was
 * filled; the first 4 reuses and every 16th after re-copy and compare
 * (texture-cache rule), and a mismatch disables caching for that key.
 * vcache_off file: copy every draw as before.
 * ---------------------------------------------------------------------------*/
enum { LD_VCACHE_SLOTS = 8192 };
static ld_vcache_entry g_vcache[LD_VCACHE_SLOTS];
static u64 g_vcache_bytes, g_vcache_hits, g_vcache_misses, g_vcache_verify_bad;
#define LD_VCACHE_BUDGET (384ull << 20)

/* ld_ww_dirty_since, but the OS is asked at most once per frame per 1 MB block
 * (a GetWriteWatch per draw and attribute cost more than the copies saved);
 * later checks in the frame read the page stamps it left. */
static int ld_vcache_ww_dirty(const u8* p, u32 len, u32 since)
{
    enum { BLOCK_SHIFT = 20 };
    static u32* checked;   /* frame+1 each 1 MB block was last queried */
    if (g_ww_state != 1 || !p || !len) return 1;
    const uintptr_t off = (uintptr_t)p - (uintptr_t)vm_base;
    if (off >= LD_WW_SPAN || (u64)off + len > LD_WW_SPAN) return 1;
    if (!checked && !(checked = (u32*)calloc((size_t)(LD_WW_SPAN >> BLOCK_SHIFT), sizeof(u32)))) return 1;
    for (u64 b = off >> BLOCK_SHIFT; b <= ((u64)off + len - 1) >> BLOCK_SHIFT; b++) {
        if (checked[b] == g_ld_frames + 1u) continue;
        checked[b] = g_ld_frames + 1u;
        if (ld_ww_dirty_since(vm_base + (b << BLOCK_SHIFT), 1u << BLOCK_SHIFT, UINT32_MAX - 1u) < 0) return 1;
    }
    for (u64 pg = off >> 12; pg <= ((u64)off + len - 1) >> 12; pg++)
        if (g_ww_page[pg] > since) return 1;
    return 0;
}

static u64 ld_vcache_key(const rsx_rawn_layout* L, u32 e0, u32 count)
{
    u64 k = fnv1a(&L->sig, sizeof L->sig, 1469598103934665603ull);
    k = fnv1a(&e0, sizeof e0, k); k = fnv1a(&count, sizeof count, k);
    for (u32 i = 0; i < L->count; i++) {
        const rsx_rawn_attr* a = &L->a[i];
        if (a->type == RSX_RAWN_CONST) { k = fnv1a(a->konst, sizeof a->konst, k); continue; }
        const u8* src = a->base + (u64)(e0 - a->first) * a->stride;
        k = fnv1a(&src, sizeof src, k); k = fnv1a(&a->stride, sizeof a->stride, k);
    }
    return k ? k : 1;
}
static int ld_vcache_dirty(const rsx_rawn_layout* L, u32 e0, u32 count, u32 since)
{
    for (u32 i = 0; i < L->count; i++) {
        const rsx_rawn_attr* a = &L->a[i];
        if (a->type == RSX_RAWN_CONST) continue;
        const u32 bytes = rsx_rawn_bytes(a->type, a->type == RSX_VTX_TYPE_CMP32 ? 1u : a->size);
        const u8* src = a->base + (u64)(e0 - a->first) * a->stride;
        if (ld_vcache_ww_dirty(src, (count - 1u) * a->stride + bytes, since)) return 1;
    }
    return 0;
}
static ld_vcache_entry* ld_vcache_slot(u64 key, int create)
{
    u32 i = (u32)(key ^ (key >> 32)) & (LD_VCACHE_SLOTS - 1);
    ld_vcache_entry* empty = NULL;
    for (u32 n = 0; n < 32; n++, i = (i + 1) & (LD_VCACHE_SLOTS - 1)) {
        if (g_vcache[i].key == key) return &g_vcache[i];
        if (!g_vcache[i].key && !empty) empty = &g_vcache[i];
    }
    return create ? empty : NULL;
}
static void ld_vcache_drop(ld_vcache_entry* e)
{
    if (e->buf) { e->buf->lpVtbl->Unmap(e->buf, 0, NULL); retire_texture(e->buf); }
    free(e->shadow);
    g_vcache_bytes -= e->bytes;
    const u8 bad = e->bad; const u64 key = e->key;
    memset(e, 0, sizeof *e);
    if (bad) { e->key = key; e->bad = 1; }   /* keep the verdict, without data */
}
/* Before the copy: a usable entry for this draw, or NULL. */
static ld_vcache_entry* ld_vcache_find(const rsx_rawn_layout* L, u32 e0, u32 count, u64* key_out)
{
    *key_out = 0;
    if (count < 3) return NULL;
    const u64 key = ld_vcache_key(L, e0, count);
    *key_out = key;
    ld_vcache_entry* e = ld_vcache_slot(key, 0);
    if (!e || e->bad || !e->buf) return NULL;
    if (ld_vcache_dirty(L, e0, count, e->epoch)) { ld_vcache_drop(e); return NULL; }
    e->uses++; e->last_frame = g_ld_frames;
    if (e->uses <= 4 || (e->uses & 15u) == 0) return NULL;   /* verify: caller copies and compares */
    return e;
}
/* After a scratch copy: verify an existing entry, or fill a new one. */
static void ld_vcache_store(u64 key, const u8* data, u32 bytes)
{
    if (!key || !bytes) return;
    ld_vcache_entry* e = ld_vcache_slot(key, 1);
    if (!e) return;
    if (e->key == key && e->bad) return;
    if (e->key == key && e->buf) {
        if (e->bytes != bytes || memcmp(e->shadow, data, bytes)) {
            g_vcache_verify_bad++;
            ld_vcache_drop(e); e->key = key; e->bad = 1;
            static int logged; if (logged++ < 8)
                fprintf(stderr, "[vcache] verification mismatch key=%016llX: this range is no longer cached\n", (unsigned long long)key);
        }
        return;
    }
    if (g_vcache_bytes + bytes > LD_VCACHE_BUDGET) {   /* evict entries idle for 120+ frames */
        for (u32 i = 0; i < LD_VCACHE_SLOTS && g_vcache_bytes + bytes > LD_VCACHE_BUDGET; i++)
            if (g_vcache[i].buf && g_ld_frames - g_vcache[i].last_frame > 120u) ld_vcache_drop(&g_vcache[i]);
        if (g_vcache_bytes + bytes > LD_VCACHE_BUDGET) return;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&buf))) return;
    void* mapped = NULL; D3D12_RANGE none = {0, 0};
    u8* shadow = (u8*)malloc(bytes);
    if (!shadow || FAILED(buf->lpVtbl->Map(buf, 0, &none, &mapped))) { free(shadow); buf->lpVtbl->Release(buf); return; }
    memcpy(mapped, data, bytes); memcpy(shadow, data, bytes);
    e->key = key; e->buf = buf; e->va = buf->lpVtbl->GetGPUVirtualAddress(buf);
    e->shadow = shadow; e->bytes = bytes; e->epoch = g_ww_epoch; e->uses = 0; e->last_frame = g_ld_frames; e->bad = 0;
    g_vcache_bytes += bytes;
}


/* ---------------------------------------------------------------------------
 * Index cache (one dense index batch per draw).
 *
 * Every draw re-parsed its big-endian guest indices, remapped them to the
 * dense vertex range and wrote the topology indices into the upload ring --
 * three passes over the indices per draw, most of them identical to the last
 * frame's. An entry keeps the guest bytes it was built from and the finished
 * topology indices in a buffer of its own. A draw whose guest index bytes
 * compare equal (memcmp, exact) under the same key (index source, count,
 * format, base index, restart, primitive) uses that buffer as is. A key whose
 * bytes keep changing is no longer cached. icache_off file: off.
 * ---------------------------------------------------------------------------*/
enum { LD_ICACHE_SLOTS = 16384 };
static ld_icache_entry g_icache[LD_ICACHE_SLOTS];
static u64 g_icache_bytes, g_icache_hits, g_icache_misses;
static u8* g_icache_pending; static u32 g_icache_pending_cap;
#define LD_ICACHE_BUDGET (128ull << 20)

static int ld_icache_enabled(void)
{
    static int diag = -1;   /* these diagnostics read the occurrence remap */
    if (diag < 0) diag = getenv("RSX_DRAW_CSV_REQUEST_FILE") || getenv("LD_CANONICAL_CAPTURE_DIR");
    if (ld_off_file("no_icache", &s_off_icache_t, &s_off_icache)) return 0;
    return !diag;
}
static ld_icache_entry* ld_icache_slot(u64 key, int create)
{
    u32 i = (u32)(key ^ (key >> 32)) & (LD_ICACHE_SLOTS - 1);
    ld_icache_entry* empty = NULL;
    for (u32 n = 0; n < 32; n++, i = (i + 1) & (LD_ICACHE_SLOTS - 1)) {
        if (g_icache[i].key == key) return &g_icache[i];
        if (!g_icache[i].key && !empty) empty = &g_icache[i];
    }
    return create ? empty : NULL;
}
static void ld_icache_drop_data(ld_icache_entry* e)
{
    if (e->buf) { e->buf->lpVtbl->Unmap(e->buf, 0, NULL); retire_texture(e->buf); }
    free(e->raw);
    g_icache_bytes -= (u64)e->n_out * 4u + e->raw_bytes;
    e->buf = NULL; e->raw = NULL; e->raw_bytes = e->n_out = 0;
}
/* After the remap: build the entry for dc.icache_key from the stashed guest
 * bytes. Returns its mapped index buffer to write the topology indices into,
 * or NULL (write them into the ring as before). */
static u32* ld_icache_store(u64 key, u32 lo, u32 range, u32 n_source, u32 n_out, ld_icache_entry** out)
{
    *out = NULL;
    ld_icache_entry* e = ld_icache_slot(key, 1);
    if (!e) return NULL;
    if (e->key == key && e->bad) return NULL;
    const u8 changes = e->key == key ? e->changes : 0;
    if (e->key == key) ld_icache_drop_data(e);
    if (changes >= 3) { e->key = key; e->bad = 1; return NULL; }   /* rewritten in place each time */
    const u64 need = (u64)n_out * 4u + dc.icache_raw_bytes;
    if (g_icache_bytes + need > LD_ICACHE_BUDGET) {   /* evict entries idle for 120+ frames */
        for (u32 i = 0; i < LD_ICACHE_SLOTS && g_icache_bytes + need > LD_ICACHE_BUDGET; i++)
            if (g_icache[i].buf && g_ld_frames - g_icache[i].last_frame > 120u) ld_icache_drop_data(&g_icache[i]);
        if (g_icache_bytes + need > LD_ICACHE_BUDGET) return NULL;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (u64)n_out * 4u; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&buf))) return NULL;
    void* mapped = NULL; D3D12_RANGE none = {0, 0};
    u8* raw = (u8*)malloc(dc.icache_raw_bytes);
    if (!raw || FAILED(buf->lpVtbl->Map(buf, 0, &none, &mapped))) { free(raw); buf->lpVtbl->Release(buf); return NULL; }
    memcpy(raw, g_icache_pending, dc.icache_raw_bytes);
    e->key = key; e->buf = buf; e->va = buf->lpVtbl->GetGPUVirtualAddress(buf);
    e->raw = raw; e->raw_bytes = dc.icache_raw_bytes;
    e->lo = lo; e->range = range; e->n_source = n_source; e->n_out = n_out;
    e->last_frame = g_ld_frames; e->bad = 0; e->changes = changes;
    g_icache_bytes += need;
    *out = e;
    return (u32*)mapped;
}

static int reserve_compact_vertices(u64 bytes)
{
    if (bytes <= dc.compact_capacity)
        return 1;
    if (bytes > SIZE_MAX)
        return 0;
    u64 next_capacity = dc.compact_capacity ? dc.compact_capacity : 1u << 20;
    while (next_capacity < bytes) {
        if (next_capacity > SIZE_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    u8* vertices = (u8*)realloc(
        dc.compact_verts, (size_t)next_capacity);
    if (!vertices)
        return 0;
    dc.compact_verts = vertices;
    dc.compact_capacity = next_capacity;
    return 1;
}

static int reserve_legacy_vertices(u32 count)
{
    if (count <= dc.cap_verts)
        return 1;
    u32 next_capacity = dc.cap_verts ? dc.cap_verts : 4096u;
    while (next_capacity < count) {
        if (next_capacity > UINT32_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    vtx_t* vertices = (vtx_t*)realloc(
        dc.verts, (size_t)next_capacity * sizeof(*vertices));
    if (!vertices)
        return 0;
    dc.verts = vertices;
    dc.cap_verts = next_capacity;
    return 1;
}

static float ld_be_f32(const u8* p)
{
    const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float ld_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static int fetch_attr(u32 i, u32 base, u32 vertex_id, u32 base_index,
                      float out[4])
{
    rsx_dsp_vertex_attr a;
    rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
    if (!a.type || !a.size) return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const u32 elem_size = rsx_vertex_attrib_size(a.type, a.size);
    const u32 stride = a.stride ? a.stride : elem_size;
    const u32 divider_mask = rsx_dsp_reg(&g.rsx, 0x1FC0);
    if (a.frequency >= 2)
        g_ld_divider_fetches++;
    const u32 source_element = rsx_vertex_element_index(
        vertex_id, base_index, a.frequency,
        (divider_mask >> i) & 1u);
    const u8* p;
    if (dc.inl) {
        /* Inline vertices ignore the array offset and the data base: the
         * stream IS the array, packed at the declared stride. */
        const u64 at = (u64)source_element * dc.inl_stride + dc.inl_off[i];
        if (at + elem_size > dc.inl_bytes) return 0;
        p = dc.inl + at;
    } else {
        p = guest_ptr(
            a.location, base + a.offset + source_element * stride, elem_size);
    }
    if (!p) return 0;
    for (u32 c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case RSX_VTX_TYPE_FLOAT:
            out[c] = ld_be_f32(p + c * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[c] = ld_be_f16(p + c * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[c] = p[c] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[c] = (float)p[c];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = v / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = (float)v;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static void fetch_one(u32 base, u32 vertex_id, u32 base_index)
{
    vtx_t v;
    for (u32 i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
        if (a.type && a.size &&
            fetch_attr(i, base, vertex_id, base_index, v.a[i])) continue;
        if (i == 0) { dc.fetch_ok = 0; return; }
        rsx_dsp_vertex_default(&g.rsx, i, v.a[i]);
        if (i == 3 && v.a[3][0] == 0 && v.a[3][1] == 0 && v.a[3][2] == 0 && v.a[3][3] == 1)
            v.a[3][0] = v.a[3][1] = v.a[3][2] = 1.0f;
    }
    push_vert(&v);
}

static void fetch_batches(void)
{
    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    for (u32 r = 0; r < dc.n_arr && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.arr[r].count && dc.fetch_ok; i++)
            fetch_one(base, dc.arr[r].first + i, 0);
    if (!dc.n_idx) return;
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia; rsx_dsp_get_index_array(&g.rsx, &ia);
    /* Restart sentinel handling, same rule as the replay harness (RPCS3
     * RSXThread.cpp:398 "if (value == restart) continue" + rsx_methods.h
     * restart_index_enabled()/restart_index()): record a cut, never fetch
     * the phantom vertex. Kill-switch RSX_NO_RESTART shared with the
     * harness for byte-exact A/B. */
    static int s_no_restart = -1;
    if (s_no_restart < 0) s_no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
    const int restart_en = !s_no_restart && rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
    for (u32 r = 0; r < dc.n_idx && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.idx[r].count && dc.fetch_ok; i++) {
            const u32 esz = ia.is_u32 ? 4 : 2;
            const u8* ip = guest_ptr(ia.location, ia.offset + (dc.idx[r].first + i) * esz, esz);
            if (!ip) { dc.fetch_ok = 0; return; }
            const u32 index = ia.is_u32
                ? (((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3])
                : (u32)((ip[0] << 8) | ip[1]);
            if (restart_en && index == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                          dc.n_verts)) {
                    dc.fetch_ok = 0;
                    return;
                }
                continue;
            }
            fetch_one(base, index, base_index);
        }
}

static void fetch_batches_hoisted(
    const rsx_vertex_layout_plan* layout, int packed_payload)
{
    for (u32 batch = 0; batch < dc.n_arr && dc.fetch_ok; batch++)
        for (u32 i = 0; i < dc.arr[batch].count && dc.fetch_ok; i++)
            push_compact_ref(dc.arr[batch].first + i, 0);

    int icache_hit = 0;
    /* Several index batches that follow each other in memory parse exactly
     * like one (the native Edge path splits a draw into 256-index batches). */
    u32 icache_count = dc.n_idx ? dc.idx[0].count : 0;
    for (u32 b = 1; b < dc.n_idx && icache_count; b++)
        icache_count = dc.idx[b].first == dc.idx[b - 1].first + dc.idx[b - 1].count
            ? icache_count + dc.idx[b].count : 0;
    if (packed_payload && icache_count && !dc.n_arr && !dc.n_refs && dc.fetch_ok && ld_icache_enabled()) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 es = ia.is_u32 ? 4u : 2u, count = icache_count;
        const u64 start64 = (u64)ia.offset + (u64)dc.idx[0].first * es, bytes64 = (u64)count * es;
        const int restart = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u8* src = count >= 6 && start64 + bytes64 <= 0x100000000ull
            ? guest_ptr(ia.location, (u32)start64, (u32)bytes64) : NULL;
        if (src) {
            const u32 kv[8] = { ia.location, (u32)start64, count, ia.is_u32, base_index,
                                (u32)restart, restart ? rsx_dsp_restart_index(&g.rsx) : 0, g.rsx.current_primitive };
            u64 key = fnv1a(kv, sizeof kv, 1469598103934665603ull);
            key = key ? key : 1;
            ld_icache_entry* e = ld_icache_slot(key, 0);
            if (e && e->bad) e = NULL, key = 0;
            if (e && e->buf && e->range > dc.cap_refs) {
                rsx_vertex_ref* grown = (rsx_vertex_ref*)realloc(dc.refs, (size_t)e->range * sizeof(*grown));
                if (grown) { dc.refs = grown; dc.cap_refs = e->range; }
            }
            if (e && e->buf && e->range <= dc.cap_refs && e->raw_bytes == (u32)bytes64 &&
                !memcmp(e->raw, src, (size_t)bytes64)) {
                for (u32 i = 0; i < e->range; i++) {
                    dc.refs[i].vertex_id = e->lo + i;
                    dc.refs[i].base_index = base_index;
                }
                dc.n_refs = e->range; dc.n_source_refs = e->n_source;
                dc.refs_remapped = 1; dc.icache = e; e->last_frame = g_ld_frames;
                icache_hit = 1; g_icache_hits++;
            } else if (key) {
                if (e && e->buf && e->changes < 255) e->changes++;
                if (g_icache_pending_cap < (u32)bytes64) {
                    u8* grown = (u8*)realloc(g_icache_pending, (size_t)bytes64);
                    if (grown) { g_icache_pending = grown; g_icache_pending_cap = (u32)bytes64; }
                }
                if (g_icache_pending_cap >= (u32)bytes64) {
                    memcpy(g_icache_pending, src, (size_t)bytes64);
                    dc.icache_key = key; dc.icache_raw_bytes = (u32)bytes64;
                }
            }
        }
    }

    if (dc.n_idx && dc.fetch_ok && !icache_hit) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array index_array;
        rsx_dsp_get_index_array(&g.rsx, &index_array);
        const u32 element_size = index_array.is_u32 ? 4u : 2u;
        static int no_restart = -1;
        if (no_restart < 0)
            no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
        const int restart_enabled =
            !no_restart &&
            rsx_dsp_restart_index_enabled(&g.rsx, index_array.is_u32);
        const u32 restart_value = rsx_dsp_restart_index(&g.rsx);

        for (u32 batch = 0; batch < dc.n_idx && dc.fetch_ok; batch++) {
            const u32 first = dc.idx[batch].first;
            const u32 count = dc.idx[batch].count;
            const u64 start64 =
                (u64)index_array.offset + (u64)first * element_size;
            const u64 bytes64 = (u64)count * element_size;
            const u8* contiguous = NULL;
            if (count && start64 <= UINT32_MAX &&
                bytes64 <= UINT32_MAX &&
                start64 + bytes64 <= 0x100000000ull) {
                contiguous = guest_ptr(
                    index_array.location, (u32)start64, (u32)bytes64);
            }
            if (contiguous && count) {
                /* Fast path: one capacity check, then direct stores. */
                if ((u64)dc.n_refs + count > dc.cap_refs) {
                    u32 next_capacity = dc.cap_refs ? dc.cap_refs : 4096u;
                    while ((u64)next_capacity < (u64)dc.n_refs + count) next_capacity *= 2u;
                    rsx_vertex_ref* grown = (rsx_vertex_ref*)realloc(dc.refs, (size_t)next_capacity * sizeof(*grown));
                    if (!grown) { dc.fetch_ok = 0; break; }
                    dc.refs = grown; dc.cap_refs = next_capacity;
                }
                rsx_vertex_ref* out_refs = dc.refs;
                u32 n = dc.n_refs;
                for (u32 i = 0; i < count; i++) {
                    const u8* src = contiguous + (size_t)i * element_size;
                    const u32 index = index_array.is_u32
                        ? (((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3])
                        : (u32)((src[0] << 8) | src[1]);
                    if (restart_enabled && index == restart_value) {
                        dc.n_refs = n;
                        if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, dc.n_refs)) {
                            dc.fetch_ok = 0;
                            break;
                        }
                        continue;
                    }
                    out_refs[n].vertex_id = index;
                    out_refs[n].base_index = base_index;
                    n++;
                }
                if (dc.fetch_ok) dc.n_refs = n;
                continue;
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u32 source_offset =
                    index_array.offset + (first + i) * element_size;
                const u8* source = contiguous
                    ? contiguous + (size_t)i * element_size
                    : guest_ptr(
                        index_array.location, source_offset, element_size);
                if (!source) {
                    dc.fetch_ok = 0;
                    break;
                }
                const u32 index = index_array.is_u32
                    ? (((u32)source[0] << 24) |
                       ((u32)source[1] << 16) |
                       ((u32)source[2] << 8) | source[3])
                    : (u32)((source[0] << 8) | source[1]);
                if (restart_enabled && index == restart_value) {
                    if (!rsx_restart_cut_push(
                            &dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                            dc.n_refs)) {
                        dc.fetch_ok = 0;
                        break;
                    }
                    continue;
                }
                push_compact_ref(index, base_index);
            }
        }
    }

    if (!dc.fetch_ok)
        return;
    if (!icache_hit) dc.n_source_refs = dc.n_refs;
    int dense_refs = icache_hit;   /* refs are one sequential range (dense remap) */
    if (packed_payload && dc.n_refs > 1 && !icache_hit) {
        u32 unique_refs = dc.n_refs;
        /* LD_NO_DENSE_REMAP=1: always hash. */
        static int no_dense = -1;
        if (no_dense < 0) no_dense = getenv("LD_NO_DENSE_REMAP") ? 1 : 0;
        if (!no_dense && rsx_vertex_remap_build_dense(
                &dc.ref_remap, dc.refs, dc.n_refs, &unique_refs)) {
            dc.refs_remapped = 1;
            dense_refs = 1;
            dc.n_refs = unique_refs;
            if (dc.n_cuts) dc.icache_key = 0;
        } else if (rsx_vertex_remap_build(
                &dc.ref_remap, dc.refs, dc.n_refs, &unique_refs)) {
            dc.refs_remapped = unique_refs < dc.n_refs;
            dc.n_refs = unique_refs;
        }
    }
    if (!dense_refs) dc.icache_key = 0;
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(
        &dc.fetch_plan, &g.rsx, layout,
        (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 attr = layout->attrs[slot];
        if (dc.fetch_plan.attr[attr].desc.frequency >= 2)
            g_ld_divider_fetches += dc.n_refs;
    }
    if (packed_payload) {
        rsx_raw28_source raw_source;
        const int raw28=ld_vertex_raw28_enabled()&&
            rsx_raw28_plan(&dc.fetch_plan,dc.refs,dc.n_refs,&raw_source);
        const u64 required = (u64)dc.n_refs * (raw28?28u:layout->stride);
        if (!reserve_compact_vertices(required)) {
            dc.fetch_ok = 0;
            return;
        }
        if (!raw28 && ld_vertex_rawn_enabled() &&
            rsx_rawn_plan(&dc.fetch_plan, dc.refs, dc.n_refs, &dc.rawn_layout) &&
            reserve_compact_vertices((u64)dc.n_refs * dc.rawn_layout.stride)) {
            if (dense_refs && !ld_off_file("no_vcache", &s_off_vcache_t, &s_off_vcache))
                dc.vcache = ld_vcache_find(&dc.rawn_layout, rsx_raw28_element(dc.refs[0]), dc.n_refs, &dc.vcache_key);
            if (dc.vcache) g_vcache_hits++;
            else {
                rsx_rawn_copy(&dc.rawn_layout, dc.refs, dc.n_refs, dc.compact_verts);
                if (dc.vcache_key) {
                    g_vcache_misses++;
                    ld_vcache_store(dc.vcache_key, dc.compact_verts, dc.n_refs * dc.rawn_layout.stride);
                }
            }
            dc.rawn = 1; dc.layout.stride = dc.rawn_layout.stride; dc.n_verts = dc.n_refs;
            return;
        }
        if(raw28){
            rsx_raw28_copy(&raw_source,dc.refs,dc.n_refs,dc.compact_verts);
            dc.raw28=1;dc.layout.stride=28u;dc.n_verts=dc.n_refs;
            static u32 accepted;
            accepted++;
            if(accepted<=4u || (accepted%600u==0 && accepted<=19200u))
                fprintf(stderr,"[vertex-raw28] accepted=%u frame=%u vertices=%u source_occurrences=%u stride=28\n",accepted,g_ld_frames,dc.n_refs,dc.n_source_refs);
            return;
        }
        /* Sequential range: fetch attribute by attribute (LD_NO_RANGE_FETCH=1: off). */
        { static int no_range = -1;
          if (no_range < 0) no_range = getenv("LD_NO_RANGE_FETCH") ? 1 : 0;
          if (dense_refs && !no_range && layout->stride &&
              rsx_vertex_fetch_range(&dc.fetch_plan, dc.refs[0].vertex_id, dc.refs[0].base_index,
                                     dc.n_refs, dc.compact_verts)) {
              dc.n_verts = dc.n_refs;
              return;
          } }
        /* Any refs: attribute-wise gather over the prepared stable ranges
         * (LD_NO_GATHER_FETCH=1: off). */
        { static int no_gather = -1;
          if (no_gather < 0) no_gather = getenv("LD_NO_GATHER_FETCH") ? 1 : 0;
          if (!no_gather && layout->stride &&
              rsx_vertex_fetch_gather(&dc.fetch_plan, dc.refs, dc.n_refs, dc.compact_verts)) {
              dc.n_verts = dc.n_refs;
              return;
          } }
        for (u32 i = 0; i < dc.n_refs; i++) {
            u8* destination = layout->stride
                ? dc.compact_verts + (u64)i * layout->stride : NULL;
            if (!rsx_vertex_fetch_one(
                    &dc.fetch_plan, &dc.refs[i], destination)) {
                dc.fetch_ok = 0;
                return;
            }
        }
    } else {
        /*
         * H and M deliberately retain the complete legacy payload.  With an
         * all-attribute layout, slot order is register order, so the helper
         * writes exactly the original attribute*16 byte offsets.
         */
        if (!reserve_legacy_vertices(dc.n_refs)) {
            dc.fetch_ok = 0;
            return;
        }
        /* Legacy treats an absent ATTR0 as a failed group, even though other
         * absent attributes use their RSX current/default value. */
        if (dc.n_refs &&
            (!dc.fetch_plan.attr[0].desc.type ||
             !dc.fetch_plan.attr[0].desc.size)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 i = 0; i < dc.n_refs; i++) {
            if (!rsx_vertex_fetch_one(
                    &dc.fetch_plan, &dc.refs[i],
                    (u8*)&dc.verts[i])) {
                dc.fetch_ok = 0;
                return;
            }
        }
    }
    dc.n_verts = dc.n_refs;
}

/* Expand the already snapshotted raw bytes, never re-read a guest producer
 * after shader compilation or a driver failure. Backwards expansion keeps
 * source bytes intact despite the larger destination ABI. */
static int ld_raw28_restore_float(const rsx_vertex_layout_plan* layout)
{
    if(!dc.raw28)return 1;
    if(!reserve_compact_vertices((u64)dc.n_verts*layout->stride))return 0;
    for(u32 n=dc.n_verts;n>0;n--){
        u8 raw[28];memcpy(raw,dc.compact_verts+(u64)(n-1u)*28u,28u);
        for(u32 slot=0;slot<layout->count;slot++){
            float* out=(float*)(dc.compact_verts+(u64)(n-1u)*layout->stride+slot*16u);
            if(!rsx_raw28_decode_attr(raw,layout->attrs[slot],out))return 0;
        }
    }
    dc.layout=*layout;dc.raw28=0;return 1;
}

static int ld_rawn_restore_float(const rsx_vertex_layout_plan* layout)
{
    if (!dc.rawn) return 1;
    if (dc.vcache) {   /* the float path decodes from a scratch copy */
        if (!reserve_compact_vertices((u64)dc.n_verts * dc.rawn_layout.stride)) return 0;
        memcpy(dc.compact_verts, dc.vcache->shadow, (size_t)dc.n_verts * dc.rawn_layout.stride);
        dc.vcache = NULL;
    }
    const u32 rs = dc.rawn_layout.stride;
    u8* raw = (u8*)malloc((size_t)dc.n_verts * rs);
    if (!raw) return 0;
    memcpy(raw, dc.compact_verts, (size_t)dc.n_verts * rs);
    int ok = reserve_compact_vertices((u64)dc.n_verts * layout->stride);
    for (u32 n = 0; ok && n < dc.n_verts; n++)
        for (u32 slot = 0; slot < layout->count && ok; slot++) {
            float* out = (float*)(dc.compact_verts + (u64)n * layout->stride + slot * 16u);
            ok = rsx_rawn_decode_attr(&dc.rawn_layout, raw + (u64)n * rs, layout->attrs[slot], out);
        }
    free(raw);
    if (!ok) return 0;
    dc.layout = *layout; dc.rawn = 0; return 1;
}

static const float* dc_vertex_attr(
    u32 vertex, u32 attr, float fallback[4])
{
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    if(dc.raw28 && vertex<dc.n_verts &&
       rsx_raw28_decode_attr(dc.compact_verts+(u64)vertex*28u,attr,fallback))
        return fallback;
    if(dc.rawn && vertex<dc.n_verts &&
       rsx_rawn_decode_attr(&dc.rawn_layout,dc.compact_verts+(u64)vertex*dc.rawn_layout.stride,attr,fallback))
        return fallback;
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);
    memcpy(fallback, dc.fetch_plan.attr[attr].default_value, 16);
    return fallback;
}

/* Diagnostic-only counterpart to dc_vertex_attr().  Compact rendering omits
 * attributes the active vertex program does not read, but the legacy replay
 * fingerprint contains all sixteen RSX attributes.  Re-fetch an omitted,
 * enabled attribute from the original guest reference so a CSV hash compares
 * the producer bytes rather than the two renderers' different upload layouts. */
static const float* dc_vertex_attr_canonical(
    u32 vertex, u32 attr, float fallback[4])
{
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    if(dc.raw28 && vertex<dc.n_verts &&
       rsx_raw28_decode_attr(dc.compact_verts+(u64)vertex*28u,attr,fallback))
        return fallback;
    if(dc.rawn && vertex<dc.n_verts &&
       rsx_rawn_decode_attr(&dc.rawn_layout,dc.compact_verts+(u64)vertex*dc.rawn_layout.stride,attr,fallback))
        return fallback;
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);

    const rsx_vertex_fetch_attr* fetch = &dc.fetch_plan.attr[attr];
    if (vertex < dc.n_verts && fetch->desc.type && fetch->desc.size &&
        fetch->elem_size) {
        const rsx_vertex_ref* ref = &dc.refs[vertex];
        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (dc.fetch_plan.divider_mask >> attr) & 1u);
        const u64 offset =
            (u64)dc.fetch_plan.base_offset + fetch->desc.offset +
            (u64)element * fetch->stride;
        if (offset <= UINT32_MAX) {
            const u8* source = guest_ptr(
                fetch->desc.location, (u32)offset, fetch->elem_size);
            if (source && rsx_vertex_decode_element(
                    fetch->desc.type, fetch->desc.size,
                    source, fallback))
                return fallback;
        }
    }
    memcpy(fallback, fetch->default_value, 16);
    return fallback;
}

/* primitive ids = raw NV4097 VERTEX_BEGIN_END arg (rsx_dispatch stores it raw;
 * matches the replay harness). These were off by one (4/5/6/7), which dropped
 * EVERY quad/triangle draw through the switch's default: return -> black. */
#define PRIM_LINES           2
#define PRIM_TRIANGLES       5
#define PRIM_TRIANGLE_STRIP  6
#define PRIM_TRIANGLE_FAN    7
#define PRIM_QUADS           8

/* Live-draw activity counters (verification: is real geometry flowing, or only
 * clears?). Reported per presented frame in rsx_live_draw_present. */
/* DRAW_ARRAYS/DRAW_INDEX_ARRAY writes are packets. Multiple packets between a
 * single BEGIN/END are deliberately coalesced into one D3D12 DrawInstanced,
 * so comparing packet count directly with executed D3D draws was invalid.
 * Keep two independently balanced ledgers instead:
 *
 *   packets_seen = packets_queued + packets_movie + packets_queue_full
 *   groups_seen  = groups_executed + every group_drop_* outcome
 *
 * An END with no accepted packet is counted separately as groups_empty and is
 * not a render attempt. This makes every early return explicit. */
typedef struct ld_stats {
    unsigned long long packets_seen;
    unsigned long long packets_queued;
    unsigned long long packets_movie;
    unsigned long long packets_queue_full;
    unsigned long long groups_seen;
    unsigned long long groups_empty;
    unsigned long long groups_executed;
    unsigned long long group_drop_fetch;
    unsigned long long group_drop_degenerate;
    unsigned long long group_drop_primitive;
    unsigned long long group_drop_alloc;
    unsigned long long group_drop_pso;
    unsigned long long group_drop_ring;
    unsigned long long group_drop_surface;
    unsigned long long clears;
    unsigned long long clear_drop_surface;
    unsigned long long implicit_depth_clears;
} ld_stats;

static ld_stats g_ld_stats;

void rsx_live_draw_a010_probe_begin(void)
{
    if ((!getenv("RSX_A010_PROBE") &&
         !getenv("RSX_A010_SURFACE_DUMP")) || !g.ready)
        return;
    /*
     * Targeted surface readbacks submit and fence the open D3D12 list. When
     * surface capture alone is requested, defer that cost until AUTH confirms
     * that both the character palette and verified camera are synchronized.
     * Explicit RSX_A010_PROBE retains its scene-open diagnostic behavior.
     */
    if (!getenv("RSX_A010_PROBE") &&
        InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) == 0)
        return;
    if (g_yz_runtime_config.a010_start_reference) {
        /* Deterministic visual-verification camera derived from the shipped
         * cam_Came_000_002.cmt frame 612. Install it on the RSX thread as soon
         * as the a010 probe opens so a later blocked PPU scene update cannot
         * prevent the reference matrix from reaching the first world draws. */
        static const float reference_matrix_612[16] = {
             3.18890834f,   -0.000270011135f,  0.08452246f,   5.81511511f,
            -0.0102274104f,  5.65674844f,      0.403935942f, -18.5085361f,
             0.0264371686f,  0.0712562195f,   -0.997207931f,  9.99292802f,
             0.0264345249f,  0.0712490938f,   -0.99710821f,  10.0919287f,
        };
        static const float reference_matrix_874[16] = {
             6.80647601f,   -0.00327668415f,  0.611324803f, -36.9135075f,
             0.00523179535f, 12.1491076f,     0.00686819648f, -38.8146474f,
             0.0894642733f,  0.000524588748f,-0.996090306f, -11.7467763f,
             0.0894553268f,  0.000524536289f,-0.995990697f, -11.6456016f,
        };
        const float* reference_matrix =
            g_yz_runtime_config.a010_reference_874
                ? reference_matrix_874 : reference_matrix_612;
        rsx_live_draw_set_a010_camera_matrix(reference_matrix);
    }
    CreateDirectoryA("scratch\\a010_probe", NULL);
    g_ld_a010_probe_start_frame = g_ld_frames;
    g_ld_a010_probe_sample = 0;
    g_ld_a010_probe_touched = 0;
    InterlockedExchange(&g_ld_a010_world_ready, 0);
    InterlockedExchange(&g_ld_a010_probe_active, 1);
    fprintf(stderr,
            "[a010-probe] BEGIN live_frame=%u surfaces=%u packets=%llu groups=%llu\n",
            g_ld_frames, g.n_surfaces, g_ld_stats.packets_seen,
            g_ld_stats.groups_seen);
    fflush(stderr);
}

int rsx_live_draw_a010_probe_active(void)
{
    return InterlockedCompareExchange(&g_ld_a010_probe_active, 0, 0) != 0;
}

int rsx_live_draw_a010_world_ready(void)
{
    return InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
}

static unsigned long long ld_groups_accounted(void)
{
    return g_ld_stats.groups_executed + g_ld_stats.group_drop_fetch +
           g_ld_stats.group_drop_degenerate + g_ld_stats.group_drop_primitive +
           g_ld_stats.group_drop_alloc + g_ld_stats.group_drop_pso +
           g_ld_stats.group_drop_ring + g_ld_stats.group_drop_surface;
}

#if defined(YZ_PERF_PROFILE)
static double ld_profile_ticks_ms(u64 ticks)
{
    return g_ld_profile.qpc_frequency
        ? (double)ticks * 1000.0 / (double)g_ld_profile.qpc_frequency
        : 0.0;
}

static void ld_profile_present(u32 frame)
{
    const LONGLONG now = ld_profile_qpc();
    const double frame_ms = ld_profile_ticks_ms(
        (u64)(now - g_ld_profile.previous_present_qpc));
    const ld_profile_counts* total = &g_ld_profile.total;
    const ld_profile_counts* previous = &g_ld_profile.previous;
#define LD_PROFILE_DELTA(field) (total->field - previous->field)
    const u64 packets =
        g_ld_stats.packets_seen - g_ld_profile.previous_packets;
    const u64 groups =
        g_ld_stats.groups_seen - g_ld_profile.previous_groups;
    const u64 executed =
        g_ld_stats.groups_executed - g_ld_profile.previous_executed;
    const u64 evictions =
        g_ld_texture_cache_evictions - g_ld_profile.previous_evictions;
    const u64 refreshes =
        g_ld_vtex_refreshes - g_ld_profile.previous_refreshes;
    const u64 used_attribute_draws =
        LD_PROFILE_DELTA(used_attribute_draws);
    const double used_attribute_average = used_attribute_draws
        ? (double)LD_PROFILE_DELTA(used_attribute_sum) /
              (double)used_attribute_draws
        : 0.0;
    const int compile_free =
        LD_PROFILE_DELTA(vs_compile_calls) == 0 &&
        LD_PROFILE_DELTA(ps_compile_calls) == 0;
    g_ld_profile.measured_frames++;
    g_ld_profile.total_frame_ms += frame_ms;
    if (compile_free) {
        g_ld_profile.compile_free_frames++;
        g_ld_profile.compile_free_frame_ms += frame_ms;
    }
    /*
     * The two matched visual-validation windows are deliberately small.
     * Emit one aggregate line per presented frame there so an improvement
     * below the generic 250 ms slow-frame threshold remains measurable.
     * Scene identity is still established by the run driver's asset marker
     * and screenshots; these values are only run-local capture locators.
     */
    const int matched_window =
        (frame >= 2240u && frame <= 2300u) ||
        (frame >= 4800u && frame <= 4900u);
    const int emit =
        frame <= 16u || matched_window || (frame & 63u) == 0u ||
        frame_ms >= 250.0 ||
        LD_PROFILE_DELTA(pso_misses) != 0 ||
        LD_PROFILE_DELTA(pso_full) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]) != 0 ||
        LD_PROFILE_DELTA(
            flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]) != 0 ||
        LD_PROFILE_DELTA(ps_constant_capacity_failures) != 0;

    if (emit) {
        if(g_ld_cpu_phases_enabled){
            double phases[LD_CPU_PHASE_COUNT],sum=0.0;
            for(u32 i=0;i<LD_CPU_PHASE_COUNT;i++){
                phases[i]=ld_profile_ticks_ms(LD_PROFILE_DELTA(draw_phase_qpc[i]));
                sum+=phases[i];
            }
            const double pack=ld_profile_ticks_ms(LD_PROFILE_DELTA(vertex_fetch_pack_qpc));
            const double draw=ld_profile_ticks_ms(LD_PROFILE_DELTA(sink_end_qpc));
            fprintf(stderr,"[draw-phase-perf] frame=%u wall_ms{pso=%.3f texture=%.3f descriptors=%.3f constants=%.3f state=%.3f draw=%.3f fetch_pack=%.3f unassigned=%.3f total=%.3f} segments{pso=%llu texture=%llu descriptors=%llu constants=%llu state=%llu draw=%llu} disjoint=1 exclusive_thread_cpu=0 nested_gpu_wait_possible=1\n",
                frame,phases[0],phases[1],phases[2],phases[3],phases[4],phases[5],pack,draw-sum-pack,draw,
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[0]),
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[1]),
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[2]),
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[3]),
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[4]),
                (unsigned long long)LD_PROFILE_DELTA(draw_phase_samples[5]));
            fprintf(stderr,"[texture-prepare-perf] frame=%u hash_calls=%llu hash_span_bytes=%llu hash_read_fail=%llu hash_ms_inside_texture=%.3f texture_cache_lookup=%llu texture_cache_probes=%llu sampler_lookup=%llu sampler_probes=%llu hash_is_texture_subset=1 hash_bytes=readable_source_span clocks_per_hash=2 clocks_per_lookup=0 surface_zdepth_probes=unmeasured exclusive_thread_cpu=0\n",
                frame,(unsigned long long)LD_PROFILE_DELTA(texture_hash_calls),
                (unsigned long long)LD_PROFILE_DELTA(texture_hash_span_bytes),
                (unsigned long long)LD_PROFILE_DELTA(texture_hash_read_fail),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(texture_hash_qpc)),
                (unsigned long long)LD_PROFILE_DELTA(texture_cache_lookups),
                (unsigned long long)LD_PROFILE_DELTA(texture_cache_probes),
                (unsigned long long)LD_PROFILE_DELTA(sampler_lookups),
                (unsigned long long)LD_PROFILE_DELTA(sampler_probes));
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO memory = {0};
        if (g_ld_profile.adapter)
            g_ld_profile.adapter->lpVtbl->QueryVideoMemoryInfo(
                g_ld_profile.adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &memory);
        fprintf(stderr,
                "[rsx-perf] frame=%u dt_ms=%.3f "
                "pso{look=%llu hit=%llu miss=%llu miss_total=%llu "
                "probes=%llu full=%llu cached=%u capacity=%u mem_kb=%.1f} "
                "reject{requests=%llu unique=%u "
                "world=%llu/%u character=%llu/%u "
                "ui=%llu/%u other=%llu/%u} "
                "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu "
                "vs_new=%llu vs_unique=%u ps_lookup=%llu ps_hit=%llu "
                "ps_compile=%llu ps_new=%llu ps_unique=%u} "
                "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
                "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
                "vertex{path=%s used_avg=%.3f used_draws=%llu used_max=%u "
                "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
                "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
                "frame{compile_free=%u} "
                "draw{packets=%llu groups=%llu exec=%llu "
                "input_v=%llu expanded_v=%llu vb_mb=%.3f} "
                "flush{present=%llu ref=%llu vb=%llu retire=%llu "
                "movie=%llu movie_present=%llu readback=%llu} "
                "tex{cached=%u evict=%llu refresh=%llu upload_mb=%.3f "
                "decode=%llu decode_ms=%.3f upload_hi_mb=%.3f "
                "retired_hi=%u} "
                "rings{vb_hi_mb=%.3f upload_total_hi_mb=%.3f "
                "vb_total_hi_mb=%.3f retired_total_hi=%u} "
                "vram{usage_mb=%.1f budget_mb=%.1f reservation_mb=%.1f}\n",
                frame, frame_ms,
                (unsigned long long)LD_PROFILE_DELTA(pso_lookups),
                (unsigned long long)LD_PROFILE_DELTA(pso_hits),
                (unsigned long long)LD_PROFILE_DELTA(pso_misses),
                (unsigned long long)total->pso_misses,
                (unsigned long long)LD_PROFILE_DELTA(pso_probes),
                (unsigned long long)LD_PROFILE_DELTA(pso_full),
                g.n_psos,
                MAX_PSOS,
                (double)sizeof(g.psos) / 1024.0,
                (unsigned long long)total->pso_full,
                g_ld_profile.n_rejected_pso_keys,
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
                g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
                g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(vs_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(vs_unique),
                g_ld_profile.n_vs_hashes,
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(ps_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(ps_unique),
                g_ld_profile.n_ps_hashes,
                ld_profile_ticks_ms(LD_PROFILE_DELTA(decompile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(vs_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(ps_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(create_pso_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(flush_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(fence_wait_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(sink_end_qpc)),
                ld_vertex_mode_name(),
                used_attribute_average,
                (unsigned long long)used_attribute_draws,
                g_ld_profile.frame_used_attribute_max,
                (double)LD_PROFILE_DELTA(legacy_vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(vertex_fetch_pack_qpc)),
                (unsigned long long)
                    LD_PROFILE_DELTA(
                        flush_reason[LD_FLUSH_VERTEX_RING]),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        flush_reason_qpc[LD_FLUSH_VERTEX_RING])),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        fence_reason_qpc[LD_FLUSH_VERTEX_RING])),
                compile_free ? 1u : 0u,
                (unsigned long long)packets,
                (unsigned long long)groups,
                (unsigned long long)executed,
                (unsigned long long)LD_PROFILE_DELTA(input_vertices),
                (unsigned long long)LD_PROFILE_DELTA(expanded_vertices),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_GUEST_REFERENCE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_READBACK]),
                g.n_textures,
                (unsigned long long)evictions,
                (unsigned long long)refreshes,
                (double)LD_PROFILE_DELTA(texture_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(texture_decode_calls),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(texture_decode_qpc)),
                (double)g_ld_profile.frame_upload_high /
                    (1024.0 * 1024.0),
                g_ld_profile.frame_retired_high,
                (double)g_ld_profile.frame_vb_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_upload_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_vb_high /
                    (1024.0 * 1024.0),
                g_ld_profile.total_retired_high,
                (double)memory.CurrentUsage / (1024.0 * 1024.0),
                (double)memory.Budget / (1024.0 * 1024.0),
                (double)memory.CurrentReservation / (1024.0 * 1024.0));
        fprintf(stderr,
                "[shader-cache-perf] frame=%u "
                "vs{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu} "
                "ps{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu}\n",
                frame,
                g.vs_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->vs_blob_lookups,
                (unsigned long long)total->vs_blob_hits,
                (unsigned long long)total->vs_blob_misses,
                (unsigned long long)total->vs_blob_inserts,
                (unsigned long long)total->vs_blob_full_rejects,
                (unsigned long long)total->vs_compile_calls,
                ld_profile_ticks_ms(total->vs_blob_lookup_qpc),
                ld_profile_ticks_ms(total->vs_compile_qpc),
                (unsigned long long)total->vs_post_boundary_distinct,
                (unsigned long long)total->vs_post_boundary_repeats,
                (unsigned long long)g.vs_blobs.retained_source_bytes,
                (unsigned long long)g.vs_blobs.retained_blob_bytes,
                g.ps_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->ps_blob_lookups,
                (unsigned long long)total->ps_blob_hits,
                (unsigned long long)total->ps_blob_misses,
                (unsigned long long)total->ps_blob_inserts,
                (unsigned long long)total->ps_blob_full_rejects,
                (unsigned long long)total->ps_compile_calls,
                ld_profile_ticks_ms(total->ps_blob_lookup_qpc),
                ld_profile_ticks_ms(total->ps_compile_qpc),
                (unsigned long long)total->ps_post_boundary_distinct,
                (unsigned long long)total->ps_post_boundary_repeats,
                 (unsigned long long)g.ps_blobs.retained_source_bytes,
                 (unsigned long long)g.ps_blobs.retained_blob_bytes);
        const u32 collapsed_by_constants =
            g_ld_profile.n_ps_constant_specialized_hashes >=
                    g_ld_profile.n_ps_canonical_hashes
                ? g_ld_profile.n_ps_constant_specialized_hashes -
                      g_ld_profile.n_ps_canonical_hashes
                : 0;
        const u32 collapsed_by_alpha =
            g_ld_profile.n_ps_exact_source_hashes >=
                    g_ld_profile.n_ps_constant_specialized_hashes
                ? g_ld_profile.n_ps_exact_source_hashes -
                      g_ld_profile.n_ps_constant_specialized_hashes
                : 0;
        fprintf(
            stderr,
            "[fp-constant-perf] frame=%u mode=%s variant_profile=%s "
            "variants{exact_source=%u constant_specialized=%u "
            "canonical=%u collapsed_constants=%u collapsed_alpha=%u "
            "tracking_overflow=%llu} "
            "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
            "ring_used=%u ring_capacity=%u capacity_failures=%llu "
            "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
            frame, ld_fp_constant_mode_name(),ld_fp_variant_profile_status(),
            g_ld_profile.n_ps_exact_source_hashes,
            g_ld_profile.n_ps_constant_specialized_hashes,
            g_ld_profile.n_ps_canonical_hashes,
            collapsed_by_constants, collapsed_by_alpha,
            (unsigned long long)
                g_ld_profile.ps_variant_tracking_overflow,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_allocations),
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_upload_bytes),
            ld_profile_ticks_ms(
                LD_PROFILE_DELTA(ps_constant_upload_qpc)),
            g.ps_cb_used, PS_CB_RING_BYTES,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_capacity_failures),
            (unsigned long long)LD_PROFILE_DELTA(
                flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                flush_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                fence_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])));
        fflush(stderr);
    }

    g_ld_profile.previous = g_ld_profile.total;
    g_ld_profile.previous_packets = g_ld_stats.packets_seen;
    g_ld_profile.previous_groups = g_ld_stats.groups_seen;
    g_ld_profile.previous_executed = g_ld_stats.groups_executed;
    g_ld_profile.previous_evictions = g_ld_texture_cache_evictions;
    g_ld_profile.previous_refreshes = g_ld_vtex_refreshes;
    g_ld_profile.previous_present_qpc = now;
    g_ld_profile.frame_upload_high = 0;
    g_ld_profile.frame_vb_high = 0;
    g_ld_profile.frame_retired_high = 0;
    g_ld_profile.frame_used_attribute_max = 0;
#undef LD_PROFILE_DELTA
}
#endif

static int ld_target_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RSX_TARGET_TRACE") ? 1 : 0;
    return enabled;
}

static void ld_trace_target(const char* event, u32 target, u32 mask)
{
    if (!ld_target_trace_enabled()) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    fprintf(stderr,
            "[rsx-target] frame=%u event=%s target=%u loc=%u off=0x%08X "
            "fmt=0x%X pitch=%u clip=%u,%u+%ux%u mask=0x%02X\n",
            g_ld_frames, event, target, sf.color_location[0], sf.color_offset[0],
            sf.color_format, sf.color_pitch[0], sf.clip_x, sf.clip_y,
            sf.clip_w, sf.clip_h, mask);
}

/* Movie mode: while a host-decoded movie owns the window
 * (rsx_live_draw_present_rgba), do not make the default boot process the
 * guest's otherwise invisible RSX stream.  That experiment materially changes
 * CRI/movie handoff scheduling and regressed the proven title/menu path.
 * RSX_MOVIE_TRACK keeps the state-tracking experiment available for a
 * focused post-movie A/B once the transition itself is deterministic. */
static volatile int g_ld_movie_mode = 0;
static int g_ld_movie_track_rsx = -1;
static int g_ld_movie_composite_ui = -1;

#if defined(YZ_PERF_PROFILE)
static int ld_vp_has_indexed_constants(const u8* ucode, u32 instrs)
{
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16u + 12u;
        const u32 d3 = (u32)p[0] | ((u32)p[1] << 8) |
                       ((u32)p[2] << 16) | ((u32)p[3] << 24);
        if ((d3 >> 1) & 1u)
            return 1;
    }
    return 0;
}

static ld_reject_class ld_classify_rejected_pso(u32 target)
{
    const u32 start = g_ld_current_pso.vp_start;
    const u8* vp_uc = start < RSX_DSP_VP_INSTR
        ? (const u8*)(g.rsx.vp + start * 4)
        : NULL;
    if (vp_uc && ld_vp_has_indexed_constants(
            vp_uc, g_ld_current_pso.vp_instrs))
        return LD_REJECT_CHARACTER;

    render_state_t rs;
    decode_render_state(&rs);
    if (target < g.n_surfaces) {
        const surface_t* surface = &g.surfaces[target];
        int scanout = 0;
        for (u32 i = 0; i < 8; i++) {
            const display_buffer_t* display = &g.display_buffers[i];
            if (display->valid &&
                display->location == surface->location &&
                display->offset == surface->offset) {
                scanout = 1;
                break;
            }
        }
        if (scanout && !rs.depth_test && rs.blend_enable)
            return LD_REJECT_UI;
        if (rs.depth_test ||
            surface->offset == 0x00E40000u ||
            surface->offset == 0x01800000u)
            return LD_REJECT_WORLD;
    }
    return LD_REJECT_OTHER;
}

static void ld_profile_note_rejected_pso(void)
{
    if (!g_ld_current_pso.valid || g.n_psos < MAX_PSOS)
        return;

    rsx_dsp_surface rsx_surface;
    rsx_dsp_get_surface(&g.rsx, &rsx_surface);
    u32 target = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == rsx_surface.color_location[0] &&
            g.surfaces[i].offset == rsx_surface.color_offset[0]) {
            target = i;
            break;
        }
    }
    const ld_reject_class draw_class = ld_classify_rejected_pso(target);
    const u8 class_bit = (u8)(1u << (u32)draw_class);
    g_ld_profile.rejected_pso_requests[draw_class]++;

    u32 key_index = g_ld_profile.n_rejected_pso_keys;
    for (u32 i = 0; i < g_ld_profile.n_rejected_pso_keys; i++) {
        if (g_ld_profile.rejected_pso_keys[i] ==
            g_ld_current_pso.key) {
            key_index = i;
            break;
        }
    }
    if (key_index == g_ld_profile.n_rejected_pso_keys) {
        if (key_index >= MAX_REJECTED_PSO_KEYS)
            return;
        g_ld_profile.rejected_pso_keys[key_index] =
            g_ld_current_pso.key;
        g_ld_profile.rejected_pso_class_masks[key_index] = 0;
        g_ld_profile.n_rejected_pso_keys++;
    }
    if (!(g_ld_profile.rejected_pso_class_masks[key_index] & class_bit)) {
        g_ld_profile.rejected_pso_class_masks[key_index] |= class_bit;
        g_ld_profile.rejected_pso_unique[draw_class]++;
    }

    if (!g_ld_profile.first_pso_full_frame) {
        g_ld_profile.first_pso_full_frame = g_ld_frames;
        const u32 surface_offset = target < g.n_surfaces
            ? g.surfaces[target].offset
            : 0;
        fprintf(stderr,
                "[pso-capacity] reached count=%u capacity=%u "
                "movie=%d orphanage_world_ready=%d "
                "first_rejected_key=%016llx class=%u "
                "surface=0x%08X frame_locator=%u\n",
                g.n_psos, MAX_PSOS, g_ld_movie_mode,
                InterlockedCompareExchange(
                    &g_ld_a010_world_ready, 0, 0) != 0,
                (unsigned long long)g_ld_current_pso.key,
                (u32)draw_class, surface_offset, g_ld_frames);
        fflush(stderr);
    }
}
#endif

static int ld_movie_composite_ui_enabled(void)
{
    if (g_ld_movie_composite_ui < 0)
        g_ld_movie_composite_ui = getenv("RSX_MOVIE_COMPOSITE_UI") ? 1 : 0;
    return g_ld_movie_composite_ui;
}

static void ld_movie_reset_rings(void)
{
    g.vb_used = 0;
    g.ib_used = 0;
    g.cb_used = 0;
    g.ps_cb_used = 0;
    g.srv_ring_used = 0;
    g.smp_ring_used = 0;
    g.depth_cleared = 0;
}

static int ld_movie_overlay_ensure(void)
{
    if (g.movie_upload && g.movie_upload_mapped &&
        g.movie_overlay_readback && g.movie_overlay_rgba &&
        g.movie_overlay_mask)
        return 1;

    g.movie_overlay_pitch = (g.width * 4 + 255) & ~255u;
    const UINT64 rb_size = (UINT64)g.movie_overlay_pitch * g.height;
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC bd = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb_size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_overlay_readback))) {
        fprintf(stderr, "[movie-ui] readback allocation failed\n");
        return 0;
    }
    g.movie_overlay_rgba = (u8*)malloc((size_t)g.width * g.height * 4);
    g.movie_overlay_mask = (u8*)malloc((size_t)g.width * g.height);
    if (!g.movie_overlay_rgba || !g.movie_overlay_mask) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] CPU overlay allocation failed\n");
        return 0;
    }

    /* Do not reuse the general guest upload ring for host frames. Guest draw
     * commands recorded between flips can still reference that memory; a
     * dedicated upload buffer lets host presentation append to the same
     * command list without a second fence/wait on every movie frame. */
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_upload))) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload allocation failed\n");
        return 0;
    }
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(g.movie_upload->lpVtbl->Map(
            g.movie_upload, 0, &no_read, (void**)&g.movie_upload_mapped))) {
        g.movie_upload->lpVtbl->Release(g.movie_upload);
        g.movie_upload = NULL;
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload map failed\n");
        return 0;
    }
    return 1;
}

/* Start movies from a known transparent guest surface. This removes the stale
 * pre-movie frame before the game begins drawing captions, while leaving the
 * swap chain entirely owned by the 30 Hz host presenter. */
static void ld_movie_overlay_begin(void)
{
    g.movie_overlay_valid = 0;
    g.movie_overlay_frames = 0;
    if (!ld_movie_overlay_ensure()) return;

    ld_flush(LD_FLUSH_MOVIE);
    const float transparent[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < g.n_surfaces; i++) {
        g.list->lpVtbl->ClearRenderTargetView(
            g.list, rtv_handle(LD_SWAP_BUFFERS + i), transparent, 0, NULL);
        ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
    }
    if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
    ld_movie_reset_rings();
    fprintf(stderr, "[movie-ui] compositor armed (%ux%u, %u guest surfaces)\n",
            g.width, g.height, g.n_surfaces);
    fflush(stderr);
}

/* Capture the guest's latest offscreen result without presenting it. The
 * guest target is not a true transparent overlay: depending on the auth
 * sequence it can contain black, a fade, or a complete rendered scene.
 * Extract only low-saturation bright glyphs from the subtitle-safe lower band
 * and synthesize a small dark outline. The decoded movie therefore remains the
 * background even when the guest rendered an opaque full-screen image. */
static void ld_movie_capture_overlay(void)
{
    if (!ld_movie_overlay_ensure()) return;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        g.movie_overlay_valid = 0;
        return;
    }
    ID3D12Resource* rt = g.surfaces[target].tex;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g.movie_overlay_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g.movie_overlay_pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);
    ld_flush(LD_FLUSH_MOVIE);

    const SIZE_T rb_size = (SIZE_T)g.movie_overlay_pitch * g.height;
    u8* mapped = NULL;
    D3D12_RANGE rr = {0, rb_size};
    if (FAILED(g.movie_overlay_readback->lpVtbl->Map(
            g.movie_overlay_readback, 0, &rr, (void**)&mapped))) {
        g.movie_overlay_valid = 0;
        return;
    }
    for (u32 y = 0; y < g.height; y++)
        memcpy(g.movie_overlay_rgba + (size_t)y * g.width * 4,
               mapped + (size_t)y * g.movie_overlay_pitch,
               (size_t)g.width * 4);
    D3D12_RANGE wr = {0, 0};
    g.movie_overlay_readback->lpVtbl->Unmap(
        g.movie_overlay_readback, 0, &wr);

    const u64 total = (u64)g.width * g.height;
    const u32 band_y0 = g.height * 52 / 100;
    const u32 band_y1 = g.height * 96 / 100;
    const u32 band_x0 = g.width * 5 / 100;
    const u32 band_x1 = g.width * 95 / 100;
    const u64 band_pixels =
        (u64)(band_y1 - band_y0) * (band_x1 - band_x0);
    u64 glyph_pixels = 0;
    memset(g.movie_overlay_mask, 0, (size_t)total);

    for (u32 y = band_y0; y < band_y1; y++) {
        for (u32 x = band_x0; x < band_x1; x++) {
            const u64 i = (u64)y * g.width + x;
            const u8* p = g.movie_overlay_rgba + i * 4;
            const int hi = p[0] > p[1]
                ? (p[0] > p[2] ? p[0] : p[2])
                : (p[1] > p[2] ? p[1] : p[2]);
            const int lo = p[0] < p[1]
                ? (p[0] < p[2] ? p[0] : p[2])
                : (p[1] < p[2] ? p[1] : p[2]);
            if (hi < 145 || hi - lo > 52)
                continue;
            int coverage = (hi - 96) * 255 / 159;
            if (coverage < 64) coverage = 64;
            if (coverage > 255) coverage = 255;
            g.movie_overlay_mask[i] = (u8)coverage;
            glyph_pixels++;
        }
    }

    /* A value of 1 denotes the synthetic outline; 64..255 are glyph
     * coverage. Work from a copy condition (>=64) so dilation does not grow
     * recursively. */
    if (glyph_pixels >= 8 && glyph_pixels < band_pixels / 6) {
        for (u32 y = band_y0; y < band_y1; y++) {
            for (u32 x = band_x0; x < band_x1; x++) {
                const u64 i = (u64)y * g.width + x;
                if (g.movie_overlay_mask[i] < 64)
                    continue;
                const u32 ya = y > 1 ? y - 2 : 0;
                const u32 yb = y + 2 < g.height ? y + 2 : g.height - 1;
                const u32 xa = x > 1 ? x - 2 : 0;
                const u32 xb = x + 2 < g.width ? x + 2 : g.width - 1;
                for (u32 oy = ya; oy <= yb; oy++)
                    for (u32 ox = xa; ox <= xb; ox++) {
                        u8* m = &g.movie_overlay_mask[(u64)oy * g.width + ox];
                        if (!*m) *m = 1;
                    }
            }
        }
    }
    g.movie_overlay_valid =
        glyph_pixels >= 8 && glyph_pixels < band_pixels / 6;
    g.movie_overlay_frames++;
    if (g.movie_overlay_frames <= 16 ||
        (g.movie_overlay_frames & 63) == 0 ||
        (!g.movie_overlay_valid && glyph_pixels)) {
        fprintf(stderr,
                "[movie-ui] overlay=%llu glyphs=%llu/%llu %s\n",
                (unsigned long long)g.movie_overlay_frames,
                (unsigned long long)glyph_pixels,
                (unsigned long long)band_pixels,
                g.movie_overlay_valid ? "accepted" : "rejected");
        fflush(stderr);
    }

    /* A guest flip still marks a new texture-generation boundary even though
     * the guest surface is not sent to the swap chain. */
    g_ld_frames++;
    ld_movie_reset_rings();
}

static void sink_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; (void)prim; dc_reset(); }
static void sink_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_arr >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = first; dc.arr[dc.n_arr].count = count; dc.n_arr++;
    g_ld_stats.packets_queued++;
}
/* INLINE_ARRAY arrives as one finished stream instead of a (first, count)
 * range, so plan the layout here -- offsets and stride come from the VTXFMT
 * declaration -- and queue it as an ordinary consecutive batch. Everything
 * downstream (topology, textures, PSO, upload) is then identical to a draw
 * out of a vertex array, because by then it IS one. */
static void sink_inline_array(void* u, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)u; (void)r;
    g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }

    const u32 stride = rsx_vertex_inline_layout(&g.rsx, dc.inl_off);
    if (!stride || bytes < stride) { g_ld_stats.packets_queue_full++; return; }

    dc.inl = data; dc.inl_bytes = bytes; dc.inl_stride = stride;
    dc.n_packets++;
    if (dc.n_arr >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = 0; dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
    g_ld_stats.packets_queued++;
}

static void sink_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_idx >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.idx[dc.n_idx].first = first; dc.idx[dc.n_idx].count = count; dc.n_idx++;
    g_ld_stats.packets_queued++;
}

void rsx_live_draw_set_a010_camera_matrix(const float* matrix16)
{
    if (!matrix16) {
        InterlockedExchange(&g_ld_a010_camera_ready, 0);
        return;
    }
    memcpy(g_ld_a010_camera_bits, matrix16, sizeof(g_ld_a010_camera_bits));
    MemoryBarrier();
    InterlockedExchange(&g_ld_a010_camera_ready, 1);
}

/* Hash the same pre-cubemap/pre-VTF PSO inputs used by the reference replay's
 * RSX_DRAW_CSV.  Keeping this legacy key lets a live draw be matched directly
 * to the known-good a010 capture even though the current live PSO key contains
 * additional feature masks. */
static u64 live_legacy_pso_key(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR)
        return 0;
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs)
        return 0;

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc)
        return 0;
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size)
        return 0;
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc)
        return 0;

    u64 key = fnv1a(vp_uc, vp_instrs * 16, 1469598103934665603ull);
    key = fnv1a(fp_uc, fp_size, key);
    const u32 fp_ctrl_key = rsx_dsp_shader_control(&g.rsx) & 0x4Eu;
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    render_state_t rs;
    decode_render_state(&rs);
    return ld_topology_pso_key(fnv1a(&rs, sizeof(rs), key), g.rsx.current_primitive);
}

/* Hash the canonical sixteen-attribute vertex stream in source occurrence
 * order.  Compact mode may deduplicate the uploaded payload, so hashing its
 * packed bytes directly is not comparable with the legacy replay renderer.
 * Re-expanding through the remap and applying the RSX defaults produces the
 * same vtx_t byte stream on both paths and places the producer/consumer
 * boundary immediately after guest-memory vertex decoding. */
static u64 live_decoded_vertex_hash(u64 attr_hash[16])
{
    u64 hash = 1469598103934665603ull;
    for (u32 attr = 0; attr < 16; attr++)
        attr_hash[attr] = 1469598103934665603ull;
    const u32 count = dc.n_source_refs ? dc.n_source_refs : dc.n_verts;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const u32 vertex = dc.refs_remapped
            ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
            : occurrence;
        for (u32 attr = 0; attr < 16; attr++) {
            float fallback[4];
            const float* value =
                dc_vertex_attr_canonical(vertex, attr, fallback);
            hash = fnv1a(value, 16u, hash);
            attr_hash[attr] = fnv1a(value, 16u, attr_hash[attr]);
        }
    }
    return hash;
}

static char g_ld_draw_pair_base[MAX_PATH];
static const char* g_ld_draw_pair_request;
static u32 g_ld_draw_scan_limit, g_ld_draw_scan_count, g_ld_draw_scan_percent = 50;
static int g_ld_draw_scan_done;
static ID3D12Resource* g_ld_draw_scan_resource;
/* Diagnostic selection only: live output zero denotes attachment A. The
 * guest NONE/B-only selectors must not qualify merely because the live
 * renderer currently keeps an A resource bound. */
static int ld_capture_color_output0(u32 color_mask, u32 color_target)
{
    if (!(color_mask & 0x01010101u)) return 0;
    /* CELL_GCM_SURFACE_TARGET_{0,MRT1,MRT2,MRT3}, rsx_commands.h. */
    return color_target == 0x01u || color_target == 0x13u ||
           color_target == 0x17u || color_target == 0x1Fu;
}
static u32 ld_draw_pair_begin(u32 prim, u32 vertices)
{
    static int initialized, attempted, first_target, color_only;
    static const char* directory;
    static u64 wanted_vp;
    static u32 from_frame, wanted_offset, minimum, maximum;
    if (!initialized) {
        initialized = 1;
        directory = getenv("LD_DRAW_PAIR_DIR");
        const char* first = getenv("LD_DRAW_PAIR_FIRST_TARGET");
        first_target = first && atoi(first) != 0;
        const char* color = getenv("LD_DRAW_PAIR_COLOR_ONLY");
        color_only = color && atoi(color) != 0;
        const char* v = getenv("LD_DRAW_PAIR_VP");
        wanted_vp = v ? _strtoui64(v, NULL, 16) : 0;
        v = getenv("LD_DRAW_PAIR_TARGET"); wanted_offset = v ? (u32)strtoul(v, NULL, 16) : 0;
        v = getenv("LD_DRAW_PAIR_FROM"); from_frame = v ? (u32)strtoul(v, NULL, 10) : 0;
        v = getenv("LD_DRAW_PAIR_MIN_VERTICES"); minimum = v ? (u32)strtoul(v, NULL, 10) : 0;
        v = getenv("LD_DRAW_PAIR_MAX_VERTICES"); maximum = v ? (u32)strtoul(v, NULL, 10) : UINT32_MAX;
        g_ld_draw_pair_request = getenv("LD_DRAW_PAIR_REQUEST_FILE");
        v = getenv("LD_DRAW_PAIR_SCAN_LIMIT");
        g_ld_draw_scan_limit = v ? (u32)strtoul(v, NULL, 10) : 0;
        if (g_ld_draw_scan_limit > 128) g_ld_draw_scan_limit = 128;
        v = getenv("LD_DRAW_PAIR_SCAN_NONBLACK_PERCENT");
        if (v) g_ld_draw_scan_percent = (u32)strtoul(v, NULL, 10);
        if (!g_ld_draw_scan_percent) g_ld_draw_scan_percent = 1;
        if (g_ld_draw_scan_percent > 100) g_ld_draw_scan_percent = 100;
    }
    if (!directory || !directory[0] || (!wanted_vp && !first_target) ||
        (first_target && !wanted_offset) ||
        (g_ld_draw_scan_limit ? (g_ld_draw_scan_done || g_ld_draw_scan_count >= g_ld_draw_scan_limit) : attempted) ||
        g_ld_frames < from_frame ||
        dc.n_source_refs < minimum || dc.n_source_refs > maximum) return LD_INVALID_SURFACE;
    const u64 vp_hash = fnv1a(g.rsx.vp, sizeof g.rsx.vp, 1469598103934665603ull);
    if (!first_target && vp_hash != wanted_vp) return LD_INVALID_SURFACE;
    rsx_dsp_surface declared_surface;
    rsx_dsp_get_surface(&g.rsx, &declared_surface);
    const u32 color_mask = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
    if (color_only && !ld_capture_color_output0(color_mask, declared_surface.color_target))
        return LD_INVALID_SURFACE;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE || g.surfaces[target].offset != wanted_offset ||
        g.surfaces[target].fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return LD_INVALID_SURFACE;
    if (g_ld_draw_scan_limit && g_ld_draw_scan_count &&
        g.surfaces[target].tex != g_ld_draw_scan_resource) {
        g_ld_draw_scan_done = 1;
        fprintf(stderr, "[draw-scan] stopped: target resource lifetime changed\n");
        return LD_INVALID_SURFACE;
    }
    if ((!g_ld_draw_scan_limit || !g_ld_draw_scan_count) &&
        g_ld_draw_pair_request && g_ld_draw_pair_request[0]) {
        FILE* request = fopen(g_ld_draw_pair_request, "rb");
        const int pending = request && fgetc(request) != EOF;
        if (request) fclose(request);
        if (!pending) return LD_INVALID_SURFACE;
    }
    attempted = 1;
    const int length = g_ld_draw_scan_limit ?
        snprintf(g_ld_draw_pair_base, sizeof g_ld_draw_pair_base,
                 "%s/draw_scan_%03u_f%06u_%016llx", directory, g_ld_draw_scan_count + 1,
                 g_ld_frames, (unsigned long long)live_legacy_pso_key()) :
        snprintf(g_ld_draw_pair_base, sizeof g_ld_draw_pair_base,
                 "%s/draw_pair_f%06u_%016llx", directory, g_ld_frames,
                 (unsigned long long)live_legacy_pso_key());
    if (length < 0 || (u32)length >= sizeof g_ld_draw_pair_base) return LD_INVALID_SURFACE;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s.json", g_ld_draw_pair_base);
    FILE* metadata = fopen(path, "wb");
    if (metadata) {
        fprintf(metadata, "{\"frame\":%u,\"vp_hash\":\"%016llx\",\"primitive\":%u,\"source_occurrences\":%u,"
                          "\"draw_vertices\":%u,\"target_slot\":%u,\"location\":%u,\"offset\":%u,\"width\":%u,\"height\":%u,"
                          "\"before_write_generation\":%u,\"before_write_kind\":%u,"
                          "\"color_mask\":%u,\"color_target\":%u,\"scan_ordinal\":%u,\"readbacks_requested\":%u}\n",
                g_ld_frames, (unsigned long long)vp_hash, prim, dc.n_source_refs, vertices, target,
                g.surfaces[target].location, g.surfaces[target].offset, g.surfaces[target].w,
                g.surfaces[target].h, g.surfaces[target].last_write_generation,
                g.surfaces[target].last_write_kind, color_mask, declared_surface.color_target,
                g_ld_draw_scan_limit ? g_ld_draw_scan_count + 1 : 0,
                g_ld_draw_scan_limit && g_ld_draw_scan_count ? 1 : 2);
        fclose(metadata);
    }
    if (g_ld_draw_scan_limit) {
        g_ld_draw_scan_resource = g.surfaces[target].tex;
        if (g_ld_draw_scan_count++) return target;
    }
    snprintf(path, sizeof path, "%s_before.ppm", g_ld_draw_pair_base);
    /* This flush happens before this draw allocates any GPU upload offsets,
     * descriptor tables or CB slots. The normal draw then rebuilds its state. */
    const u64 pixels = ld_dump_surface_ppm(path, &g.surfaces[target]);
    fprintf(stderr, "[draw-pair] before %s nonblack=%llu\n", path, (unsigned long long)pixels);
    if (pixels == UINT64_MAX && g_ld_draw_scan_limit) g_ld_draw_scan_done = 1;
    return pixels != UINT64_MAX ? target : LD_INVALID_SURFACE;
}

static void ld_draw_pair_end(u32 target)
{
    if (target == LD_INVALID_SURFACE || target >= g.n_surfaces) return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s_after.ppm", g_ld_draw_pair_base);
    /* Called only after the selected actual draw and all source state
     * restoration/ring accounting. A flush cannot invalidate its offsets. */
    const u64 pixels = ld_dump_surface_ppm(path, &g.surfaces[target]);
    fprintf(stderr, "[draw-pair] after %s nonblack=%llu\n", path, (unsigned long long)pixels);
    if (g_ld_draw_scan_limit) {
        const u64 area = (u64)g.surfaces[target].w * g.surfaces[target].h;
        const int coverage = pixels != UINT64_MAX && pixels * 100 >= area * g_ld_draw_scan_percent;
        if (coverage || g_ld_draw_scan_count >= g_ld_draw_scan_limit) g_ld_draw_scan_done = 1;
        snprintf(path, sizeof path, "%s_result.json", g_ld_draw_pair_base);
        FILE* result = fopen(path, "wb");
        if (result) {
            fprintf(result, "{\"scan_ordinal\":%u,\"nonblack\":%llu,\"pixels\":%llu,"
                            "\"after_write_generation\":%u,\"after_write_kind\":%u,"
                            "\"coverage_reached\":%d,\"scan_done\":%d}\n",
                    g_ld_draw_scan_count, (unsigned long long)pixels, (unsigned long long)area,
                    g.surfaces[target].last_write_generation, g.surfaces[target].last_write_kind,
                    coverage, g_ld_draw_scan_done);
            fclose(result);
        }
        fprintf(stderr, "[draw-scan] ordinal=%u coverage=%llu/%llu threshold=%u done=%d\n",
                g_ld_draw_scan_count, (unsigned long long)pixels, (unsigned long long)area,
                g_ld_draw_scan_percent, g_ld_draw_scan_done);
    }
    if (pixels != UINT64_MAX && (!g_ld_draw_scan_limit || g_ld_draw_scan_done) &&
        g_ld_draw_pair_request && g_ld_draw_pair_request[0]) {
        FILE* request = fopen(g_ld_draw_pair_request, "wb");
        if (request) { fclose(request); fprintf(stderr, "[draw-pair] request acknowledged\n"); }
    }
}

static u32 ld_canonical_sample_occurrences(u32 prim, u32 total, int spread, u32 out[64])
{
    if (!out) return 0;
    if (!spread) {
        const u32 count = total > 64 ? 64 : total;
        for (u32 i = 0; i < count; i++) out[i] = i;
        return count;
    }
    /* Complete triangles only. Strip/fan/restart semantics are deliberately
     * not guessed by this diagnostic sampling selector. */
    if (prim != RSX_PRIMITIVE_TRIANGLES || total < 3 || total % 3) return 0;
    const u32 triangles = total / 3;
    const u32 selected = triangles > 21 ? 21 : triangles;
    for (u32 i = 0; i < selected; i++) {
        const u32 triangle = selected == 1 ? 0 : (u32)((u64)i * (triangles - 1) / (selected - 1));
        for (u32 j = 0; j < 3; j++) out[i * 3 + j] = triangle * 3 + j;
    }
    return selected * 3;
}

/* One CPU-only snapshot of the exact canonical stream the draw consumes.
 * Raw float bits preserve NaN/Inf without invalid JSON or decimal loss. */
static void ld_canonical_capture_fp(const char* directory, u64 legacy_key, u32 ps_cb_offset)
{
    if (!ld_canonical_fp_enabled()) return;
    char base[MAX_PATH], path[MAX_PATH + 32];
    const int length = snprintf(base, sizeof base, "%s/canonical_f%06u_%016llx",
                               directory, g_ld_frames, (unsigned long long)legacy_key);
    if (length < 0 || (u32)length >= sizeof base) return;
    const char* source = NULL;
    for (u32 i = 0; i < g.n_psos; i++)
        if (g.psos[i].key == g_ld_current_pso.key) {
            source = g.psos[i].capture_ps_hlsl;
            break;
        }
    snprintf(path, sizeof path, "%s.ps.hlsl", base);
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs(source ? source : "// Exact cached PS source unavailable; no substitute generated.\n", f);
        fclose(f);
    }
    const u8* program = guest_ptr(g_ld_current_pso.fp_location,
                                 g_ld_current_pso.fp_offset, g_ld_current_pso.fp_size);
    snprintf(path, sizeof path, "%s.fp.bin", base);
    f = program ? fopen(path, "wb") : NULL;
    if (f) { fwrite(program, 1, g_ld_current_pso.fp_size, f); fclose(f); }
    const u32 slots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 cb_bytes = (slots + 2u) * 16u;
    const int cb_available = ld_fp_constants_buffered() && g.ps_cb_mapped &&
        ps_cb_offset <= PS_CB_RING_BYTES && cb_bytes <= PS_CB_RING_BYTES - ps_cb_offset;
    snprintf(path, sizeof path, "%s.pscb.bin", base);
    f = cb_available ? fopen(path, "wb") : NULL;
    if (f) { fwrite(g.ps_cb_mapped + ps_cb_offset, 1, cb_bytes, f); fclose(f); }
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    render_state_t rs;
    decode_render_state(&rs);
    const u32 packer = rsx_dsp_reg(&g.rsx, 0x1FECu);
    u32 alpha_bits; memcpy(&alpha_bits, &g.fp_alpha_ref, sizeof alpha_bits);
    snprintf(path, sizeof path, "%s.fp.json", base);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"schema\":1,\"frame\":%u,\"pipeline_key\":\"%016llx\","
               "\"exact_compiled_source_available\":%d,\"source_fnv1a\":\"%016llx\","
               "\"location\":%u,\"offset\":%u,\"program_bytes\":%u,\"program_available\":%d,"
               "\"program_provenance\":\"nonfrozen guest reread at canonical capture; exact cached compiled HLSL and uploaded CB are authoritative\","
               "\"buffered\":%d,\"constant_count\":%u,\"uploaded_cb_available\":%d,"
               "\"uploaded_cb_offset\":%u,\"uploaded_cb_bytes\":%u,\"alpha_ref_bits\":%u,"
               "\"fog_mode_raw\":%u,\"fog_param0_bits\":%u,\"fog_param1_bits\":%u,\"shader_window_raw\":%u,"
               "\"control_effective\":%u,\"control_raw\":%u,\"shader_packer\":%u,"
               "\"color_format\":%u,\"output_srgb_enabled\":%d,"
               "\"cube_mask\":%u,\"row1d_mask\":%u,\"unnorm_mask\":%u,\"shadow_mask\":%u,"
               "\"blend\":{\"enabled\":%u,\"src_rgb\":%u,\"dst_rgb\":%u,\"src_alpha\":%u,"
               "\"dst_alpha\":%u,\"eq_rgb\":%u,\"eq_alpha\":%u,\"color_mask\":%u,\"color_target\":%u},"
               "\"constants_bits\":[",
            g_ld_frames, (unsigned long long)g_ld_current_pso.key, source != NULL,
            (unsigned long long)(source ? fnv1a(source, strlen(source), 1469598103934665603ull) : 0),
            g_ld_current_pso.fp_location, g_ld_current_pso.fp_offset, g_ld_current_pso.fp_size,
            program != NULL, ld_fp_constants_buffered(), g.fp_constants.count, cb_available,
            ps_cb_offset, cb_available ? cb_bytes : 0, alpha_bits,
            rsx_dsp_reg(&g.rsx, 0x08CCu), rsx_dsp_reg(&g.rsx, 0x08D0u), rsx_dsp_reg(&g.rsx, 0x08D4u),
            rsx_dsp_reg(&g.rsx, 0x1D88u),
            g_ld_current_pso.fp_control, rsx_dsp_shader_control(&g.rsx), packer, sf.color_format,
            rsx_fp_srgb_output_enabled(packer, rsx_dsp_shader_control(&g.rsx), sf.color_format),
            g_ld_current_pso.cube_mask, g_ld_capture_row1d_mask, g_ld_capture_unnorm_mask,
            g_ld_capture_shadow_mask, rs.blend_enable, rs.sf_rgb, rs.df_rgb, rs.sf_a, rs.df_a,
            rs.eq_rgb, rs.eq_a, rs.color_mask, sf.color_target);
    for (u32 i = 0; i < g.fp_constants.count; i++)
        fprintf(f, "%s[%u,%u,%u,%u]", i ? "," : "", g.fp_constants.values[i][0],
                g.fp_constants.values[i][1], g.fp_constants.values[i][2], g.fp_constants.values[i][3]);
    fprintf(f, "],\"textures\":[");
    for (u32 unit = 0; unit < 16; unit++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, unit, &t);
        const rsx_fp_shadow_desc* sh = &g_ld_capture_shadow[unit];
        fprintf(f, "%s{\"unit\":%u,\"enabled\":%u,\"location\":%u,\"offset\":%u,"
                   "\"format\":%u,\"dimension\":%u,\"cube\":%u,\"mips\":%u,"
                   "\"width\":%u,\"height\":%u,\"depth\":%u,\"pitch\":%u,\"wrap\":%u,"
                   "\"remap\":%u,\"filter\":%u,\"control0\":%u,\"border_color\":%u,"
                   "\"unnorm_dimensions\":[%u,%u],\"row1d_width\":%u,"
                   "\"shadow\":{\"enabled\":%u,\"func\":%u,\"linear\":%u,\"unnormalized\":%u}}",
                unit ? "," : "", unit, t.enabled, t.location, t.offset, t.format, t.dimension,
                t.cubemap, t.mipmaps, t.width, t.height, t.depth, t.pitch, t.wrap, t.remap,
                t.filter, t.control0, t.border_color, g_ld_capture_unnorm_dim[unit][0],
                g_ld_capture_unnorm_dim[unit][1], g_ld_capture_row1d_width[unit],
                sh->enabled, sh->func, sh->linear, sh->unnormalized);
    }
    fprintf(f, "]}\n");
    fclose(f);
    fprintf(stderr, "[canonical-fp] wrote %s exact_source=%d constants=%u uploaded_cb=%d\n",
            path, source != NULL, g.fp_constants.count, cb_available);
}
/* Diagnostic only: the resolved descriptor slot, not a guest-address search,
 * identifies the resource that the pending draw will actually sample. */
static void ld_canonical_capture_surface(const char* directory, u64 key,
                                         const u32 slots[16])
{
    const char* enabled = getenv("LD_CANONICAL_CAPTURE_SURFACES");
    if (!enabled || atoi(enabled) == 0) return;
    const u32 slot = slots[14];
    const int direct = slot >= SRV_SURFACE_BASE && slot < SRV_SURFACE_BASE + g.n_surfaces;
    const surface_t* s = direct ? &g.surfaces[slot - SRV_SURFACE_BASE] : NULL;
    const int supported = s && s->tex && s->fmt == DXGI_FORMAT_R32_FLOAT;
    char path[MAX_PATH + 64];
    u64 nonblack = UINT64_MAX;
    if (supported) {
        const u32 cb = g.cb_used, ps = g.ps_cb_used, vb = g.vb_used;
        const u32 srv = g.srv_ring_used, smp = g.smp_ring_used;
        snprintf(path, sizeof path, "%s/canonical_f%06u_%016llx.unit14.ppm",
                 directory, g_ld_frames, (unsigned long long)key);
        /* All sampled direct color surfaces have already transitioned to PSR.
         * This fence resets command-list bindings but preserves ring storage. */
        g_ld_canonical_surface_rebind = 1;
        nonblack = ld_dump_resource_ppm(path, s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (cb != g.cb_used || ps != g.ps_cb_used || vb != g.vb_used ||
            srv != g.srv_ring_used || smp != g.smp_ring_used) {
            fprintf(stderr, "[canonical-surface] ring reservation changed; refusing pending draw\n");
            g.ready = 0;
        }
    }
    snprintf(path, sizeof path, "%s/canonical_f%06u_%016llx.unit14.json",
             directory, g_ld_frames, (unsigned long long)key);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"schema\":1,\"frame\":%u,\"pipeline_key\":\"%016llx\","
               "\"unit\":14,\"resolved_srv_slot\":%u,\"timing\":\"before_captured_draw\","
               "\"captured\":%s,\"status\":\"%s\","
               "\"producer_depth_sources\":\"unavailable_not_tracked\","
               "\"raw_sidecar_requires\":\"LD_RAW_SURFACE_DUMP=1\"",
            g_ld_frames, (unsigned long long)g_ld_current_pso.key, slot,
            nonblack != UINT64_MAX ? "true" : "false",
            !direct ? "unsupported_resolved_slot" : !supported ? "unsupported_resource_format" :
            nonblack == UINT64_MAX ? "readback_failed" : "captured_direct_R32F");
    if (s) {
        fprintf(f, ",\"owner_surface\":%u,\"location\":%u,\"offset\":%u,"
                   "\"width\":%u,\"height\":%u,\"dxgi_format\":%u,\"pitch\":%u,"
                   "\"restored_state\":\"PIXEL_SHADER_RESOURCE\"",
                slot - SRV_SURFACE_BASE, s->location, s->offset, s->w, s->h,
                (u32)s->fmt, s->pitch);
#if !defined(YZ_PERF_CLEAN)
        fprintf(f, ",\"resource_serial\":%u,\"last_write_generation\":%u,\"last_write_kind\":%u",
                s->resource_serial, s->last_write_generation, s->last_write_kind);
#else
        fprintf(f, ",\"last_write_generation\":null,\"generation_status\":\"unavailable_clean_build\"");
#endif
    }
    fputs("}\n", f); fclose(f);
}
/* BC1 diagnostics operate exclusively on GPU readback bytes. The preview is
 * a CPU integer decode, not a claim of bit-identical GPU interpolation. */
static void ld_capture_bc1_decode(const u8* blocks, u32 w, u32 h, u8* rgba)
{
    const u32 bw=(w+3u)/4u;
    for(u32 by=0;by<(h+3u)/4u;by++)for(u32 bx=0;bx<bw;bx++) {
        const u8* b=blocks+((size_t)by*bw+bx)*8u;
        const u32 c0=b[0]|((u32)b[1]<<8), c1=b[2]|((u32)b[3]<<8);
        const u32 bits=b[4]|((u32)b[5]<<8)|((u32)b[6]<<16)|((u32)b[7]<<24);
        u8 c[4][4]={{0}};
        for(u32 i=0;i<2;i++) {
            const u32 v=i?c1:c0, r=(v>>11)&31u,gc=(v>>5)&63u,bl=v&31u;
            c[i][0]=(u8)((r<<3)|(r>>2));c[i][1]=(u8)((gc<<2)|(gc>>4));
            c[i][2]=(u8)((bl<<3)|(bl>>2));c[i][3]=255;
        }
        for(u32 k=0;k<3;k++) {
            c[2][k]=(u8)(c0>c1?(2u*c[0][k]+c[1][k])/3u:(c[0][k]+c[1][k])/2u);
            c[3][k]=(u8)(c0>c1?(c[0][k]+2u*c[1][k])/3u:0u);
        }
        c[2][3]=255;c[3][3]=c0>c1?255:0;
        for(u32 y=0;y<4 && by*4+y<h;y++)for(u32 x=0;x<4 && bx*4+x<w;x++)
            memcpy(rgba+((size_t)(by*4+y)*w+bx*4+x)*4,c[(bits>>(2u*(y*4+x)))&3u],4);
    }
}
static u64 ld_capture_bc1_readback(const char* path, ID3D12Resource* resource,
                                  const D3D12_RESOURCE_DESC* desc)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={0};
    UINT rows=0;UINT64 rowbytes=0,total=0;
    g.dev->lpVtbl->GetCopyableFootprints(g.dev,desc,0,1,0,&footprint,&rows,&rowbytes,&total);
    const u32 w=(u32)desc->Width,h=desc->Height;
    if(!w || !h || w>4096 || h>4096 || rowbytes!=((w+3u)/4u)*8u ||
       rows!=(h+3u)/4u || !total || total>64u*1024u*1024u)return UINT64_MAX;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd={0};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width=total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
    bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb=NULL;
    if(FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&bd,
       D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&rb)))return UINT64_MAX;
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=resource;
    /* Only mip0 is copied; leave all other sampled mip states untouched. */
    barrier.Transition.Subresource=0;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
    src.pResource=resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&barrier);
    ld_flush(LD_FLUSH_READBACK);
    u64 result=UINT64_MAX;u8* mapped=NULL;
    D3D12_RANGE range={0,(SIZE_T)total};
    if(g.ready && SUCCEEDED(rb->lpVtbl->Map(rb,0,&range,(void**)&mapped))) {
        u8* packed=(u8*)malloc((size_t)rowbytes*rows);
        u8* rgba=(u8*)malloc((size_t)w*h*4);
        if(packed && rgba) {
            for(u32 y=0;y<rows;y++)memcpy(packed+(size_t)y*rowbytes,
                mapped+footprint.Offset+(size_t)y*footprint.Footprint.RowPitch,(size_t)rowbytes);
            ld_capture_bc1_decode(packed,w,h,rgba);
            char out[MAX_PATH+96];snprintf(out,sizeof out,"%s.bc1",path);
            FILE* f=fopen(out,"wb");int ok=f && fwrite(packed,1,(size_t)rowbytes*rows,f)==(size_t)rowbytes*rows;
            if(f)fclose(f);
            snprintf(out,sizeof out,"%s.rgba",path);f=fopen(out,"wb");
            ok=ok && f!=NULL;if(f){if(fwrite(rgba,4,(size_t)w*h,f)!=(size_t)w*h)ok=0;fclose(f);}
            snprintf(out,sizeof out,"%s.ppm",path);f=fopen(out,"wb");
            ok=ok && f!=NULL;if(f){fprintf(f,"P6\n%u %u\n255\n",w,h);
                for(size_t i=0;i<(size_t)w*h;i++)if(fwrite(rgba+i*4,1,3,f)!=3)ok=0;fclose(f);}
            if(ok)result=0;
        }
        free(packed);free(rgba);D3D12_RANGE empty={0,0};rb->lpVtbl->Unmap(rb,0,&empty);
    }
    rb->lpVtbl->Release(rb);return result;
}
static void ld_canonical_capture_texture0(const char* directory, u64 key, const u32 slots[16])
{
    const char* enabled = getenv("LD_CANONICAL_CAPTURE_SURFACES");
    if (!enabled || atoi(enabled) == 0) return;
    const u32 slot = slots[0];
    const int cache_slot = slot >= SRV_TEXTURE_BASE && slot < SRV_TEXTURE_BASE + g.n_textures;
    const texcache_t* entry = cache_slot ? &g.textures[slot - SRV_TEXTURE_BASE] : NULL;
    D3D12_RESOURCE_DESC desc = {0};
    if (entry && entry->tex) entry->tex->lpVtbl->GetDesc(entry->tex, &desc);
    const int bc1=desc.Format==DXGI_FORMAT_BC1_UNORM || desc.Format==DXGI_FORMAT_BC1_UNORM_SRGB;
    const int supported = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || bc1) && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.Width <= UINT32_MAX;
    char path[MAX_PATH + 64]; u64 nonblack = UINT64_MAX;
    if (supported) {
        /* Metadata adapter only; this remains the actual cache-owned texture. */
        surface_t metadata = {0};
        metadata.tex = entry->tex; metadata.w = (u32)desc.Width;
        metadata.h = desc.Height; metadata.fmt = desc.Format;
        metadata.location = entry->location; metadata.offset = entry->offset;
        const u32 cb=g.cb_used, ps=g.ps_cb_used, vb=g.vb_used;
        const u32 srv=g.srv_ring_used, smp=g.smp_ring_used;
        snprintf(path,sizeof path,"%s/canonical_f%06u_%016llx.unit0.ppm",
                 directory,g_ld_frames,(unsigned long long)key);
        g_ld_canonical_surface_rebind = 1;
        nonblack = bc1 ? ld_capture_bc1_readback(path,entry->tex,&desc) :
            ld_dump_resource_ppm(path,&metadata,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if(cb!=g.cb_used || ps!=g.ps_cb_used || vb!=g.vb_used ||
           srv!=g.srv_ring_used || smp!=g.smp_ring_used) {
            fprintf(stderr,"[canonical-surface] unit0 ring reservation changed; refusing pending draw\n");
            g.ready=0;
        }
    }
    snprintf(path,sizeof path,"%s/canonical_f%06u_%016llx.unit0.json",
             directory,g_ld_frames,(unsigned long long)key);
    FILE* f=fopen(path,"wb");if(!f)return;
    fprintf(f,"{\"schema\":1,\"frame\":%u,\"pipeline_key\":\"%016llx\",\"unit\":0,"
              "\"resolved_srv_slot\":%u,\"timing\":\"before_captured_draw\",\"captured\":%s,"
              "\"status\":\"%s\",\"subresource\":0,\"raw_sidecar_requires\":\"LD_RAW_SURFACE_DUMP=1\"",
            g_ld_frames,(unsigned long long)g_ld_current_pso.key,slot,
            nonblack!=UINT64_MAX?"true":"false",!cache_slot?"unsupported_resolved_slot":
            !supported?"unsupported_resource_format":nonblack==UINT64_MAX?"readback_failed":
            bc1?"captured_GPU_BC1_bytes":"captured_decoded_RGBA8");
    if(bc1)fprintf(f,",\"compressed_provenance\":\"actual_GPU_mip0_readback\","
        "\"preview_decode\":\"CPU_BC1_565_integer_palette_may_differ_GPU_rounding\","
        "\"preview_srgb\":\"encoded_values_no_linearization\",\"block_bytes\":8,\"block_width\":4,\"block_height\":4");
    if(entry)fprintf(f,",\"cache_index\":%u,\"location\":%u,\"offset\":%u,\"guest_format\":%u,"
              "\"guest_width\":%u,\"guest_height\":%u,\"guest_pitch\":%u,\"guest_remap\":%u,"
              "\"guest_mips\":%u,\"width\":%llu,\"height\":%u,\"dxgi_format\":%u,"
              "\"content_hash\":\"%016llx\",\"last_use_serial\":%llu,\"resource_serial\":null,"
              "\"resource_serial_status\":\"not_tracked_by_texture_cache\",\"restored_state\":\"PIXEL_SHADER_RESOURCE\"",
            slot-SRV_TEXTURE_BASE,entry->location,entry->offset,entry->format,entry->width,entry->height,
            entry->pitch,entry->remap,entry->mipmaps,(unsigned long long)desc.Width,desc.Height,(u32)desc.Format,
            (unsigned long long)entry->content_hash,(unsigned long long)entry->last_use_serial);
    fputs("}\n",f);fclose(f);
}
static void ld_canonical_capture(u32 prim, u32 vertices, u32 target, const float xf[8], u32 ps_cb_offset, const u32 slots[16])
{
    static int initialized, attempted;
    static const char* directory;
    static u64 wanted_vp, wanted_pso, wanted_ps_hash;
    static u32 from_frame, minimum_vertices, maximum_vertices, target_offset;
    static int filter_target, spread, first_target, color_only;
    if (!initialized) {
        initialized = 1;
        directory = getenv("LD_CANONICAL_CAPTURE_DIR");
        const char* first = getenv("LD_CANONICAL_CAPTURE_FIRST_TARGET");
        first_target = first && atoi(first) != 0;
        const char* color = getenv("LD_CANONICAL_CAPTURE_COLOR_ONLY");
        color_only = color && atoi(color) != 0;
        const char* v = getenv("LD_CANONICAL_CAPTURE_VP");
        wanted_vp = v ? _strtoui64(v, NULL, 16) : 0;
        v = getenv("LD_CANONICAL_CAPTURE_PSO");
        wanted_pso = v ? _strtoui64(v, NULL, 16) : 0;
        v = getenv("LD_CANONICAL_CAPTURE_PS_HASH");
        wanted_ps_hash = v ? _strtoui64(v, NULL, 16) : 0;
        v = getenv("LD_CANONICAL_CAPTURE_FROM");
        from_frame = v ? (u32)strtoul(v, NULL, 10) : 2040;
        v = getenv("LD_CANONICAL_CAPTURE_MIN_VERTICES");
        minimum_vertices = v ? (u32)strtoul(v, NULL, 10) : 0;
        v = getenv("LD_CANONICAL_CAPTURE_MAX_VERTICES");
        maximum_vertices = v ? (u32)strtoul(v, NULL, 10) : UINT32_MAX;
        v = getenv("LD_CANONICAL_CAPTURE_TARGET");
        filter_target = v && v[0];
        target_offset = filter_target ? (u32)strtoul(v, NULL, 16) : 0;
        v = getenv("LD_CANONICAL_CAPTURE_SPREAD");
        spread = v && atoi(v) != 0;
    }
    if (!directory || !directory[0] || attempted || g_ld_frames < from_frame ||
        (!wanted_vp && !wanted_pso && !wanted_ps_hash && !first_target) ||
        (first_target && (!filter_target || !target_offset))) return;
    const u32 occurrences = dc.n_source_refs ? dc.n_source_refs : dc.n_verts;
    if (occurrences < minimum_vertices || occurrences > maximum_vertices || target >= g.n_surfaces ||
        (filter_target && g.surfaces[target].offset != target_offset)) return;
    const u64 vp_hash = fnv1a(g.rsx.vp, sizeof g.rsx.vp, 1469598103934665603ull);
    if (wanted_vp && wanted_vp != vp_hash) return;
    const u64 legacy_key = live_legacy_pso_key();
    if (wanted_pso && wanted_pso != legacy_key) return;
    /* Optional exact cached HLSL selector; no source lookup/hash when unset.
     * All configured selectors intersect, including FIRST_TARGET mode. */
    if (wanted_ps_hash) {
        const char* source = NULL;
        for (u32 i = 0; i < g.n_psos; i++)
            if (g.psos[i].key == g_ld_current_pso.key) {
                source = g.psos[i].capture_ps_hlsl;
                break;
            }
        if (!source || fnv1a(source, (u32)strlen(source), 1469598103934665603ull) != wanted_ps_hash)
            return;
    }
    rsx_dsp_surface declared_surface;
    rsx_dsp_get_surface(&g.rsx, &declared_surface);
    const u32 color_mask = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
    if (color_only && !ld_capture_color_output0(color_mask, declared_surface.color_target)) return;
    u32 selected_occurrences[64];
    const u32 count = ld_canonical_sample_occurrences(prim, occurrences, spread, selected_occurrences);
    if (!count) {
        static int rejected;
        if (!rejected++) fprintf(stderr, "[canonical-capture] unsupported sampling primitive=%u occurrences=%u spread=%d\n", prim, occurrences, spread);
        return;
    }
    /* Diagnostic output directories may be new. Do not consume the one-shot
     * capture if the destination cannot be created. */
    if (!CreateDirectoryA(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        static int directory_error;
        if (!directory_error++) fprintf(stderr, "[canonical-capture] cannot create directory %s\n", directory);
        return;
    }
    attempted = 1;
    ld_canonical_capture_fp(directory, legacy_key, ps_cb_offset);
    ld_canonical_capture_surface(directory, legacy_key, slots);
    if (g.ready) ld_canonical_capture_texture0(directory, legacy_key, slots);
    char path[MAX_PATH];
    const int length = snprintf(path, sizeof path, "%s/canonical_f%06u_%016llx.json",
                               directory, g_ld_frames, (unsigned long long)legacy_key);
    if (length < 0 || (u32)length >= sizeof path) return;
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[canonical-capture] cannot open %s\n", path); return; }
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    const u32 available = start < RSX_DSP_VP_INSTR ? (RSX_DSP_VP_INSTR - start) * 16 : 0;
    const u8* program = available ? (const u8*)(g.rsx.vp + start * 4) : NULL;
    const u32 instructions = program ? rsx_vp_program_size_instrs(program, available) : 0;
    fprintf(f, "{\n\"schema\":1,\"frame\":%u,\"vp_hash\":\"%016llx\",\"legacy_pso_key\":\"%016llx\","
               "\"pipeline_key\":\"%016llx\",\"primitive\":%u,\"draw_vertices\":%u,\"source_occurrences\":%u,"
               "\"captured_occurrences\":%u,\"refs_remapped\":%d,\"target_slot\":%u,\"vp_start\":%u,\"vp_instructions\":%u,"
               "\"input_mask\":%u,\"vtex_mask\":%u,\"base_offset\":%u,\"divider_mask\":%u,\n\"vp_program_hex\":\"",
            g_ld_frames, (unsigned long long)vp_hash, (unsigned long long)legacy_key,
            (unsigned long long)g_ld_current_pso.key, prim, vertices, occurrences, count,
            dc.refs_remapped, target, start, instructions, g_ld_current_pso.input_mask,
            g_ld_current_pso.vtex_mask, dc.fetch_plan.base_offset, dc.fetch_plan.divider_mask);
    for (u32 i = 0; i < instructions * 16; i++) fprintf(f, "%02X", program[i]);
    const surface_t* surface = &g.surfaces[target];
    fprintf(f, "\",\n\"target\":{\"location\":%u,\"offset\":%u,\"width\":%u,\"height\":%u,"
               "\"dxgi_format\":%u,\"guest_format\":%u,\"pitch\":%u,\"raster_type\":%u,"
               "\"write_generation\":%u,\"write_kind\":%u,\"color_mask\":%u,\"color_target\":%u},\n\"vp_store_hex\":\"",
            surface->location, surface->offset, surface->w, surface->h, (u32)surface->fmt,
            rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu, surface->pitch, surface->raster_type,
            surface->last_write_generation, surface->last_write_kind,
            color_mask, declared_surface.color_target);
    for (u32 i = 0; i < sizeof g.rsx.vp; i++) fprintf(f, "%02X", ((const u8*)g.rsx.vp)[i]);
    fprintf(f, "\",\n\"constants_bits\":[");
    for (u32 i = 0; i < RSX_DSP_NUM_CONSTANTS; i++)
        fprintf(f, "%s[%u,%u,%u,%u]", i ? "," : "", g.rsx.constants[i][0],
                g.rsx.constants[i][1], g.rsx.constants[i][2], g.rsx.constants[i][3]);
    fprintf(f, "],\n\"viewport_epilogue_bits\":[");
    for (u32 i = 0; i < 8; i++) { u32 bits; memcpy(&bits, xf + i, 4); fprintf(f, "%s%u", i ? "," : "", bits); }
    fprintf(f, "],\n\"defaults_bits\":[");
    for (u32 attr = 0; attr < 16; attr++) {
        u32 bits[4]; memcpy(bits, dc.fetch_plan.attr[attr].default_value, 16);
        fprintf(f, "%s[%u,%u,%u,%u]", attr ? "," : "", bits[0], bits[1], bits[2], bits[3]);
    }
    fprintf(f, "],\n\"registers\":[");
    for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++)
        fprintf(f, "%s%u", i ? "," : "", rsx_dsp_reg(&g.rsx, i * 4));
    fprintf(f, "],\n\"sampling\":\"%s\",\n\"vertices\":[\n", spread ? "spread_triangles" : "prefix");
    for (u32 sample = 0; sample < count; sample++) {
        const u32 occurrence = selected_occurrences[sample];
        const u32 vertex = dc.refs_remapped ? rsx_vertex_remap_index(&dc.ref_remap, occurrence) : occurrence;
        fprintf(f, "%s{\"occurrence\":%u,\"mapped_vertex\":%u,\"attributes_bits\":[", sample ? ",\n" : "", occurrence, vertex);
        for (u32 attr = 0; attr < 16; attr++) {
            float fallback[4]; u32 bits[4];
            const float* value = dc_vertex_attr_canonical(vertex, attr, fallback);
            memcpy(bits, value, 16);
            fprintf(f, "%s[%u,%u,%u,%u]", attr ? "," : "", bits[0], bits[1], bits[2], bits[3]);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n]}\n"); fclose(f);
    fprintf(stderr, "[canonical-capture] wrote %s occurrences=%u/%u\n", path, count, occurrences);
    if (instructions) {
        static char hlsl[256 * 1024];
        rsx_vp_set_branch_base(start);
        const int result = rsx_vp_decompile_compact_ex(program, instructions * 16,
            g_ld_current_pso.vtex_mask, g_ld_current_pso.input_mask, hlsl, sizeof hlsl);
        snprintf(path, sizeof path, "%s/canonical_f%06u_%016llx.vs.hlsl", directory,
                 g_ld_frames, (unsigned long long)legacy_key);
        f = fopen(path, "wb");
        if (f) { fputs(result > 0 ? hlsl : "<vp translation failed>", f); fclose(f); }
    }
}

/* RSX_DRAW_CSV=path: uncapped per-draw fingerprints for direct comparison
 * with the working RPCS3 .rxs replay.  Default-off and renderer-neutral. */
/* The open RSX_DRAW_CSV and its gate, for marker rows from outside a draw
 * (rsx_live_draw_blit notes each 2D copy where it falls in the draw stream). */
static FILE* s_csv_file;
static int   s_csv_gate_open = 1;

/* A nonempty request arms one bounded recording. Poll once per actual frame,
 * acknowledge by truncation, and never retrigger in this process. */
static int ld_csv_request_gate(const char* request, u32 frame, u64 rows, u64 limit,
                               int* state, u32* last_poll)
{
    if (*state == 2) return 0;
    if (rows >= limit) { *state = 2; return 0; }
    if (*state == 1) return 1;
    if (*last_poll == frame) return 0;
    *last_poll = frame;
    FILE* f = fopen(request, "rb");
    const int pending = f && fgetc(f) != EOF;
    if (f) fclose(f);
    if (!pending) return 0;
    *state = 1;
    f = fopen(request, "wb");
    if (f) fclose(f);
    fprintf(stderr, "[draw-csv-request] started frame=%u limit=%llu request=%s\n",
            frame, (unsigned long long)limit, request);
    return 1;
}

static void live_draw_csv_emit(u32 prim, u32 n_tri, const char* outcome)
{
#if defined(YZ_PERF_CLEAN) && !defined(YZ_PERF_PROFILE)
    (void)prim;
    (void)n_tri;
    (void)outcome;
#else
    static int inited = 0;
    static FILE* file = NULL;
    static u64 draw = 0;
    static const char* request = (const char*)-1;
    if (request == (const char*)-1) request = getenv("RSX_DRAW_CSV_REQUEST_FILE");
    if (request && !request[0]) request = NULL;
    /* Bound diagnostic I/O without dropping or changing a guest draw. */
    { static long limit = -1;
      if (limit < 0) { const char* e = getenv("RSX_DRAW_CSV_LIMIT"); limit = e ? atol(e) : 0; }
      if (request) {
          static int state;
          static u32 last_poll = UINT32_MAX;
          if (limit <= 0) limit = 4000;
          s_csv_gate_open = ld_csv_request_gate(request, g_ld_frames, draw,
                                                (u64)limit, &state, &last_poll);
          if (!s_csv_gate_open) return;
      }
      if (limit > 0 && draw >= (u64)limit) return; }
    /* RSX_DRAW_CSV_FROM=<frame>: skip rows before that frame. Logging every
     * draw from boot slows a 10-minute run to an hour. */
    { static long from = -1;
      if (from < 0) { const char* e = getenv("RSX_DRAW_CSV_FROM"); from = e ? atol(e) : 0; }
      if (!request && (long)g_ld_frames < from) return; }
    /* RSX_DRAW_CSV_GATE=<file>: rows only while <file> exists (polled every
     * 512 draws), so a capture can be switched on when the scene of interest
     * is on screen instead of guessing its frame number. */
    { static const char* gate = (const char*)-1; static int open_gate = 0; static unsigned poll;
      if (gate == (const char*)-1) gate = getenv("RSX_DRAW_CSV_GATE");
      if (gate && !request) {
          if ((poll++ & 511u) == 0) { struct stat st; open_gate = stat(gate, &st) == 0; }
          s_csv_gate_open = open_gate;
          if (!open_gate) return;
      } }
    static int a010_only = -1;
    if (a010_only < 0) a010_only = getenv("RSX_DRAW_CSV_A010_ONLY") != NULL;
    if (a010_only) {
        if (InterlockedCompareExchange(
                &g_yz_a010_root_active, 0, 0) == 0)
            return;
    }
    if (!inited) {
        inited = 1;
        const char* path = getenv("RSX_DRAW_CSV");
        if (path && path[0]) {
            file = fopen(path, "w");
            s_csv_file = file;
            if (file) {
                fprintf(file,
                    "draw,frame,outcome,surf,prim,verts,source_verts,decoded_hash,"
                    "pso_key,regs_hash,vp_hash,"
                    "const_hash,blend,dtest,dwrite,dfunc,cull,cullface,"
                    "frontface,cmask,vpx,vpy,vpw,vph,sclx,scly,sclz,"
                    "trnx,trny,trnz,clipw,cliph,zeta_off,zeta_pitch,zeta_loc,"
                    "seen_vtex,seen_vtxfmt,seen_freqdiv,"
                    "attr0_hash,attr1_hash,attr2_hash,attr3_hash,"
                    "attr4_hash,attr5_hash,attr6_hash,attr7_hash,"
                    "attr8_hash,attr9_hash,attr10_hash,attr11_hash,"
                    "attr12_hash,attr13_hash,attr14_hash,attr15_hash,"
                    "active_attr_mask,used_attr_mask,tex\n");
                fprintf(stderr, "[live-diff] RSX_DRAW_CSV armed: %s\n", path);
            } else {
                fprintf(stderr, "[live-diff] cannot open RSX_DRAW_CSV: %s\n",
                        path);
            }
            fflush(stderr);
        }
    }
    if (!file)
        return;

    const u32 target = current_surface();
    const u32 surf = target < g.n_surfaces ? g.surfaces[target].offset : 0;
    /* LD_DRAW_DUMP=<dir> [LD_DRAW_DUMP_EVERY=k, default 10]: in the first
     * gated frame, read back each k-th draw's target BEFORE it is recorded,
     * named by frame-relative index -- bisects which draw changes a surface. */
    { static const char* dir = (const char*)-1; static u32 every, frame = ~0u, idx, dumped_frame = ~0u;
      if (dir == (const char*)-1) { dir = getenv("LD_DRAW_DUMP");
          const char* e = getenv("LD_DRAW_DUMP_EVERY"); every = e ? (u32)atoi(e) : 10; if (!every) every = 10; }
      if (frame != g_ld_frames) { frame = g_ld_frames; idx = 0; }
      if (dir && target < g.n_surfaces && (dumped_frame == ~0u || dumped_frame == frame) &&
          (idx % every) == 0) {
          dumped_frame = frame;
          char path[MAX_PATH * 2];
          snprintf(path, sizeof(path), "%s\\draw_f%06u_i%04u_%08X.ppm", dir, frame, idx, surf);
          ld_dump_surface_ppm(path, &g.surfaces[target]);
      }
      idx++; }
    const u64 regs_hash = fnv1a(
        g.rsx.regs, RSX_DSP_NUM_REGS * sizeof(g.rsx.regs[0]),
        1469598103934665603ull);
    const u64 vp_hash = fnv1a(
        g.rsx.vp, sizeof(g.rsx.vp), 1469598103934665603ull);
    const u64 const_hash = fnv1a(
        g.rsx.constants, sizeof(g.rsx.constants), 1469598103934665603ull);
    render_state_t rs;
    decode_render_state(&rs);
    rsx_dsp_viewport vp;
    rsx_dsp_get_viewport(&g.rsx, &vp);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);

    u32 seen_vtex = 0, seen_vtxfmt = 0;
    for (u32 i = 0; i < 32; i++)
        seen_vtex += g.rsx.seen[(0x0900u >> 2) + i];
    for (u32 i = 0; i < 16; i++)
        seen_vtxfmt += g.rsx.seen[(0x1740u >> 2) + i];
    const u32 seen_freqdiv = g.rsx.seen[0x1FC0u >> 2];
    u64 attr_hash[16];
    const u64 decoded_hash = live_decoded_vertex_hash(attr_hash);
    u32 active_attr_mask = 0;
    for (u32 attr = 0; attr < 16; attr++)
        if (dc.fetch_plan.attr[attr].desc.type &&
            dc.fetch_plan.attr[attr].desc.size)
            active_attr_mask |= 1u << attr;

    fprintf(file,
        "%llu,%u,%s,0x%X,%u,%u,%u,%016llx,"
        "%016llx,%016llx,%016llx,%016llx,"
        "%u,%u,%u,0x%X,%u,0x%X,0x%X,0x%08X,"
        "%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%u,%u,0x%X,%u,%u,%u,%u,%u",
        (unsigned long long)draw++, g_ld_frames, outcome, surf, prim, n_tri,
        dc.n_source_refs ? dc.n_source_refs : dc.n_verts,
        (unsigned long long)decoded_hash,
        (unsigned long long)live_legacy_pso_key(),
        (unsigned long long)regs_hash, (unsigned long long)vp_hash,
        (unsigned long long)const_hash,
        rs.blend_enable, rs.depth_test, rs.depth_write, rs.depth_func,
        rs.cull_enable, rs.cull_face, rs.front_face, rs.color_mask,
        vp.x, vp.y, vp.w, vp.h,
        vp.scale[0], vp.scale[1], vp.scale[2],
        vp.translate[0], vp.translate[1], vp.translate[2],
        sf.clip_w, sf.clip_h, sf.zeta_offset, sf.zeta_pitch, sf.zeta_location,
        seen_vtex, seen_vtxfmt, seen_freqdiv);
    for (u32 attr = 0; attr < 16; attr++)
        fprintf(file, ",%016llx", (unsigned long long)attr_hash[attr]);
    fprintf(file, ",0x%04X,0x%04X,",
            active_attr_mask, dc.layout.mask & 0xFFFFu);
    /* Enabled fragment texture units, so a pass that samples a surface (or an
     * empty copy of one) is visible in the row: unit:loc:offset:fmt:WxH. */
    for (u32 u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled)
            fprintf(file, "%u:%u:%X:%02X:%ux%u[remap=%X/wrap=%X/ctl0=%X/filter=%X/dim=%u/mips=%u/pitch=%u] ",
                    u, t.location, t.offset, t.format & 0xFF, t.width, t.height,
                    t.remap, t.wrap, t.control0, t.filter, t.dimension, t.mipmaps, t.pitch);
    }
    if (rs.blend_enable)
        fprintf(file, "bf=%X/%X/%X/%X eq=%X/%X ", rs.sf_rgb, rs.df_rgb, rs.sf_a, rs.df_a,
                rs.eq_rgb, rs.eq_a);
    fprintf(file, "at=%u/0x%X/0x%X ", rs.alpha_test_enable, rs.alpha_func, rs.alpha_ref_raw);
    fprintf(file, "sc=%08X/%08X ", rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL));
    if (rs.stencil_enable)
        fprintf(file, "st=%X/%X/%X/%X/%X/%X ref=%X ", rs.s_func, rs.s_func_mask, rs.s_write_mask, rs.s_fail, rs.s_zfail, rs.s_zpass, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC + 4));
    /* Inline draws (HUD sprites): the largest vertex-colour alpha byte, attr 3
     * as UB4 -- an element faded to 0 is drawn but invisible. */
    if (dc.inl && dc.inl_stride) {
        rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
        if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
            u32 amax = 0;
            for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                if (dc.inl[o] > amax) amax = dc.inl[o];
            fprintf(file, "va=%u ", amax);
        }
        /* ...and the xy bounding box of attr 0 when it is float (screen-space HUD). */
        rsx_dsp_vertex_attr p0; rsx_dsp_get_vertex_attr(&g.rsx, 0, &p0);
        if (p0.type == RSX_VTX_TYPE_FLOAT && p0.size >= 2) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (u32 o = dc.inl_off[0]; o + 8 <= dc.inl_bytes; o += dc.inl_stride) {
                u32 xb = (dc.inl[o] << 24) | (dc.inl[o+1] << 16) | (dc.inl[o+2] << 8) | dc.inl[o+3];
                u32 yb = (dc.inl[o+4] << 24) | (dc.inl[o+5] << 16) | (dc.inl[o+6] << 8) | dc.inl[o+7];
                float x, y; memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
            fprintf(file, "bb=%.0f,%.0f-%.0f,%.0f ", x0, y0, x1, y1);
        }
    }
    fprintf(file, "A=%u:%X/%u fmt=%u rt=%u ", sf.color_location[0], sf.color_offset[0], sf.color_pitch[0], sf.color_format, sf.raster_type);
    fprintf(file, "ct=0x%X B=%u:%X C=%u:%X D=%u:%X ", sf.color_target,
            sf.color_location[1], sf.color_offset[1], sf.color_location[2], sf.color_offset[2],
            sf.color_location[3], sf.color_offset[3]);
    fputc('\n', file);
    fflush(file);
#endif
}

/* RSX_A010_GEOM: trace one stable orphanage mesh.  This is draw 520 from
 * the healthy a010 reference: a 270-vertex triangle strip on the main world
 * surface with PSO 7d9f....  The model-initialization repair now reproduces
 * this exact tuple in the live stream.  Keeping the tuple exact makes the gate
 * quiet outside a010 while allowing capture/playback only after known-good
 * world geometry has actually reached the backend. */
static void live_a010_geom_trace(u32 prim, u32 n_tri)
{
    static int enabled = -1;
    static u32 logged = 0;
    static u32 reference_logged = 0;
    if (enabled < 0)
        enabled = getenv("RSX_A010_GEOM") ? 1 : 0;
    if (prim != PRIM_TRIANGLE_STRIP || n_tri != 270)
        return;

    const u32 target = current_surface();
    const u32 surf =
        target < g.n_surfaces ? g.surfaces[target].offset : 0;
    const u64 key = live_legacy_pso_key();
    if (surf != 0x01800000u || key != 0x7d9f528329f20c52ull)
        return;
    /*
     * This topology/PSO/surface tuple is the stable Kiryu mesh measured in
     * the healthy a010 capture. Reaching this point means vertex fetch has
     * completed and the scene has produced real world geometry, rather than
     * only clears and fullscreen post-processing draws.
     */
    if (InterlockedCompareExchange(&g_ld_a010_world_ready, 1, 0) == 0) {
        fprintf(stderr,
                "[a010-world-ready] frame=%u surface=0x%X "
                "pso=%016llx vertices=%u\n",
                g_ld_frames, surf, (unsigned long long)key, n_tri);
        fflush(stderr);
    }
    if (!enabled)
        return;
    if (InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) != 0) {
        if (reference_logged >= 8)
            return;
        reference_logged++;
    } else {
        if (logged >= 8)
            return;
        logged++;
    }

    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    float mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    u32 nan_count = 0;
    for (u32 vi = 0; vi < dc.n_verts; vi++) {
        float fallback[4];
        const float* p = dc_vertex_attr(vi, 0, fallback);
        for (u32 k = 0; k < 4; k++) {
            if (p[k] != p[k]) {
                nan_count++;
                continue;
            }
            if (p[k] < mn[k]) mn[k] = p[k];
            if (p[k] > mx[k]) mx[k] = p[k];
        }
    }

    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    rsx_dsp_vertex_attr a0, a7;
    rsx_dsp_get_vertex_attr(&g.rsx, 0, &a0);
    rsx_dsp_get_vertex_attr(&g.rsx, 7, &a7);

    fprintf(stderr,
        "[a010-geom] match=%u frame=%u fetched=%u expanded=%u cuts=%u "
        "pos=[%.6g %.6g %.6g %.6g]-[%.6g %.6g %.6g %.6g] nan=%u "
        "decoded_hash=%016llx\n",
        logged, g_ld_frames, dc.n_verts, n_tri, dc.n_cuts,
        mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
        nan_count,
        (unsigned long long)fnv1a(
            ld_vertex_compact_payload()
                ? (const void*)dc.compact_verts
                : (const void*)dc.verts,
            ld_vertex_compact_payload()
                ? (u32)((u64)dc.n_verts * dc.layout.stride)
                : (u32)((u64)dc.n_verts * sizeof(vtx_t)),
            1469598103934665603ull));
    fprintf(stderr,
        "[a010-geom] base=0x%X base_index=%u "
        "idx(loc=%u off=0x%X u32=%u first=%u count=%u) "
        "a0(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u) "
        "a7(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u)\n",
        base, base_index,
        ia.location, ia.offset, ia.is_u32,
        dc.n_idx ? dc.idx[0].first : 0,
        dc.n_idx ? dc.idx[0].count : 0,
        a0.type, a0.size, a0.stride, a0.location, a0.offset, a0.frequency,
        a7.type, a7.size, a7.stride, a7.location, a7.offset, a7.frequency);

    for (u32 slot = 108; slot <= 115; slot++) {
        const u32* c = rsx_dsp_constant(&g.rsx, slot);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c%u=(%.6g %.6g %.6g %.6g) "
            "raw=%08X/%08X/%08X/%08X\n",
            slot, v[0].f, v[1].f, v[2].f, v[3].f,
            v[0].u, v[1].u, v[2].u, v[3].u);
    }
    {
        const u32* c = rsx_dsp_constant(&g.rsx, 467);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c467=(%.6g %.6g %.6g %.6g)\n",
            v[0].f, v[1].f, v[2].f, v[3].f);
    }
    {
        float camera[4][4];
        float c467[4];
        for (u32 row = 0; row < 4; row++)
            memcpy(camera[row], rsx_dsp_constant(&g.rsx, 108u + row),
                   sizeof(camera[row]));
        memcpy(c467, rsx_dsp_constant(&g.rsx, 467), sizeof(c467));
        rsx_dsp_viewport vp;
        rsx_dsp_get_viewport(&g.rsx, &vp);
        float sx_min = 1e30f, sy_min = 1e30f;
        float sx_max = -1e30f, sy_max = -1e30f;
        float w_min = 1e30f, w_max = -1e30f;
        u32 finite = 0, in_front = 0, in_view = 0;
        for (u32 vi = 0; vi < dc.n_verts; vi++) {
            float p_fallback[4], weight_fallback[4], bone_fallback[4];
            const float* p = dc_vertex_attr(vi, 0, p_fallback);
            const float* weight =
                dc_vertex_attr(vi, 1, weight_fallback);
            const float* bone =
                dc_vertex_attr(vi, 7, bone_fallback);
            const float pw = p[3] != 0.0f ? p[3] : 1.0f;
            /*
             * Exact transform used by healthy PSO 7d9f...: ATTR7 selects
             * four 3-row bone matrices at c112+, ATTR1.yzw supplies weights
             * 1..3, and weight 0 is implicit.  The old probe incorrectly
             * treated c112..c115 as one fixed 4x4 matrix.
             */
            const float influence[4] = {
                c467[1] - weight[1] - weight[2] - weight[3],
                weight[1], weight[2], weight[3]
            };
            float skin[3][4] = {{0}};
            for (u32 inf = 0; inf < 4; inf++) {
                const u32 base_slot =
                    (u32)(bone[inf] * c467[0]);
                for (u32 row = 0; row < 3; row++) {
                    const float* palette = (const float*)
                        rsx_dsp_constant(
                            &g.rsx, (112u + base_slot + row) & 511u);
                    for (u32 lane = 0; lane < 4; lane++)
                        skin[row][lane] +=
                            influence[inf] * palette[lane];
                }
            }
            const float r7x =
                p[0]*skin[0][0] + p[1]*skin[0][1] +
                p[2]*skin[0][2] + pw*skin[0][3];
            const float r7y =
                p[0]*skin[1][0] + p[1]*skin[1][1] +
                p[2]*skin[1][2] + pw*skin[1][3];
            const float r7z =
                p[0]*skin[2][0] + p[1]*skin[2][1] +
                p[2]*skin[2][2] + pw*skin[2][3];
            const float r4w =
                c467[2] * (p[0] + p[1] + p[2]) + pw;
            const float cx =
                r7x*camera[0][0] + r7y*camera[0][1] +
                r7z*camera[0][2] + r4w*camera[0][3];
            const float cy =
                r7x*camera[1][0] + r7y*camera[1][1] +
                r7z*camera[1][2] + r4w*camera[1][3];
            const float cw =
                r7x*camera[3][0] + r7y*camera[3][1] +
                r7z*camera[3][2] + r4w*camera[3][3];
            if (cx != cx || cy != cy || cw != cw || cw == 0.0f)
                continue;
            finite++;
            if (cw > 0.0f) in_front++;
            if (cw < w_min) w_min = cw;
            if (cw > w_max) w_max = cw;
            const float sx = (cx / cw) * vp.scale[0] + vp.translate[0];
            const float sy = (cy / cw) * vp.scale[1] + vp.translate[1];
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
            if (cw > 0.0f && sx >= 0.0f && sx < (float)vp.w &&
                sy >= 0.0f && sy < (float)vp.h)
                in_view++;
        }
        fprintf(stderr,
                "[a010-clip] frame=%u finite=%u/%u front=%u view=%u "
                "screen=[%.3f %.3f]-[%.3f %.3f] w=[%.6g %.6g] "
                "viewport=%ux%u\n",
                g_ld_frames, finite, dc.n_verts, in_front, in_view,
                sx_min, sy_min, sx_max, sy_max, w_min, w_max,
                vp.w, vp.h);
        if (InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            static LONG acceptance_written = 0;
            if (InterlockedCompareExchange(
                    &acceptance_written, 1, 0) == 0) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "reference-mesh frame=%u finite=%u/%u "
                            "front=%u view=%u screen=%.3f,%.3f,"
                            "%.3f,%.3f viewport=%u,%u\n",
                            g_ld_frames, finite, dc.n_verts,
                            in_front, in_view, sx_min, sy_min,
                            sx_max, sy_max, vp.w, vp.h);
                    fclose(acceptance);
                }
            }
        }
    }
    fflush(stderr);
}

typedef enum {
    LD_COMPACT_EXPAND_OK = 1,
    LD_COMPACT_EXPAND_DEGENERATE = 0,
    LD_COMPACT_EXPAND_ALLOC = -1,
    LD_COMPACT_EXPAND_PRIMITIVE = -2
} ld_compact_expand_result;

static void compact_vertex_copy(
    u8* destination, u32 destination_vertex, u32 source_vertex)
{
    if (!dc.layout.stride)
        return;
    memcpy(
        destination + (u64)destination_vertex * dc.layout.stride,
        dc.compact_verts + (u64)source_vertex * dc.layout.stride,
        dc.layout.stride);
}

static ld_compact_expand_result expand_compact_topology(
    u32 prim, u8** expanded, u32* expanded_vertices, int* owned)
{
    *expanded = dc.compact_verts;
    *expanded_vertices = 0;
    *owned = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES:
        *expanded_vertices = dc.n_verts - dc.n_verts % 3;
        break;
    case PRIM_TRIANGLE_STRIP: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                if (i & 1) {
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                } else {
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                }
            }
        }
        break;
    }
    case PRIM_TRIANGLE_FAN: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                compact_vertex_copy(*expanded, write++, begin);
                compact_vertex_copy(*expanded, write++, begin + i);
                compact_vertex_copy(*expanded, write++, begin + i + 1);
            }
        }
        break;
    }
    case PRIM_QUADS: {
        const u32 quads = dc.n_verts / 4;
        if (!quads)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = quads * 6;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc(
            (size_t)*expanded_vertices * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 quad = 0; quad < quads; quad++) {
            const u32 base = quad * 4;
            compact_vertex_copy(*expanded, write++, base);
            compact_vertex_copy(*expanded, write++, base + 1);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 3);
            compact_vertex_copy(*expanded, write++, base);
        }
        break;
    }
    default:
        return LD_COMPACT_EXPAND_PRIMITIVE;
    }
    return *expanded_vertices
        ? LD_COMPACT_EXPAND_OK : LD_COMPACT_EXPAND_DEGENERATE;
}

static u32 topology_index_count(u32 prim)
{
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_LINES: {
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_source_refs,
                                       segment, &begin, &count);
            total += count - count % 2u;
        }
        return total;
    }
    case PRIM_TRIANGLES:
        return dc.n_source_refs - dc.n_source_refs % 3u;
    case PRIM_TRIANGLE_STRIP:
    case PRIM_TRIANGLE_FAN: {
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case PRIM_QUADS:
        return (dc.n_source_refs / 4) * 6;
    default:
        return 0;
    }
}

static u32 topology_vertex_index(u32 occurrence)
{
    return dc.refs_remapped
        ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
        : occurrence;
}

static void write_topology_indices(u32 prim, u32* indices)
{
    u32 write = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_LINES:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(dc.cuts, dc.n_cuts, dc.n_source_refs,
                                       segment, &begin, &count);
            count -= count % 2u;
            for (u32 i = 0; i < count; i++)
                indices[write++] = topology_vertex_index(begin + i);
        }
        break;
    case PRIM_TRIANGLES: {
        const u32 count =
            dc.n_source_refs - dc.n_source_refs % 3u;
        for (u32 occurrence = 0; occurrence < count; occurrence++)
            indices[write++] = topology_vertex_index(occurrence);
        break;
    }
    case PRIM_TRIANGLE_STRIP:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = topology_vertex_index(
                    begin + i + (i & 1u));
                indices[write++] = topology_vertex_index(
                    begin + i + 1u - (i & 1u));
                indices[write++] = topology_vertex_index(begin + i + 2u);
            }
        }
        break;
    case PRIM_TRIANGLE_FAN:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = topology_vertex_index(begin);
                indices[write++] = topology_vertex_index(begin + i);
                indices[write++] = topology_vertex_index(begin + i + 1u);
            }
        }
        break;
    case PRIM_QUADS:
        for (u32 quad = 0; quad < dc.n_source_refs / 4; quad++) {
            const u32 base = quad * 4;
            indices[write++] = topology_vertex_index(base);
            indices[write++] = topology_vertex_index(base + 1u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 3u);
            indices[write++] = topology_vertex_index(base);
        }
        break;
    }
}

typedef struct { u32 primitive, reason; u64 count; } ld_primitive_reject_record;
static ld_primitive_reject_record g_ld_primitive_rejects[32];
static u32 g_ld_primitive_reject_used, g_ld_primitive_reject_repeat_lines;
static void ld_primitive_reject_trace(u32 primitive, u32 reason)
{
    static int enabled = -1;
    if (enabled < 0) { const char* e = getenv("LD_PRIMITIVE_REJECT_TRACE"); enabled = e && atoi(e) != 0; }
    if (!enabled) return;
    for (u32 i = 0; i < g_ld_primitive_reject_used; i++) {
        ld_primitive_reject_record* entry = &g_ld_primitive_rejects[i];
        if (entry->primitive != primitive || entry->reason != reason) continue;
        entry->count++;
        if (!(entry->count & (entry->count - 1)) && g_ld_primitive_reject_repeat_lines < 128) {
            g_ld_primitive_reject_repeat_lines++;
            fprintf(stderr, "[primitive-reject-count] frame=%u primitive=%u reason=%u count=%llu\n",
                    g_ld_frames, primitive, reason, (unsigned long long)entry->count);
        }
        return;
    }
    if (g_ld_primitive_reject_used >= 32) return;
    ld_primitive_reject_record* entry = &g_ld_primitive_rejects[g_ld_primitive_reject_used++];
    entry->primitive = primitive; entry->reason = reason; entry->count = 1;
    rsx_dsp_surface sf; rsx_dsp_get_surface(&g.rsx, &sf);
    u32 fp_location;
    const u32 fp_offset = rsx_dsp_fragment_program(&g.rsx, &fp_location);
    const u8* fp_head = guest_ptr(fp_location, fp_offset, 16);
    fprintf(stderr, "[primitive-reject] frame=%u raw_primitive=%u primitive=%u reason=%u(%s) "
                    "packets=%u fetched=%u source_occurrences=%u source_refs=%u remapped=%d cuts=%u "
                    "target=%u:0x%08X size=%ux%u color_target=0x%X vp_store_hash=%016llx "
                    "fp=%u:0x%08X fp_head16_hash=%016llx fp_ctrl=%08X indices=",
            g_ld_frames, g.rsx.current_primitive, primitive, reason,
            reason == 1 ? "unsupported_topology" : reason == 2 ? "insufficient_vertices" : "restart_segments_or_degenerate",
            dc.n_packets, dc.n_verts, dc.n_source_refs, dc.n_refs, dc.refs_remapped, dc.n_cuts,
            sf.color_location[0], sf.color_offset[0], sf.clip_w, sf.clip_h, sf.color_target,
            (unsigned long long)fnv1a(g.rsx.vp, sizeof g.rsx.vp, 1469598103934665603ull),
            fp_location, fp_offset,
            (unsigned long long)(fp_head ? fnv1a(fp_head, 16, 1469598103934665603ull) : 0),
            rsx_dsp_shader_control(&g.rsx));
    for (u32 occurrence = 0; occurrence < dc.n_source_refs && occurrence < 8; occurrence++) {
        const u32 index = dc.refs_remapped ? rsx_vertex_remap_index(&dc.ref_remap, occurrence) : occurrence;
        if (dc.refs && index < dc.n_refs)
            fprintf(stderr, "%s%u(base=%u)", occurrence ? "," : "", dc.refs[index].vertex_id, dc.refs[index].base_index);
    }
    fprintf(stderr, " batches_idx=%u first=%u count=%u batches_array=%u\n", dc.n_idx,
            dc.n_idx ? dc.idx[0].first : 0, dc.n_idx ? dc.idx[0].count : 0, dc.n_arr);
}
static void ld_primitive_reject_summary(void)
{
    for (u32 i = 0; i < g_ld_primitive_reject_used; i++)
        fprintf(stderr, "[primitive-reject-total] primitive=%u reason=%u count=%llu\n",
                g_ld_primitive_rejects[i].primitive, g_ld_primitive_rejects[i].reason,
                (unsigned long long)g_ld_primitive_rejects[i].count);
}
static void sink_end_impl(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    if (!dc.n_packets) { g_ld_stats.groups_empty++; return; }
    /* skip_pso.txt (working directory, re-read once a second): hex PSO keys
     * (bind-trace pso=) whose draws are dropped -- isolates which draws
     * produce an artefact. Diagnostic only; absent file = no cost beyond the
     * once-a-second check. */
    { static u64 keys[64]; static u32 n_keys; static ULONGLONG chk;
      const ULONGLONG now = GetTickCount64();
      if (now - chk > 1000) {
          chk = now; n_keys = 0;
          FILE* f = fopen("skip_pso.txt", "r");
          if (f) { unsigned long long k; while (n_keys < 64 && fscanf(f, "%llx", &k) == 1) keys[n_keys++] = k; fclose(f); }
      }
      if (n_keys) {
          const u64 key = live_legacy_pso_key();
          for (u32 i = 0; i < n_keys; i++) if (keys[i] == key) return;
      } }
    /* skip_tex.txt: hex texture offsets (bind-trace u0[0:0x...]); a draw with
     * any enabled unit at one of them is dropped. Stable across frames, unlike
     * PSO keys. Diagnostic only. */
    { static u32 offs[64]; static u32 n_offs; static ULONGLONG chk;
      const ULONGLONG now = GetTickCount64();
      if (now - chk > 1000) {
          chk = now; n_offs = 0;
          FILE* f = fopen("skip_tex.txt", "r");
          if (f) { unsigned k; while (n_offs < 64 && fscanf(f, "%x", &k) == 1) offs[n_offs++] = k; fclose(f); }
      }
      if (n_offs)
          for (u32 u = 0; u < 16; u++) {
              rsx_dsp_texture tt; rsx_dsp_get_texture(&g.rsx, u, &tt);
              if (!tt.enabled) continue;
              for (u32 i = 0; i < n_offs; i++) if (offs[i] == tt.offset) return;
          } }
    g_ld_stats.groups_seen++;
    /*
     * a010's native per-mesh constant builder can overwrite the otherwise
     * valid reconstructed camera with NaNs after the ordinary camera upload.
     * Repair at the last measured boundary, immediately before the draw
     * consumes the constants. Once the synchronized a010 reference is armed,
     * replace finite-but-wrong native uploads too: the measured failure
     * matrix is finite, yet projects the model thousands of pixels offscreen.
     */
    if (InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0) != 0) {
        int camera_has_nan = 0;
        for (u32 slot = 108; slot <= 111 && !camera_has_nan; slot++)
            for (u32 lane = 0; lane < 4; lane++) {
                const u32 bits = g.rsx.constants[slot][lane];
                if ((bits & 0x7F800000u) == 0x7F800000u &&
                    (bits & 0x007FFFFFu) != 0) {
                    camera_has_nan = 1;
                    break;
                }
            }
        if (camera_has_nan || g_yz_runtime_config.a010_start_reference ||
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            for (u32 i = 0; i < 16; i++)
                g.rsx.constants[108 + i / 4][i % 4] =
                    g_ld_a010_camera_bits[i];
            static u64 repairs = 0;
            repairs++;
            if (repairs <= 16 || (repairs & (repairs - 1)) == 0) {
                fprintf(stderr,
                        "[a010-rsx-camera-repair] n=%llu frame=%u "
                        "c108=%08X/%08X/%08X/%08X\n",
                        (unsigned long long)repairs, g_ld_frames,
                        g.rsx.constants[108][0], g.rsx.constants[108][1],
                        g.rsx.constants[108][2], g.rsx.constants[108][3]);
                fflush(stderr);
            }
        }
    }
    /* A polygon is a convex fan around its first vertex -- what the register-
     * file engine (rsx_draw_engine.c) and the null backend already make of one.
     * Left unmapped it was dropped outright: Twisted Metal's legal screens
     * submit 16-vertex polygons. */
    const u32 prim = g.rsx.current_primitive == 10u /* POLYGON */
        ? PRIM_TRIANGLE_FAN : g.rsx.current_primitive;
    rsx_vertex_layout_plan used_layout;
    ld_current_vertex_layout(&used_layout);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.used_attribute_draws++;
    g_ld_profile.total.used_attribute_sum += used_layout.count;
    if (used_layout.count > g_ld_profile.frame_used_attribute_max)
        g_ld_profile.frame_used_attribute_max = used_layout.count;
    if (used_layout.count > g_ld_profile.total_used_attribute_max)
        g_ld_profile.total_used_attribute_max = used_layout.count;
    const LONGLONG fetch_pack_begin = ld_profile_qpc();
#endif
    if (ld_vertex_compact_payload()) {
        fetch_batches_hoisted(&used_layout, 1);
    } else if (ld_vertex_hoist_fetch()) {
        rsx_vertex_layout_plan legacy_layout;
        ld_layout_plan_get(0xFFFFu, &legacy_layout);
        fetch_batches_hoisted(&legacy_layout, 0);
    } else {
        fetch_batches();
    }
    if (!dc.n_source_refs)
        dc.n_source_refs = dc.n_verts;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vertex_fetch_pack_qpc +=
        (u64)(ld_profile_qpc() - fetch_pack_begin);
#endif
    if (!dc.n_verts || !dc.fetch_ok) { g_ld_stats.group_drop_fetch++; return; }

    /* Preserve validated occurrence order while allowing repeated compact
     * vertex references to share one fetched and uploaded payload. */
    int indexed = 0;
    if (prim == PRIM_LINES)
        indexed = 1; /* pair boundaries/restarts and compact remaps preserved */
    else if (!rsx_vertex_topology_plan(prim, dc.refs_remapped, &indexed)) {
        ld_primitive_reject_trace(prim, 1);
        g_ld_stats.group_drop_primitive++;
        return;
    }
    u32 n_tri = indexed
        ? topology_index_count(prim)
        : dc.n_source_refs - dc.n_source_refs % 3;

    if (!n_tri) {
        const u32 minimum = prim == PRIM_LINES ? 2u : prim == PRIM_QUADS ? 4u : 3u;
        ld_primitive_reject_trace(prim, dc.n_source_refs < minimum ? 2u : 3u);
        g_ld_stats.group_drop_degenerate++;
        return;
    }

    live_a010_geom_trace(prim, n_tri);

    const u32 draw_pair_target = ld_draw_pair_begin(prim, n_tri);

    ID3D12PipelineState* pso = NULL;
    if(dc.raw28){
        pso=get_pso(&used_layout,1,prim,1);
        if(!pso){
            g_ld_raw28_shader_failed=1;
            fprintf(stderr,"[vertex-raw28] shader_or_pso_failed fallback=snapshotted_float4 disabled_until_restart=1\n");
            if(!ld_raw28_restore_float(&used_layout))return;
        }
    }
    if(dc.rawn){
        g_ld_rawn_cur=&dc.rawn_layout;
        pso=get_pso(&used_layout,1,prim,2);
        g_ld_rawn_cur=NULL;
        if(!pso){
            static int logged;
            if(logged++<8)fprintf(stderr,"[vertex-rawn] shader_or_pso_failed sig=%016llX fallback=float4\n",(unsigned long long)dc.rawn_layout.sig);
            if(!ld_rawn_restore_float(&used_layout))return;
        }
    }

    const u32 vertex_stride =
        dc.raw28 ? 28u : dc.rawn ? dc.rawn_layout.stride :
        ld_vertex_compact_payload() ? used_layout.stride : VERT_STRIDE;
    const u32 uploaded_vertices = indexed ? dc.n_verts : n_tri;
    const void* vertex_data = ld_vertex_compact_payload()
        ? (const void*)dc.compact_verts : (const void*)dc.verts;
    const u64 draw_vb_bytes = (u64)uploaded_vertices * vertex_stride;
    const u64 draw_ib_bytes = indexed ? (u64)n_tri * sizeof(u32) : 0;
    const u64 vb_capacity = VERT_BUFFER_SIZE;
    if (draw_vb_bytes > vb_capacity || draw_ib_bytes > INDEX_BUFFER_SIZE) {
        live_draw_csv_emit(prim, n_tri, "drop_vbring_oversize");
        g_ld_stats.group_drop_ring++;
        return;
    }
    if ((u64)g.vb_used + draw_vb_bytes > vb_capacity ||
        (u64)g.ib_used + draw_ib_bytes > INDEX_BUFFER_SIZE) {
        /*
         * The replay-proven renderer recycles this upload ring mid-frame.
         * Live used to drop every remaining draw instead, which discarded
         * 181/184 sampled a010 groups after the orphanage workload filled the
         * 256K-vertex ring.  We are still before this draw's memcpy, so it is
         * safe to submit/wait and reuse every transient draw ring here.
         */
        ld_flush(LD_FLUSH_VERTEX_RING);
        g.vb_used = 0;
        g.ib_used = 0;
        g.cb_used = 0;
        g.ps_cb_used = 0;
        g.srv_ring_used = 0;
        g.smp_ring_used = 0;
    }
    if (dc.vcache && (!dc.rawn || uploaded_vertices != dc.n_refs)) {
        /* Not drawn straight from the entry: copy its bytes as scratch. */
        if (reserve_compact_vertices((u64)dc.n_refs * dc.rawn_layout.stride))
            memcpy(dc.compact_verts, dc.vcache->shadow, (size_t)dc.n_refs * dc.rawn_layout.stride);
        dc.vcache = NULL;
        vertex_data = dc.compact_verts;
    }
    if (draw_vb_bytes && !dc.vcache)
        memcpy(
            g.vb_mapped + g.vb_used, vertex_data,
            (size_t)draw_vb_bytes);
    if (dc.icache && (!indexed || dc.icache->n_out != n_tri)) {
        static int logged;   /* not expected: the primitive is part of the key */
        if (logged++ < 8) fprintf(stderr, "[icache] index count mismatch %u/%u: draw dropped\n", dc.icache->n_out, n_tri);
        return;
    }
    if (indexed && !dc.icache) {
        u32* dst = NULL;
        if (dc.icache_key && dc.refs_remapped)
            dst = ld_icache_store(dc.icache_key, dc.refs[0].vertex_id, dc.n_refs, dc.n_source_refs, n_tri, &dc.icache);
        if (dst) g_icache_misses++;
        write_topology_indices(prim, dst ? dst : (u32*)((u8*)g.ib_mapped + g.ib_used));
    }

    /* OFF retains the original PSO lookup/creation order after ring writes. */
    if(!pso)pso=get_pso(
        ld_vertex_mask_signature() ? &used_layout : NULL,
        ld_vertex_compact_payload(),prim,0);
    if(!pso){
#if defined(YZ_PERF_PROFILE)
        ld_profile_note_rejected_pso();
#endif
        live_draw_csv_emit(prim,n_tri,"drop_pso");
        g_ld_stats.group_drop_pso++;return;
    }

    u32 vertex_cb_offset = 0;
    int vertex_cb_plan = rsx_vertex_constant_ring_plan(
        g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED, &vertex_cb_offset);
    if (vertex_cb_plan == 0) {
#if !defined(YZ_PERF_CLEAN)
        g_ld_vertex_constant_ring_recycles++;
        g_ld_frame_vertex_constant_ring_recycles++;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_CB_RECYCLE, g_ld_frames, g_ld_last_present_target,
            g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            (u32)g_ld_stats.groups_executed,
            g_ld_fifo_get, g_ld_fifo_put);
#endif
        ld_flush(LD_FLUSH_VERTEX_CONSTANT_RING);
        if (!g.ready)
            return;
        g.cb_used = 0;
        vertex_cb_plan = rsx_vertex_constant_ring_plan(
            g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            &vertex_cb_offset);
    }
    if (vertex_cb_plan != 1) {
        live_draw_csv_emit(prim, n_tri, "drop_cbring_invalid");
        g_ld_stats.group_drop_ring++;
        return;
    }
    g.cb_used = vertex_cb_offset;
    u32 ps_cb_offset = 0;
    if (!ld_upload_pixel_constants(&ps_cb_offset)) {
        live_draw_csv_emit(prim, n_tri, "drop_ps_cbring");
        g_ld_stats.group_drop_ring++;
        return;
    }

    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        live_draw_csv_emit(prim, n_tri, "drop_surface");
        g_ld_stats.group_drop_surface++;
        return;
    }
    /* LD_SKIP_SAMPLING=<hex offset>: drop draws that sample a texture at that
     * offset. Debug only -- removing a post-process pass shows whether the
     * scene under it has any colour at all. */
    { static int64_t skip = -2;
      if (skip == -2) { const char* e = getenv("LD_SKIP_SAMPLING"); skip = e ? (int64_t)strtoul(e, 0, 16) : -1; }
      if (skip >= 0)
          for (u32 u = 0; u < 16; u++) {
              rsx_dsp_texture st; rsx_dsp_get_texture(&g.rsx, u, &st);
              if (st.enabled && st.offset == (u32)skip) return;
          } }
    /* LD_TRACE_PSO=<hex key>: raw vertex attributes + unit-0 texels of the
     * first three draws with that CSV pso_key. */
    { static u64 tk = 1; static int tn;
      if (tk == 1) { const char* e = getenv("LD_TRACE_PSO"); tk = e ? _strtoui64(e, 0, 16) : 0; }
      if (tk && live_legacy_pso_key() == tk && tn >= 3) {
          static u32 every;
          if (dc.inl && (++every % 60) == 0)
              fprintf(stderr, "[pso-trace] frame=%u col=%02X%02X%02X%02X\n", g_ld_frames,
                      dc.inl[16], dc.inl[17], dc.inl[18], dc.inl[19]);
      }
      /* LD_TRACE_PSO_FROM=<frame>: the first three draws at or after that
       * frame -- a PSO's first uses can predate the data it later draws. */
      static long tfrom = -1;
      if (tfrom < 0) { const char* e = getenv("LD_TRACE_PSO_FROM"); tfrom = e ? atol(e) : 0; }
      if (tk && tn < 3 && live_legacy_pso_key() == tk && (long)g_ld_frames >= tfrom) {
          tn++;
          if (dc.inl) {
              fprintf(stderr, "[pso-trace] inline stride=%u bytes=%u:", dc.inl_stride, dc.inl_bytes);
              for (u32 k = 0; k < dc.inl_bytes && k < 6 * dc.inl_stride; k++)
                  fprintf(stderr, "%s%02X", (k % dc.inl_stride) ? "" : " |", dc.inl[k]);
              fprintf(stderr, "\n");
          }
          for (u32 ai = 0; ai < 16; ai++) {
              rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
              if (!va.type) continue;
              fprintf(stderr, "[pso-trace] attr%u type=%u size=%u stride=%u loc=%u off=0x%X:",
                      ai, va.type, va.size, va.stride, va.location, va.offset);
              for (u32 vi = 0; vi < 6; vi++) {
                  const u8* p = guest_ptr(va.location, va.offset + vi * va.stride, 16);
                  if (!p) break;
                  fprintf(stderr, " |");
                  for (u32 k = 0; k < (va.type == RSX_VTX_TYPE_FLOAT ? 4u * va.size : 4u) && k < 16; k++)
                      fprintf(stderr, "%02X", p[k]);
              }
              fprintf(stderr, "\n");
          }
          /* The raw array registers and base (the attrN lines above print the
           * decoded offset only), and the constant colour a disabled attr3
           * feeds -- the two things a draw that vanishes is usually missing. */
          fprintf(stderr, "[pso-trace] vdata base_offset=0x%X base_index=%u raw attr0 off=0x%08X fmt=0x%08X\n",
                  rsx_dsp_vertex_data_base_offset(&g.rsx), rsx_dsp_vertex_data_base_index(&g.rsx),
                  rsx_dsp_reg(&g.rsx, 0x1680), rsx_dsp_reg(&g.rsx, 0x1740));
          { float d3[4]; rsx_dsp_vertex_default(&g.rsx, 3, d3);
            fprintf(stderr, "[pso-trace] attr3 const=%g %g %g %g\n", d3[0], d3[1], d3[2], d3[3]); }
          /* Every non-zero transform constant (raw words), so the VS can be
           * evaluated offline against exactly what this draw saw. */
          for (u32 q = 0; q < 512; q++) {
              const u32* w = g.rsx.constants[q];
              if (!(w[0] | w[1] | w[2] | w[3])) continue;
              fprintf(stderr, "[pso-const] %u %08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
          }
          for (u32 ai = 0; ai < 16; ai++) {   /* the constant value each attribute falls back to */
              float d[4]; rsx_dsp_vertex_default(&g.rsx, ai, d);
              fprintf(stderr, "[pso-vdef] %u %g %g %g %g\n", ai, d[0], d[1], d[2], d[3]);
          }
          { u32 fl = 0; const u32 fo = rsx_dsp_fragment_program(&g.rsx, &fl);
            const u8* fp = guest_ptr(fl, fo, 16 * 16);
            if (fp) { fprintf(stderr, "[pso-trace] fp %u:0x%X:", fl, fo);
                      for (u32 k = 0; k < 16 * 16; k++) fprintf(stderr, "%s%02X", (k & 15) ? "" : " ", fp[k]);
                      fprintf(stderr, "\n"); } }
          rsx_dsp_texture t0; rsx_dsp_get_texture(&g.rsx, 0, &t0);
          const u8* tp = guest_ptr(t0.location, t0.offset, 32);
          if (tp) { { rsx_dsp_texture t1; rsx_dsp_get_texture(&g.rsx, 1, &t1); fprintf(stderr, "[pso-trace] outmask=0x%X inmask=0x%X tex0 fmt=0x%X remap=0x%X wrap=0x%X border=0x%08X filter=0x%X tex1 remap=0x%X wrap=0x%X:", rsx_dsp_reg(&g.rsx, 0x1FF4), rsx_dsp_reg(&g.rsx, 0x1FF0), t0.format, t0.remap, t0.wrap, rsx_dsp_reg(&g.rsx, 0x1A1C), t0.filter, t1.remap, t1.wrap); }
                    for (u32 k = 0; k < 32; k++) fprintf(stderr, "%02X", tp[k]);
                    fprintf(stderr, "\n"); }
      } }
    /* LD_FORCE_VA=1: debug -- inline draws whose UB4 vertex colour (attr 3)
     * has alpha 0 everywhere get alpha 255, to see what a faded-out element is. */
    { static int fva = -1; if (fva < 0) fva = getenv("LD_FORCE_VA") ? 1 : 0;
      if (fva && dc.inl && dc.inl_stride) {
          rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
          if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
              u32 amax = 0;
              for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                  if (dc.inl[o] > amax) amax = dc.inl[o];
              if (!amax)
                  for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                      ((u8*)dc.inl)[o] = 255;
          } } }
    /* LD_SKIP_PSO=<hex key>: drop draws with that CSV pso_key. Debug only. */
    { static u64 skipk = 1;
      if (skipk == 1) { const char* e = getenv("LD_SKIP_PSO"); skipk = e ? _strtoui64(e, 0, 16) : 0; }
      if (skipk && live_legacy_pso_key() == skipk) return; }
    /* MRT: colour targets B..D, resolved (and created) here, before any of
     * this draw's commands are recorded. They share A's format and clip. */
    u32 mrt[4] = { target, 0, 0, 0 };
    u32 n_mrt = 1;
    {
        rsx_dsp_surface msf;
        rsx_dsp_get_surface(&g.rsx, &msf);
        const u32 want = ld_rt_count(msf.color_target);
        for (u32 r = 1; r < want; r++) {
            const u32 s = surface_get(msf.color_location[r], msf.color_offset[r],
                                      msf.clip_w, msf.clip_h,
                                      ld_surface_format(msf.color_format));
            if (s == LD_INVALID_SURFACE || s == target) break;
            g.surfaces[s].pitch = msf.color_pitch[r];
            g.surfaces[s].raster_type = msf.raster_type;
            mrt[n_mrt++] = s;
        }
    }
    live_draw_csv_emit(prim, n_tri, "execute");
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    { static u32 last_target = LD_INVALID_SURFACE;
      if (target != last_target) { ld_trace_target("draw", target, 0); last_target = target; } }
    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 current_zslot = g.depth
        ? zdepth_get(sf.zeta_location, sf.zeta_offset, sf.clip_w, sf.clip_h,
                     (rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) &&
                     (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u))
        : 0;
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_TEXTURE);
    u32 slots[SRV_TABLE_SIZE], smp_slots[SMP_TABLE_SIZE];
    u32 surf_used[SRV_TABLE_SIZE], n_surf_used = 0;
    u64 owned_cube_pinned = 0;
    u32 zdepth_used[SRV_TABLE_SIZE], n_zdepth_used = 0;
    u32 texture_mask = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++) smp_slots[u] = SMP_DEFAULT;
    /* A draw that samples its own zeta while not writing depth (deferred
     * lighting reading the scene depth it also depth-tests against, soft
     * particles) reads the snapshot: the zeta does not change during such a
     * draw, so that is what RSX returns. Before, it fell back to guest VRAM,
     * which the host-side depth never reaches (depth 0). A draw that writes
     * the zeta it reads keeps that fallback. */
    const int self_zeta_read = current_zslot &&
        !((rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) &&
          (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u));
    /* LD_UNITS=1: every enabled texture unit of every draw that samples PS1
     * VRAM, once per distinct combination.
     *
     * ps1_netemu's display composite is a Cg program picked from seven
     * (CG_fp_gradient / orientation / mofix / smart / upscale / upscale_smart /
     * sharpen) and its uniforms are texture0, texture1 and hwidth -- TWO
     * textures. Every probe here so far has looked at unit 0 only, so "the
     * renderer reads the right bytes" was never actually tested for the second
     * one. This lists them. */
    { static int lu = -1; static u32 seen[8]; static int ns;
      if (lu < 0) lu = getenv("LD_UNITS") ? 1 : 0;
      if (lu) {
          u32 key = 0; int ps1 = 0;
          for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
              rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
              if (!tq.enabled) continue;
              key = key * 31u + (tq.offset ^ (tq.format << 8) ^ (q << 24));
              if (tq.location == 1u && tq.offset >= 0x400000u &&
                  tq.offset < 0x480000u) ps1 = 1;
          }
          if (ps1) {
              int fresh = 1;
              for (int z = 0; z < ns; z++) if (seen[z] == key) fresh = 0;
              if (fresh && ns < 8) {
                  seen[ns++] = key;
                  fprintf(stderr, "[units] draw sampling PS1 VRAM:");
                  for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
                      rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
                      if (!tq.enabled) continue;
                      fprintf(stderr, " u%u[%u:0x%08X fmt=0x%02X %ux%u p=%u]",
                              q, tq.location, tq.offset, tq.format,
                              tq.width, tq.height, tq.pitch);
                  }
                  fprintf(stderr, "\n");
              }
          }
      } }
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        /* LD_PS1_BUF0=1 -- DIAGNOSTIC, not a fix. Force a 24-bit PS1 VRAM
         * bind at buffer 1 (offset 0x4003C0) to sample buffer 0 (0x400000).
         *
         * After the intro FMV both the driven and undriven paths end on a blank
         * screen, and during it every bind is 0x4003C0 while the content that
         * exists -- the decoded FMV frame -- sits in buffer 0. That is the shape
         * of a display flip parked on the buffer the game is not drawing into.
         * If forcing buffer 0 puts a picture back on screen the flip is the bug;
         * if the screen stays blank it is not. Nothing else distinguishes those
         * two, and the override is one line. */
        { static int fb0 = -1;
          if (fb0 < 0) fb0 = getenv("LD_PS1_BUF0") ? 1 : 0;
          if (fb0 && t.location == 1u && t.offset == 0x4003C0u)
              t.offset = 0x400000u; }
        if (!t.enabled) continue;
        texture_mask |= 1u << u;
        if (ld_is_depth_tex(&t)) {
            static u64 dseen[16]; static int dn;
            const u64 dk = ((u64)t.wrap << 32) ^ t.border_color ^ ((u64)u << 28);
            int fresh = dn < 16;
            for (int z = 0; z < dn; z++) if (dseen[z] == dk) fresh = 0;
            if (fresh) {
                dseen[dn++] = dk;
                fprintf(stderr, "[depth-tex] unit=%u fmt=0x%02X %ux%u wrap=0x%08X border=0x%08X\n",
                        u, t.format, t.width, t.height, t.wrap, t.border_color);
            }
        }
        smp_slots[u] = sampler_slot(&t, sampler_key(&t));
        if (t.cubemap) {
            u32 cube_slot = 0;
            const int cube_srv = owned_cube_srv(&t, owned_cube_pinned, &cube_slot);
            if (cube_srv >= 0) {
                owned_cube_pinned |= 1ull << cube_slot;
                slots[u] = (u32)cube_srv;
                g_ld_bind_surf++;
                continue;
            }
            if (cube_srv == -2) {
                g_ld_stats.group_drop_surface++;
                return;
            }
            /* No GPU-owned faces: use the existing six-face guest upload,
             * never a 2D colour alias for a TextureCube shader slot. */
            slots[u] = texture_srv_slot(&t);
            if (slots[u] == SRV_WHITE) { g_ld_stats.group_drop_surface++; return; }
            g_ld_bind_real++;
            continue;
        }
        /* Pick WHICH surface at this offset to sample.
         *
         * Matching on (location, offset) alone was fine when only one surface
         * could exist per offset. Now that surfaces are keyed on size too --
         * they have to be, or the two passes destroy each other's contents --
         * several can, and taking the first match by offset takes an arbitrary
         * one. ps1_netemu composites the PS1 framebuffer into a 720x512 surface
         * at offset 0x0 and then upscales into a 1280x720 output; the upscale
         * pass samples offset 0x0 and was getting the EMPTY 1280x720 surface
         * that happens to sit earlier in the table.
         *
         * Prefer the surface whose dimensions match what the texture
         * descriptor actually declares; failing that, the most recently drawn
         * one, which is the only other defensible answer. */
        int sampled = -1;
        /* An address can be both a colour target and a zeta (a shadow map
         * pass that binds the same memory as both, colour writes masked).
         * A depth-format read of an address with a written host zeta is a
         * depth read: try the zeta first. Before, the colour surface at the
         * same address was taken (zeros). */
        int zeta_pref = 0;
        const u32 zview_format = t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
        if (zview_format == TEX_FMT_DEPTH24_D8 || zview_format == TEX_FMT_A8R8G8B8)
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset &&
                    (g.zdepths[i].had_write || g.zdepths[i].snapshot_valid)) {
                    zeta_pref = 1;
                    break;
                }
        if (!zeta_pref) { u32 best_gen = 0;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              const surface_t* sf_i = &g.surfaces[i];
              if (sf_i->location != t.location || sf_i->offset != t.offset ||
                  i == target)
                  continue;
              if (sf_i->w == t.width && sf_i->h == t.height) { sampled = (int)i; break; }
              if (sampled < 0 || sf_i->last_draw_generation > best_gen) {
                  sampled = (int)i;
                  best_gen = sf_i->last_draw_generation;
              }
          } }
        /* LD_UVDBG=1: the texture coordinates of the draw that samples the PS1
         * framebuffer. Placed here rather than in fetch_batches because that
         * function is not on this draw's path -- probing it printed nothing at
         * all, which is how that was established.
         *
         * This is the last unmeasured link: the PS3 side binds all 1024x512 of
         * PS1 VRAM as ONE texture, so which part reaches the screen is decided
         * entirely by these UVs, and every pixel the PS1 draws lives at
         * x >= 640 while columns 0..639 are black. */
        { static int uvd = -1;
          if (uvd < 0) uvd = getenv("LD_UVDBG") ? 1 : 0;
          if (uvd && t.location == 1u && t.offset == 0x400000u) {
              static unsigned long n = 0;
              if (PS3_LOG_FIRST(n, 4)) {
                  const u32 vbase = rsx_dsp_vertex_data_base_offset(&g.rsx);
                  fprintf(stderr, "[uvdbg] unit=%u ps1fb %ux%u pitch=%u fmt=0x%02X"
                                  " base=0x%X target=%u\n",
                          u, t.width, t.height, t.pitch, t.format,
                          vbase, target);
                  /* The GPU state that can silently discard a draw. Clears are
                   * visible on these surfaces and the draws are not, so one of
                   * these is throwing the fragments away. */
                  { const u32 sch = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
                    const u32 scv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
                    fprintf(stderr, "[uvdbg]   vp=(%u,%u %ux%u) scale=(%.2f %.2f)"
                                    " xlate=(%.2f %.2f) scissor=(%u+%u, %u+%u)"
                                    " depth_test=%u zwrite=%u zfunc=0x%X"
                                    " colmask=0x%08X blend=%u cull=%u/%u"
                                    " surf=%ux%u\n",
                            vp.x, vp.y, vp.w, vp.h,
                            vp.scale[0], vp.scale[1],
                            vp.translate[0], vp.translate[1],
                            sch & 0xFFFFu, sch >> 16, scv & 0xFFFFu, scv >> 16,
                            rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A78) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A7C),
                            rsx_dsp_reg(&g.rsx, M_COLOR_MASK),
                            rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x1918) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x090C),
                            sf.clip_w, sf.clip_h);
                    /* The PS1 framebuffer's fragment program unpacks the
                     * sampled value arithmetically (NV40 has no integer ops):
                     *
                     *   r0 = sample(tex0, tc0 * (1/1024, 1/512))
                     *   r0 = r0 * fp_constants[0].x + fp_constants[0].y
                     *   r0 = floor(r0) / 8.0
                     *   r0 = sign-preserving truncate
                     *   r0 = r0 * fp_constants[1].x + fp_constants[1].y
                     *
                     * So the output depends entirely on those two constants.
                     * Wrong or absent, it produces exactly the repeating stripe
                     * pattern the composite currently shows. */
                    fprintf(stderr, "[uvdbg]   fp_const count=%u mode=%c"
                                    " c0=(%.6f %.6f %.6f %.6f)"
                                    " c1=(%.6f %.6f %.6f %.6f)\n",
                            g.fp_constants.count, g.fp_constant_mode,
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][0] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][1] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][2] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][3] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][0] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][1] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][2] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][3] : 0u));
                  }
                  for (u32 vi = 0; vi < 4; vi++) {
                      float pos[4] = {0,0,0,1}, tc[4] = {0,0,0,1};
                      const int okp = fetch_attr(0, vbase, vi, 0, pos);
                      const int okt = fetch_attr(8, vbase, vi, 0, tc);
                      fprintf(stderr, "[uvdbg]   v%u pos%s(%.1f %.1f) tc%s(%.4f %.4f)\n",
                              vi, okp ? "" : "!", pos[0], pos[1],
                              okt ? "" : "!", tc[0], tc[1]);
                  }
              }
          } }
        if (sampled < 0 && LD_DIAG_ENABLED("LD_ALIAS_DBG")) {
            static u32 n_dbg = 0;
            if (PS3_LOG_FIRST(n_dbg, 24)) {
                fprintf(stderr, "[alias-miss] tex %u:0x%08X fmt=0x%02X %ux%u target=%u surf:",
                        t.location, t.offset, t.format, t.width, t.height, target);
                for (u32 i = 0; i < g.n_surfaces; i++)
                    fprintf(stderr, " [%u]%u:0x%08X %ux%u", i, g.surfaces[i].location,
                            g.surfaces[i].offset, g.surfaces[i].w, g.surfaces[i].h);
                fprintf(stderr, "\n");
            }
        }
        if (sampled < 0 && !zeta_pref && g.surfaces[target].location == t.location &&
            g.surfaces[target].offset == t.offset) {
            /* Reads its own render target: sample a copy of it. */
            slots[u] = surface_feedback_srv(target);
            g_ld_bind_surf++;
        } else if (sampled >= 0) {
            /* LD_ALIAS_DBG also reports the HIT. A texture that aliases a render
             * surface when it should not is exactly as interesting as one that
             * fails to alias when it should, and only the miss was visible --
             * so a title whose every bind resolves to a surface (binds[real=0
             * surf=N] in the frame stats) had no way to show WHY. Guitar Hero
             * III does that: every bind lands on a surface and not one real
             * texture is ever created, so the screen stays black. */
            static int alias_dbg = -1;
            if (alias_dbg < 0) alias_dbg = getenv("LD_ALIAS_DBG") != NULL;
            if (alias_dbg) {
                static u32 n_hit = 0;
                if (PS3_LOG_FIRST(n_hit, 24))
                    fprintf(stderr, "[alias-hit] tex %u:0x%08X fmt=0x%02X %ux%u "
                            "-> surface[%d] %u:0x%08X %ux%u" "\n",
                            t.location, t.offset, t.format, t.width, t.height,
                            sampled, g.surfaces[sampled].location,
                            g.surfaces[sampled].offset,
                            g.surfaces[sampled].w, g.surfaces[sampled].h);
            }
            {   /* a smaller texture reads the surface's top-left samples, texel for sample */
                const int sw = surface_window((u32)sampled, &t, alias_dbg);
                if (sw >= 0) { slots[u] = (u32)sw; g_ld_bind_surf++; continue; }
            }
            /* No sub-rect of the surface fits, and the guest wrote this memory after the
             * surface's last GPU write: the guest's texture (F031, ld_alias_guest_newer). */
            if (ld_alias_guest_newer((u32)sampled, &t)) {
                const u32 gs = texture_srv_slot(&t);
                if (gs != SRV_WHITE) { slots[u] = gs; g_ld_bind_real++; continue; }
            }
            slots[u] = SRV_SURFACE_BASE + sampled;
            g_ld_bind_surf++;
            int seen = 0;
            for (u32 k = 0; k < n_surf_used; k++) if (surf_used[k] == (u32)sampled) seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE) surf_used[n_surf_used++] = (u32)sampled;
        } else {
            int sampled_depth = -1;
            const u32 texture_base_fmt =
                t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
            /* Mirror the replay-proven depth-RT contract: address and format
             * must match, and the pass must have executed a depth-writing draw.
             * Clear-only zetas intentionally fall through to guest VRAM. */
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8 || texture_base_fmt == TEX_FMT_A8R8G8B8)
                for (u32 i = 0; i < g.n_zdepths; i++)
                    if (g.zdepths[i].location == t.location &&
                        g.zdepths[i].offset == t.offset &&
                        (current_zslot != 1 + i || self_zeta_read)) {
                        if (texture_base_fmt == TEX_FMT_A8R8G8B8 ?
                            zdepth_packed_snapshot(1 + i) : zdepth_snapshot(1 + i))
                            sampled_depth = (int)i;
                        else if (texture_base_fmt == TEX_FMT_DEPTH24_D8 && g.zdepths[i].cleared &&
                                 g.zdepths[i].clear_depth >= 1.0f) {
                            /* Cleared to the far plane and not drawn since: on the RSX
                             * this memory holds the clear value. Guest VRAM never sees
                             * our GPU depth, so reading it returned a stale image --
                             * Macross 30 showed an old scene's fighter shadows. */
                            sampled_depth = -2;
                            g_ld_zdepth_srv_reject_no_write++;
                        } else {
                            g_ld_zdepth_srv_reject_no_write++;
                            if (!g.zdepths[i].reject_logged &&
                                rsx_live_draw_a010_probe_active()) {
                                g.zdepths[i].reject_logged = 1;
                                fprintf(stderr,
                                    "[zetatrack] a010 reject #%u %u:0x%X "
                                    "draws=%llu dtest=%llu dwrite=%llu "
                                    "both=%llu snapshot=%d\n",
                                    i, g.zdepths[i].location,
                                    g.zdepths[i].offset,
                                    (unsigned long long)g.zdepths[i].draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_test_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_write_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_both_draws,
                                    g.zdepths[i].snapshot_valid);
                            }
                        }
                        break;
                    }
            if (sampled_depth == -2) {
                slots[u] = SRV_WHITE;   /* depth 1.0 everywhere */
                g_ld_bind_white++;
            } else if (sampled_depth >= 0) {
                slots[u] = (texture_base_fmt == TEX_FMT_A8R8G8B8 ?
                    SRV_ZPACK_BASE : SRV_ZDEPTH_BASE) + sampled_depth;
                g_ld_zdepth_srv_binds++;
                int seen = 0;
                for (u32 k = 0; k < n_zdepth_used; k++)
                    if (zdepth_used[k] == (u32)sampled_depth) seen = 1;
                if (!seen && n_zdepth_used < SRV_TABLE_SIZE)
                    zdepth_used[n_zdepth_used++] = (u32)sampled_depth;
            } else {
                slots[u] = texture_srv_slot(&t);
                if (slots[u] == SRV_WHITE) g_ld_bind_white++; else g_ld_bind_real++;
            }
        }
    }

    if(ld_consumer_trace_enabled())for(u32 unit=0;unit<SRV_TABLE_SIZE;unit++) {
        rsx_dsp_texture t;rsx_dsp_get_texture(&g.rsx,unit,&t);
        if(!t.enabled||!ld_consumer_trace_match(t.location,t.offset,t.pitch?(u64)t.pitch*t.height:1)||!ld_consumer_trace_take())continue;
        u32 slot=slots[unit];const char*kind="other_GPU_or_unknown";u32 owner=LD_INVALID_SURFACE;
        if(slot==SRV_WHITE)kind="white";
        else if(slot>=SRV_TEXTURE_BASE&&slot<SRV_TEXTURE_BASE+g.n_textures)kind="guest_upload";
        else if(slot>=SRV_SURFACE_BASE&&slot<SRV_SURFACE_BASE+g.n_surfaces){kind="surface";owner=slot-SRV_SURFACE_BASE;}
        else if(slot>=SRV_FEEDBACK_BASE&&slot<SRV_FEEDBACK_BASE+g.n_surfaces){kind="feedback";owner=slot-SRV_FEEDBACK_BASE;}
        else if(slot>=SRV_SURF_WIN_BASE&&slot<SRV_SURF_WIN_BASE+g.n_surfaces*LD_SWIN_PER){kind="surface_window";owner=(slot-SRV_SURF_WIN_BASE)/LD_SWIN_PER;}
        fprintf(stderr,"[resource-consumer] n=%u frame=%u event=fragment_slot_decision unit=%u guest_location=%u guest_offset=%08X guest_fmt=%02X width=%u height=%u pitch=%u remap=%08X slot=%u kind=%s target=%u pso=%016llX",s_consumer_rows,g_ld_frames,unit,t.location,t.offset,t.format,t.width,t.height,t.pitch,t.remap,slot,kind,target,(unsigned long long)live_legacy_pso_key());
        if(owner<g.n_surfaces){const surface_t*s=&g.surfaces[owner];fprintf(stderr," owner_slot=%u owner_location=%u owner_offset=%08X owner_fmt=%u",owner,s->location,s->offset,(u32)s->fmt);
#if !defined(YZ_PERF_CLEAN)
            fprintf(stderr," owner_serial=%u owner_write_generation=%u owner_write_kind=%u",s->resource_serial,s->last_write_generation,s->last_write_kind);
#endif
        }else if(slot>=SRV_TEXTURE_BASE&&slot<SRV_TEXTURE_BASE+g.n_textures){const texcache_t*e=&g.textures[slot-SRV_TEXTURE_BASE];fprintf(stderr," content_hash=%016llX last_hash_frame=%u",(unsigned long long)e->content_hash,e->last_hash_frame);}
        fprintf(stderr," timing=after_slot_selection_before_draw_no_readback\n");
    }

    /* bind_trace file in the working directory: one frame of every draw's
     * texture decisions (which guest address each unit reads, and whether it
     * resolved to a host surface, zeta, guest upload or white), deleted after.
     * Finds a pass whose render-to-texture output is never sampled back
     * (water reflection, env maps). */
    { static DWORD bt_check; static int bt_state; static u32 bt_frame, bt_rows;
      const DWORD now = GetTickCount();
      if (bt_state == 0 && now - bt_check > 500u) {
          bt_check = now;
          if (GetFileAttributesA("bind_trace") != INVALID_FILE_ATTRIBUTES) {
              DeleteFileA("bind_trace");
              bt_state = 1; bt_frame = g_ld_frames + 1; bt_rows = 0;
              fprintf(stderr, "[bind-trace] armed for frame %u\n", bt_frame);
          }
      }
      /* LD_BIND_TRACE_TARGET=<hex offset>: arm automatically, once, on the
       * first draw into that target (passes that run only at level load). */
      { static int tt_init; static u32 tt_off; static int tt_done;
        if (!tt_init) { const char* e = getenv("LD_BIND_TRACE_TARGET");
                        tt_off = e ? (u32)strtoul(e, 0, 16) : 0; tt_init = 1; }
        if (tt_off && !tt_done && bt_state == 0 && g.surfaces[target].offset == tt_off) {
            tt_done = 1; bt_state = 1; bt_frame = g_ld_frames; bt_rows = 0;
            fprintf(stderr, "[bind-trace] armed for frame %u (target 0x%08X)\n", bt_frame, tt_off);
        } }
      g_bt_on = bt_state == 1 && g_ld_frames == bt_frame;
      if (bt_state == 1 && g_ld_frames > bt_frame) {
          bt_state = 0; g_bt_on = 0;
          fprintf(stderr, "[bind-trace] done rows=%u\n", bt_rows);
          for (u32 i = 0; i < g.n_surfaces; i++)
              fprintf(stderr, "[bind-trace] surface[%u] %u:0x%08X %ux%u fmt=%u drawgen=%u\n", i,
                      g.surfaces[i].location, g.surfaces[i].offset, g.surfaces[i].w, g.surfaces[i].h,
                      (u32)g.surfaces[i].fmt, g.surfaces[i].last_draw_generation);
      }
      /* LD_MIDDUMP_OFFSET=<hex>: in the traced frame, read that surface back
       * the moment the next draw leaves it -- what the pass itself produced,
       * before anything later in the frame touches it. */
      { static int md_init; static u32 md_off; static u32 md_prev = LD_INVALID_SURFACE; static u32 md_n;
        if (!md_init) { const char* e = getenv("LD_MIDDUMP_OFFSET"); md_off = e ? (u32)strtoul(e, 0, 16) : 0; md_init = 1; }
        if (md_off && bt_state == 1 && g_ld_frames == bt_frame) {
            if (md_prev != LD_INVALID_SURFACE && md_prev != target && g.surfaces[md_prev].offset == md_off) {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "middump_f%06u_%u_%08X.ppm", g_ld_frames, md_n++, md_off);
                const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[md_prev]);
                fprintf(stderr, "[bind-trace] middump %s nonblack=%llu at d=%u\n", path,
                        (unsigned long long)nb, bt_rows);
            }
            md_prev = target;
        } }
      if (bt_state == 1 && g_ld_frames == bt_frame && bt_rows < 6000) {
          const surface_t* ts = &g.surfaces[target];
          bt_rows++;
          /* First draw of each PSO in the traced frame: its fragment
           * constants as floats (light direction/colour live here). */
          { static u64 seen_pso[256]; static u32 n_seen, seen_frame;
            if (seen_frame != bt_frame) { seen_frame = bt_frame; n_seen = 0; }
            const u64 pk = live_legacy_pso_key();
            int fresh = n_seen < 256;
            for (u32 z = 0; z < n_seen; z++) if (seen_pso[z] == pk) fresh = 0;
            if (fresh) {
                seen_pso[n_seen++] = pk;
                fprintf(stderr, "[bind-trace] fpc pso=%016llx n=%u", (unsigned long long)pk, g.fp_constants.count);
                for (u32 c = 0; c < g.fp_constants.count && c < 48u; c++)
                    fprintf(stderr, " c%u=(%g,%g,%g,%g)", c,
                            ld_c2f(g.fp_constants.values[c][0]), ld_c2f(g.fp_constants.values[c][1]),
                            ld_c2f(g.fp_constants.values[c][2]), ld_c2f(g.fp_constants.values[c][3]));
                fprintf(stderr, "\n[bind-trace] vpc pso=%016llx bbits=%08X", (unsigned long long)pk, rsx_dsp_reg(&g.rsx, 0x1FF8u));
                for (u32 c = 256; c < 468; c = c == 267 ? 464 : c + 1) {
                    float f[4]; memcpy(f, &g.rsx.constants[c][0], 16);
                    fprintf(stderr, " c%u=(%g,%g,%g,%g)", c, f[0], f[1], f[2], f[3]);
                }
                fprintf(stderr, "\n[bind-trace] attrs pso=%016llx", (unsigned long long)pk);
                { const u32 vbase = rsx_dsp_vertex_data_base_offset(&g.rsx);
                  for (u32 ai = 0; ai < 16; ai++) {
                      rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
                      if (!va.type || !va.size) continue;
                      float v0[4] = {0,0,0,0}, v1[4] = {0,0,0,0};
                      const int ok0 = fetch_attr(ai, vbase, 0, 0, v0);
                      const int ok1 = fetch_attr(ai, vbase, 1, 0, v1);
                      fprintf(stderr, " a%u[t%u s%u st%u v0%s=(%.3g,%.3g,%.3g,%.3g) v1%s=(%.3g,%.3g,%.3g,%.3g)]",
                              ai, va.type, va.size, va.stride, ok0 ? "" : "!", v0[0], v0[1], v0[2], v0[3],
                              ok1 ? "" : "!", v1[0], v1[1], v1[2], v1[3]);
                  } }
                fprintf(stderr, "\n");
            } }
          fprintf(stderr, "[bind-trace] d=%u tgt=%u:0x%08X %ux%u z=%u cm=%08X zt=%u zw=%u zf=%X bl=%u st=%u/%X/%X/%X cull=%u/%X/%X clip=%X sc=%08X/%08X vp=(%u,%u %ux%u s=%.1f,%.1f t=%.1f,%.1f) clipwh=%ux%u ct=%X B=%X C=%X cfmt=%X pso=%016llx",
                  bt_rows, ts->location, ts->offset, ts->w, ts->h, current_zslot,
                  rsx_dsp_reg(&g.rsx, M_COLOR_MASK), rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u,
                  rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u, rsx_dsp_reg(&g.rsx, 0x0A6C),
                  rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u, rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1u, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC), rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF), rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_MASK), rsx_dsp_reg(&g.rsx, M_CULL_FACE_ENABLE) & 1u, rsx_dsp_reg(&g.rsx, M_CULL_FACE), rsx_dsp_reg(&g.rsx, M_FRONT_FACE), rsx_dsp_reg(&g.rsx, 0x1478),
                  rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL),
                  vp.x, vp.y, vp.w, vp.h, vp.scale[0], vp.scale[1], vp.translate[0], vp.translate[1],
                  sf.clip_w, sf.clip_h, sf.color_target, sf.color_offset[1], sf.color_offset[2], sf.color_format, (unsigned long long)live_legacy_pso_key());
          fprintf(stderr, " aa=%X at=%u/%X/%X", rsx_dsp_reg(&g.rsx, 0x1D7Cu), rsx_dsp_reg(&g.rsx, 0x0304u) & 1u,
                  rsx_dsp_reg(&g.rsx, 0x0308u), rsx_dsp_reg(&g.rsx, 0x030Cu));
          /* Constant ("current") values of attributes without an array: a
           * shader may read them as per-draw parameters (terrain blend). */
          for (u32 ai = 0; ai < 16; ai++) {
              rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
              if (va.type && va.size) continue;
              const u32 m0 = 0x1C00u + ai * 0x10u;
              if (!(g.rsx.seen[m0 >> 2] | g.rsx.seen[(m0 >> 2) + 1] | g.rsx.seen[(m0 >> 2) + 2] | g.rsx.seen[(m0 >> 2) + 3])) continue;
              float k[4]; rsx_dsp_vertex_default(&g.rsx, ai, k);
              fprintf(stderr, " k%u=(%g,%g,%g,%g)", ai, k[0], k[1], k[2], k[3]);
          }
          for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
              rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
              if (!tq.enabled) continue;
              const u32 s = slots[q];
              const char* k = "?";
              if (s == SRV_WHITE) k = "white";
              else if (s >= SRV_TEXTURE_BASE && s < SRV_TEXTURE_BASE + g.n_textures) k = "guest";
              else if (s >= SRV_SURFACE_BASE && s < SRV_SURFACE_BASE + g.n_surfaces) k = "surf";
              else if (s >= SRV_FEEDBACK_BASE && s < SRV_FEEDBACK_BASE + g.n_surfaces) k = "fb";
              else if (s >= SRV_SURF_WIN_BASE && s < SRV_SURF_WIN_BASE + g.n_surfaces * LD_SWIN_PER) k = "swin";
              else if (s >= SRV_ZDEPTH_BASE && s < SRV_ZDEPTH_BASE + g.n_zdepths) k = "zeta";
              else if (s >= SRV_ZPACK_BASE && s < SRV_ZPACK_BASE + g.n_zdepths) k = "zpack";
              else k = "other";
              /* dump_units file: also write each linear guest texture of the
               * traced frame as raw bytes (tex_<loc>_<offset>_<w>x<h>_p<pitch>_f<fmt>.raw). */
              if (!strcmp(k, "guest") && (tq.format & 0x20u) && tq.pitch && tq.width <= 2048u && tq.height <= 2048u &&
                  GetFileAttributesA("dump_units") != INVALID_FILE_ATTRIBUTES) {
                  char path[128];
                  snprintf(path, sizeof path, "tex_%u_%08X_%ux%u_p%u_f%02X.raw", tq.location, tq.offset,
                           tq.width, tq.height, tq.pitch, tq.format);
                  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
                      const u8* tp = guest_ptr(tq.location, tq.offset, tq.pitch * tq.height);
                      FILE* tf = tp ? fopen(path, "wb") : NULL;
                      if (tf) { fwrite(tp, 1, (size_t)tq.pitch * tq.height, tf); fclose(tf); }
                  }
              }
              fprintf(stderr, " flt%u=%08X/c0=%08X/mip%u", q, tq.filter, tq.control0, tq.mipmaps);
              fprintf(stderr, " u%u[%u:0x%08X f%02X %ux%u%s w%05X %s]", q, tq.location, tq.offset,
                      tq.format, tq.width, tq.height, tq.cubemap ? " cube" : "",
                      tq.wrap & 0xFFFFFu, k);
          }
          fprintf(stderr, "\n");
      } }

    /* Confirm whether a specifically requested atlas actually sampled the
     * guest upload or bypassed it through a GPU-owned alias. Cache hits are
     * included; no readback or resource mutation occurs here. */
    { static int enabled = -1; static u32 logged;
      if (enabled < 0) {
          const char* e = getenv("LD_TEXRGBA_DUMP_OFFSET");
          enabled = e && e[0] && getenv("LD_TEXRGBA_DUMP");
      }
      if (enabled && logged < 12) for (u32 unit = 0; unit < SRV_TABLE_SIZE && logged < 12; unit++) {
          rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, unit, &t);
          if (!t.enabled || !ld_texrgba_dump_offset(&t)) continue;
          const u32 slot = slots[unit];
          const int guest = slot >= SRV_TEXTURE_BASE && slot < SRV_TEXTURE_BASE + MAX_TEXTURES;
          fprintf(stderr, "[tex-rgba-bind] frame=%u unit=%u source=%u:0x%08X slot=%u kind=%s target=%u pso=%016llx\n",
                  g_ld_frames, unit, t.location, t.offset, slot,
                  slot == SRV_WHITE ? "white" : guest ? "guest-upload" : "GPU-owned",
                  target, (unsigned long long)live_legacy_pso_key());
          logged++;
      } }
    const u32 vtex_mask = vertex_texture_mask();
    u32 vtex_slots[SRV_TABLE_SIZE];
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
        vtex_slots[u] = SRV_WHITE;
    /* Resolve and upload every vertex texture before recording this draw's
     * graphics state.  Cache refresh can retire a texture and flush the open
     * command list; doing that after SetPipelineState/root bindings silently
     * loses those bindings on the replacement list. */
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        vtex_slots[u] = vertex_texture_srv_slot(&vt);
        if (vtex_slots[u] != SRV_WHITE)
            g_ld_vtex_binds++;
        else
            g_ld_vtex_unsupported++;
    }
    const u32 required_srv_tables = 1u + (vtex_mask != 0);
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_NONE);
    if (g.srv_ring_used + required_srv_tables > SRV_RING_TABLES ||
        sampler_table_needs_flush(smp_slots)) {
        /* Every descriptor referenced by the old command list is consumed
         * before recycling its shader-visible heap entries. Texture upload
         * commands recorded while resolving slots are also completed here;
         * no render-state command for this draw has been recorded yet. */
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        if (!g.ready) return;
        g.srv_ring_used = 0;
        g.smp_ring_used = 0;
    }

    ld_cpu_draw_phase_switch(LD_CPU_PHASE_STATE);
    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv; int have_dsv = 0;
    if (g.depth) {
        dsv = dsv_handle(current_zslot);
        have_dsv = 1;
        if (current_zslot) {
            zdepth_t* z = &g.zdepths[current_zslot - 1];
            if (!z->cleared) {
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                z->cleared = 1;
                z->had_write = 0;
                g_ld_stats.implicit_depth_clears++;
            }
            ld_zeta_import_color(current_zslot);
        } else if (!g.depth_cleared) {
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
            g.depth_cleared = 1;
            g_ld_stats.implicit_depth_clears++;
        }
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        rtvs[0] = rtv;
        for (u32 r = 1; r < n_mrt; r++)
            rtvs[r] = rtv_handle(LD_SWAP_BUFFERS + mrt[r]);
        g.list->lpVtbl->OMSetRenderTargets(g.list, n_mrt, rtvs, FALSE,
                                           have_dsv ? &dsv : NULL);
    }
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_DESCRIPTORS);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots, 1);
    const D3D12_GPU_DESCRIPTOR_HANDLE stbl = sampler_table(smp_slots);
    D3D12_GPU_DESCRIPTOR_HANDLE vtex_table = {0};
    if (vtex_mask)
        vtex_table = srv_table(vtex_slots, 0);
    u64 descriptor_signature =
        fnv1a(slots, sizeof(slots), 1469598103934665603ull);
    descriptor_signature =
        fnv1a(smp_slots, sizeof(smp_slots), descriptor_signature);
    if (vtex_mask)
        descriptor_signature =
            fnv1a(vtex_slots, sizeof(vtex_slots), descriptor_signature);
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_NONE);

    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_CONSTANTS);
    u8* cbdst = g.cb_mapped + g.cb_used;
    memcpy(cbdst, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16);
    rsx_vp_pack_branch_bits(xf, rsx_dsp_reg(&g.rsx, 0x1FF8u));
    memcpy(cbdst + 512 * 16, xf, sizeof(xf));
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_NONE);
    g_ld_canonical_surface_rebind = 0;
    ld_canonical_capture(prim, n_tri, target, xf, ps_cb_offset, slots);
    if (!g.ready) return;
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_STATE);
    if (g_ld_canonical_surface_rebind) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        rtvs[0] = rtv;
        for (u32 r = 1; r < n_mrt; r++)
            rtvs[r] = rtv_handle(LD_SWAP_BUFFERS + mrt[r]);
        g.list->lpVtbl->OMSetRenderTargets(g.list, n_mrt, rtvs, FALSE, have_dsv ? &dsv : NULL);
        g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
       
    }

    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig_x);
    g.list->lpVtbl->SetGraphicsRootConstantBufferView(
        g.list, 0, g.cb->lpVtbl->GetGPUVirtualAddress(g.cb) + g.cb_used);
    if (ld_fp_constants_buffered())
        g.list->lpVtbl->SetGraphicsRootConstantBufferView(
            g.list, 4,
            g.ps_cb->lpVtbl->GetGPUVirtualAddress(g.ps_cb) +
                ps_cb_offset);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, stbl);
    if (vtex_mask)
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(
            g.list, 3, vtex_table);
    g.cb_used += CB_BLOCK_ALIGNED;

    /* Render scale: NDC maps over the HOST extent of the target; the guest
     * viewport transform above stays in guest pixels. */
    const surface_t* const tsurf = &g.surfaces[target];
    const float rsf = ld_sxf(tsurf);
    D3D12_VIEWPORT dvp = {0, 0, W * rsf, H * rsf, 0.0f, 1.0f};
    D3D12_RECT sc = {
        0, 0,
        (LONG)(tsurf->w ? ld_sx(tsurf, tsurf->w) : ld_sx(tsurf, g.width)),
        (LONG)(tsurf->h ? ld_sx(tsurf, tsurf->h) : ld_sx(tsurf, g.height))
    };
    /* Guest scissor, intersected with the surface rect. The nv40 reset value
     * is a full-window 4096x4096; never-written regs read 0 here, so an
     * unwritten scissor means "no scissor". A WRITTEN zero width or height is
     * a real, empty scissor and must clip everything: GH3's gameplay venue
     * sits under a ViewportElement whose clip window is off-screen (scissor
     * x=1039 w=0), and drawing it full-screen painted its never-rendered
     * texture over the whole scene. */
    {
        const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        /* The scissor is in pixels too: scale it to the sample grid. */
        const LONG ax = (LONG)sf.aa_sx, ay = (LONG)sf.aa_sy;
        /* Render scale: scale the end points (x+w), not x and w apart, or
         * scissors meeting at an odd x leave a 1-pixel gap or overlap. */
        const u32 gx0 = (sh & 0xFFFFu) * (u32)ax, gx1 = gx0 + (sh >> 16) * (u32)ax;
        const u32 gy0 = (sv & 0xFFFFu) * (u32)ay, gy1 = gy0 + (sv >> 16) * (u32)ay;
        const LONG sx = (LONG)ld_sx(tsurf, gx0), sw = (LONG)ld_sx(tsurf, gx1) - sx;
        const LONG sy = (LONG)ld_sx(tsurf, gy0), svh = (LONG)ld_sx(tsurf, gy1) - sy;
        const int written = g.rsx.seen[M_SCISSOR_HORIZONTAL >> 2] != 0 ||
                            g.rsx.seen[M_SCISSOR_VERTICAL >> 2] != 0;
        if (written) {
            if (sx > sc.left)            sc.left   = sx;
            if (sy > sc.top)             sc.top    = sy;
            if (sx + sw  < sc.right)     sc.right  = sx + sw;
            if (sy + svh < sc.bottom)    sc.bottom = sy + svh;
            if (sc.right  < sc.left)     sc.right  = sc.left;
            if (sc.bottom < sc.top)      sc.bottom = sc.top;
        }
    }
    g.list->lpVtbl->RSSetViewports(g.list, 1, &dvp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    /* Dynamic stencil reference (kept out of the PSO key on purpose). */
    g.list->lpVtbl->OMSetStencilRef(
        g.list, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = dc.vcache ? dc.vcache->va : g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = vertex_stride;
    vbv.SizeInBytes = (u32)draw_vb_bytes;
    if (vertex_stride)
        g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, prim == PRIM_LINES
        ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_NONE);
    {
        const u64 serial = ++g_ld_recent_draw_total;
        ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        memset(draw, 0, sizeof(*draw));
        draw->serial = serial;
        draw->frame = g_ld_frames;
        draw->pso_key = g_ld_current_pso.key;
        draw->descriptor_signature = descriptor_signature;
        draw->vp_start = g_ld_current_pso.vp_start;
        draw->vp_instrs = g_ld_current_pso.vp_instrs;
        draw->fp_location = g_ld_current_pso.fp_location;
        draw->fp_offset = g_ld_current_pso.fp_offset;
        draw->fp_size = g_ld_current_pso.fp_size;
        draw->fp_control = g_ld_current_pso.fp_control;
        draw->cube_mask = g_ld_current_pso.cube_mask;
        draw->vtex_mask = g_ld_current_pso.vtex_mask;
        draw->txl_mask = g_ld_current_pso.txl_mask;
        draw->texture_mask = texture_mask;
        draw->primitive = prim;
        draw->vertices = n_tri;
        draw->target = target;
        draw->zslot = current_zslot;
        draw->clip_x = sf.clip_x;
        draw->clip_y = sf.clip_y;
        draw->clip_w = sf.clip_w;
        draw->clip_h = sf.clip_h;
        draw->srv_ring_used = g.srv_ring_used;
        draw->sampler_ring_used = g.smp_ring_used;
        draw->cb_used = g.cb_used;
        draw->vb_used = g.vb_used;
    }
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_DRAW);
    if (indexed) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = dc.icache ? dc.icache->va :
            g.ib->lpVtbl->GetGPUVirtualAddress(g.ib) + g.ib_used;
        ibv.SizeInBytes = (u32)draw_ib_bytes;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        g.list->lpVtbl->IASetIndexBuffer(g.list, &ibv);
        g.list->lpVtbl->DrawIndexedInstanced(
            g.list, n_tri, 1, 0, 0, 0);
    } else {
        g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);
    }
    if (g_bt_on)
        fprintf(stderr, "[bind-trace] drawn tgt=0x%08X z=%u verts=%u indexed=%d prim=%u\n",
                g.surfaces[target].offset, current_zslot, (u32)n_tri, indexed, (u32)prim);
    ld_cpu_draw_phase_switch(LD_CPU_PHASE_NONE);
    if(dc.raw28){
        static u32 submitted;
        submitted++;
        if(submitted<=4u || (submitted%600u==0 && submitted<=19200u))
            fprintf(stderr,"[vertex-raw28] submitted=%u frame=%u indexed=%d vertices=%u upload_bytes=%llu pso=%016llX\n",submitted,g_ld_frames,indexed,n_tri,(unsigned long long)draw_vb_bytes,(unsigned long long)g_ld_current_pso.key);
    }
    ld_surface_note_write(target, LD_SURFACE_WRITE_DRAW);
    for (u32 r = 1; r < n_mrt; r++)
        ld_surface_note_write(mrt[r], LD_SURFACE_WRITE_DRAW);
    if (sf.color_target && rsx_dsp_reg(&g.rsx, M_COLOR_MASK)) {
        g.surfaces[target].ct_draw_gen = g_ld_surface_generation;
        g_ld_last_ct.slot = target;   /* rsx_live_draw_user_command */
        g_ld_last_ct.location = g.surfaces[target].location;
        g_ld_last_ct.offset = g.surfaces[target].offset;
    }
    if (current_zslot) {
        render_state_t depth_state;
        decode_render_state(&depth_state);
        zdepth_t* z = &g.zdepths[current_zslot - 1];
        if (depth_state.depth_write || depth_state.stencil_enable)
            z->ds_write_gen = g_ld_surface_generation;
        if (depth_state.stencil_enable) z->packed_generation = UINT64_MAX;
        z->draws++;
        if (depth_state.depth_test) z->depth_test_draws++;
        if (depth_state.depth_write) z->depth_write_draws++;
        if (depth_state.depth_test && depth_state.depth_write)
            z->depth_both_draws++;
        /* A write-enable bit alone does not prove that this draw produced a
         * usable depth map.  With depth testing disabled RSX does not execute
         * the depth pass represented by this tracked zeta. */
        if (depth_state.depth_test && depth_state.depth_write)
            z->had_write = 1;
    }
    g_ld_stats.groups_executed++;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.input_vertices += dc.n_source_refs;
    g_ld_profile.total.expanded_vertices += n_tri;
    g_ld_profile.total.legacy_vertex_upload_bytes +=
        (u64)(prim == PRIM_TRIANGLES ? n_tri : dc.n_source_refs) *
        VERT_STRIDE;
    g_ld_profile.total.vertex_upload_bytes += draw_vb_bytes;
#endif

    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    /* A guest-side failure can occur before the next flip even though useful
     * orphanage draws have already reached internal world targets.  Preserve
     * a few progressively later surface snapshots directly from the draw
     * stream so presentation is not a prerequisite for visual diagnosis. */
    if (LD_DIAG_ENABLED("RSX_A010_EAGER_DUMP") &&
        InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0) {
        static u64 eager_origin = 0;
        static u32 eager_sample = 0;
        if (!eager_origin)
            eager_origin = g_ld_stats.groups_executed;
        if (eager_sample < 4u &&
            g_ld_stats.groups_executed >=
                eager_origin + (u64)eager_sample * 32u) {
            CreateDirectoryA("scratch\\a010_probe", NULL);
            for (u32 i = 0; i < g.n_surfaces; i++) {
                const u32 off = g.surfaces[i].offset;
                if (off != 0x00E40000u && off != 0x01800000u &&
                    off != 0x02D10000u && off != 0x02710000u &&
                    off != 0x01440000u)
                    continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\eager_%03u_group_%08llu_"
                         "surface_%02u_off_%08X.ppm",
                         eager_sample,
                         (unsigned long long)g_ld_stats.groups_executed,
                         i, off);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
            }
            fprintf(stderr,
                    "[a010-eager-dump] sample=%u groups=%llu surfaces=%u\n",
                    eager_sample,
                    (unsigned long long)g_ld_stats.groups_executed,
                    g.n_surfaces);
            eager_sample++;
            fflush(stderr);
        }
    }
    if (!dc.vcache) g.vb_used += (u32)draw_vb_bytes;
    if (!dc.icache) g.ib_used += (u32)draw_ib_bytes;
    ld_profile_note_ring_highwater();
    ld_draw_pair_end(draw_pair_target);
}

static void sink_end(void* user, const rsx_dispatch* r)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
#endif
    ld_cpu_draw_phase_begin();
    sink_end_impl(user, r);
    ld_cpu_draw_phase_end();
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.sink_end_qpc += (u64)(ld_profile_qpc() - begin);
#endif
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    g_ld_stats.clears++;
    const u32 target = current_surface();
    if (g_bt_on)
        fprintf(stderr, "[bind-trace] clear tgt=%s%u:0x%08X %ux%u mask=0x%02X argb=0x%08X cmask=0x%08X\n",
                target == LD_INVALID_SURFACE ? "none/" : "",
                target == LD_INVALID_SURFACE ? 0u : g.surfaces[target].location,
                target == LD_INVALID_SURFACE ? 0u : g.surfaces[target].offset,
                target == LD_INVALID_SURFACE ? 0u : g.surfaces[target].w,
                target == LD_INVALID_SURFACE ? 0u : g.surfaces[target].h,
                mask, rsx_dsp_clear_color(&g.rsx), rsx_dsp_reg(&g.rsx, M_COLOR_MASK));
    if (target == LD_INVALID_SURFACE) { g_ld_stats.clear_drop_surface++; return; }
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    if (ld_target_trace_enabled()) {
        static u32 last_clear_target = LD_INVALID_SURFACE;
        const int changed = target != last_clear_target;
        if (changed) {
            ld_trace_target("clear-target", target, mask);
            last_clear_target = target;
        }
        /* A broken scene can issue tens of thousands of clears. Preserve the
         * opening sequence and a periodic heartbeat without turning tracing
         * itself into a scheduler perturbation. Target changes are always
         * emitted above. */
        if (g_ld_stats.clears <= 256 || (g_ld_stats.clears & 1023) == 0) {
            const u32 z = rsx_dsp_clear_zstencil(&g.rsx);
            fprintf(stderr,
                    "[rsx-clear] frame=%u n=%llu target=%u mask=0x%02X "
                    "argb=0x%08X z24=0x%06X stencil=0x%02X\n",
                    g_ld_frames, g_ld_stats.clears, target, mask,
                    rsx_dsp_clear_color(&g.rsx), z >> 8, z & 0xFF);
        }
    }
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#clear,%u,0x%X,mask=0x%02X,scissor=0x%08X/0x%08X,cmask=0x%08X,argb=0x%08X\n",
                g_ld_frames, g.surfaces[target].offset, mask,
                rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL),
                rsx_dsp_reg(&g.rsx, M_COLOR_MASK), rsx_dsp_clear_color(&g.rsx));
        fflush(s_csv_file);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    const DXGI_FORMAT clear_format = g.surfaces[target].fmt;
    /* RSX colour CLEAR is a byte-format operation, not a floating-point
     * clear. Float attachments ignore this command; B8 obeys only BLUE. */
    const int clear_float = clear_format == DXGI_FORMAT_R32_FLOAT ||
        clear_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        clear_format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (!clear_float &&
        (clear_format != DXGI_FORMAT_R8_UNORM || (mask & RSX_CLEAR_COLOR_B)) &&
        (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A))) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                         (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
        if (clear_format == DXGI_FORMAT_R8_UNORM) col[0] = col[2];
        /* LD_CLEAR_TEST=1: clear to magenta instead of the guest's colour.
         *
         * This validates the MEASUREMENT, not the renderer. LD_SURF_DUMP
         * reports nonblack=0 for every surface, and every conclusion drawn from
         * that depends on the readback actually being able to see a non-black
         * pixel. If magenta clears still read back as 0, the readback is broken
         * and those conclusions are void. Establish that X would be visible
         * before concluding X never happens. */
        { static int ct = -1;
          if (ct < 0) ct = getenv("LD_CLEAR_TEST") ? 1 : 0;
          if (ct) { col[0] = 1.0f; col[1] = 0.0f; col[2] = 1.0f; col[3] = 1.0f; } }
        g.list->lpVtbl->ClearRenderTargetView(g.list, rtv, col, 0, NULL);
        ld_surface_note_write(target, LD_SURFACE_WRITE_CLEAR);
    }
    if ((mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h, (mask & RSX_CLEAR_DEPTH) != 0);
        if (zslot && (mask & RSX_CLEAR_STENCIL))
            g.zdepths[zslot - 1].packed_generation = UINT64_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
        D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
        if (mask & RSX_CLEAR_DEPTH)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
        if (mask & RSX_CLEAR_STENCIL)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0. Same
         * decode as the draw engine's clear. */
        float cd;
        {
            const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
            const int z16 = sf.depth_format == 1u;
            cd = !zs ? 1.0f
                           : z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                 : (float)(zs >> 8) / 16777215.0f;
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                clear_flags, cd, z16 ? (UINT8)0 : (UINT8)(zs & 0xFFu), 0, NULL);
        }
        if (zslot) {
            g.zdepths[zslot - 1].cleared = 1;
            if (mask & RSX_CLEAR_DEPTH) {
                g.zdepths[zslot - 1].clear_depth = cd;
                g.zdepths[zslot - 1].had_write = 0;
                /* Each guest zeta owns a distinct live resource.  A clear
                 * invalidates its older published image; do not resolve it
                 * speculatively.  The first later texture consumer resolves
                 * the newly written pass exactly once. */
                g.zdepths[zslot - 1].snapshot_valid = 0;
            }
        } else {
            if (mask & RSX_CLEAR_DEPTH)
                g.depth_cleared = 1;
        }
    }
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    const u32 buffer_id = arg & 7u;
    g_ld_flip_requested++;
    g_ld_last_requested_buffer = buffer_id;
    if (g_ld_movie_mode) {
        if (ld_movie_composite_ui_enabled()) ld_movie_capture_overlay();
        g_ld_flip_consumed++;
        g_ld_last_consumed_buffer = buffer_id;
        return;
    }
    rsx_live_draw_present(buffer_id);
    g_ld_flip_consumed++;
    g_ld_last_consumed_buffer = buffer_id;
}

/* ---------------------------------------------------------------------------
 * device / resource setup
 * -----------------------------------------------------------------------*/
static int make_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE xrange = {0};
    xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    xrange.NumDescriptors = SRV_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE srange = {0};
    srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    srange.NumDescriptors = SMP_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE vrange = {0};
    vrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vrange.NumDescriptors = RSX_DSP_NUM_VERTEX_TEXTURES;
    vrange.BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER xp[5] = {0};
    xp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[0].Descriptor.ShaderRegister = 0;
    xp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[1].DescriptorTable.NumDescriptorRanges = 1;
    xp[1].DescriptorTable.pDescriptorRanges = &xrange;
    xp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[2].DescriptorTable.NumDescriptorRanges = 1;
    xp[2].DescriptorTable.pDescriptorRanges = &srange;
    xp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[3].DescriptorTable.NumDescriptorRanges = 1;
    xp[3].DescriptorTable.pDescriptorRanges = &vrange;
    xp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[4].Descriptor.ShaderRegister = 1;
    xp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC vsmp[RSX_DSP_NUM_VERTEX_TEXTURES] = {0};
    for (u32 i = 0; i < RSX_DSP_NUM_VERTEX_TEXTURES; i++) {
        vsmp[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        vsmp[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].MaxLOD = D3D12_FLOAT32_MAX;
        vsmp[i].ShaderRegister = i;
        vsmp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 5; rsd.pParameters = xp;
    rsd.NumStaticSamplers = RSX_DSP_NUM_VERTEX_TEXTURES;
    rsd.pStaticSamplers = vsmp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err); return -1;
    }
    HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                    sig->lpVtbl->GetBufferSize(sig),
                                                    &IID_ID3D12RootSignature, (void**)&g.rootsig_x);
    if (SUCCEEDED(hr))
        rsp_rt_register_root(sig->lpVtbl->GetBufferPointer(sig),
                             sig->lpVtbl->GetBufferSize(sig), g.rootsig_x);
    sig->lpVtbl->Release(sig);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int make_depth_snapshot_pipeline(void)
{
    static const char source[] =
        "Texture2D<float> depth_source : register(t0);\n"
        "RWTexture2D<float> depth_destination : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint width, height;\n"
        "    depth_destination.GetDimensions(width, height);\n"
        "    if (id.x < width && id.y < height)\n"
        "        depth_destination[id.xy] = "
        "depth_source.Load(int3(id.xy, 0));\n"
        "}\n";
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = ld_builtin_compile(
        source, sizeof(source) - 1u, "depth_snapshot_compute", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot shader compile failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors)
                       : "<no diagnostics>");
        if (errors) errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors) errors->lpVtbl->Release(errors);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        parameters[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description = {0};
    description.NumParameters = 2;
    description.pParameters = parameters;
    ID3DBlob* serialized = NULL;
    ID3DBlob* signature_errors = NULL;
    hr = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &signature_errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot root signature failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                signature_errors
                    ? (const char*)signature_errors->lpVtbl->GetBufferPointer(
                          signature_errors)
                    : "<no diagnostics>");
        if (signature_errors)
            signature_errors->lpVtbl->Release(signature_errors);
        shader->lpVtbl->Release(shader);
        return -1;
    }
    if (signature_errors)
        signature_errors->lpVtbl->Release(signature_errors);
    hr = g.dev->lpVtbl->CreateRootSignature(
        g.dev, 0, serialized->lpVtbl->GetBufferPointer(serialized),
        serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&g.depth_snapshot_rootsig);
    serialized->lpVtbl->Release(serialized);
    if (FAILED(hr)) {
        shader->lpVtbl->Release(shader);
        return -1;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {0};
    pipeline.pRootSignature = g.depth_snapshot_rootsig;
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    hr = g.dev->lpVtbl->CreateComputePipelineState(
        g.dev, &pipeline, &IID_ID3D12PipelineState,
        (void**)&g.depth_snapshot_pso);
    shader->lpVtbl->Release(shader);
    return SUCCEEDED(hr) ? 0 : -1;
}

int rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                       rsx_live_guest_ptr_fn guest_fn, void* guest_user)
{
    (void)ld_texture_hash_mode();
    if (!rsx_live_draw_enabled()) return 0;
    if (g.ready) return 0;
    g_ld_surface_demand_count = 0;
    g_ld_surface_demand_untracked = 0;
    g_ld_surface_capacity_rejects = 0;
    ld_present_measure_init();
#if defined(YZ_PERF_PROFILE)
    memset(&g_ld_profile, 0, sizeof(g_ld_profile));
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_ld_profile.qpc_frequency = frequency.QuadPart;
        g_ld_profile.previous_present_qpc = ld_profile_qpc();
    }
#endif
    g.width = width; g.height = height;
    g.guest_ptr = guest_fn; g.guest_user = guest_user;
    {
        const char* requested = getenv("RSX_FP_CONSTANT_MODE");
        char mode = 'B';
        if (requested && requested[0]) {
            if ((requested[0] == 'L' || requested[0] == 'l') &&
                (!requested[1] ||
                 _stricmp(requested, "literal") == 0))
                mode = 'L';
            else if ((requested[0] == 'B' || requested[0] == 'b') &&
                     (!requested[1] ||
                      _stricmp(requested, "buffered") == 0))
                mode = 'B';
            else
                fprintf(
                    stderr,
                    "[rsx-fp-constants] unknown "
                    "RSX_FP_CONSTANT_MODE='%s'; using buffered\n",
                    requested);
        }
        g.fp_constant_mode = mode;
        fprintf(
            stderr,
            "[rsx-fp-constants] mode=%s (startup-selected) "
            "d3dcompile_flags1=0x0 d3dcompile_flags2=0x0\n",
            ld_fp_constant_mode_name());
    }
    {
        const char* requested = getenv("RSX_VERTEX_MODE");
        const char* old_path = getenv("RSX_VERTEX_PATH");
        char mode = 'C';
        if (requested && requested[0] && !requested[1]) {
            mode = (char)toupper((unsigned char)requested[0]);
        } else if (!requested && old_path) {
            mode = strcmp(old_path, "compact") == 0 ? 'C' : 'L';
        }
        switch (mode) {
        case 'L':
            g.vertex_features = 0;
            break;
        case 'H':
            g.vertex_features = LD_VERTEX_HOIST_FETCH;
            break;
        case 'M':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE;
            break;
        case 'C':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        default:
            fprintf(stderr,
                    "[rsx-vertex] unknown RSX_VERTEX_MODE='%s'; "
                    "using C\n",
                    requested ? requested : "");
            mode = 'C';
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        }
        g.vertex_mode = mode;
#if !defined(YZ_PERF_CLEAN)
        const char* diag_dir = getenv("RSX_VERTEX_DIAG_DIR");
        if (diag_dir && diag_dir[0]) {
            strncpy(
                g.vertex_diag_dir, diag_dir,
                sizeof(g.vertex_diag_dir) - 1u);
            g.vertex_diag_dir[sizeof(g.vertex_diag_dir) - 1u] = '\0';
        }
#endif
        fprintf(stderr,
                "[rsx-vertex] mode=%s features=0x%X "
                "(startup-selected)\n",
                ld_vertex_mode_name(), g.vertex_features);
    }
    g_ld_flip_requested = 0;
    g_ld_flip_consumed = 0;
    g_ld_last_requested_buffer = UINT32_MAX;
    g_ld_last_consumed_buffer = UINT32_MAX;
    g_ld_last_present_target = UINT32_MAX;
#if !defined(YZ_PERF_CLEAN)
    g_ld_surface_generation = 0;
    g_ld_surface_resource_serial = 0;
    g_ld_guest_blit_generation = 0;
    g_ld_present_copy_generation = 0;
    g_ld_vertex_constant_ring_recycles = 0;
    g_ld_frame_vertex_constant_ring_recycles = 0;
    g_ld_fifo_get = 0;
    g_ld_fifo_put = 0;
    InterlockedExchange(&g_ld_diag_post_movie_pending, 0);
    InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
#endif
    g_ld_last_dump_fingerprint = 0;
    memset(g_ld_layout_cache, 0, sizeof(g_ld_layout_cache));
    g_ld_layout_cache_count = 0;

    ld_enable_debug_layer();
    ld_enable_dred();
    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory))) return -1;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g.dev))) {
        factory->lpVtbl->Release(factory); return -1;
    }
#if defined(YZ_PERF_PROFILE)
    {
        LUID luid = {0};
        g.dev->lpVtbl->GetAdapterLuid(g.dev, &luid);
        factory->lpVtbl->EnumAdapterByLuid(
            factory, luid, &IID_IDXGIAdapter3,
            (void**)&g_ld_profile.adapter);
    }
#endif
    ld_open_info_queue();
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue);
    g.dev->lpVtbl->CreateCommandAllocator(g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          &IID_ID3D12CommandAllocator, (void**)&g.alloc);
    g.dev->lpVtbl->CreateCommandList(g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
                                     &IID_ID3D12GraphicsCommandList, (void**)&g.list);
    if (g.queue) g.queue->lpVtbl->SetName(g.queue, L"rsx-live-queue");
    if (g.list) g.list->lpVtbl->SetName(g.list, L"rsx-live-list");
    g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g.fence);
    g.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if(g.queue&&g.list&&g.fence)ld_gt_init();

    /* swap chain bound to the runtime's HWND */
    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    ld_rs_config();   /* the swap chain shows the scaled display buffers */
    scd.Width = ld_rs_up(width); scd.Height = ld_rs_up(height); scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = LD_SWAP_BUFFERS; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(factory->lpVtbl->CreateSwapChainForHwnd(factory, (IUnknown*)g.queue,
            (HWND)hwnd, &scd, NULL, NULL, &sc1))) {
        factory->lpVtbl->Release(factory); return -1;
    }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g.swap);
    sc1->lpVtbl->Release(sc1);
    factory->lpVtbl->Release(factory);

    /* RTV heap: [0..1] backbuffers, [2..] surface cache */
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = LD_SWAP_BUFFERS + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.rtv_heap);
    g.rtv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 i = 0; i < LD_SWAP_BUFFERS; i++) {
        g.swap->lpVtbl->GetBuffer(g.swap, i, &IID_ID3D12Resource, (void**)&g.backbuf[i]);
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.backbuf[i], NULL, rtv_handle(i));
    }

    /* upload arena */
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UPLOAD_SIZE;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.upload);
    D3D12_RANGE rr = {0, 0};
    g.upload->lpVtbl->Map(g.upload, 0, &rr, (void**)&g.upload_mapped);

    bd.Width = VERT_BUFFER_SIZE;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.vb);
    g.vb->lpVtbl->Map(g.vb, 0, &rr, (void**)&g.vb_mapped);

    bd.Width = INDEX_BUFFER_SIZE;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.ib);
    g.ib->lpVtbl->Map(g.ib, 0, &rr, (void**)&g.ib_mapped);

    bd.Width = CB_RING_BYTES;
    g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g.cb);
    g.cb->lpVtbl->Map(g.cb, 0, &rr, (void**)&g.cb_mapped);

    bd.Width = PS_CB_RING_BYTES;
    g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
        &IID_ID3D12Resource, (void**)&g.ps_cb);
    if (g.ps_cb)
        g.ps_cb->lpVtbl->Map(
            g.ps_cb, 0, &rr, (void**)&g.ps_cb_mapped);

    /* SRV heaps */
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SRV_CPU_HEAP_SLOTS; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_cpu_heap);
    hd.NumDescriptors = SRV_RING_TABLES * SRV_TABLE_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_heap);
    g.srv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* sampler heaps */
    D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    shd.NumDescriptors = SMP_CACHE_SLOTS + 1; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_cpu_heap);
    shd.NumDescriptors = SMP_RING_TABLES * SMP_TABLE_SIZE;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_heap);
    g.smp_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    /* Fail init (fall back to null present) rather than crash later if any
     * shader-visible/CPU descriptor heap didn't create -- e.g. an over-limit
     * sampler heap would return NULL here and fault in sampler_table. */
    if (!g.srv_cpu_heap || !g.srv_heap || !g.smp_cpu_heap || !g.smp_heap) {
        fprintf(stderr, "[live-draw] descriptor heap alloc failed "
                "(srv_cpu=%p srv=%p smp_cpu=%p smp=%p)\n",
                (void*)g.srv_cpu_heap, (void*)g.srv_heap,
                (void*)g.smp_cpu_heap, (void*)g.smp_heap);
        return -1;
    }
    {
        D3D12_SAMPLER_DESC def = {0};
        def.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def.MaxLOD = D3D12_FLOAT32_MAX; def.MaxAnisotropy = 1;
        g.dev->lpVtbl->CreateSampler(g.dev, &def, smp_cpu(SMP_DEFAULT));
    }

    /* depth */
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {0};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1 + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap);
    g.dsv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(
        g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    {
        D3D12_HEAP_PROPERTIES dhp = {0}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC drd = {0};
        drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        drd.Width = ld_rs_up(width); drd.Height = ld_rs_up(height); drd.DepthOrArraySize = 1;
        drd.MipLevels = 1; drd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; drd.SampleDesc.Count = 1;
        drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv = {0}; dcv.Format = drd.Format; dcv.DepthStencil.Depth = 1.0f;
        if (SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp, D3D12_HEAP_FLAG_NONE, &drd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, &IID_ID3D12Resource, (void**)&g.depth))) {
            D3D12_CPU_DESCRIPTOR_HANDLE dh;
            g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dh);
            g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.depth, NULL, dh);
        } else {
            g.depth = NULL;
        }
    }

    if (make_root_signature() != 0 ||
        make_depth_snapshot_pipeline() != 0)
        return -1;

    static const u8 white[4] = {255, 255, 255, 255};
    g.white_tex = create_texture_rgba(white, 1, 1);
    if (g.white_tex) srv_write(SRV_WHITE, g.white_tex);

    /* dispatcher + sink */
    rsx_dispatch_sink sink = {0};
    sink.user = &g;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.inline_array = sink_inline_array;
    sink.draw_index_array = sink_draw_index;
    sink.flip = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);

    if(!g_ld_debug_layer_enabled)ld_af_init();
    /* LD_ASYNC_IMMEDIATE_LABELS=1: keep async command submission but write
     * labels when the FIFO reaches them, as the synchronous path does.
     * Deferring every label to GPU completion made the Edge jobs and the
     * PPU wait a full GPU round trip per label (3 FPS). */
    { const char* il=getenv("LD_ASYNC_IMMEDIATE_LABELS");
      if(g_af.on&&!(il&&!strcmp(il,"1")))rsx_set_backend_label_hook(ld_af_label,NULL);
      else if(g_af.on)fprintf(stderr,"[async-frame] immediate labels\n"); }
    if(g_af.label_batch)rsx_set_backend_label_boundary(ld_af_batch_boundary,NULL);
    g.ready = 1;
    g.enabled = 1;
    return 0;
}

void rsx_live_draw_seed_registers(const u32* regs, u32 count)
{
    if (g.ready) rsx_dispatch_seed_registers(&g.rsx, regs, count);
}
void rsx_live_draw_seed_transform_program(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_program(&g.rsx, words, count);
}
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_constants(&g.rsx, words, count);
}

void rsx_live_draw_set_display_buffer(
    u32 buffer_id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    display_buffer_t* display = &g.display_buffers[buffer_id];
    display->location = location;
    display->offset = offset;
    display->pitch = pitch;
    display->width = width;
    display->height = height;
    display->valid = width && height;
    display->resolve_src = 0;
    fprintf(stderr,
            "[live-draw] display buffer %u = loc%u:0x%08X pitch=%u %ux%u\n",
            buffer_id, location, offset, pitch, width, height);
}

static void ld_dispatch_texture_trace(u32 method, u32 arg)
{
    static int initialized, enabled;
    static u32 wanted, alternate, count;
    static u64 seen[128];
    if (!initialized) {
        initialized = 1;
        const char* e = getenv("LD_TEX_DESCRIPTOR_TRACE");
        enabled = e && e[0];
        wanted = enabled ? (u32)strtoul(e, NULL, 16) : 0;
        e = getenv("LD_TEX_DESCRIPTOR_TRACE_ALT");
        alternate = e && e[0] ? (u32)strtoul(e, NULL, 16) : wanted;
    }
    const u32 canonical = method & 0x1FFCu;
    const int relevant = enabled && count < 128 &&
        ((canonical >= 0x1A00u && canonical <= 0x1A1Cu) || canonical == 0x1840u);
    rsx_dsp_texture before;
    if (relevant) rsx_dsp_get_texture(&g.rsx, 0, &before);
    rsx_dispatch_method(&g.rsx, method, arg);
    if (!relevant) return;
    rsx_dsp_texture after;
    rsx_dsp_get_texture(&g.rsx, 0, &after);
    if (before.offset != wanted && before.offset != alternate &&
        after.offset != wanted && after.offset != alternate) return;
    const u32 fields[] = {method, arg, before.offset, after.offset, after.location,
                          after.format, after.width, after.height, after.pitch,
                          after.remap, after.control0};
    const u64 signature = fnv1a(fields, sizeof fields, 1469598103934665603ull);
    for (u32 i = 0; i < count; i++) if (seen[i] == signature) return;
    seen[count++] = signature;
    fprintf(stderr, "[tex-desc-write] n=%u frame=%u raw_method=0x%04X method=0x%04X arg=0x%08X "
                    "fifo_get=0x%08X fifo_put=0x%08X old=%u:0x%08X new=%u:0x%08X "
                    "fmt=0x%02X size=%ux%u pitch=%u remap=0x%04X dim=%u cube=%u mips=%u enabled=%u\n",
            count, g_ld_frames, method, canonical, arg, g_ld_fifo_get, g_ld_fifo_put,
            before.location, before.offset, after.location, after.offset, after.format,
            after.width, after.height, after.pitch, after.remap, after.dimension,
            after.cubemap, after.mipmaps, after.enabled);
}
void rsx_live_draw_method(u32 method, u32 arg)
{
    /* Host-private marker from the native Edge job: NOP (0x0100) with
     * 0xED6E00cc arms cull mode cc for the next BEGIN..END group. */
    { const u32 cm = method & 0x1FFCu;
      if (cm == 0x0100u && (arg >> 16) == 0xED6Eu) g_ld_force_cull_pending = arg & 0xFFu;
      else if (cm == 0x1808u && arg) { g_ld_force_cull_active = g_ld_force_cull_pending; g_ld_force_cull_pending = 0;
          /* Async frames: submit the recorded prefix every N draw groups so the
           * GPU overlaps the CPU (LD_ASYNC_PREFIX_DRAWS, default 200, 0 = off). */
          if (g_af.on) {
              static int every = -1; static u32 since;
              if (every < 0) { const char* e = getenv("LD_ASYNC_PREFIX_DRAWS"); every = e ? atoi(e) : 200; }
              if (every > 0 && ++since >= (u32)every) { since = 0; (void)ld_af_prefix_submit(); }
          } } }
    /* Bounded ingress trace for the a010 live-vs-capture comparison.  Keep the
     * raw method (including its FIFO subchannel bits) visible: if an SPU-built
     * command list binds NV4097 on a different subchannel, feeding the raw
     * 0x2xxx-shifted method into the canonical dispatcher silently stores the
     * state in the wrong register bank. */
    static int state_trace = -1;
    static u32 state_trace_lines = 0;
    if (state_trace < 0)
        state_trace = getenv("RSX_VERTEX_STATE_TRACE") ? 1 : 0;
    if (state_trace && state_trace_lines < 512) {
        const u32 canonical = method & 0x1FFCu;
        const int vertex_texture =
            canonical >= 0x0900u && canonical < 0x0980u;
        const int vertex_format =
            canonical >= 0x1740u && canonical < 0x1780u;
        const int frequency_divider = canonical == 0x1FC0u;
        if (vertex_texture || vertex_format || frequency_divider) {
            fprintf(stderr,
                    "[rsx-vstate] frame=%u raw=0x%04X sub=%u canonical=0x%04X "
                    "arg=0x%08X\n",
                    g_ld_frames, method & 0xFFFFu, (method >> 13) & 7u,
                    canonical, arg);
            fflush(stderr);
            state_trace_lines++;
        }
    }

    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        for (;;) {
            if (g_ld_movie_mode || g_ld_host_waiting) {
                while (g_ld_host_waiting)
                    SwitchToThread();
                AcquireSRWLockExclusive(&g_ld_access_lock);
                if (!g.ready) {
                    ReleaseSRWLockExclusive(&g_ld_access_lock);
                    return;
                }
                if (g_ld_movie_mode && !composite) {
                    if (g_ld_movie_track_rsx < 0)
                        g_ld_movie_track_rsx =
                            getenv("RSX_MOVIE_TRACK") ? 1 : 0;
                    if (!g_ld_movie_track_rsx) {
                        ReleaseSRWLockExclusive(&g_ld_access_lock);
                        return;
                    }
                }
                ld_dispatch_texture_trace(method, arg);
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                return;
            }
            InterlockedIncrement(&g_ld_guest_active);
            MemoryBarrier();
            if (!g_ld_movie_mode && !g_ld_host_waiting) {
                if (g.ready)
                    ld_dispatch_texture_trace(method, arg);
                InterlockedDecrement(&g_ld_guest_active);
                return;
            }
            InterlockedDecrement(&g_ld_guest_active);
        }
    }

    if (!g.ready) return;
    if (g_ld_movie_mode) {
        if (g_ld_movie_track_rsx < 0)
            g_ld_movie_track_rsx = getenv("RSX_MOVIE_TRACK") ? 1 : 0;
        if (!g_ld_movie_track_rsx) return;
    }
    ld_dispatch_texture_trace(method, arg);
}

void rsx_live_draw_set_movie_mode(int on)
{
    static unsigned long long suppressed_at_start = 0;
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (on && rsx_live_draw_a010_probe_active()) {
        fprintf(stderr,
                "[a010-probe] END movie-mode live_frame=%u elapsed_frames=%u "
                "surfaces=%u samples=%u touched=0x%016llX\n",
                g_ld_frames, g_ld_frames - g_ld_a010_probe_start_frame,
                g.n_surfaces, g_ld_a010_probe_sample,
                (unsigned long long)g_ld_a010_probe_touched);
        fflush(stderr);
        InterlockedExchange(&g_ld_a010_probe_active, 0);
    }
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        if (on) InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 1);
        while (InterlockedCompareExchange(&g_ld_guest_active, 0, 0) != 0)
            SwitchToThread();
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (on) {
        suppressed_at_start = g_ld_stats.packets_movie;
        if (composite) ld_movie_overlay_begin();
        else g_ld_movie_mode = 1;
    } else {
        if (composite) {
            g.movie_overlay_valid = 0;
            /* Do not expose a partially rendered auth/fade surface between
             * the last host movie frame and the next clean guest scene. */
            ld_flush(LD_FLUSH_MOVIE);
            const float black[4] = {0, 0, 0, 1};
            for (u32 i = 0; i < g.n_surfaces; i++) {
                g.list->lpVtbl->ClearRenderTargetView(
                    g.list, rtv_handle(LD_SWAP_BUFFERS + i), black, 0, NULL);
                ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
            }
            if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
            ld_movie_reset_rings();
            InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 0);
            fprintf(stderr, "[movie-ui] compositor disarmed after %llu guest overlays\n",
                    (unsigned long long)g.movie_overlay_frames);
            fflush(stderr);
        } else {
            g_ld_movie_mode = 0;
        }
        if (!composite && g_ld_movie_track_rsx > 0) {
            fprintf(stderr,
                    "[live-draw] movie handoff: tracked RSX state, suppressed %llu guest draw packets\n",
                    g_ld_stats.packets_movie - suppressed_at_start);
            fflush(stderr);
        }
#if !defined(YZ_PERF_CLEAN)
        if (g.vertex_diag_dir[0]) {
            InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
            InterlockedExchange(&g_ld_diag_post_movie_pending, 1);
        }
#endif
    }
    ld_vertex_diag_emit(on ? "movie-begin" : "movie-end", 0);
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

u32 rsx_live_draw_get_frames(void) { return g_af.on?g_af.complete_frames:g_ld_frames; }

void rsx_live_draw_set_fifo_position(u32 get, u32 put)
{
#if defined(YZ_PERF_CLEAN)
    (void)get;
    (void)put;
#else
    g_ld_fifo_get = get;
    g_ld_fifo_put = put;
#endif
}

static int ld_guest_write_overlaps(u32 a, u64 a_bytes, u32 b, u64 b_bytes)
{
    /* Half-open byte intervals; widening before addition prevents a high
     * guest address from wrapping around and spuriously aliasing offset0. */
    return a_bytes && b_bytes && (u64)a < (u64)b + b_bytes &&
           (u64)b < (u64)a + a_bytes;
}
void rsx_live_draw_note_guest_write(u32 location, u32 offset, u32 bytes)
{
    if (!g.ready || !bytes || location > 1u) return;
    for (u32 i = 0; i < g.n_textures; i++) {
        texcache_t* e = &g.textures[i];
        if (e->location != location) continue;
        rsx_dsp_texture t = {0};
        t.location = e->location; t.offset = e->offset; t.format = e->format;
        t.width = e->width; t.height = e->height; t.pitch = e->pitch;
        t.cubemap = e->cubemap; t.mipmaps = e->mipmaps; t.dimension = e->dimension;
        const u32 span = texture_source_span(&t);
        if (!span || ld_guest_write_overlaps(offset, bytes, e->offset, span))
            e->last_hash_frame = ~g_ld_frames;
    }
    for (u32 i = 0; i < g.n_vtex; i++) {
        vtexcache_t* e = &g.vtex[i];
        if (e->location != location) continue;
        rsx_dsp_vertex_texture t = {0};
        t.format = e->format;
        vertex_texture_format_t format;
        const int valid = vertex_texture_format(&t, &format);
        const u64 pitch = e->pitch ? e->pitch :
                          (u64)e->width * (valid ? format.bytes_per_texel : 0);
        const u64 span = pitch * e->height;
        if (!valid || !span || ld_guest_write_overlaps(offset, bytes, e->offset, span))
            e->last_hash_frame = ~g_ld_frames;
    }
}
void rsx_live_draw_note_inline_transfer(u32 dma, u32 offset, u32 value)
{
#if defined(YZ_PERF_CLEAN)
    (void)dma;
    (void)offset;
    (void)value;
#else
    if (!g.ready || !yz_frontier_trace_is_armed())
        return;
    const u32 location = dma == 0xFEED0000u ? 0u :
                         dma == 0xFEED0001u ? 1u : UINT32_MAX;
    if (location == UINT32_MAX)
        return;
    for (u32 i = 0; i < g.n_surfaces; ++i) {
        surface_t* surface = &g.surfaces[i];
        const u64 begin = surface->offset;
        const u64 end = begin + (u64)surface->w * surface->h * ld_surface_texel_bytes(surface->fmt);
        if (surface->location != location || offset < begin || offset >= end)
            continue;
        surface->last_guest_blit_generation = ++g_ld_guest_blit_generation;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_GUEST_BLIT, i, location,
            surface->last_guest_blit_generation, offset, value,
            g_ld_fifo_get, g_ld_fifo_put, surface->last_write_generation);
    }
#endif
}

void* rsx_live_draw_get_present_thread_handle(void)
{
#if defined(YZ_PPU_SAMPLE)
    return (void*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ld_present_thread_handle, NULL, NULL);
#else
    return NULL;
#endif
}

static u32 g_ld_last_frame_draws = 0;
/* Draws in the last COMPLETED frame (title-bar telemetry: distinguishes
 * "presenting fresh content" from "flipping a static image" -- the dead
 * journal-consumer limp state renders ~0 draws/frame while flips tick). */
u32 rsx_live_draw_get_last_draws(void) { return g_ld_last_frame_draws; }

double rsx_live_draw_get_present_fps(void)
{
    const u64 total = g_ld_present_total;
    if (total < 2u || g_ld_qpc_frequency <= 0)
        return 0.0;

    const u64 available =
        total < LD_PRESENT_RING_CAP ? total : LD_PRESENT_RING_CAP;
    const ld_present_sample* newest =
        &g_ld_present_ring[(total - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    if (newest->present_id != total)
        return 0.0;

    const ld_present_sample* oldest = newest;
    u64 intervals = 0;
    for (u64 back = 1u; back < available; ++back) {
        const u64 present_id = total - back;
        const ld_present_sample* candidate =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (candidate->present_id != present_id)
            break;
        oldest = candidate;
        intervals = back;
        const double elapsed =
            (double)(newest->qpc - oldest->qpc) /
            (double)g_ld_qpc_frequency;
        /*
         * Keep at least a 15-second view and about 30 intervals when possible;
         * cap at 30 seconds so sub-1-FPS operation still updates usefully.
         */
        if (elapsed >= 30.0 || (elapsed >= 15.0 && intervals >= 30u))
            break;
    }

    const LONGLONG ticks = newest->qpc - oldest->qpc;
    return intervals != 0u && ticks > 0
        ? (double)intervals * (double)g_ld_qpc_frequency / (double)ticks
        : 0.0;
}

void rsx_live_draw_dump_present_samples(void)
{
    ld_present_measure_dump();
}

/* Present a host-decoded RGBA8 frame to the window: copy it straight into the
 * swap-chain backbuffer (both R8G8B8A8_UNORM at the swap size) and Present.
 * The frame is clamped to the backbuffer size. Call from a single thread with
 * movie mode on (so guest draws don't touch g.list). */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h)
{
    if(!ld_af_drain())return;
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (!g.ready || !rgba) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }
    if (composite && !ld_movie_overlay_ensure()) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
        return;
    }
    if (w > g.width)  w = g.width;
    if (h > g.height) h = g.height;
    const u32 pitch = (w * 4 + 255) & ~255u;          /* D3D12 copy pitch align */
    if ((UINT64)pitch * h > UPLOAD_SIZE) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }

    extern float cellAvconfExt_display_gamma(void);
    const float movie_gamma = cellAvconfExt_display_gamma();
    u8 gamma_lut[256];
    if (movie_gamma != 1.0f)
        for (u32 i = 0; i < 256; i++)
            gamma_lut[i] = (u8)(powf((float)i / 255.0f, movie_gamma) * 255.0f + 0.5f);
    u8* host_upload = composite ? g.movie_upload_mapped : g.upload_mapped;
    for (u32 y = 0; y < h; y++) {
        u8* dstrow = host_upload + (size_t)y * pitch;
        memcpy(dstrow, rgba + (size_t)y * w * 4, (size_t)w * 4);
        if (composite && g.movie_overlay_valid && g.movie_overlay_rgba &&
            g.movie_overlay_mask) {
            const u8* ov = g.movie_overlay_rgba + (size_t)y * g.width * 4;
            const u8* mask =
                g.movie_overlay_mask + (size_t)y * g.width;
            for (u32 x = 0; x < w; x++, ov += 4) {
                const int coverage = mask[x];
                if (!coverage) continue;
                u8* out = dstrow + (size_t)x * 4;
                if (coverage == 1) {
                    out[0] = (u8)((out[0] * 64 + 127) / 255);
                    out[1] = (u8)((out[1] * 64 + 127) / 255);
                    out[2] = (u8)((out[2] * 64 + 127) / 255);
                    continue;
                }
                const int white = ov[0] > ov[1]
                    ? (ov[0] > ov[2] ? ov[0] : ov[2])
                    : (ov[1] > ov[2] ? ov[1] : ov[2]);
                for (int c = 0; c < 3; c++) {
                    const int v =
                        (white * coverage +
                         out[c] * (255 - coverage) + 127) / 255;
                    out[c] = (u8)v;
                }
                out[3] = 255;
            }
        }
        /* Calibrate the composed movie once, leaving alpha and source assets
         * untouched. Scene/game UI calibration happens only at final present. */
        if (movie_gamma != 1.0f)
            for (u32 x = 0; x < w; x++)
                for (u32 c = 0; c < 3; c++)
                    dstrow[x * 4 + c] = gamma_lut[dstrow[x * 4 + c]];
    }

    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    if(!ld_af_claim(bbi))return;
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = composite ? g.movie_upload : g.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush(LD_FLUSH_MOVIE_PRESENT);
    {
        const HRESULT present_hr = ld_swap_present(g_ld_frames, 1u);
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames);
        else {
            fprintf(stderr,
                    "[d3d-fail] movie Present hr=0x%08lX frame=%u\n",
                    (unsigned long)present_hr, g_ld_frames);
            ld_dump_dred("MoviePresent", present_hr);
            g.ready = 0;
        }
    }
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

/* Env-gated (RSX_DUMP) framebuffer dump: read the current color surface back
 * and write a binary PPM. Self-contained -- creates + releases its own readback
 * buffer, so no init/struct changes. Uses g.list which ld_flush leaves open. */
/* Readback texel -> 8-bit RGB.  This title renders HDR into FP16 targets, so a
 * dump has to tone it down rather than reinterpret the bytes.  Reinhard keeps
 * the whole range visible without pretending to match the guest tonemapper. */
static void ld_dump_texel_rgb(const u8* p, int fp16, u8* out)
{
    if (!fp16) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    for (int c = 0; c < 3; c++) {
        const u16 h = (u16)(p[c * 2] | (p[c * 2 + 1] << 8));
        const int ex = (h >> 10) & 0x1F, ma = h & 0x3FF;
        float f;
        if (ex == 0)       f = (float)ma * (1.0f / 16384.0f) * (1.0f / 64.0f);
        else if (ex == 31) f = 65504.0f;
        else { f = 1.0f + (float)ma / 1024.0f;
               int k = ex - 15;
               while (k > 0) { f *= 2.0f; k--; }
               while (k < 0) { f *= 0.5f; k++; } }
        if (h & 0x8000) f = 0.0f;
        f = f / (1.0f + f);                          /* Reinhard */
        out[c] = (u8)(f * 255.0f + 0.5f);
    }
}
static void ld_dump_surface_texel_rgb(const u8* p, DXGI_FORMAT format, u8* out)
{
    if (format == DXGI_FORMAT_R8_UNORM) {
        out[0] = out[1] = out[2] = p[0];
    } else if (format == DXGI_FORMAT_R32_FLOAT ||
               format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        for (u32 c = 0; c < 3; ++c) {
            float value;
            memcpy(&value, p + (format == DXGI_FORMAT_R32_FLOAT ? 0 : c * 4), 4);
            if (!(value >= 0.0f)) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            out[c] = (u8)(value * 255.0f + 0.5f);
        }
    } else {
        ld_dump_texel_rgb(p, format == DXGI_FORMAT_R16G16B16A16_FLOAT, out);
    }
}

/* --record: when set, ld_dump_surface_ppm appends to this ffmpeg pipe instead
 * of opening `path`, and skips its per-dump diagnostics (every frame goes
 * through here while recording). */
static FILE* g_ld_record_to;

typedef struct {
    u64 finite_count, nonzero_count, nan_count, inf_count;
    float minimum, maximum;
} ld_float_channel_stats;

static void ld_float_stats_add(ld_float_channel_stats* stats, const u8* bytes)
{
    u32 bits;
    memcpy(&bits, bytes, 4);
    const u32 exponent = bits & 0x7F800000u;
    const u32 mantissa = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u) {
        if (mantissa) stats->nan_count++;
        else stats->inf_count++;
        return;
    }
    float value;
    memcpy(&value, bytes, 4);
    if (!stats->finite_count || value < stats->minimum) stats->minimum = value;
    if (!stats->finite_count || value > stats->maximum) stats->maximum = value;
    stats->finite_count++;
    if (bits & 0x7FFFFFFFu) stats->nonzero_count++;
}

/* Inspect the already-fenced readback. No additional GPU submission, format
 * conversion or display substitution is involved. Packed sidecars contain host
 * little-endian texels with row padding removed, not guest-endian memory. */
static void ld_surface_readback_diagnostics(const char* path, const surface_t* surface,
                                           const u8* pixels, u32 row_pitch)
{
    if (!path || !pixels || !surface) return;
    const u32 channels = surface->fmt == DXGI_FORMAT_R32_FLOAT ? 1u :
        surface->fmt == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4u : 0u;
    ld_float_channel_stats stats[4] = {0};
    const u32 texel = ld_surface_texel_bytes(surface->fmt);
    if (channels) {
        for (u32 y = 0; y < surface->h; ++y)
            for (u32 x = 0; x < surface->w; ++x)
                for (u32 c = 0; c < channels; ++c)
                    ld_float_stats_add(&stats[c], pixels + (size_t)y * row_pitch +
                                       (size_t)x * texel + c * 4u);
        for (u32 c = 0; c < channels; ++c)
            fprintf(stderr, "[surface-float] %s channel=%u finite=%llu nonzero=%llu "
                    "nan=%llu inf=%llu min=%.9g max=%.9g\n", path, c,
                    (unsigned long long)stats[c].finite_count,
                    (unsigned long long)stats[c].nonzero_count,
                    (unsigned long long)stats[c].nan_count,
                    (unsigned long long)stats[c].inf_count,
                    (double)stats[c].minimum, (double)stats[c].maximum);
    }
    const char* enabled = getenv("LD_RAW_SURFACE_DUMP");
    if (!enabled || atoi(enabled) == 0) return;
    char raw_path[MAX_PATH * 2], json_path[MAX_PATH * 2];
    if (snprintf(raw_path, sizeof(raw_path), "%s.raw", path) >= (int)sizeof(raw_path) ||
        snprintf(json_path, sizeof(json_path), "%s.json", path) >= (int)sizeof(json_path)) return;
    FILE* raw = fopen(raw_path, "wb");
    if (!raw) return;
    const size_t packed_pitch = (size_t)surface->w * texel;
    int ok = 1;
    for (u32 y = 0; y < surface->h; ++y)
        if (fwrite(pixels + (size_t)y * row_pitch, 1, packed_pitch, raw) != packed_pitch) { ok = 0; break; }
    if (fclose(raw) != 0) ok = 0;
    FILE* json = fopen(json_path, "wb");
    if (!json) return;
    fprintf(json, "{\"width\":%u,\"height\":%u,\"dxgi_format\":%u,"
            "\"bytes_per_texel\":%u,\"packed_row_bytes\":%llu,"
            "\"host_byte_order\":\"little\",\"location\":%u,\"offset\":%u,"
            "\"complete\":%s,\"float_channels\":[", surface->w, surface->h,
            (u32)surface->fmt, texel, (unsigned long long)packed_pitch,
            surface->location, surface->offset, ok ? "true" : "false");
    for (u32 c = 0; c < channels; ++c) {
        if (c) fputc(',', json);
        fprintf(json, "{\"finite\":%llu,\"nonzero_finite\":%llu,\"nan\":%llu,\"inf\":%llu,"
                "\"minimum\":", (unsigned long long)stats[c].finite_count,
                (unsigned long long)stats[c].nonzero_count,
                (unsigned long long)stats[c].nan_count, (unsigned long long)stats[c].inf_count);
        if (stats[c].finite_count) fprintf(json, "%.9g,\"maximum\":%.9g}",
            (double)stats[c].minimum, (double)stats[c].maximum);
        else fputs("null,\"maximum\":null}", json);
    }
    fputs("]}\n", json);
    fclose(json);
}

static u64 ld_dump_resource_ppm(const char* path, const surface_t* surface,
                                D3D12_RESOURCE_STATES input_state)
{
    ID3D12Resource* rt = surface ? surface->tex : NULL;
    if (!rt) return UINT64_MAX;
    const u32 width = surface->hw ? surface->hw : surface->w;    /* host size (render scale) */
    const u32 height = surface->hh ? surface->hh : surface->h;
    if (!width || !height) return UINT64_MAX;
    const u32 texel = ld_surface_texel_bytes(surface->fmt);
    const u32 pitch = (width * texel + 255) & ~255u;             /* 256-align */
    const UINT64 rb_size = (UINT64)pitch * height;

    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rb_size;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)))
        return UINT64_MAX;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = input_state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = surface->fmt;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = input_state;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush(LD_FLUSH_READBACK);                   /* copy completes on the GPU */

    u64 nonblack = UINT64_MAX;
    u64 fingerprint = 1469598103934665603ull;
    u8* px = NULL; D3D12_RANGE rr = {0, (SIZE_T)rb_size};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, (void**)&px))) {
        nonblack = 0;
        if (!g_ld_record_to)
            ld_surface_readback_diagnostics(path, surface, px, pitch);
        if (!g_ld_record_to) {   /* raw texels, to tell an FP16 target from 8-bit data read as one */
            const u8* r0 = px + (SIZE_T)(height / 4) * pitch + (width / 4) * texel;
            fprintf(stderr, "[dump-raw] %s fmt=%d texel=%u bytes:", path, (int)surface->fmt, texel);
            for (u32 bi = 0; bi < 16; bi++) fprintf(stderr, " %02X", r0[bi]);
            fprintf(stderr, "\n");
        }
        FILE* f = g_ld_record_to ? g_ld_record_to : fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_surface_texel_rgb(pixel, surface->fmt, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                    fwrite(rgb, 1, 3, f);  /* RGBA->RGB */
                }
            }
            if (texel == 4u) {   /* alpha lane: masks (glow) may live there only */
                u64 an = 0; unsigned amax = 0;
                for (u32 y = 0; y < height; y++)
                    for (u32 x = 0; x < width; x++) {
                        const u8 a = px[(SIZE_T)y * pitch + x * 4u + 3u];
                        if (a) an++; if (a > amax) amax = a;
                    }
                fprintf(stderr, "[live-draw] %s alpha nonzero=%llu max=%u" "\n", path, (unsigned long long)an, amax);
            }
            if (f != g_ld_record_to) {
                fclose(f);
                fprintf(stderr, "[live-draw] wrote %s\n", path);
            }
        } else {
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_surface_texel_rgb(pixel, surface->fmt, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                }
            }
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
        g_ld_last_dump_fingerprint = fingerprint;
    }
    rb->lpVtbl->Release(rb);
    return nonblack;
}

static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface)
{
    return ld_dump_resource_ppm(path, surface, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

/* Snapshot the actual depth plane through the renderer's existing compute
 * conversion, then read its R32 resource in its tracked state. The metadata
 * adapter neither allocates a colour target nor participates in presentation. */
static void ld_dump_tracked_depth(const char* directory)
{
    for (u32 i = 0; i < g.n_zdepths; ++i) {
        zdepth_t* z = &g.zdepths[i];
        /* LD_DEPTH_DUMP_ALL=1: every tracked zeta, not only the two this was
         * written for. */
        { static int all = -1; if (all < 0) all = getenv("LD_DEPTH_DUMP_ALL") ? 1 : 0;
          if (!all && z->offset != 0x03A70000u && z->offset != 0x036B0000u) continue; }
        if (!zdepth_snapshot(i + 1u) || !z->snapshot_valid) {
            fprintf(stderr, "[depth-dump] unavailable %u:0x%08X had_write=%d draws=%llu\n",
                z->location, z->offset, z->had_write, (unsigned long long)z->draws);
            continue;
        }
        surface_t metadata = {0};
        metadata.tex = z->snapshot;
        metadata.fmt = DXGI_FORMAT_R32_FLOAT;
        metadata.w = z->snapshot_w; metadata.h = z->snapshot_h;
        metadata.location = z->location; metadata.offset = z->offset;
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\f%06u_depth_%02u_%08X_%ux%u.ppm",
            directory, g_ld_frames, i, z->offset, metadata.w, metadata.h);
        const u64 nonblack = ld_dump_resource_ppm(path, &metadata, z->snapshot_state);
        fprintf(stderr, "[depth-dump] %u:0x%08X %ux%u valid=%d draws=%llu "
            "depth_both=%llu nonblack=%llu\n", z->location, z->offset,
            metadata.w, metadata.h, z->snapshot_valid,
            (unsigned long long)z->draws, (unsigned long long)z->depth_both_draws,
            (unsigned long long)nonblack);
        if (z->packed_snapshot && z->packed_generation == z->snapshot_generation) {
            metadata.tex = z->packed_snapshot;
            metadata.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            metadata.w = z->packed_w; metadata.h = z->packed_h;
            snprintf(path, sizeof(path), "%s\\f%06u_depthpacked_%02u_%08X_%ux%u.ppm",
                directory, g_ld_frames, i, z->offset, metadata.w, metadata.h);
            ld_dump_resource_ppm(path, &metadata, z->packed_state);
        }
    }
}

#if !defined(YZ_PERF_CLEAN)
static void ld_parity_capture_surface(
    const char* directory, const char* phase, u32 serial,
    u32 buffer_id, u32 target)
{
    if (!directory || !directory[0] || target >= g.n_surfaces)
        return;
    surface_t* surface = &g.surfaces[target];
    char path[MAX_PATH * 2];
    snprintf(
        path, sizeof(path),
        "%s\\%s_%03u_flip_%llu_buf_%u_surface_%u_write_%u_kind_%u.ppm",
        directory, phase, serial,
        (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->last_write_generation,
        surface->last_write_kind);
    const u64 nonblack = ld_dump_surface_ppm(path, surface);
    fprintf(
        stderr,
        "[parity-capture] phase=%s serial=%u flip=%llu buffer=%u "
        "surface=%u resource=%u write=%u kind=%u draw=%u clear=%u "
        "copy=%u blit=%u resolve=%u other=%u guest_blit=%u "
        "present_copy=%u get=%08X put=%08X cb=%u vb=%u srv=%u "
        "groups=%llu drop_ring=%llu cb_recycles=%llu frame_cb_recycles=%u "
        "hash=%016llX nonblack=%s%llu path=%s\n",
        phase, serial, (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->resource_serial,
        surface->last_write_generation, surface->last_write_kind,
        surface->last_draw_generation, surface->last_clear_generation,
        surface->last_copy_generation, surface->last_blit_generation,
        surface->last_resolve_generation, surface->last_other_generation,
        surface->last_guest_blit_generation,
        surface->last_present_copy_generation,
        g_ld_fifo_get, g_ld_fifo_put, g.cb_used, g.vb_used,
        g.srv_ring_used,
        (unsigned long long)g_ld_stats.groups_executed,
        (unsigned long long)g_ld_stats.group_drop_ring,
        (unsigned long long)g_ld_vertex_constant_ring_recycles,
        g_ld_frame_vertex_constant_ring_recycles,
        (unsigned long long)g_ld_last_dump_fingerprint,
        nonblack == UINT64_MAX ? "unknown:" : "",
        (unsigned long long)(nonblack == UINT64_MAX ? 0 : nonblack),
        path);
    fflush(stderr);
}
#endif

int rsx_live_draw_debug_dump_surface(
    u32 location, u32 offset, const char* path)
{
    if (!g.ready || !path || !path[0]) return -1;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* surface = &g.surfaces[i];
        if (surface->location != location || surface->offset != offset)
            continue;
        const u64 nonblack = ld_dump_surface_ppm(path, surface);
        if (nonblack == UINT64_MAX) return -1;
        fprintf(stderr,
                "[live-replay-boundary] surface=%u:%08X index=%u "
                "size=%ux%u fmt=%d nonblack=%llu path=%s\n",
                location, offset, i, surface->w, surface->h, (int)surface->fmt,
                (unsigned long long)nonblack, path);
        fflush(stderr);
        return 0;
    }
    fprintf(stderr,
            "[live-replay-boundary] missing surface=%u:%08X path=%s\n",
            location, offset, path);
    return 1;
}

static void ld_vertex_diag_emit(const char* reason, int dump_surface)
{
    if (!g.ready || !g.vertex_diag_dir[0])
        return;
    rsx_dsp_surface state_surface;
    rsx_dsp_get_surface(&g.rsx, &state_surface);
    u32 current = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == state_surface.color_location[0] &&
            g.surfaces[i].offset == state_surface.color_offset[0]) {
            current = i;
            break;
        }
    }
    const u32 presented = g_ld_last_present_target;
    const surface_t* presented_surface =
        presented < g.n_surfaces ? &g.surfaces[presented] : NULL;
    char attrs[80];
    size_t used = 0;
    attrs[0] = '\0';
    for (u32 attr = 0; attr < 16; attr++) {
        if (!(g_ld_current_pso.input_mask & (1u << attr)))
            continue;
        const int written = snprintf(
            attrs + used, sizeof(attrs) - used,
            "%s%u", used ? "," : "", attr);
        if (written < 0 || (size_t)written >= sizeof(attrs) - used)
            break;
        used += (size_t)written;
    }
    const UINT64 d3d_messages = g_ld_info_queue
        ? g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue)
        : 0;
#if defined(YZ_PERF_PROFILE)
    const u64 pso_lookups = g_ld_profile.total.pso_lookups;
    const u64 pso_hits = g_ld_profile.total.pso_hits;
    const u64 pso_misses = g_ld_profile.total.pso_misses;
    const u64 pso_full = g_ld_profile.total.pso_full;
#else
    const u64 pso_lookups = 0, pso_hits = 0, pso_misses = 0, pso_full = 0;
#endif
    fprintf(stderr,
            "[rsx-vertex-state] reason=%s mode=%s frame=%u "
            "flip{requested=%llu last_req=%u consumed=%llu last_cons=%u} "
            "surface{present=%u off=0x%08X size=%ux%u current=%u} "
            "draw{packets=%llu groups=%llu exec=%llu "
            "drop_fetch=%llu drop_pso=%llu drop_ring=%llu} "
            "pso{look=%llu hit=%llu miss=%llu full=%llu "
            "cached=%u capacity=%u} "
            "vp{hash=%016llX start=%u instrs=%u mask=0x%04X "
            "attrs=[%s] stride=%u packed=%u} "
            "d3d{debug=%d queued_messages=%llu}\n",
            reason ? reason : "unknown", ld_vertex_mode_name(), g_ld_frames,
            (unsigned long long)g_ld_flip_requested,
            g_ld_last_requested_buffer,
            (unsigned long long)g_ld_flip_consumed,
            g_ld_last_consumed_buffer,
            presented,
            presented_surface ? presented_surface->offset : 0,
            presented_surface ? presented_surface->w : 0,
            presented_surface ? presented_surface->h : 0,
            current,
            (unsigned long long)g_ld_stats.packets_seen,
            (unsigned long long)g_ld_stats.groups_seen,
            (unsigned long long)g_ld_stats.groups_executed,
            (unsigned long long)g_ld_stats.group_drop_fetch,
            (unsigned long long)g_ld_stats.group_drop_pso,
            (unsigned long long)g_ld_stats.group_drop_ring,
            (unsigned long long)pso_lookups,
            (unsigned long long)pso_hits,
            (unsigned long long)pso_misses,
            (unsigned long long)pso_full,
            g.n_psos, MAX_PSOS,
            (unsigned long long)g_ld_current_pso.vp_hash,
            g_ld_current_pso.vp_start,
            g_ld_current_pso.vp_instrs,
            g_ld_current_pso.input_mask,
            attrs,
            g_ld_current_pso.input_stride,
            g_ld_current_pso.packed_offsets,
            g_ld_debug_layer_enabled,
            (unsigned long long)d3d_messages);
    if (dump_surface && presented_surface && g.vertex_diag_dir[0]) {
        char path[MAX_PATH * 2];
        snprintf(
            path, sizeof(path), "%s\\last_present_surface.ppm",
            g.vertex_diag_dir);
        const u64 nonblack =
            ld_dump_surface_ppm(path, presented_surface);
        fprintf(stderr,
                "[rsx-vertex-surface] mode=%s target=%u "
                "pixels=%llu nonblack=%s%llu path=%s\n",
                ld_vertex_mode_name(), presented,
                (unsigned long long)presented_surface->w *
                    presented_surface->h,
                nonblack == UINT64_MAX ? "unknown:" : "",
                (unsigned long long)(
                    nonblack == UINT64_MAX ? 0 : nonblack),
                path);
    }
    fflush(stderr);
}

void rsx_live_draw_present(u32 buffer_id)
{
    { static u32 last; if (g_ld_frames - last >= 300u) { last = g_ld_frames;
        fprintf(stderr, "[vcache] hits=%llu misses=%llu verify_bad=%llu bytes=%llu\n",
                (unsigned long long)g_vcache_hits, (unsigned long long)g_vcache_misses,
                (unsigned long long)g_vcache_verify_bad, (unsigned long long)g_vcache_bytes);
        fprintf(stderr, "[cube-scan] cached=%llu scanned=%llu owned=%llu\n",
                (unsigned long long)g_ld_cube_neg_hits, (unsigned long long)g_ld_cube_scans,
                (unsigned long long)g_ld_cube_owned);
        fprintf(stderr, "[icache] hits=%llu misses=%llu bytes=%llu\n",
                (unsigned long long)g_icache_hits, (unsigned long long)g_icache_misses,
                (unsigned long long)g_icache_bytes); } }
   
    if (!g.ready) return;
    ld_ww_update();

    /* LD_SURF_DUMP=<dir>: dump EVERY live surface once, with its non-black
     * count, at LD_SURF_DUMP_FRAME (default 1200).
     *
     * LD_FRAME_DUMP only reads back the presented surface, which answers "is
     * the screen black" (it is: nonblack=0 on every frame) but not WHERE the
     * pixels stop. ps1_netemu composites PS1 VRAM into a 720x512 surface and
     * then upscales into the presented 1280x720 one, so the interesting
     * question is which of those is already empty. */
    { static int done = 0; const char* sd = getenv("LD_SURF_DUMP");
      u32 at = 1200; { const char* fe = getenv("LD_SURF_DUMP_FRAME");
                       if (fe) at = (u32)strtoul(fe, 0, 0); }
      /* LD_SURF_DUMP_ON_VRAM=1 waits for PS1 VRAM to hold a drawn frame instead
       * of firing at a frame number. Needs PS1_PC=1, which is what samples it. */
      int gate;
      { static int onv = -1;
        if (onv < 0) onv = getenv("LD_SURF_DUMP_ON_VRAM") ? 1 : 0;
        gate = onv ? (g_ld_ps1_vram_ready != 0) : (g_ld_frames >= at);
        /* LD_SURF_DUMP_GATE=<file>: fire when <file> appears instead. */
        static const char* gf = (const char*)-1;
        if (gf == (const char*)-1) gf = getenv("LD_SURF_DUMP_GATE");
        /* Re-arms when the file goes away, so each appearance dumps once. */
        if (gf && sd) { FILE* f = fopen(gf, "rb"); gate = f != NULL; if (f) fclose(f);
                        if (!gate) done = 0; } }
      if (sd && sd[0] && !done && gate) {
          done = 1;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\f%06u_surf_%02u_%08X_%ux%u.ppm",
                       sd, g_ld_frames, i, g.surfaces[i].offset,
                       g.surfaces[i].w, g.surfaces[i].h);
              const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[i]);
              fprintf(stderr, "[surf-dump] slot=%u %u:0x%08X %ux%u nonblack=%llu"
                              " draw_gen=%u clear_gen=%u\n",
                      i, g.surfaces[i].location, g.surfaces[i].offset,
                      g.surfaces[i].w, g.surfaces[i].h,
                      (unsigned long long)(nb == UINT64_MAX ? 0 : nb),
                      g.surfaces[i].last_draw_generation,
                      g.surfaces[i].last_clear_generation);
          }
          ld_dump_tracked_depth(sd);
      } }
    /* LD_FRAME_DUMP=<dir> [+ LD_FRAME_DUMP_EVERY=<n>, default 300]: write the
     * PRESENTED surface to a .ppm every n flips. The engine could already dump
     * a surface, but only at shutdown or behind the parity harness -- neither
     * reachable when a title is simply left running, which is exactly when you
     * want to see what is on screen. PrintWindow and CopyFromScreen both come
     * back white for a D3D12 swapchain, so a readback here is the only way to
     * answer "what is it actually drawing?". */
    /* The dump itself runs once the presented surface is known (below): a
     * title that scans out through cellResc never matches a registered display
     * buffer and presents the fallback surface, so matching here dumped nothing. */
    static int dump_every = -1; static const char* dump_dir; static unsigned dump_n;
    if (dump_every < 0) { dump_dir = getenv("LD_FRAME_DUMP");
                          const char* e = getenv("LD_FRAME_DUMP_EVERY");
                          dump_every = (dump_dir && dump_dir[0]) ? (e ? atoi(e) : 300) : 0;
                          if (dump_every <= 0 && dump_dir && dump_dir[0]) dump_every = 300; }
    /* LD_FRAME_DUMP_FROM=<frame>: start dumping there (dense captures of a
     * late scene, e.g. frames for a GIF, without thousands of early files). */
    static long dump_from = -1;
    if (dump_from < 0) { const char* e = getenv("LD_FRAME_DUMP_FROM"); dump_from = e ? atol(e) : 0; }
    const int periodic_eligible = dump_every > 0 && (long)g_ld_frames >= dump_from;
    const int periodic_dump = periodic_eligible && (dump_n++ % (unsigned)dump_every) == 0;
    static const char* dump_request_path = (const char*)-1;
    if (dump_request_path == (const char*)-1)
        dump_request_path = getenv("LD_FRAME_DUMP_REQUEST_FILE");
    int dump_requested = 0;
    static ULONGLONG dump_request_checked;   /* polled at most 4 times a second */
    if (dump_request_path && dump_request_path[0] && dump_dir && dump_dir[0] &&
        GetTickCount64() - dump_request_checked > 250) {
        dump_request_checked = GetTickCount64();
        FILE* request = fopen(dump_request_path, "rb");
        if (request) { dump_requested = fgetc(request) != EOF; fclose(request); }
    }
    /* Preserve the periodic cadence and its existing numeric filenames. A
     * forced request before the periodic FROM gate uses its own namespace. */
    static unsigned early_request_n;
    const int dump_now = periodic_dump || dump_requested;
    /* A flip names a registered display buffer. The current color target may
     * be an offscreen shadow/postprocess surface at that instant; copying it
     * caused a010 to present black despite executing the scene's draws. */
    u32 target = LD_INVALID_SURFACE;
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const display_buffer_t* display = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location == display->location &&
                g.surfaces[i].offset == display->offset &&
                g.surfaces[i].w == display->width &&
                g.surfaces[i].h == display->height &&
                (!g.surfaces[i].pitch || g.surfaces[i].pitch == display->pitch)) {
                target = i;
                break;
            }
        }
    }
    if (target == LD_INVALID_SURFACE && buffer_id < 8 &&
        g.display_buffers[buffer_id].valid && g.display_buffers[buffer_id].resolve_src > 0 &&
        (u32)g.display_buffers[buffer_id].resolve_src <= g.n_surfaces)
        target = (u32)g.display_buffers[buffer_id].resolve_src - 1;   /* see note_resolve */
    if (target == LD_INVALID_SURFACE) {
        target = current_surface();
        static u32 fallback_logs = 0;
        if (PS3_LOG_FIRST(fallback_logs, 32))
            fprintf(stderr,
                    "[live-draw] flip %u has no registered/rendered scanout; "
                    "falling back to current surface %u\n",
                    buffer_id, target);
    }
    if (target == LD_INVALID_SURFACE) {
        if(g_af.on){ld_af_blank_present();return;}
        fprintf(stderr, "[live-draw] frame present skipped: no color surface\n");
        return;
    }
    g_ld_last_present_target = target;
    ld_consumer_surface_event("present_target_selected",target,LD_INVALID_SURFACE);
    if (dump_now) {
        char path[MAX_PATH * 2];
        if (dump_requested && !periodic_eligible)
            snprintf(path, sizeof(path), "%s\\frame_request_%06u.ppm", dump_dir, early_request_n++);
        else
            snprintf(path, sizeof(path), "%s\\frame_%06u.ppm", dump_dir, dump_n - 1u);
        const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[target]);
        fprintf(stderr, "[frame-dump] %s %ux%u nonblack=%llu\n", path,
                g.surfaces[target].w, g.surfaces[target].h,
                (unsigned long long)(nb == UINT64_MAX ? 0 : nb));
        fflush(stderr);
        if (dump_requested && nb != UINT64_MAX) {
            FILE* saved = fopen(path, "rb");
            const int output_exists = saved && fgetc(saved) != EOF;
            if (saved) fclose(saved);
            FILE* request = output_exists ? fopen(dump_request_path, "wb") : NULL;
            if (request) {
                fclose(request);
                fprintf(stderr, "[frame-dump-request] acknowledged %s frame=%u\n", dump_request_path, g_ld_frames);
            }
        }
    }
    /* LD_RECORD=<out.mp4> (boot_main's --record): every presented frame, as
     * PPM, into an ffmpeg pipe -- ffmpeg's image2pipe reads concatenated PPMs
     * as a stream. Fragmented MP4 so a run that is killed rather than exiting
     * still leaves a playable file. Frames are stamped at a fixed 30 fps, so
     * playback speed is only right when the title presents at 30.
     * ponytail: one blocking GPU readback per frame; a ring of readback
     * buffers if recording ever has to run at full speed. */
    { static FILE* rec = (FILE*)-1;
      if (rec == (FILE*)-1) {
          const char* out = getenv("LD_RECORD");
          rec = NULL;
          if (out && out[0]) {
              char cmd[MAX_PATH * 2];
              snprintf(cmd, sizeof(cmd),
                       "ffmpeg -loglevel error -y -f image2pipe -framerate 30 -c:v ppm -i - "
                       "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" -pix_fmt yuv420p "
                       "-movflags frag_keyframe+empty_moov \"%s\"", out);
              rec = _popen(cmd, "wb");
              fprintf(stderr, "[record] %s -> %s\n", rec ? "ffmpeg" : "FAILED to start ffmpeg", out);
          }
      }
      if (rec) {
          g_ld_record_to = rec;
          ld_dump_surface_ppm(NULL, &g.surfaces[target]);
          g_ld_record_to = NULL;
          fflush(rec);
      } }
    const u32 current = current_surface();
    surface_t* presented_surface = &g.surfaces[target];
    /* LD_PRESENT_DBG=1: is this surface actually finished? A surface whose last
     * clear is NEWER than its last draw has been wiped and not redrawn -- that
     * presents as a black frame. Counting them separates "we present the wrong
     * buffer" from "we present the right buffer too early". */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("LD_PRESENT_DBG") ? 1 : 0;
      if (dbg) {
          static unsigned long long n = 0, blank = 0;
          n++;
          if (presented_surface->last_draw_generation <
              presented_surface->last_clear_generation) blank++;
          if ((n % 256) == 0)
              /* Also say WHICH surface. Surfaces are now keyed on size as well
               * as offset, so several exist at once and "we present the wrong
               * one" is a live possibility -- the PS1 composite is 720x512 and
               * the PS3 output 1280x720. */
              fprintf(stderr,
                      "[present] %llu/%llu presents showed a surface cleared "
                      "after its last draw (%.1f%%); target=%u %u:0x%X %ux%u"
                      " draw_gen=%u clear_gen=%u\n",
                      (unsigned long long)blank, (unsigned long long)n,
                      100.0 * (double)blank / (double)n,
                      target, presented_surface->location,
                      presented_surface->offset,
                      presented_surface->w, presented_surface->h,
                      presented_surface->last_draw_generation,
                      presented_surface->last_clear_generation);
      } }
#if !defined(YZ_PERF_CLEAN)
    presented_surface->last_present_copy_generation =
        ++g_ld_present_copy_generation;
    yz_frontier_trace_emit(
        YZ_FT_PARITY_FLIP, buffer_id, target,
        (u32)g_ld_flip_requested,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        g_ld_fifo_get, g_ld_fifo_put, current);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE, target, presented_surface->last_write_kind,
        presented_surface->last_write_generation,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        presented_surface->last_copy_generation,
        presented_surface->last_blit_generation,
        presented_surface->last_resolve_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE_AUX, target,
        presented_surface->resource_serial,
        presented_surface->last_create_generation,
        presented_surface->last_other_generation,
        presented_surface->last_guest_blit_generation,
        presented_surface->offset, presented_surface->location,
        presented_surface->last_present_copy_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_RENDER, target, current,
        (u32)g_ld_stats.groups_executed,
        (u32)g_ld_stats.group_drop_ring,
        g.cb_used, g.vb_used, g.srv_ring_used,
        presented_surface->last_present_copy_generation);
#endif
    { static u32 last_present_target = LD_INVALID_SURFACE;
      if (target != last_present_target) {
          ld_trace_target("present", target, buffer_id);
          last_present_target = target;
      } }
    ID3D12Resource* srcimg = presented_surface->tex;
    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    if(!ld_af_claim(bbi))return;
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER bar[2] = {0};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = srcimg;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = bb;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    /* The swap chain is the scaled canvas (LD_RENDER_SCALE). A presented
     * surface of any other host size is stretched in with a draw;
     * CopyResource needs identical sizes. */
    extern float cellAvconfExt_display_gamma(void);
    const float display_gamma = cellAvconfExt_display_gamma();
    const int stretch = display_gamma != 1.0f ||
                        presented_surface->hw != ld_rs_up(g.width) ||
                        presented_surface->hh != ld_rs_up(g.height);
    ID3D12PipelineState* stretch_pso = stretch ? ld_scaled_blit_pso_ex(DXGI_FORMAT_R8G8B8A8_UNORM, display_gamma != 1.0f) : NULL;
    if (stretch && stretch_pso) {
        u32 sslots[SRV_TABLE_SIZE];
        for (u32 i = 0; i < SRV_TABLE_SIZE; i++) sslots[i] = SRV_WHITE;
        sslots[0] = SRV_SURFACE_BASE + target;
        const D3D12_GPU_DESCRIPTOR_HANDLE stable = srv_table(sslots, 0);
        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);
        D3D12_CPU_DESCRIPTOR_HANDLE brtv = rtv_handle(bbi);
        g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &brtv, FALSE, NULL);
        ID3D12DescriptorHeap* sheaps[] = {g.srv_heap, g.smp_heap};
        g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, sheaps);
        g.list->lpVtbl->SetPipelineState(g.list, stretch_pso);
        g.list->lpVtbl->SetGraphicsRootSignature(g.list, g_sblit_rs);
        const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 4, full, 0);
        g.list->lpVtbl->SetGraphicsRoot32BitConstants(g.list, 0, 1, &display_gamma, 4);
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, stable);
        D3D12_VIEWPORT svp = { 0.0f, 0.0f, (float)ld_rs_up(g.width), (float)ld_rs_up(g.height), 0.0f, 1.0f };
        D3D12_RECT ssc = { 0, 0, (LONG)ld_rs_up(g.width), (LONG)ld_rs_up(g.height) };
        g.list->lpVtbl->RSSetViewports(g.list, 1, &svp);
        g.list->lpVtbl->RSSetScissorRects(g.list, 1, &ssc);
        g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->lpVtbl->DrawInstanced(g.list, 3, 1, 0, 0);
        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);
    } else {
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

    g.list->lpVtbl->CopyResource(g.list, bb, srcimg);

    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);
    }

    ld_flush(LD_FLUSH_PRESENT);
    {
        const HRESULT present_hr = ld_swap_present(g_ld_frames + 1u, 0u);
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames + 1u);
        else {
            fprintf(stderr,
                    "[d3d-fail] Present hr=0x%08lX frame=%u "
                    "target=%u size=%ux%u window=%ux%u\n",
                    (unsigned long)present_hr, g_ld_frames,
                    target, g.surfaces[target].w, g.surfaces[target].h,
                    g.width, g.height);
            ld_dump_dred("Present", present_hr);
            g.ready = 0;
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (InterlockedCompareExchange(
            &g_ld_diag_post_movie_pending, 0, 0) != 0) {
        const LONG post_movie_present =
            InterlockedIncrement(&g_ld_diag_post_movie_presents);
        if (post_movie_present == 32 &&
            InterlockedCompareExchange(
                &g_ld_diag_post_movie_pending, 0, 1) == 1) {
            char path[MAX_PATH * 2];
            snprintf(
                path, sizeof(path),
                "%s\\semantic_checkpoint_surface.ppm",
                g.vertex_diag_dir);
            const u64 nonblack =
                ld_dump_surface_ppm(path, &g.surfaces[target]);
            fprintf(stderr,
                    "[rsx-vertex-surface] reason=post-movie-32 "
                    "mode=%s target=%u pixels=%llu "
                    "nonblack=%s%llu path=%s\n",
                    ld_vertex_mode_name(), target,
                    (unsigned long long)g.surfaces[target].w *
                        g.surfaces[target].h,
                    nonblack == UINT64_MAX ? "unknown:" : "",
                    (unsigned long long)(
                        nonblack == UINT64_MAX ? 0 : nonblack),
                    path);
            fflush(stderr);
        }
    }
#endif

    { static unsigned long long packets_at_last_frame = 0;
      g_ld_last_frame_draws = (u32)(g_ld_stats.packets_seen - packets_at_last_frame);
      packets_at_last_frame = g_ld_stats.packets_seen; }
    g_ld_frames++;
#if defined(YZ_PERF_PROFILE)
    ld_profile_present(g_ld_frames);
#endif
#if !defined(YZ_PERF_CLEAN)
    /* Autonomous parity capture.  Scalar records remain circular and cheap.
     * Readback begins only after two alternating display surfaces stop
     * receiving writes while FIFO progress continues, and again for the first
     * 16 held-movement presentations.  The latter continues counting to 120
     * without readback so validation does not materially alter movement. */
    {
        static int parity_enabled = -1;
        static char parity_dir[MAX_PATH * 2];
        static u32 observed_write[MAX_SURFACES];
        static u8 observed_valid[MAX_SURFACES];
        static u32 prior_target = LD_INVALID_SURFACE;
        static u32 prior_get;
        static u32 prior_put;
        static u32 dialogue_stable;
        static u64 dialogue_targets;
        static int dialogue_armed;
        static u32 dialogue_captures;
        static u32 movement_frames;
        static u32 movement_captures;
        if (parity_enabled < 0) {
            const char* enabled = getenv("RSX_PARITY_DIAG");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            parity_enabled = enabled && *enabled && directory && *directory;
            if (parity_enabled) {
                strncpy(parity_dir, directory, sizeof(parity_dir) - 1u);
                parity_dir[sizeof(parity_dir) - 1u] = '\0';
            }
        }
        if (parity_enabled) {
            const LONG movement_phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            const u64 elapsed = g_yz_auto_start_tick
                ? GetTickCount64() - g_yz_auto_start_tick : 0;
            surface_t* surface = &g.surfaces[target];
            const int fifo_progress =
                g_ld_fifo_get != prior_get || g_ld_fifo_put != prior_put;
            const int alternating =
                prior_target != LD_INVALID_SURFACE && prior_target != target;
            const int unchanged = observed_valid[target] &&
                observed_write[target] == surface->last_write_generation;

            if (!dialogue_armed && !dialogue_captures &&
                movement_phase == 0 &&
                ((g_ld_frame_vertex_constant_ring_recycles != 0 &&
                  elapsed >= 300000ull) || elapsed >= 420000ull)) {
                if (g_ld_frame_vertex_constant_ring_recycles != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE HIGH-DRAW TRIGGER "
                        "elapsed_ms=%llu frame_recycles=%u total_recycles=%llu "
                        "drop_ring=%llu frame=%u flip=%llu\n",
                        (unsigned long long)elapsed,
                        g_ld_frame_vertex_constant_ring_recycles,
                        (unsigned long long)
                            g_ld_vertex_constant_ring_recycles,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                } else if (fifo_progress && alternating && unchanged) {
                    dialogue_stable++;
                    if (target < 64u)
                        dialogue_targets |= 1ull << target;
                } else if (fifo_progress) {
                    dialogue_stable = 0;
                    dialogue_targets = target < 64u ? 1ull << target : 0;
                }
                if (dialogue_stable >= 12u &&
                    dialogue_targets &&
                    (dialogue_targets & (dialogue_targets - 1u)) != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE TRIGGER elapsed_ms=%llu "
                        "stable=%u targets=%016llX drop_ring=%llu "
                        "frame=%u flip=%llu\n",
                        (unsigned long long)elapsed, dialogue_stable,
                        (unsigned long long)dialogue_targets,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }
            if (dialogue_armed && dialogue_captures < 16u) {
                const u32 capture = ++dialogue_captures;
                ld_parity_capture_surface(
                    parity_dir, "dialogue", capture, buffer_id, target);
                if (capture == 16u) {
                    dialogue_armed = 0;
                    yz_frontier_trace_dump(5u);
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE COMPLETE captures=16 "
                        "frame=%u flip=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }

            if (movement_phase == 4) {
                movement_frames++;
                if (movement_captures < 16u) {
                    const u32 capture = ++movement_captures;
                    ld_parity_capture_surface(
                        parity_dir, "movement", capture, buffer_id, target);
                    if (capture == 16u)
                        yz_frontier_trace_dump(6u);
                }
                if (movement_frames == 120u) {
                    yz_frontier_trace_dump(7u);
                    fprintf(
                        stderr,
                        "[parity-diag] MOVEMENT 120 COMPLETE "
                        "captures=16 frame=%u flip=%llu groups=%llu "
                        "drop_ring=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested,
                        (unsigned long long)g_ld_stats.groups_executed,
                        (unsigned long long)g_ld_stats.group_drop_ring);
                    fflush(stderr);
                }
            }

            observed_write[target] = surface->last_write_generation;
            observed_valid[target] = 1;
            prior_target = target;
            prior_get = g_ld_fifo_get;
            prior_put = g_ld_fifo_put;
        }
    }
    /* Capture the actual presented surface at acknowledged input boundaries.
     * This is independent of desktop visibility/focus and is inert unless the
     * promotion controller explicitly enables RSX_MOVEMENT_PROOF. */
    {
        static int movement_capture = -1;
        static char movement_dir[MAX_PATH * 2];
        if (movement_capture < 0) {
            const char* enabled = getenv("RSX_MOVEMENT_PROOF");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            movement_capture = enabled && directory && *directory;
            if (movement_capture) {
                strncpy(movement_dir, directory,
                        sizeof(movement_dir) - 1u);
                movement_dir[sizeof(movement_dir) - 1u] = '\0';
            }
        }
        if (movement_capture) {
            const LONG phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            static u64 next_probe_tick;
            static u32 probe_serial;
            static u64 probe_delay_ms;
            static u64 probe_interval_ms;
            static int probe_configured;

            if (!probe_configured) {
                const char* delay = getenv("RSX_MOVEMENT_PROOF_DELAY_MS");
                const char* interval = getenv("RSX_MOVEMENT_PROBE_INTERVAL_MS");
                probe_delay_ms = delay && *delay
                    ? _strtoui64(delay, NULL, 10) : 780000ull;
                probe_interval_ms = interval && *interval
                    ? _strtoui64(interval, NULL, 10) : 0ull;
                if (probe_interval_ms && probe_interval_ms < 30000ull)
                    probe_interval_ms = 30000ull;
                probe_configured = 1;
            }

            /* A time threshold alone cannot distinguish authored camera work
             * from playable Akiyama.  In visual-arm mode, retain occasional
             * renderer-surface probes while dialogue input continues.  The
             * proof is armed only after one of these images visibly shows the
             * gameplay HUD, avoiding timing-dependent false positives. */
            if (phase == 0 && probe_interval_ms && g_yz_auto_start_tick) {
                const u64 now = GetTickCount64();
                const u64 elapsed = now - g_yz_auto_start_tick;
                if (elapsed >= probe_delay_ms &&
                    (!next_probe_tick || now >= next_probe_tick)) {
                    char path[MAX_PATH * 2];
                    const u32 serial = ++probe_serial;
                    snprintf(path, sizeof(path),
                             "%s\\frontier_probe_%03u.ppm",
                             movement_dir, serial);
                    {
                        const u64 nonblack =
                            ld_dump_surface_ppm(path, &g.surfaces[target]);
                        fprintf(stderr,
                                "[movement-proof] frontier-probe serial=%u "
                                "elapsed_ms=%llu frame=%u nonblack=%s%llu "
                                "path=%s\n",
                                serial, (unsigned long long)elapsed,
                                g_ld_frames,
                                nonblack == UINT64_MAX ? "unknown:" : "",
                                (unsigned long long)(
                                    nonblack == UINT64_MAX ? 0 : nonblack),
                                path);
                        fflush(stderr);
                    }
                    next_probe_tick = now + probe_interval_ms;
                }
            }
            const char* name = NULL;
            LONG next_phase = phase;
            if (phase == 1) {
                name = "movement_before.ppm";
                next_phase = 2;
            } else if (phase == 3) {
                name = "camera_after.ppm";
                next_phase = 4;
            } else if (phase == 5) {
                name = "movement_after_60s.ppm";
                next_phase = 6;
            }
            if (name) {
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s\\%s", movement_dir, name);
                ld_movement_camera_snapshot(phase);
                {
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[movement-proof] capture phase=%ld frame=%u "
                            "flip=%llu nonblack=%s%llu path=%s\n",
                            phase, g_ld_frames,
                            (unsigned long long)g_ld_flip_requested,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    fflush(stderr);
                }
                InterlockedCompareExchange(
                    &g_yz_movement_proof_phase, next_phase, phase);
                if (next_phase == 6) {
                    fprintf(stderr,
                            "[movement-proof] COMPLETE sustained_ms=60000 "
                            "camera_ms=5000 frame=%u\n", g_ld_frames);
                    fflush(stderr);
                }
            }
        }
    }
    /* Narrow validation readback for the four user-visible orphanage shots.
     * This captures the renderer's actual presented surface and therefore is
     * independent of desktop focus, window visibility, and screenshot APIs.
     * It is inert unless the validation controller supplies both variables. */
    {
        static int validation_capture = -1;
        static u32 validation_dumped = 0;
        static u64 validation_origin = UINT64_MAX;
        static char validation_dir[MAX_PATH * 2];
        if (validation_capture < 0) {
            const char* enabled = getenv("RSX_VALIDATION_CAPTURE");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            validation_capture = enabled && directory && *directory;
            if (validation_capture) {
                strncpy(validation_dir, directory,
                        sizeof(validation_dir) - 1u);
                validation_dir[sizeof(validation_dir) - 1u] = '\0';
            }
        }
        /* The a010 file-lifetime gate is the authoritative scene boundary in
         * clean production builds.  The older exact-mesh gate was useful for
         * a geometry experiment but is not guaranteed to occur in every
         * authored camera block, so using it for validation can miss the
         * entire scene even while valid draws are being presented. */
        if (validation_capture && validation_origin == UINT64_MAX &&
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0) {
            validation_origin = g_ld_flip_requested;
            fprintf(stderr,
                    "[validation-capture] a010 origin flip=%llu frame=%u\n",
                    (unsigned long long)validation_origin, g_ld_frames);
        }
        if (validation_capture && validation_origin != UINT64_MAX) {
            /* Scene-open itself is a 43-flip black/fade interval.  Preserve
             * the original four-shot spacing after that measured lead-in so
             * the promotion run lands on the outdoor ground, children,
             * over-shoulder hair, and face close-up rather than the fade,
             * sink, and pre-pickup phone shots. */
            static const u64 offsets[] = {43, 128, 302, 316};
            static const char* labels[] = {
                "background-ground", "children-eyes", "hair", "face-eyes"
            };
            for (u32 i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
                const u64 requested = validation_origin + offsets[i];
                if (!(validation_dumped & (1u << i)) &&
                    g_ld_flip_requested >= requested) {
                    char path[MAX_PATH * 2];
                    snprintf(path, sizeof(path),
                             "%s\\orphanage_gpu_%s_flip_%llu.ppm",
                             validation_dir, labels[i],
                             (unsigned long long)requested);
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[validation-capture] label=%s requested_flip=%llu "
                            "actual_flip=%llu frame=%u target=%u "
                            "nonblack=%s%llu path=%s\n",
                            labels[i], (unsigned long long)requested,
                            (unsigned long long)g_ld_flip_requested,
                            g_ld_frames, target,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    validation_dumped |= 1u << i;
                }
            }
        }
    }
#endif
    /* First 32 frames verbatim, then every 32nd: keeps the log bounded while
     * making the TRUE frame count measurable from the log. (The old hard cap
     * at 32 made "stalls at frame ~32" unfalsifiable from the .err alone.) */
    if (g_ld_frames <= 32 || (g_ld_frames & 31) == 0)
        fprintf(stderr,
                "[live-draw] frame %u packets[seen=%llu queued=%llu movie=%llu qfull=%llu] "
                "groups[seen=%llu exec=%llu empty=%llu drop{fetch=%llu degen=%llu prim=%llu "
                "alloc=%llu pso=%llu ring=%llu surface=%llu}] clears[guest=%llu badsurf=%llu implicitZ=%llu] "
                "textures[cached=%u/%u full=%llu decodefail=%llu depthSRV=%llu rejectZ=%llu] "
                "vtex[cached=%u enabled=%llu binds=%llu uploads=%llu refresh=%llu "
                "unsupported=%llu missingTXL=%llu] divider=%llu "
                "balance[p=%s g=%s] binds[white=%llu real=%llu surf=%llu] (cumulative)\n",
                g_ld_frames,
                g_ld_stats.packets_seen, g_ld_stats.packets_queued,
                g_ld_stats.packets_movie, g_ld_stats.packets_queue_full,
                g_ld_stats.groups_seen, g_ld_stats.groups_executed,
                g_ld_stats.groups_empty, g_ld_stats.group_drop_fetch,
                g_ld_stats.group_drop_degenerate, g_ld_stats.group_drop_primitive,
                g_ld_stats.group_drop_alloc, g_ld_stats.group_drop_pso,
                g_ld_stats.group_drop_ring, g_ld_stats.group_drop_surface,
                g_ld_stats.clears, g_ld_stats.clear_drop_surface,
                g_ld_stats.implicit_depth_clears,
                g.n_textures, MAX_TEXTURES,
                (unsigned long long)g_ld_texture_cache_full,
                (unsigned long long)g_ld_texture_decode_fail,
                (unsigned long long)g_ld_zdepth_srv_binds,
                (unsigned long long)g_ld_zdepth_srv_reject_no_write,
                g.n_vtex,
                (unsigned long long)g_ld_vtex_enabled,
                (unsigned long long)g_ld_vtex_binds,
                (unsigned long long)g_ld_vtex_uploads,
                (unsigned long long)g_ld_vtex_refreshes,
                (unsigned long long)g_ld_vtex_unsupported,
                (unsigned long long)g_ld_vtex_missing_for_txl,
                (unsigned long long)g_ld_divider_fetches,
                g_ld_stats.packets_seen == g_ld_stats.packets_queued +
                    g_ld_stats.packets_movie + g_ld_stats.packets_queue_full ? "ok" : "BAD",
                g_ld_stats.groups_seen == ld_groups_accounted() ? "ok" : "BAD",
                g_ld_bind_white, g_ld_bind_real, g_ld_bind_surf);
    /* [fps] heartbeat: direct frame-rate logging, one line per ~5s wall.
     * Exists because pace repeatedly had to be inferred from frame-counter
     * arithmetic and log ordering, and two such inferences were wrong in one
     * night (s42). Volume-bounded per LESSONS #6c. */
    { static ULONGLONG fps_t0 = 0; static u32 fps_f0 = 0;
      ULONGLONG now = GetTickCount64();
      if (fps_t0 == 0) { fps_t0 = now; fps_f0 = g_ld_frames; }
      else if (now - fps_t0 >= 5000) {
          /* PS1_PC=1: the R3000 state block (0x76C080) on the same heartbeat.
           *
           * READ +0x124 (instructions retired), NOT +0x108. The interpreter
           * loads PC from +0x108 on entry (lwz r26, 0x108(r23) at 0x001066CC)
           * and writes it back only in its epilogue (stw r26, 0x108(r23) at
           * 0x00106964) -- and it has not returned, because it is entered once
           * and loops internally. So +0x108 is stale from boot: 280,000 reads
           * across 14 samples gave 0xBFC00000 every time while +0x124 climbed
           * past a billion. Reading that as "stuck at the reset vector" would
           * be wrong twice over -- the epilogue does `ori r26, r26, 0x80`
           * first, so a real exit there could not even write 0xBFC00000.
           *
           * +0x124 is updated from inside the loop and is the honest progress
           * signal. Finding the LIVE pc needs the interpreter's register, not
           * this field. */
          { static int pc_on = -1;
            if (pc_on < 0) pc_on = getenv("PS1_PC") ? 1 : 0;
            if (pc_on) {
                extern uint32_t vm_read32(uint64_t);
                /* One read of +0x108 cannot tell a reset loop from a field that
                 * is simply not live between interpreter entries -- both look
                 * like a constant 0xBFC00000. So sample it hard: 20000 reads,
                 * and report how many DISTINCT values appear plus the lowest
                 * and highest. A live PC sweeps; a dead field does not. */
                uint32_t lo = 0xFFFFFFFFu, hi = 0, prev = 0, chg = 0;
                for (int k = 0; k < 20000; k++) {
                    uint32_t v = vm_read32(0x76C080u + 0x108u);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                    if (k && v != prev) chg++;
                    prev = v;
                }
                /* The PS1 GPU command ring. func_0010F658 pushes one 0x100-byte
                 * packet per GP0 batch (header 3 at +0, payload from +0x10) and
                 * advances a write offset; the four GPU SPUs consume it. If this
                 * offset never moves, the R3000 is not producing draw commands
                 * at all, and no amount of SPU-side work will help.
                 *
                 *   ring_off_ptr = *(TOC-0x794C)   (TOC = 0x1C3D30)
                 *   ring_base    = *(TOC-0x7918)
                 * from func_0010F658's first two instructions. */
                uint32_t roff_p = vm_read32(0x1BC3E4u);
                uint32_t rbase  = vm_read32(0x1BC418u);
                uint32_t roff   = roff_p ? vm_read32(roff_p) : 0xFFFFFFFFu;
                /* Each GPU SPU polls its OWN local store at 0x15010 for that
                 * offset (it reports that address to the PPU through its
                 * outbound mailbox during init), and a raw SPU's local store is
                 * aliased into guest memory at 0xE0000000 + n*0x100000 -- so
                 * these four reads see exactly what the SPU sees. If they match
                 * `off`, the kick landed and the SPUs are simply not reacting;
                 * if they do not, the kick is being lost. Opposite problems. */
                /* +0x15010 is the offset the PPU publishes to this SPU;
                 * +0x15020 is how far the SPU has actually CONSUMED (its work
                 * loop at 0x3158 advances it by 0x100 per packet under the same
                 * 0x007FFFFF mask the PPU uses, and stores it back). The pair
                 * separates "the SPUs are not being told" from "the SPUs are
                 * told and not working" from "both are fine and the problem is
                 * downstream in compositing". */
                uint32_t ls[4], cons[4];
                for (int n = 0; n < 4; n++) {
                    uint32_t w = 0xE0000000u + (uint32_t)n * 0x100000u;
                    ls[n]   = vm_read32(w + 0x15010u);
                    cons[n] = vm_read32(w + 0x15020u);
                }
                /* The scheduler's own state. func_00105FA8 hands the
                 * interpreter a budget at +0x120; when that is permanently zero
                 * the interpreter spins without executing. These are the fields
                 * it works from, so a frozen run says WHICH state it froze in
                 * rather than just that it froze. */
                fprintf(stderr, "[ps1] pc=0x%08X(chg=%u) exited=%u total=%u"
                                " cost=%u budget=%d reason=%d evhead=0x%08X"
                                " ring[off=0x%08X] pub[%06X %06X %06X %06X]"
                                " done[%06X %06X %06X %06X]\n",

                        lo, chg,
                        vm_read32(0x76C080u + 0x110u),
                        vm_read32(0x76C080u + 0x124u),
                        vm_read32(0x76C080u + 0x114u),
                        (int32_t)vm_read32(0x76C080u + 0x120u),
                        (int32_t)vm_read32(0x76C080u + 0x138u),
                        vm_read32(0x76C080u + 0x540u),
                        roff, ls[0], ls[1], ls[2], ls[3],
                        cons[0], cons[1], cons[2], cons[3]);
                /* Does PS1 VRAM actually have pixels in it?
                 *
                 * The texture the PS3 side binds for the PS1 framebuffer is
                 * location 1 (RSX local), offset 0x400000, 1024x512 fmt 0xE2 --
                 * exactly PS1 VRAM. Everything upstream of here is now known
                 * good (the four GPU SPUs consume every packet the R3000
                 * produces), so this is the first place the picture can go
                 * missing: either the SPUs are not writing pixels, or they are
                 * and our compositing drops them. Count non-zero words rather
                 * than guessing from a window capture -- PrintWindow on a D3D12
                 * swapchain cannot tell "black" from "capture failed". */
                { const u8* fb = guest_ptr(1u, 0x400000u, 1024u * 512u * 2u);
                  /* The guest EA of PS1 VRAM, once. Watching 0xC0400000 --
                   * derived from cellGcmSys's localAddress -- caught no writes
                   * at all from either the PPU store path or SPU DMA, which is
                   * either a real finding or a wrong address. This says which,
                   * instead of assuming. */
                  { static int shown = 0;
                    extern uint8_t* vm_base;
                    if (!shown && fb && vm_base) { shown = 1;
                        fprintf(stderr, "[ps1] VRAM guest EA = 0x%08X\n",
                                (u32)(size_t)(fb - vm_base)); } }
                  if (!fb) fprintf(stderr, "[ps1] vram: not mapped\n");
                  else {
                      u32 nz = 0, first = 0xFFFFFFFFu, n = 1024u * 512u / 2u;
                      const u32* w32 = (const u32*)fb;
                      for (u32 i = 0; i < n; i++)
                          if (w32[i]) { nz++; if (first == 0xFFFFFFFFu) first = i * 4u; }
                      fprintf(stderr, "[ps1] vram nonzero=%u/%u first=+0x%X\n",
                              nz, n, first == 0xFFFFFFFFu ? 0u : first);
                      /* PS1_EVENTS=1: the three kernel events the BIOS polls.
                       *
                       * The BIOS parks at 0xBFC53840 calling TestEvent
                       * (B-table 0x0B, trampoline 0xBFC58B60) on three
                       * descriptors loaded from PS1 kernel RAM 0x0000B21C,
                       * 0x0000B224 and 0x0000B228. Reading their event CLASS
                       * says what it is waiting for; the PS1 CD class is
                       * 0xF0000003.
                       *
                       * PS1 RAM base is *(TOC-0x79D4) = 0x00770780, and the
                       * interpreter reads PS1 memory with lwbrx -- PS1 RAM is
                       * stored LITTLE-endian in guest memory, so every read
                       * here has to be byte-swapped. Reading it big-endian
                       * would produce plausible-looking nonsense. */
                      { static int ev = -1; static int ev_done = 0;
                        if (ev < 0) ev = getenv("PS1_EVENTS") ? 1 : 0;
                        static int ev_beats = 0;
                        if (ev && !ev_done && ++ev_beats >= 3) {
                            ev_done = 1;
                            const u32 ram = vm_read32(0x001BC35Cu);
                            #define PS1LE(a) __builtin_bswap32(vm_read32((ram) + (u32)(a)))
                            const u32 tot_evcb = PS1LE(0x0120);
                            const u32 tot_size = PS1LE(0x0124);
                            fprintf(stderr, "[ps1ev] ram=0x%08X  EvCB tbl=0x%08X size=%u\n",
                                    ram, tot_evcb, tot_size);
                            /* All FIVE slots CdInit fills, not just the three
                             * the poll loop reads: -0x4de8/-0x4de4/-0x4de0/
                             * -0x4ddc/-0x4dd8 = 0xB218..0xB228. All zero means
                             * CdInit never ran; a mix means it ran partway. */
                            static const u32 slot[5] = { 0xB218u, 0xB21Cu, 0xB220u,
                                                         0xB224u, 0xB228u };
                            for (int k = 0; k < 5; k++) {
                                const u32 h = PS1LE(slot[k]);
                                fprintf(stderr, "[ps1ev]   slot 0x%04X handle=0x%08X",
                                        slot[k], h);
                                if ((h >> 24) == 0xF1u && tot_evcb) {
                                    const u32 idx = h & 0xFFFFu;
                                    const u32 cb  = (tot_evcb & 0x1FFFFFu) + idx * 0x1Cu;
                                    fprintf(stderr, "  idx=%u class=0x%08X status=0x%08X"
                                                    " spec=0x%08X mode=0x%08X",
                                            idx, PS1LE(cb + 0), PS1LE(cb + 4),
                                            PS1LE(cb + 8), PS1LE(cb + 12));
                                }
                                fprintf(stderr, "\n");
                            }
                            /* Dump the whole EvCB table. The table pointer
                             * itself is valid (0xA000E028, 448 bytes = 16
                             * entries of 0x1C), so the PS1 kernel IS up and
                             * events CAN be opened. Whether any ARE distinguishes
                             * "CdInit specifically never ran" from "nothing ever
                             * opens an event", which are very different faults.
                             * EvCB layout: class +0, status +4, spec +8,
                             * mode +0xC, func +0x10. */
                            { const u32 tb = tot_evcb & 0x1FFFFFu;
                              const u32 n = tot_size / 0x1Cu;
                              u32 live = 0;
                              for (u32 i = 0; i < n && i < 32; i++) {
                                  const u32 cb = tb + i * 0x1Cu;
                                  const u32 cls = PS1LE(cb + 0);
                                  const u32 st  = PS1LE(cb + 4);
                                  if (!cls && !st) continue;
                                  live++;
                                  fprintf(stderr, "[ps1ev]   EvCB[%2u] class=0x%08X"
                                                  " status=0x%08X spec=0x%08X"
                                                  " mode=0x%08X func=0x%08X\n",
                                          i, cls, st, PS1LE(cb + 8),
                                          PS1LE(cb + 12), PS1LE(cb + 16));
                              }
                              fprintf(stderr, "[ps1ev]   %u of %u EvCB entries in use\n",
                                      live, n); }
                            #undef PS1LE
                        } }
                      /* PS1_GP0HIST=1: which GP0 PRIMITIVES the ring actually
                       * carries, as a histogram over the last 64 packets.
                       *
                       * "The menu draws but nothing 3D does" is a statement
                       * about primitive TYPES, and no counter here reported
                       * those -- packets, groups and VRAM pixels all count the
                       * same whether the PS1 is drawing a textured rectangle or
                       * a gouraud triangle. GP0 opcodes: 0x20-0x3F polygons,
                       * 0x40-0x5F lines, 0x60-0x7F rectangles, 0x80+ transfers,
                       * 0x00-0x1F misc/state. If the 3D is missing because the
                       * polygons never reach the ring, that is upstream in the
                       * R3000; if they reach it, it is the rasteriser or the
                       * composite. Opposite halves of the pipeline. */
                      { static int gh = -1;
                        if (gh < 0) gh = getenv("PS1_GP0HIST") ? 1 : 0;
                        if (gh && rbase && roff >= 0x4000u) {
                            unsigned cls[8] = {0}; unsigned pk_types[8] = {0};
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                pk_types[vm_read32(pk) & 7u]++;
                                for (u32 q = 0; q < 60u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if (!w) continue;
                                    cls[(w >> 24) >> 5]++;   /* opcode/0x20 */
                                }
                            }
                            /* PS1 VRAM comes out solid 0x83E0 -- R=0, G=31,
                             * B=0, mask bit set -- across the full 640-pixel
                             * display width, fully written (207 DMA writes per
                             * 64-byte block). Something paints the screen
                             * green. GP0 0x02 is Fill Rectangle and its colour
                             * word is right here in the ring, so print the
                             * fills: if the guest asks for green, the bug is
                             * upstream in the R3000; if it asks for something
                             * else, our SPU rasteriser is packing the colour
                             * wrong. Opposite halves of the pipeline. */
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                for (u32 q = 0; q < 58u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if ((w >> 24) != 0x02u) continue;
                                    fprintf(stderr, "[gp0fill] pkt+0x%X"
                                                    " cmd=%08X xy=%08X wh=%08X\n",
                                            0x10u + q * 4u, w,
                                            vm_read32(pk + 0x14u + q * 4u),
                                            vm_read32(pk + 0x18u + q * 4u));
                                    break;
                                }
                            }
                            fprintf(stderr, "[gp0hist] last 64 pkts types[");
                            for (int q = 0; q < 8; q++) fprintf(stderr, "%u ", pk_types[q]);
                            fprintf(stderr, "] words: misc=%u POLY=%u line=%u"
                                            " rect=%u xfer=%u %u %u %u\n",
                                    cls[0], cls[1], cls[2], cls[3],
                                    cls[4], cls[5], cls[6], cls[7]);
                        } }
                      /* PS1_RINGDUMP=1: the last few GP0 command packets.
                       *
                       * func_0010F658 writes one 0x100-byte packet per batch:
                       * a type word at +0 (it writes 3 in one variant and 2 in
                       * another) and the payload from +0x10. The four GPU SPUs
                       * consume every packet and never DMA a pixel to VRAM, so
                       * the question is what these packets actually contain --
                       * PS1 GP0 drawing commands, or only state and sync. */
                      { static int rd2 = -1; static int shown = 0;
                        if (rd2 < 0) rd2 = getenv("PS1_RINGDUMP") ? 1 : 0;
                        if (rd2 && shown < 3 && rbase && roff >= 0x300u) {
                            shown++;
                            for (int k = 3; k >= 1; k--) {
                                const u32 pk = rbase + ((roff - (u32)k * 0x100u) & 0x007FFFFFu);
                                fprintf(stderr, "[ring] pkt@0x%08X type=%u:", pk,
                                        vm_read32(pk));
                                for (u32 q = 0; q < 12; q++)
                                    fprintf(stderr, " %08X", vm_read32(pk + 0x10u + q * 4u));
                                fprintf(stderr, "\n");
                            }
                        } }
                      { static u32 rdy = 0;
                        if (!rdy) { const char* re = getenv("PS1_VRAM_READY");
                                    rdy = re ? (u32)strtoul(re, 0, 0) : 20000u; }
                        if (nz >= rdy) g_ld_ps1_vram_ready = 1; }
                      /* PS1_FBDUMP=<path>: write PS1 VRAM out as a PPM.
                       *
                       * This exists because "the window is black" was an
                       * ASSUMPTION for a long stretch of this port: PrintWindow
                       * on a D3D12 swapchain returns black whether the page is
                       * black or the capture simply failed, so every screenshot
                       * was unfalsifiable. This reads the pixels the PS1 itself
                       * produced, straight out of guest memory, with no D3D and
                       * no window involved. 1024x512, 16-bit 1-5-5-5, pitch
                       * 2048 -- the format the [tex-refresh] line reports. */
                      /* ONE dump, taken once the framebuffer has at least
                       * PS1_FBDUMP_MIN non-zero words (default 20000).
                       *
                       * It used to write a 1.5 MB PPM on every heartbeat, and
                       * that instrumentation was itself most of the
                       * "nondeterminism" this port was chasing: runs carrying
                       * the periodic dump reached 0-7,127 non-zero words, while
                       * clean runs reach 75,527 every time. Measuring the thing
                       * was changing it. One dump, gated on content. */
                      { const char* dp = getenv("PS1_FBDUMP");
                        static int dumped = 0;
                        u32 want = 20000;
                        { const char* mn = getenv("PS1_FBDUMP_MIN");
                          if (mn) want = (u32)strtoul(mn, 0, 0); }
                        if (dp && !dumped && nz >= want) {
                            dumped = 1;
                            char path[512];
                            snprintf(path, sizeof path, "%s", dp);
                            FILE* f = fopen(path, "wb");
                            if (f) {
                                fprintf(f, "P6\n1024 512\n255\n");
                                for (u32 y = 0; y < 512; y++) {
                                    const u8* row = fb + (size_t)y * 2048u;
                                    for (u32 x = 0; x < 1024; x++) {
                                        u16 p = (u16)(row[x * 2] | (row[x * 2 + 1] << 8));
                                        u8 rgb[3];
                                        rgb[0] = (u8)(((p >> 10) & 0x1F) * 255 / 31);
                                        rgb[1] = (u8)(((p >> 5)  & 0x1F) * 255 / 31);
                                        rgb[2] = (u8)(( p        & 0x1F) * 255 / 31);
                                        fwrite(rgb, 1, 3, f);
                                    }
                                }
                                fclose(f);
                            }
                        } }
                  } }
                (void)hi; (void)rbase;
            } }
          fprintf(stderr, "[fps] %.1f (frames %u..%u over %.1fs)\n",
                  (g_ld_frames - fps_f0) * 1000.0 / (double)(now - fps_t0),
                  fps_f0, g_ld_frames, (now - fps_t0) / 1000.0);
          fps_t0 = now; fps_f0 = g_ld_frames;
      } }
    /* RSX_DUMP_EVERY=N: also dump every Nth frame, not just the first 8 --
     * a title whose content starts after the boot clears is invisible otherwise. */
    static int ld_dump_every = -1;
    if (ld_dump_every < 0) { const char* e = getenv("RSX_DUMP_EVERY"); ld_dump_every = e ? atoi(e) : 0; }
    if (LD_DIAG_ENABLED("RSX_DUMP") &&
        (g_ld_frames <= 8 || (ld_dump_every > 0 && (g_ld_frames % (u32)ld_dump_every) == 0))) {
        /* Dump the presented color surface (RENDER_TARGET state -> safe). */
        const u32 cur = current_surface();
        if (cur != LD_INVALID_SURFACE) {
            char path[256];
            snprintf(path, sizeof(path), "scratch\\ld_frame_%02u.ppm", g_ld_frames);
            ld_dump_surface_ppm(path, &g.surfaces[cur]);
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (rsx_live_draw_a010_probe_active()) {
        const u32 elapsed = g_ld_frames - g_ld_a010_probe_start_frame;
        const int targeted = getenv("RSX_A010_SURFACE_DUMP") != NULL;
        u32 sample_every = 16u;
        const char *sample_every_env = getenv("RSX_A010_SURFACE_EVERY");
        if (sample_every_env) {
            const int parsed = atoi(sample_every_env);
            if (parsed > 0 && parsed <= 64)
                sample_every = (u32)parsed;
        }
        /* Loading consumes roughly 150 fast flips before a010. Sampling every
         * 16 frames spans the load and complete AUTH window while keeping the
         * synchronous readbacks from becoming the scene's clock. */
        const int world_ready =
            InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
        /* Targeted readbacks are intentionally expensive: each surface dump
         * submits and fences the open D3D12 list.  Do not perturb the producer
         * while it is still assembling the first a010 world command chain;
         * begin targeted capture only after a real scene mesh was observed. */
        const int capture_once =
            targeted && getenv("RSX_A010_CAPTURE_ONCE") != NULL;
        const int explicit_probe_fallback =
            capture_once && getenv("RSX_A010_PROBE") != NULL &&
            elapsed >= 192u;
        /* The exact healthy-mesh tuple is the strongest capture gate, but a
         * broken natural scene may never produce it. An explicitly requested
         * one-shot probe therefore gets one bounded fallback after 192 scene
         * flips, late enough to be past a010's loading/transition frames. */
        if ((!targeted || world_ready || explicit_probe_fallback) &&
            (elapsed % sample_every) == 0 &&
            (!targeted || elapsed <= 208u ||
             (capture_once && g_ld_a010_probe_sample == 0u))) {
            u64 mask = g_ld_a010_probe_touched;
            const u32 cur = current_surface();
            if (targeted) {
                mask = 0;
                for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                    const u32 off = g.surfaces[i].offset;
                    /* Working a010 replay: 0xE40000 and 0x1800000 carry
                     * 1,481/1,727 scene draws; 0x2D10000 and 0x2710000 are
                     * the two depth-derived color passes; 0x1440000 is the
                     * final seven-draw composite. */
                    if (off == 0x00E40000u || off == 0x01800000u ||
                        off == 0x02D10000u || off == 0x02710000u ||
                        off == 0x01440000u ||
                        i == target)
                        mask |= 1ull << i;
                }
            } else if (cur < 64) {
                mask |= 1ull << cur;
            }
            fprintf(stderr,
                    "[a010-probe] SAMPLE n=%u live_frame=%u elapsed=%u "
                    "present=%u current=%u buffer=%u touched=0x%016llX "
                    "surfaces=%u targeted=%d\n",
                    g_ld_a010_probe_sample, g_ld_frames, elapsed, target, cur,
                    buffer_id, (unsigned long long)mask, g.n_surfaces,
                    targeted);
            for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                if (!(mask & (1ull << i))) continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\sample_%03u_frame_%05u_"
                         "surface_%02u_off_%08X.ppm",
                         g_ld_a010_probe_sample, g_ld_frames, i,
                         g.surfaces[i].offset);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
                fprintf(stderr,
                        "[a010-probe] SURFACE sample=%u index=%u location=%u "
                        "offset=0x%08X size=%ux%u role=%s\n",
                        g_ld_a010_probe_sample, i, g.surfaces[i].location,
                        g.surfaces[i].offset, g.surfaces[i].w,
                        g.surfaces[i].h,
                        i == target ? "present" : "offscreen");
            }
            g_ld_a010_probe_touched = 0;
            g_ld_a010_probe_sample++;
            fflush(stderr);
            if (capture_once) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "surface-capture sample=%u frame=%u mask=%016llX\n",
                            g_ld_a010_probe_sample - 1u, g_ld_frames,
                            (unsigned long long)mask);
                    fclose(acceptance);
                }
                InterlockedExchange(&g_ld_a010_probe_active, 0);
            }
        }
        if (elapsed >= 640 &&
            (!capture_once || g_ld_a010_probe_sample != 0u)) {
            fprintf(stderr,
                    "[a010-probe] END frame-cap live_frame=%u samples=%u\n",
                    g_ld_frames, g_ld_a010_probe_sample);
            fflush(stderr);
            InterlockedExchange(&g_ld_a010_probe_active, 0);
        }
    }
#endif

    /* new frame: reset per-frame ring cursors */
    g.vb_used = 0; g.ib_used = 0; g.cb_used = 0; g.ps_cb_used = 0;
    g.srv_ring_used = 0; g.smp_ring_used = 0;
#if !defined(YZ_PERF_CLEAN)
    g_ld_frame_vertex_constant_ring_recycles = 0;
#endif
    g.depth_cleared = 0;
    /* Per-zeta resources model persistent RSX memory.  Do not mark them
     * uncleared at a host-frame boundary: the implicit-clear branch would
     * erase depth rendered in an earlier frame and demote had_write before a
     * later pass can sample that zeta.  Only an actual guest depth clear may
     * clear/demote a tracked zeta (sink_clear does that above). */
}

void rsx_live_draw_shutdown(void)
{
    if (!g.ready) return;
    fprintf(stderr,
            "[surface-capacity-summary] allocated=%u capacity=%u depth=%u unique=%u census_capacity=%u untracked_requests=%llu capacity_rejects=%llu exact=%u\n",
            g.n_surfaces, MAX_SURFACES, g.n_zdepths,
            g_ld_surface_demand_count, MAX_SURFACE_DEMAND_KEYS,
            (unsigned long long)g_ld_surface_demand_untracked,
            (unsigned long long)g_ld_surface_capacity_rejects,
            g_ld_surface_demand_untracked == 0);
    ld_vertex_diag_emit("shutdown", 1);
    ld_present_measure_dump();
    /* let the GPU drain, then release. (Best-effort; process teardown also
     * reclaims.) */
    ld_flush(LD_FLUSH_SHUTDOWN);
    /* Failed Signal/device loss cannot prove safe release. Keep every GPU
       reference alive for process/device teardown rather than free in flight. */
    if(g_af.on&&(g_af.poisoned||!g.ready))return;
    if(!ld_af_completion_shutdown())return;
    ld_af_counts_emit("shutdown");
    ld_gt_shutdown();
    if(g_af.on){rsx_set_backend_label_boundary(NULL,NULL);rsx_set_backend_label_hook(NULL,NULL);}
#if defined(YZ_PERF_PROFILE)
    fprintf(stderr,
            "[rsx-perf-summary] frames=%u "
            "pso{look=%llu hit=%llu miss=%llu probes=%llu full=%llu "
            "cached=%u capacity=%u mem_kb=%.1f} "
            "reject{requests=%llu unique=%u "
            "world=%llu/%u character=%llu/%u "
            "ui=%llu/%u other=%llu/%u} "
            "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu vs_unique=%u "
            "ps_lookup=%llu ps_hit=%llu ps_compile=%llu ps_unique=%u} "
            "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
            "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
            "vertex{path=%s used_avg=%.3f used_max=%u "
            "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
            "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
            "frame{count=%llu avg_ms=%.3f compile_free=%llu "
            "compile_free_avg_ms=%.3f} "
            "draw{input_v=%llu expanded_v=%llu vb_mb=%.3f} "
            "tex{cached=%u evictions=%llu upload_mb=%.3f "
            "decode=%llu decode_ms=%.3f} "
            "high{upload_mb=%.3f vb_mb=%.3f retired=%u}\n",
            g_ld_frames,
            (unsigned long long)g_ld_profile.total.pso_lookups,
            (unsigned long long)g_ld_profile.total.pso_hits,
            (unsigned long long)g_ld_profile.total.pso_misses,
            (unsigned long long)g_ld_profile.total.pso_probes,
            (unsigned long long)g_ld_profile.total.pso_full,
            g.n_psos,
            MAX_PSOS,
            (double)sizeof(g.psos) / 1024.0,
            (unsigned long long)g_ld_profile.total.pso_full,
            g_ld_profile.n_rejected_pso_keys,
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
            g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
            g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            g_ld_profile.n_vs_hashes,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            g_ld_profile.n_ps_hashes,
            ld_profile_ticks_ms(g_ld_profile.total.decompile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.create_pso_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.flush_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.fence_wait_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.sink_end_qpc),
            ld_vertex_mode_name(),
            g_ld_profile.total.used_attribute_draws
                ? (double)g_ld_profile.total.used_attribute_sum /
                      (double)g_ld_profile.total.used_attribute_draws
                : 0.0,
            g_ld_profile.total_used_attribute_max,
            (double)g_ld_profile.total.legacy_vertex_upload_bytes /
                (1024.0 * 1024.0),
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            ld_profile_ticks_ms(
                g_ld_profile.total.vertex_fetch_pack_qpc),
            (unsigned long long)
                g_ld_profile.total.flush_reason[LD_FLUSH_VERTEX_RING],
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .flush_reason_qpc[LD_FLUSH_VERTEX_RING]),
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .fence_reason_qpc[LD_FLUSH_VERTEX_RING]),
            (unsigned long long)g_ld_profile.measured_frames,
            g_ld_profile.measured_frames
                ? g_ld_profile.total_frame_ms /
                      (double)g_ld_profile.measured_frames
                : 0.0,
            (unsigned long long)g_ld_profile.compile_free_frames,
            g_ld_profile.compile_free_frames
                ? g_ld_profile.compile_free_frame_ms /
                      (double)g_ld_profile.compile_free_frames
                : 0.0,
            (unsigned long long)g_ld_profile.total.input_vertices,
            (unsigned long long)g_ld_profile.total.expanded_vertices,
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            g.n_textures,
            (unsigned long long)g_ld_texture_cache_evictions,
            (double)g_ld_profile.total.texture_upload_bytes /
                (1024.0 * 1024.0),
            (unsigned long long)
                g_ld_profile.total.texture_decode_calls,
            ld_profile_ticks_ms(
                g_ld_profile.total.texture_decode_qpc),
            (double)g_ld_profile.total_upload_high / (1024.0 * 1024.0),
            (double)g_ld_profile.total_vb_high / (1024.0 * 1024.0),
            g_ld_profile.total_retired_high);
    fprintf(stderr,
            "[shader-cache-summary] "
            "vs{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu} "
            "ps{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu}\n",
            g.vs_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_blob_misses,
            (unsigned long long)g_ld_profile.total.vs_blob_inserts,
            (unsigned long long)g_ld_profile.total.vs_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.vs_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_repeats,
            (unsigned long long)g.vs_blobs.retained_source_bytes,
            (unsigned long long)g.vs_blobs.retained_blob_bytes,
            g.ps_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_blob_misses,
            (unsigned long long)g_ld_profile.total.ps_blob_inserts,
            (unsigned long long)g_ld_profile.total.ps_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.ps_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_repeats,
            (unsigned long long)g.ps_blobs.retained_source_bytes,
            (unsigned long long)g.ps_blobs.retained_blob_bytes);
    fprintf(
        stderr,
        "[fp-constant-summary] mode=%s variant_profile=%s "
        "variants{exact_source=%u constant_specialized=%u canonical=%u "
        "collapsed_constants=%u collapsed_alpha=%u "
        "tracking_overflow=%llu} "
        "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
        "ring_capacity=%u capacity_failures=%llu "
        "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
        ld_fp_constant_mode_name(),ld_fp_variant_profile_status(),
        g_ld_profile.n_ps_exact_source_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes,
        g_ld_profile.n_ps_canonical_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes >=
                g_ld_profile.n_ps_canonical_hashes
            ? g_ld_profile.n_ps_constant_specialized_hashes -
                  g_ld_profile.n_ps_canonical_hashes
            : 0,
        g_ld_profile.n_ps_exact_source_hashes >=
                g_ld_profile.n_ps_constant_specialized_hashes
            ? g_ld_profile.n_ps_exact_source_hashes -
                  g_ld_profile.n_ps_constant_specialized_hashes
            : 0,
        (unsigned long long)
            g_ld_profile.ps_variant_tracking_overflow,
        (unsigned long long)
            g_ld_profile.total.ps_constant_allocations,
        (unsigned long long)
            g_ld_profile.total.ps_constant_upload_bytes,
        ld_profile_ticks_ms(
            g_ld_profile.total.ps_constant_upload_qpc),
        PS_CB_RING_BYTES,
        (unsigned long long)
            g_ld_profile.total.ps_constant_capacity_failures,
        (unsigned long long)g_ld_profile.total.flush_reason[
            LD_FLUSH_PIXEL_CONSTANT_RING],
        ld_profile_ticks_ms(g_ld_profile.total.flush_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]),
        ld_profile_ticks_ms(g_ld_profile.total.fence_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]));
    fflush(stderr);
#endif
    if (g_ld_shader_disk_ready > 0) {
        fprintf(stderr,
                "[shader-disk-cache-summary] hits{vs=%llu ps=%llu} "
                "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
                "rejects=%llu\n",
                (unsigned long long)g_ld_shader_disk_hits[0],
                (unsigned long long)g_ld_shader_disk_hits[1],
                (unsigned long long)g_ld_shader_disk_misses[0],
                (unsigned long long)g_ld_shader_disk_misses[1],
                (unsigned long long)g_ld_shader_disk_writes[0],
                (unsigned long long)g_ld_shader_disk_writes[1],
                (unsigned long long)g_ld_shader_disk_rejects);
    }
    ld_primitive_reject_summary();
    for (u32 i = 0; i < g.n_psos; i++) {
        if (g.psos[i].pso) g.psos[i].pso->lpVtbl->Release(g.psos[i].pso);
        free(g.psos[i].capture_ps_hlsl);
        g.psos[i].capture_ps_hlsl = NULL;
    }
    g_ld_capture_ps_source_bytes = 0;
    shader_blob_cache_release(&g.vs_blobs);
    shader_blob_cache_release(&g.ps_blobs);
    for (u32 i = 0; i < g.n_textures; i++) if (g.textures[i].tex) g.textures[i].tex->lpVtbl->Release(g.textures[i].tex);
    for (u32 i = 0; i < MAX_OWNED_CUBES; i++) if (g.owned_cubes[i].tex)
        g.owned_cubes[i].tex->lpVtbl->Release(g.owned_cubes[i].tex);
    for (u32 i = 0; i < g.n_vtex; i++) if (g.vtex[i].tex) g.vtex[i].tex->lpVtbl->Release(g.vtex[i].tex);
    for (u32 i = 0; i < g.n_surfaces; i++) if (g.surfaces[i].tex) g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].tex)
            g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
        if (g.zdepths[i].snapshot)
            g.zdepths[i].snapshot->lpVtbl->Release(
                g.zdepths[i].snapshot);
        for (u32 spare = 0; spare < 3; ++spare)
            if (g.zdepths[i].snapshot_spare[spare])
                g.zdepths[i].snapshot_spare[spare]->lpVtbl->Release(g.zdepths[i].snapshot_spare[spare]);
        if (g.zdepths[i].packed_snapshot)
            g.zdepths[i].packed_snapshot->lpVtbl->Release(g.zdepths[i].packed_snapshot);
    }
    if (g.movie_upload) g.movie_upload->lpVtbl->Release(g.movie_upload);
    if (g.movie_overlay_readback) g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
    if (g.movie_overlay_rgba) free(g.movie_overlay_rgba);
    if (g.movie_overlay_mask) free(g.movie_overlay_mask);
    if (g.white_tex) g.white_tex->lpVtbl->Release(g.white_tex);
    if (g.depth) g.depth->lpVtbl->Release(g.depth);
    if (g.ps_cb) g.ps_cb->lpVtbl->Release(g.ps_cb);
    if (g.depth_snapshot_pso)
        g.depth_snapshot_pso->lpVtbl->Release(g.depth_snapshot_pso);
    if (g.depth_pack_pso) g.depth_pack_pso->lpVtbl->Release(g.depth_pack_pso);
    if (g.depth_pack_rootsig) g.depth_pack_rootsig->lpVtbl->Release(g.depth_pack_rootsig);
    for (u32 i = 0; i < 4; i++)
        if (g.downsample_pso[i]) g.downsample_pso[i]->lpVtbl->Release(g.downsample_pso[i]);
    if (g.downsample_rootsig)
        g.downsample_rootsig->lpVtbl->Release(g.downsample_rootsig);
    if (g.depth_snapshot_rootsig)
        g.depth_snapshot_rootsig->lpVtbl->Release(
            g.depth_snapshot_rootsig);
    if (g.rootsig_x) g.rootsig_x->lpVtbl->Release(g.rootsig_x);
    if (g.swap) g.swap->lpVtbl->Release(g.swap);
    if (g_ld_info_queue) {
        g_ld_info_queue->lpVtbl->Release(g_ld_info_queue);
        g_ld_info_queue = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    if (g_ld_profile.adapter) {
        g_ld_profile.adapter->lpVtbl->Release(g_ld_profile.adapter);
        g_ld_profile.adapter = NULL;
    }
#endif
    if(g_af.on){
        for(u32 i=0;i<LD_AF_CONTEXTS;i++){
            ld_af_context *c=&g_af.ctx[i];
            if(i!=g_af.active)ld_af_release_retired(c);
            for(u32 b=0;b<5;b++)if(c->buffer[b]&&!(i==g_af.active&&b==4))c->buffer[b]->lpVtbl->Release(c->buffer[b]);
            if(c->srv)c->srv->lpVtbl->Release(c->srv);
            if(c->smp)c->smp->lpVtbl->Release(c->smp);
            if(c->list)c->list->lpVtbl->Release(c->list);
            if(c->alloc)c->alloc->lpVtbl->Release(c->alloc);
        }
        memset(&g_af,0,sizeof(g_af));
    }
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    memset(&g, 0, sizeof(g));
    if (dc.verts) { free(dc.verts); dc.verts = NULL; dc.cap_verts = 0; }
    if (dc.refs) { free(dc.refs); dc.refs = NULL; dc.cap_refs = 0; }
    if (dc.compact_verts) {
        free(dc.compact_verts);
        dc.compact_verts = NULL;
        dc.compact_capacity = 0;
    }
    if (dc.cuts) { free(dc.cuts); dc.cuts = NULL; dc.cap_cuts = 0; }
}

#endif /* _WIN32 */
