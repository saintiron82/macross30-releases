/*
 * Renderer-side glue for rsx_shader_prep.h: counters, gameplay miss records,
 * observed PSO descriptors and the local pipeline library. Render thread only.
 *
 * Environment switches (all default on):
 *   RSX_NO_SHADER_SEED=1   ignore the shipped DXBC seed
 *   RSX_NO_PSO_LIBRARY=1   never open the local pipeline library
 *   RSX_NO_PSO_RECORD=1    do not append newly seen PSO descriptors
 */
#ifndef RSX_SHADER_PREP_RUNTIME_H
#define RSX_SHADER_PREP_RUNTIME_H
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#include "rsx_shader_prep.h"

enum { RSP_STAGE_V = 0, RSP_STAGE_P = 1, RSP_STAGE_B = 2, RSP_STAGES = 3 };
enum { RSP_REJECT_FORMAT = 0, RSP_REJECT_SOURCE = 1, RSP_REJECT_DXBC = 2, RSP_REJECT_KINDS = 3 };

typedef struct rsp_runtime {
    int initialized, library_enabled, record_enabled;
    rsp_library_status library_status;
    HRESULT library_hr;
    ID3D12PipelineLibrary* library;
    void* library_blob;
    rsp_library_header library_header;
    rsp_inventory known;                 /* shipped inventory + observed file */
    int inventory_records, observed_records;
    long observed_good_end;          /* valid prefix of the observed file */
    uint64_t root_hash;
    ID3D12RootSignature* root;
    uint8_t root_blob[RSP_MAX_ROOT_BYTES];
    uint32_t root_bytes;
    FILE* observed;
    char cache_dir[MAX_PATH];
    uint64_t user_hits[RSP_STAGES], seed_hits[RSP_STAGES], compiles[RSP_STAGES], compile_failures[RSP_STAGES];
    double compile_ms[RSP_STAGES], compile_max_ms[RSP_STAGES];
    uint64_t user_rejects[RSP_REJECT_KINDS], seed_rejects[RSP_REJECT_KINDS];
    uint64_t pso_library_hits, pso_created_known, pso_created_unknown, pso_create_failures;
    uint64_t pso_bypass, pso_recorded, pso_record_failures, pso_events;
    double pso_create_ms, pso_create_max_ms, pso_load_ms;
} rsp_runtime;

static rsp_runtime g_rsp;

static uint32_t rsp_stage_index(uint32_t stage)
{
    return stage == 'V' ? RSP_STAGE_V : stage == 'P' ? RSP_STAGE_P : RSP_STAGE_B;
}

static double rsp_now_ms(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return 1000.0 * (double)now.QuadPart / (double)frequency.QuadPart;
}

static void rsp_rt_log_stats(const char* reason)
{
    const rsp_runtime* r = &g_rsp;
    fprintf(stderr,
        "[shader-prep] stats reason=%s seed_hits{v=%llu p=%llu b=%llu} user_hits{v=%llu p=%llu b=%llu} "
        "compiles{v=%llu p=%llu b=%llu} compile_ms{v=%.1f p=%.1f b=%.1f} rejects{user=%llu/%llu/%llu seed=%llu/%llu/%llu} "
        "pso{library_hits=%llu created_known=%llu created_unknown=%llu recorded=%llu bypass=%llu failures=%llu create_ms=%.1f max_ms=%.2f load_ms=%.1f} "
        "library=%s known=%u\n",
        reason,
        (unsigned long long)r->seed_hits[0], (unsigned long long)r->seed_hits[1], (unsigned long long)r->seed_hits[2],
        (unsigned long long)r->user_hits[0], (unsigned long long)r->user_hits[1], (unsigned long long)r->user_hits[2],
        (unsigned long long)r->compiles[0], (unsigned long long)r->compiles[1], (unsigned long long)r->compiles[2],
        r->compile_ms[0], r->compile_ms[1], r->compile_ms[2],
        (unsigned long long)r->user_rejects[0], (unsigned long long)r->user_rejects[1], (unsigned long long)r->user_rejects[2],
        (unsigned long long)r->seed_rejects[0], (unsigned long long)r->seed_rejects[1], (unsigned long long)r->seed_rejects[2],
        (unsigned long long)r->pso_library_hits, (unsigned long long)r->pso_created_known,
        (unsigned long long)r->pso_created_unknown, (unsigned long long)r->pso_recorded,
        (unsigned long long)r->pso_bypass, (unsigned long long)r->pso_create_failures,
        r->pso_create_ms, r->pso_create_max_ms, r->pso_load_ms,
        rsp_library_status_name(r->library_status), r->known.count);
}

