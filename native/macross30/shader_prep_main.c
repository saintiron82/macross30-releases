/*
 * MacrossShaderPrep: first-run / after-update graphics pipeline preparation.
 *
 * Builds the local D3D12 pipeline library the renderer opens at startup
 * (rsx_shader_prep_runtime.h) from device-independent PSO descriptor records:
 * the shipped inventory (assets\rsx_pso_inventory_v1.bin) plus descriptors the
 * game observed on this PC (<cache>\pso-observed-v1.bin). Shader bytecode comes
 * from the user cache or the shipped seed and must match each record's DXBC
 * digest. The resulting library is valid only for this adapter + driver; the
 * game refuses it otherwise and simply creates PSOs as before.
 *
 * Bounded (record cap, optional time limit), resumable (atomic checkpoints;
 * a restart skips names already stored) and cancellable (cancel file or
 * Ctrl+C; exit code 2). Progress is reported as JSON lines on stdout.
 *
 * Exit codes: 0 complete, 2 cancelled/time-bounded (resume later),
 *             3 library unsupported (driver cache warmed only), 1 error,
 *             10 (--status) preparation needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "rsx_shader_prep.h"

#define PREP_TOOL_VERSION "1.0.0"
#define SHADER_CACHE_VERSION 2u
#define SHADER_CACHE_MAGIC 0x43535A59u
#define STATUS_NAME "pso-library-v1.json"

typedef struct { uint32_t magic, version, stage, source_length, blob_length, reserved; uint64_t source_hash; } cache_header;

static volatile LONG g_ctrl_c;
static BOOL WINAPI on_ctrl(DWORD type) { (void)type; InterlockedExchange(&g_ctrl_c, 1); return TRUE; }

typedef struct options {
    char cache_dir[MAX_PATH];
    wchar_t seed_dir[MAX_PATH];
    wchar_t inventory[MAX_PATH];
    char cancel_file[MAX_PATH];
    double max_seconds;
    uint32_t checkpoint_every;
    uint32_t cancel_after;      /* test hook: stop after N newly created PSOs */
    int status_only, warp, rebuild, quiet;
} options;

typedef struct counters {
    uint32_t total, processed, already, created, missing_shader, bad_shader, failed, store_conflict, no_root;
} counters;

static double now_ms(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)f.QuadPart;
}

static void json_string(FILE* f, const char* s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", (unsigned char)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void json_wstring(FILE* f, const wchar_t* s)
{
    char utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, sizeof utf8, NULL, NULL);
    json_string(f, utf8);
}

static void print_counters(FILE* f, const counters* c)
{
    fprintf(f, "\"total\":%u,\"processed\":%u,\"already\":%u,\"created\":%u,\"prepared\":%u,"
               "\"missing_shader\":%u,\"bad_shader\":%u,\"failed\":%u,\"store_conflict\":%u,\"no_root\":%u",
            c->total, c->processed, c->already, c->created, c->already + c->created,
            c->missing_shader, c->bad_shader, c->failed, c->store_conflict, c->no_root);
}

/* Shader bytecode for a record reference: user cache first, then seed. The
 * file's DXBC must be valid and its digest/length must equal the record's. */
static void* load_shader(const options* o, const rsp_shader_ref* ref, size_t* size, int* bad)
{
    *size = 0; *bad = 0;
    const uint64_t hash = ((uint64_t)ref->source_hash_hi << 32) | ref->source_hash_lo;
    for (int pass = 0; pass < 2; pass++) {
        wchar_t path[MAX_PATH];
        if (pass == 0) {
            wchar_t dir[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, o->cache_dir, -1, dir, MAX_PATH);
            _snwprintf(path, MAX_PATH, L"%s\\%c_%016llX_%08X.bin", dir, (wchar_t)ref->stage,
                       (unsigned long long)hash, ref->source_length);
        } else {
            _snwprintf(path, MAX_PATH, L"%s\\%c_%016llX_%08X.bin", o->seed_dir, (wchar_t)ref->stage,
                       (unsigned long long)hash, ref->source_length);
        }
        path[MAX_PATH - 1] = 0;
        FILE* f = _wfopen(path, L"rb");
        if (!f) continue;
        cache_header h;
        void* blob = NULL;
        int ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == SHADER_CACHE_MAGIC &&
                 h.version == SHADER_CACHE_VERSION && h.stage == ref->stage &&
                 h.source_hash == hash && h.source_length == ref->source_length &&
                 h.blob_length == ref->bytecode_length && h.blob_length > 0 && h.blob_length <= 16u * 1024u * 1024u;
        if (ok) ok = fseek(f, (long)h.source_length, SEEK_CUR) == 0;
        if (ok) { blob = malloc(h.blob_length); ok = blob && fread(blob, 1, h.blob_length, f) == h.blob_length; }
        fclose(f);
        if (ok) ok = rsp_dxbc_valid(blob, h.blob_length) && memcmp((uint8_t*)blob + 4, ref->digest, 16) == 0;
        if (ok) { *size = h.blob_length; return blob; }
        free(blob);
        *bad = 1;
    }
    return NULL;
}