static int rsp_power_of_two_tick(uint64_t value)
{
    return value <= 4u || (value & (value - 1u)) == 0u;
}

/* Disk/seed loader outcome. source: 0 user cache, 1 shipped seed. */
static void rsp_rt_note_load(uint32_t stage, int source, int hit, int reject_kind)
{
    const uint32_t s = rsp_stage_index(stage);
    if (hit) {
        uint64_t* counter = source ? &g_rsp.seed_hits[s] : &g_rsp.user_hits[s];
        ++*counter;
        if (source && rsp_power_of_two_tick(*counter))
            rsp_rt_log_stats("seed-hit");
    } else if (reject_kind >= 0 && reject_kind < RSP_REJECT_KINDS) {
        (source ? g_rsp.seed_rejects : g_rsp.user_rejects)[reject_kind]++;
        rsp_rt_log_stats(source ? "seed-reject" : "user-reject");
    }
}

/* Every real compile is a coverage miss: logged and appended to the miss log. */
static void rsp_rt_note_compile(uint32_t stage, uint64_t hash, uint32_t source_length,
                                double ms, uint32_t frame, HRESULT hr, const char* cache_dir)
{
    const uint32_t s = rsp_stage_index(stage);
    g_rsp.compiles[s]++;
    if (FAILED(hr)) g_rsp.compile_failures[s]++;
    g_rsp.compile_ms[s] += ms;
    if (ms > g_rsp.compile_max_ms[s]) g_rsp.compile_max_ms[s] = ms;
    const uint64_t total = g_rsp.compiles[0] + g_rsp.compiles[1] + g_rsp.compiles[2];
    if (total <= 256u)
        fprintf(stderr, "[shader-miss] stage=%c source=%016llX bytes=%u compile_ms=%.3f frame=%u hr=0x%08lX\n",
                (char)stage, (unsigned long long)hash, source_length, ms, frame, (unsigned long)hr);
    if (cache_dir && cache_dir[0] && total <= 4096u) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof path, "%s\\%s", cache_dir, RSP_MISS_LOG_NAME) < (int)sizeof path) {
            FILE* f = fopen(path, "ab");
            if (f) {
                SYSTEMTIME t; GetSystemTime(&t);
                fprintf(f, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%c\t%016llX\t%u\t%.3f\t%u\t0x%08lX\n",
                        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                        (char)stage, (unsigned long long)hash, source_length, ms, frame, (unsigned long)hr);
                fclose(f);
            }
        }
    }
    rsp_rt_log_stats("compile");
}

/* The main draw root signature; others (post-process, blits) bypass. */
static void rsp_rt_register_root(const void* blob, size_t bytes, ID3D12RootSignature* root)
{
    if (!blob || !bytes || bytes > RSP_MAX_ROOT_BYTES) return;
    memcpy(g_rsp.root_blob, blob, bytes);
    g_rsp.root_bytes = (uint32_t)bytes;
    g_rsp.root_hash = rsp_fnv1a(blob, bytes, RSP_FNV_SEED);
    g_rsp.root = root;
}

static void rsp_rt_init(ID3D12Device* dev, const char* cache_dir)
{
    if (g_rsp.initialized) return;
    g_rsp.initialized = 1;
    if (cache_dir) snprintf(g_rsp.cache_dir, sizeof g_rsp.cache_dir, "%s", cache_dir);
    g_rsp.library_enabled = !getenv("RSX_NO_PSO_LIBRARY") && g_rsp.cache_dir[0];
    g_rsp.record_enabled = !getenv("RSX_NO_PSO_RECORD") && g_rsp.cache_dir[0];

    wchar_t inventory_path[MAX_PATH];
    const DWORD override_length = GetEnvironmentVariableW(L"RSX_PSO_INVENTORY", inventory_path, MAX_PATH);
    const int have_inventory = override_length && override_length < MAX_PATH
        ? 1 : rsp_module_path(inventory_path, MAX_PATH, RSP_INVENTORY_NAME);
    if (have_inventory) {
        FILE* f = _wfopen(inventory_path, L"rb");
        if (f) { g_rsp.inventory_records = rsp_inventory_read_file(&g_rsp.known, f); fclose(f); }
        else g_rsp.inventory_records = -1;
    }
    char path[MAX_PATH];
    if (g_rsp.cache_dir[0] && snprintf(path, sizeof path, "%s\\%s", g_rsp.cache_dir, RSP_OBSERVED_NAME) < (int)sizeof path) {
        FILE* f = fopen(path, "rb");
        if (f) {
            g_rsp.observed_records = rsp_inventory_read_file(&g_rsp.known, f);
            g_rsp.observed_good_end = g_rsp.observed_records >= 0 ? g_rsp.known.last_good_end : 0;
            fclose(f);
        } else {
            g_rsp.observed_records = -1;
        }
    }

    rsp_adapter_identity id = rsp_query_adapter(dev);
    if (!g_rsp.library_enabled) {
        g_rsp.library_status = RSP_LIB_ABSENT;
    } else if (!rsp_library_supported(dev)) {
        g_rsp.library_status = RSP_LIB_UNSUPPORTED;
    } else if (snprintf(path, sizeof path, "%s\\%s", g_rsp.cache_dir, RSP_LIBRARY_NAME) < (int)sizeof path) {
        const double begin = rsp_now_ms();
        g_rsp.library_status = rsp_library_open(dev, path, &id, &g_rsp.library, &g_rsp.library_blob,
                                                &g_rsp.library_header, &g_rsp.library_hr);
        g_rsp.pso_load_ms += rsp_now_ms() - begin;
    }
    fprintf(stderr,
            "[shader-prep] init library=%s hr=0x%08lX entries=%u complete=%u adapter=%04X:%04X:%08X:%02X umd=%llu "
            "inventory=%d observed=%d known=%u corrupt_chunks=%u root=%016llX record_bytes=%u\n",
            rsp_library_status_name(g_rsp.library_status), (unsigned long)g_rsp.library_hr,
            g_rsp.library_status == RSP_LIB_LOADED ? g_rsp.library_header.entries : 0u,
            g_rsp.library_status == RSP_LIB_LOADED ? g_rsp.library_header.complete : 0u,
            id.vendor_id, id.device_id, id.subsys_id, id.revision, (unsigned long long)id.umd,
            g_rsp.inventory_records, g_rsp.observed_records, g_rsp.known.count,
            g_rsp.known.corrupt_chunks, (unsigned long long)g_rsp.root_hash,
            (unsigned)sizeof(rsp_pso_record));
}

static void rsp_rt_record(const rsp_pso_record* rec, rsp_key key)
{
    if (!g_rsp.record_enabled || rsp_inventory_has(&g_rsp.known, key)) return;
    if (!g_rsp.observed) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof path, "%s\\%s", g_rsp.cache_dir, RSP_OBSERVED_NAME) >= (int)sizeof path) return;
        /* Keep the valid prefix read at init; a torn tail from an earlier
         * crash is cut off so new records stay readable. */
        FILE* f = g_rsp.observed_good_end > 0 ? fopen(path, "r+b") : fopen(path, "wb");
        if (!f) { g_rsp.pso_record_failures++; g_rsp.record_enabled = 0; return; }
        if (g_rsp.observed_good_end > 0) {
            if (_chsize(_fileno(f), g_rsp.observed_good_end) != 0 || fseek(f, 0, SEEK_END) != 0) {
                fclose(f); g_rsp.pso_record_failures++; g_rsp.record_enabled = 0; return;
            }
        } else if (!rsp_write_file_header(f)) {
            fclose(f); g_rsp.pso_record_failures++; g_rsp.record_enabled = 0; return;
        }
        g_rsp.observed = f;
        /* Every append session restates the root signature so a later reader
         * never needs another file to rebuild a descriptor. */
        rsp_write_chunk(f, RSP_CHUNK_ROOT, &g_rsp.root_hash, 8, g_rsp.root_blob, g_rsp.root_bytes);
    }
    if (rsp_write_chunk(g_rsp.observed, RSP_CHUNK_PSOD, rec, sizeof *rec, NULL, 0) && fflush(g_rsp.observed) == 0) {
        g_rsp.pso_recorded++;
        rsp_inventory_add(&g_rsp.known, rec);
    } else {
        g_rsp.pso_record_failures++;
    }
}

typedef struct rsp_rt_query {
    int active, known;
    rsp_pso_record record;
    rsp_key key;
    double begin;
} rsp_rt_query;