static int write_library(const options* o, ID3D12PipelineLibrary* lib, const rsp_adapter_identity* id,
                         uint32_t entries, uint64_t inventory_hash, int complete)
{
    const SIZE_T bytes = lib->lpVtbl->GetSerializedSize(lib);
    if (!bytes || bytes > 512u * 1024u * 1024u) return 0;
    void* data = malloc(bytes);
    if (!data) return 0;
    if (FAILED(lib->lpVtbl->Serialize(lib, data, bytes))) { free(data); return 0; }
    rsp_library_header h;
    memset(&h, 0, sizeof h);
    h.magic = RSP_LIBRARY_MAGIC; h.version = RSP_LIBRARY_VERSION; h.header_bytes = sizeof h;
    h.record_version = RSP_RECORD_VERSION;
    h.vendor_id = id->vendor_id; h.device_id = id->device_id; h.subsys_id = id->subsys_id; h.revision = id->revision;
    h.umd_lo = (uint32_t)id->umd; h.umd_hi = (uint32_t)(id->umd >> 32);
    h.description_hash_lo = (uint32_t)id->description_hash; h.description_hash_hi = (uint32_t)(id->description_hash >> 32);
    h.entries = entries; h.blob_bytes = (uint32_t)bytes;
    const uint64_t blob_hash = rsp_fnv1a(data, bytes, RSP_FNV_SEED);
    h.blob_hash_lo = (uint32_t)blob_hash; h.blob_hash_hi = (uint32_t)(blob_hash >> 32);
    h.inventory_hash_lo = (uint32_t)inventory_hash; h.inventory_hash_hi = (uint32_t)(inventory_hash >> 32);
    h.complete = (uint32_t)complete;
    char path[MAX_PATH], temporary[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", o->cache_dir, RSP_LIBRARY_NAME);
    snprintf(temporary, sizeof temporary, "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId());
    FILE* f = fopen(temporary, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, bytes, f) == bytes && fflush(f) == 0;
    if (f && fclose(f)) ok = 0;
    free(data);
    if (!ok || !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary);
        return 0;
    }
    return 1;
}

static void write_status(const options* o, const char* status, const char* library_state,
                         const rsp_adapter_identity* id, const counters* c, uint64_t inventory_hash,
                         int inventory_records, int observed_records, double ms)
{
    char path[MAX_PATH], temporary[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", o->cache_dir, STATUS_NAME);
    snprintf(temporary, sizeof temporary, "%s.tmp", path);
    FILE* f = fopen(temporary, "wb");
    if (!f) return;
    fprintf(f, "{\"tool\":\"MacrossShaderPrep\",\"tool_version\":\"%s\",\"status\":", PREP_TOOL_VERSION);
    json_string(f, status);
    fprintf(f, ",\"library_at_start\":");
    json_string(f, library_state);
    fprintf(f, ",\"record_version\":%u,\"inventory_hash\":\"%016llX\",\"inventory_records\":%d,\"observed_records\":%d,",
            RSP_RECORD_VERSION, (unsigned long long)inventory_hash, inventory_records, observed_records);
    fprintf(f, "\"adapter\":{\"vendor_id\":%u,\"device_id\":%u,\"subsys_id\":%u,\"revision\":%u,\"umd\":%llu,\"description\":",
            id->vendor_id, id->device_id, id->subsys_id, id->revision, (unsigned long long)id->umd);
    json_wstring(f, id->description);
    fprintf(f, "},\"seed_dir\":");
    json_wstring(f, o->seed_dir);
    fprintf(f, ",\"inventory\":");
    json_wstring(f, o->inventory);
    fprintf(f, ",\"elapsed_ms\":%.1f,", ms);
    print_counters(f, c);
    fprintf(f, ",\"driver_blob_portable\":false}\n");
    fclose(f);
    MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING);
}

static void emit(const options* o, const char* event, const counters* c, double elapsed, const char* extra)
{
    if (o->quiet) return;
    printf("{\"event\":\"%s\",", event);
    print_counters(stdout, c);
    printf(",\"elapsed_ms\":%.1f%s%s}\n", elapsed, extra && extra[0] ? "," : "", extra ? extra : "");
    fflush(stdout);
}

static int should_stop(const options* o, double begin, uint32_t created_this_run)
{
    if (InterlockedCompareExchange(&g_ctrl_c, 0, 0)) return 1;
    if (o->cancel_file[0] && GetFileAttributesA(o->cancel_file) != INVALID_FILE_ATTRIBUTES) return 1;
    if (o->max_seconds > 0 && now_ms() - begin > o->max_seconds * 1000.0) return 1;
    if (o->cancel_after && created_this_run >= o->cancel_after) return 1;
    return 0;
}

static int parse(options* o, int argc, wchar_t** argv)
{
    memset(o, 0, sizeof *o);
    o->checkpoint_every = 32;
    rsp_module_path(o->seed_dir, MAX_PATH, L"assets\\rsx_shader_cache_v2");
    rsp_module_path(o->inventory, MAX_PATH, RSP_INVENTORY_NAME);
    for (int i = 1; i < argc; i++) {
        const wchar_t* a = argv[i];
        const wchar_t* v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (wcscmp(a, L##name) == 0)
        if (ARG("--status")) o->status_only = 1;
        else if (ARG("--warp")) o->warp = 1;
        else if (ARG("--rebuild")) o->rebuild = 1;
        else if (ARG("--quiet")) o->quiet = 1;
        else if (v && ARG("--cache-dir")) { WideCharToMultiByte(CP_UTF8, 0, v, -1, o->cache_dir, MAX_PATH, NULL, NULL); i++; }
        else if (v && ARG("--seed-dir")) { _snwprintf(o->seed_dir, MAX_PATH, L"%s", v); i++; }
        else if (v && ARG("--inventory")) { _snwprintf(o->inventory, MAX_PATH, L"%s", v); i++; }
        else if (v && ARG("--cancel-file")) { WideCharToMultiByte(CP_UTF8, 0, v, -1, o->cancel_file, MAX_PATH, NULL, NULL); i++; }
        else if (v && ARG("--max-seconds")) { o->max_seconds = _wtof(v); i++; }
        else if (v && ARG("--checkpoint-every")) { o->checkpoint_every = (uint32_t)_wtoi(v); i++; }
        else if (v && ARG("--cancel-after")) { o->cancel_after = (uint32_t)_wtoi(v); i++; }
        else { fwprintf(stderr, L"unknown argument %s\n", a); return 0; }
#undef ARG
    }
    if (!o->cache_dir[0]) { fprintf(stderr, "--cache-dir is required\n"); return 0; }
    if (!o->checkpoint_every) o->checkpoint_every = 1;
    return 1;
}

static uint64_t inventory_hash(const rsp_inventory* inv)
{
    uint64_t h = RSP_FNV_SEED;
    h = rsp_fnv1a(inv->keys, (size_t)inv->count * sizeof *inv->keys, h);
    return h;
}

int wmain(int argc, wchar_t** argv)
{
    options o;
    if (!parse(&o, argc, argv)) return 1;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    const double begin = now_ms();
    CreateDirectoryA(o.cache_dir, NULL);

    rsp_inventory inv;
    memset(&inv, 0, sizeof inv);
    int inventory_records = -1, observed_records = -1;
    FILE* f = _wfopen(o.inventory, L"rb");
    if (f) { inventory_records = rsp_inventory_read_file(&inv, f); fclose(f); }
    char observed[MAX_PATH];
    snprintf(observed, sizeof observed, "%s\\%s", o.cache_dir, RSP_OBSERVED_NAME);
    f = fopen(observed, "rb");
    if (f) { observed_records = rsp_inventory_read_file(&inv, f); fclose(f); }
    const uint64_t inv_hash = inventory_hash(&inv);

    ID3D12Device* dev = NULL;
    HRESULT hr;
    if (o.warp) {
        IDXGIFactory4* factory = NULL; IDXGIAdapter* warp = NULL;
        hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
        if (SUCCEEDED(hr)) hr = factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void**)&warp);
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice((IUnknown*)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev);
        if (warp) warp->lpVtbl->Release(warp);
        if (factory) factory->lpVtbl->Release(factory);
    } else {
        /* Same adapter choice as the renderer (default adapter). */
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev);
    }
    counters c;
    memset(&c, 0, sizeof c);
    c.total = inv.count;
    if (FAILED(hr) || !dev) {
        printf("{\"event\":\"error\",\"reason\":\"device\",\"hr\":\"0x%08lX\"}\n", (unsigned long)hr);
        return 1;
    }
    const rsp_adapter_identity id = rsp_query_adapter(dev);
    const int supported = rsp_library_supported(dev);

    /* Existing library: keep and extend when it belongs to this adapter/driver. */
    ID3D12PipelineLibrary* lib = NULL;
    void* lib_blob = NULL;
    rsp_library_header existing;
    memset(&existing, 0, sizeof existing);
    rsp_library_status state = RSP_LIB_UNSUPPORTED;
    char library_path[MAX_PATH];
    snprintf(library_path, sizeof library_path, "%s\\%s", o.cache_dir, RSP_LIBRARY_NAME);
    if (supported) {
        state = o.rebuild ? RSP_LIB_ABSENT
                          : rsp_library_open(dev, library_path, &id, &lib, &lib_blob, &existing, NULL);
    }
    if (!o.quiet) {
        printf("{\"event\":\"start\",\"tool_version\":\"%s\",\"library_at_start\":\"%s\",\"library_supported\":%d,"
               "\"inventory_records\":%d,\"observed_records\":%d,\"known\":%u,\"inventory_hash\":\"%016llX\","
               "\"existing_complete\":%u,\"existing_inventory_hash\":\"%08X%08X\",\"corrupt_chunks\":%u,"
               "\"adapter\":\"%04X:%04X:%08X:%02X\",\"umd\":%llu}\n",
               PREP_TOOL_VERSION, rsp_library_status_name(state), supported, inventory_records, observed_records,
               inv.count, (unsigned long long)inv_hash, existing.complete, existing.inventory_hash_hi,
               existing.inventory_hash_lo, inv.corrupt_chunks, id.vendor_id, id.device_id, id.subsys_id,
               id.revision, (unsigned long long)id.umd);
        fflush(stdout);
    }

    if (o.status_only) {
        const int current = state == RSP_LIB_LOADED && existing.complete &&
            existing.inventory_hash_lo == (uint32_t)inv_hash && existing.inventory_hash_hi == (uint32_t)(inv_hash >> 32);
        printf("{\"event\":\"status\",\"needed\":%d,\"library\":\"%s\",\"known\":%u}\n",
               supported && inv.count && !current, rsp_library_status_name(state), inv.count);
        return supported && inv.count && !current ? 10 : 0;
    }

    ID3D12Device1* dev1 = NULL;
    if (supported && !lib) {
        if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12Device1, (void**)&dev1)) && dev1) {
            if (FAILED(dev1->lpVtbl->CreatePipelineLibrary(dev1, NULL, 0, &IID_ID3D12PipelineLibrary, (void**)&lib)))
                lib = NULL;
            dev1->lpVtbl->Release(dev1);
        }
    }

    ID3D12RootSignature* roots[RSP_MAX_ROOTS] = {0};
    for (uint32_t i = 0; i < inv.root_count; i++)
        if (FAILED(dev->lpVtbl->CreateRootSignature(dev, 0, inv.roots[i].data, inv.roots[i].bytes,
                                                    &IID_ID3D12RootSignature, (void**)&roots[i])))
            roots[i] = NULL;

    uint32_t dirty = 0, created_this_run = 0;
    uint32_t stored_entries = state == RSP_LIB_LOADED ? existing.entries : 0u;
    double last_checkpoint = now_ms(), last_progress = 0;
    int cancelled = 0, write_failed = 0;
    emit(&o, "progress", &c, now_ms() - begin, NULL);
    for (uint32_t i = 0; i < inv.count; i++) {
        if (should_stop(&o, begin, created_this_run)) { cancelled = 1; break; }
        const rsp_pso_record* rec = &inv.records[i];
        const uint64_t root_hash = ((uint64_t)rec->root_hash_hi << 32) | rec->root_hash_lo;
        ID3D12RootSignature* root = NULL;
        for (uint32_t r = 0; r < inv.root_count; r++) if (inv.roots[r].hash == root_hash) root = roots[r];
        c.processed++;
        if (!root) { c.no_root++; continue; }
        size_t vs_bytes = 0, ps_bytes = 0; int vs_bad = 0, ps_bad = 0;
        void* vs = load_shader(&o, &rec->vs, &vs_bytes, &vs_bad);
        void* ps = vs ? load_shader(&o, &rec->ps, &ps_bytes, &ps_bad) : NULL;
        if (!vs || !ps) {
            if (vs_bad || ps_bad) c.bad_shader++; else c.missing_shader++;
            free(vs); free(ps);
            continue;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
        D3D12_INPUT_ELEMENT_DESC elements[RSP_MAX_INPUTS];
        rsp_desc_from_record(&desc, elements, rec, root, vs, vs_bytes, ps, ps_bytes);
        wchar_t name[40];
        rsp_key_name(inv.keys[i], name);
        ID3D12PipelineState* pso = NULL;
        if (lib && SUCCEEDED(lib->lpVtbl->LoadGraphicsPipeline(lib, name, &desc, &IID_ID3D12PipelineState, (void**)&pso)) && pso) {
            c.already++;
        } else if (SUCCEEDED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void**)&pso)) && pso) {
            /* Without library support this still warms the driver's own cache. */
            if (!lib) { c.created++; created_this_run++; }
            else if (SUCCEEDED(lib->lpVtbl->StorePipeline(lib, name, pso))) {
                c.created++; created_this_run++; dirty++; stored_entries++;
            } else {
                c.store_conflict++;
            }
        } else {
            c.failed++;
        }
        if (pso) pso->lpVtbl->Release(pso);
        free(vs); free(ps);
        if (lib && dirty && (dirty >= o.checkpoint_every || now_ms() - last_checkpoint > 2000.0)) {
            if (!write_library(&o, lib, &id, stored_entries, inv_hash, 0)) write_failed = 1;
            dirty = 0; last_checkpoint = now_ms();
            emit(&o, "checkpoint", &c, now_ms() - begin, NULL);
        }
        if (now_ms() - last_progress > 100.0 || i + 1 == inv.count) {
            emit(&o, "progress", &c, now_ms() - begin, NULL);
            last_progress = now_ms();
        }
    }
    /* Complete means every record whose shaders are available was prepared. */
    const int complete = !cancelled && !c.failed && !c.store_conflict && !write_failed;
    if (lib && (dirty || complete != (int)existing.complete || state != RSP_LIB_LOADED ||
                existing.inventory_hash_lo != (uint32_t)inv_hash || existing.inventory_hash_hi != (uint32_t)(inv_hash >> 32))) {
        if (!write_library(&o, lib, &id, stored_entries, inv_hash, complete)) write_failed = 1;
    }
    const char* status = write_failed ? "error" : cancelled ? "cancelled" : !lib ? "unsupported" : complete ? "complete" : "partial";
    write_status(&o, status, rsp_library_status_name(state), &id, &c, inv_hash, inventory_records, observed_records, now_ms() - begin);
    char extra[96];
    snprintf(extra, sizeof extra, "\"status\":\"%s\",\"library_entries\":%u", status, stored_entries);
    emit(&o, "done", &c, now_ms() - begin, extra);

    for (uint32_t i = 0; i < inv.root_count; i++) if (roots[i]) roots[i]->lpVtbl->Release(roots[i]);
    if (lib) lib->lpVtbl->Release(lib);
    free(lib_blob);
    dev->lpVtbl->Release(dev);
    rsp_inventory_free(&inv);
    if (write_failed) return 1;
    if (cancelled) return 2;
    if (!lib) return 3;
    return complete ? 0 : 1;
}