/* Try the local library. Returns a PSO on hit; otherwise the caller creates
 * it normally and reports through rsp_rt_pso_created. */
static ID3D12PipelineState* rsp_rt_pso_lookup(
    rsp_rt_query* q, ID3D12Device* dev, const char* cache_dir,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    uint64_t vs_hash, uint32_t vs_length, uint64_t ps_hash, uint32_t ps_length)
{
    memset(q, 0, sizeof *q);
    q->begin = rsp_now_ms();
    rsp_rt_init(dev, cache_dir);
    if (!g_rsp.root || desc->pRootSignature != g_rsp.root) { g_rsp.pso_bypass++; return NULL; }
    rsp_shader_ref vs, ps;
    rsp_shader_ref_fill(&vs, 'V', vs_hash, vs_length, desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
    rsp_shader_ref_fill(&ps, 'P', ps_hash, ps_length, desc->PS.pShaderBytecode, desc->PS.BytecodeLength);
    if (!rsp_record_from_desc(&q->record, desc, g_rsp.root_hash, &vs, &ps)) { g_rsp.pso_bypass++; return NULL; }
    q->active = 1;
    q->key = rsp_record_key(&q->record);
    q->known = rsp_inventory_has(&g_rsp.known, q->key);
    if (!g_rsp.library) return NULL;
    wchar_t name[40];
    rsp_key_name(q->key, name);
    ID3D12PipelineState* pso = NULL;
    const HRESULT hr = g_rsp.library->lpVtbl->LoadGraphicsPipeline(
        g_rsp.library, name, desc, &IID_ID3D12PipelineState, (void**)&pso);
    if (SUCCEEDED(hr) && pso) {
        const double ms = rsp_now_ms() - q->begin;
        g_rsp.pso_library_hits++;
        g_rsp.pso_load_ms += ms;
        if (g_rsp.pso_library_hits <= 1024u)
            fprintf(stderr, "[pso-library-hit] key=%016llX%016llX load_ms=%.3f\n",
                    (unsigned long long)q->key.hi, (unsigned long long)q->key.lo, ms);
        if (rsp_power_of_two_tick(g_rsp.pso_library_hits)) rsp_rt_log_stats("pso-library-hit");
        return pso;
    }
    return NULL;
}

static void rsp_rt_pso_created(rsp_rt_query* q, HRESULT hr, uint32_t frame)
{
    const double ms = rsp_now_ms() - q->begin;
    if (!q->active) return;
    if (FAILED(hr)) { g_rsp.pso_create_failures++; return; }
    g_rsp.pso_create_ms += ms;
    if (ms > g_rsp.pso_create_max_ms) g_rsp.pso_create_max_ms = ms;
    if (q->known) g_rsp.pso_created_known++; else g_rsp.pso_created_unknown++;
    const uint64_t events = ++g_rsp.pso_events;
    if (events <= 512u)
        fprintf(stderr, "[pso-miss] key=%016llX%016llX known=%d library=%s create_ms=%.3f frame=%u\n",
                (unsigned long long)q->key.hi, (unsigned long long)q->key.lo, q->known,
                rsp_library_status_name(g_rsp.library_status), ms, frame);
    if (!q->known) rsp_rt_record(&q->record, q->key);
    if (rsp_power_of_two_tick(events)) rsp_rt_log_stats("pso-create");
}

/* Self-test only: drop library/inventory state so init runs again. */
static void rsp_rt_reset(void)
{
    if (g_rsp.library) g_rsp.library->lpVtbl->Release(g_rsp.library);
    free(g_rsp.library_blob);
    if (g_rsp.observed) fclose(g_rsp.observed);
    rsp_inventory_free(&g_rsp.known);
    const uint64_t root_hash = g_rsp.root_hash;
    ID3D12RootSignature* root = g_rsp.root;
    uint8_t blob[RSP_MAX_ROOT_BYTES];
    const uint32_t bytes = g_rsp.root_bytes;
    memcpy(blob, g_rsp.root_blob, bytes);
    memset(&g_rsp, 0, sizeof g_rsp);
    g_rsp.root_hash = root_hash; g_rsp.root = root; g_rsp.root_bytes = bytes;
    memcpy(g_rsp.root_blob, blob, bytes);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif /* RSX_SHADER_PREP_RUNTIME_H */
