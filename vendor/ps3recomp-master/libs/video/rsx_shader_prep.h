/*
 * Shader seed / PSO preparation support shared by the live renderer and the
 * standalone preparation tool (native/macross30/shader_prep_main.c).
 *
 * Three layers, each optional and each falling back to the ordinary path:
 *  1. Portable DXBC seed shipped next to the executable
 *     (assets\rsx_shader_cache_v2). The renderer's loader still compares the
 *     complete generated HLSL; this file adds the module path, a DXBC
 *     container checksum check and per-stage counters.
 *  2. Fixed built-in shaders (post-process, blits, original bloom kernels) use
 *     the same cache format under stage 'B', keyed by an identity text that
 *     holds name/entry/target/flags plus the complete source.
 *  3. Graphics PSO descriptors of the main draw path are recorded in a
 *     device-independent form ("PSOD" records). A local D3D12 pipeline library
 *     built on the user's PC from those records is opened only when the
 *     adapter/driver identity matches; driver blobs are never distributed.
 *
 * Nothing here changes generated HLSL, render state or pixel arithmetic.
 */
#ifndef RSX_SHADER_PREP_H
#define RSX_SHADER_PREP_H
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <io.h>

#define RSP_SEED_DIR_FORMAT     L"assets\\rsx_shader_cache_v%u"
#define RSP_INVENTORY_NAME      L"assets\\rsx_pso_inventory_v1.bin"
#define RSP_OBSERVED_NAME       "pso-observed-v1.bin"
#define RSP_LIBRARY_NAME        "pso-library-v1.bin"
#define RSP_MISS_LOG_NAME       "coverage-misses-v1.tsv"
#define RSP_FILE_MAGIC          0x49505352u /* "RSPI" */
#define RSP_FILE_VERSION        1u
#define RSP_CHUNK_ROOT          0x544F4F52u /* "ROOT" */
#define RSP_CHUNK_PSOD          0x444F5350u /* "PSOD" */
#define RSP_LIBRARY_MAGIC       0x4C505352u /* "RSPL" */
#define RSP_LIBRARY_VERSION     1u
#define RSP_RECORD_VERSION      1u
#define RSP_MAX_INPUTS          16u
#define RSP_MAX_RECORDS         16384u
#define RSP_MAX_ROOTS           8u
#define RSP_MAX_ROOT_BYTES      4096u
#define RSP_BUILTIN_TAG         "RSPB1\n"

/* ------------------------------------------------------------------------
 * Hashing / DXBC container validation
 * ----------------------------------------------------------------------*/
static uint64_t rsp_fnv1a(const void* data, size_t n, uint64_t h)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
#define RSP_FNV_SEED 1469598103934665603ull

static uint32_t rsp_rotl(uint32_t x, uint32_t c) { return (x << c) | (x >> (32u - c)); }

static void rsp_md5_block(uint32_t state[4], const uint8_t block[64])
{
    static const uint32_t s[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7*i) & 15; }
        f += a + k[i] + m[g];
        a = d; d = c; c = b; b += rsp_rotl(f, s[i]);
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void rsp_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* DXBC digest: MD5 rounds over the container after magic+digest, with the
 * container's own length/padding layout in the final block. */
static void rsp_dxbc_checksum(const uint8_t* data, size_t size, uint8_t out[16])
{
    const uint8_t* body = data + 20;
    const uint32_t length = (uint32_t)(size - 20);
    const uint32_t bits = length * 8u;
    const uint32_t full = length / 64u, rest = length % 64u;
    uint32_t state[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    uint8_t block[64];
    for (uint32_t i = 0; i < full; i++) rsp_md5_block(state, body + i * 64u);
    const uint8_t* tail = body + full * 64u;
    if (rest >= 56u) {
        memset(block, 0, 64); memcpy(block, tail, rest); block[rest] = 0x80;
        rsp_md5_block(state, block);
        memset(block, 0, 64); rsp_put32(block, bits); rsp_put32(block + 60, (bits >> 2) | 1u);
    } else {
        memset(block, 0, 64); rsp_put32(block, bits); memcpy(block + 4, tail, rest);
        block[4 + rest] = 0x80; rsp_put32(block + 60, (bits >> 2) | 1u);
    }
    rsp_md5_block(state, block);
    for (int i = 0; i < 4; i++) rsp_put32(out + i * 4, state[i]);
}

static int rsp_dxbc_valid(const void* blob, size_t size)
{
    const uint8_t* p = (const uint8_t*)blob;
    if (!p || size < 32u || memcmp(p, "DXBC", 4) != 0) return 0;
    const uint32_t declared = (uint32_t)p[24] | ((uint32_t)p[25] << 8) |
                              ((uint32_t)p[26] << 16) | ((uint32_t)p[27] << 24);
    if (declared != size) return 0;
    uint8_t digest[16];
    rsp_dxbc_checksum(p, size, digest);
    return memcmp(digest, p + 4, 16) == 0;
}

/* ------------------------------------------------------------------------
 * Paths
 * ----------------------------------------------------------------------*/
/* Module-relative wide path; returns 0 when it does not fit. */
static int rsp_module_path(wchar_t* out, size_t capacity, const wchar_t* relative)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)capacity);
    if (!n || n >= capacity) return 0;
    wchar_t* tail = wcsrchr(out, L'\\');
    if (!tail) return 0;
    ++tail;
    const size_t remaining = capacity - (size_t)(tail - out);
    const int written = _snwprintf(tail, remaining, L"%s", relative);
    return written >= 0 && (size_t)written < remaining;
}

/* Optional override for tests and the preparation tool's seed location. */
static int rsp_seed_dir(wchar_t* out, size_t capacity, unsigned version)
{
    const DWORD n = GetEnvironmentVariableW(L"RSX_SHADER_SEED_DIR", out, (DWORD)capacity);
    if (n && n < capacity) return 1;
    wchar_t relative[64];
    _snwprintf(relative, 64, RSP_SEED_DIR_FORMAT, version);
    return rsp_module_path(out, capacity, relative);
}

static FILE* rsp_seed_open(unsigned version, uint32_t stage, uint64_t hash, uint32_t source_length)
{
    if (getenv("RSX_NO_SHADER_SEED")) return NULL;
    wchar_t path[MAX_PATH];
    if (!rsp_seed_dir(path, MAX_PATH, version)) return NULL;
    const size_t used = wcslen(path);
    const int written = _snwprintf(path + used, MAX_PATH - used, L"\\%c_%016llX_%08X.bin",
                                   (wchar_t)stage, (unsigned long long)hash, source_length);
    if (written < 0 || (size_t)written >= MAX_PATH - used) return NULL;
    return _wfopen(path, L"rb");
}

/* ------------------------------------------------------------------------
 * Built-in shader identity: name/entry/target/flags plus the full source.
 * ----------------------------------------------------------------------*/
static char* rsp_builtin_identity(const void* source, size_t bytes, const char* name,
                                  const char* entry, const char* target,
                                  unsigned flags1, unsigned flags2, uint32_t* length)
{
    char head[512];
    const int n = snprintf(head, sizeof head, RSP_BUILTIN_TAG "name=%s\nentry=%s\ntarget=%s\nflags1=%08X\nflags2=%08X\n\n",
                           name ? name : "", entry ? entry : "", target ? target : "", flags1, flags2);
    if (n <= 0 || (size_t)n >= sizeof head || bytes > 1024u * 1024u) return NULL;
    char* out = (char*)malloc((size_t)n + bytes + 1u);
    if (!out) return NULL;
    memcpy(out, head, (size_t)n);
    memcpy(out + n, source, bytes);
    out[(size_t)n + bytes] = 0;
    *length = (uint32_t)((size_t)n + bytes);
    return out;
}

/* ------------------------------------------------------------------------
 * Device-independent graphics PSO descriptor record
 * ----------------------------------------------------------------------*/
typedef struct rsp_shader_ref {
    uint32_t stage, source_length, source_hash_lo, source_hash_hi, bytecode_length;
    uint32_t digest[4];
} rsp_shader_ref;

typedef struct rsp_pso_record {
    uint32_t version;
    uint32_t root_hash_lo, root_hash_hi;
    rsp_shader_ref vs, ps;
    uint32_t blend[2 + 8 * 10];
    uint32_t sample_mask;
    uint32_t raster[11];
    uint32_t depth[14];
    uint32_t input_count;
    uint32_t input[RSP_MAX_INPUTS][6];
    uint32_t strip_cut, topology, rt_count, rtv[8], dsv, sample_count, sample_quality, node_mask, flags;
} rsp_pso_record;

typedef struct rsp_key { uint64_t lo, hi; } rsp_key;

static rsp_key rsp_record_key(const rsp_pso_record* r)
{
    rsp_key k;
    k.lo = rsp_fnv1a(r, sizeof *r, RSP_FNV_SEED);
    k.hi = rsp_fnv1a(r, sizeof *r, 0x84222325CBF29CE4ull);
    return k;
}

static int rsp_key_cmp(const void* a, const void* b)
{
    const rsp_key* x = (const rsp_key*)a; const rsp_key* y = (const rsp_key*)b;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return 0;
}

static void rsp_key_name(rsp_key k, wchar_t out[40])
{
    _snwprintf(out, 40, L"PSO_%016llX%016llX", (unsigned long long)k.hi, (unsigned long long)k.lo);
}

static void rsp_shader_ref_fill(rsp_shader_ref* ref, uint32_t stage, uint64_t hash,
                                uint32_t source_length, const void* bytecode, size_t bytes)
{
    memset(ref, 0, sizeof *ref);
    ref->stage = stage; ref->source_length = source_length;
    ref->source_hash_lo = (uint32_t)hash; ref->source_hash_hi = (uint32_t)(hash >> 32);
    ref->bytecode_length = (uint32_t)bytes;
    if (bytecode && bytes >= 20u) memcpy(ref->digest, (const uint8_t*)bytecode + 4, 16);
}

static uint32_t rsp_float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float rsp_bits_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Returns 0 for descriptors this format does not represent (they bypass the
 * library and are never recorded). */
static int rsp_record_from_desc(rsp_pso_record* r, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                                uint64_t root_hash, const rsp_shader_ref* vs, const rsp_shader_ref* ps)
{
    memset(r, 0, sizeof *r);
    if (d->DS.BytecodeLength || d->HS.BytecodeLength || d->GS.BytecodeLength ||
        d->StreamOutput.NumEntries || d->CachedPSO.CachedBlobSizeInBytes ||
        d->InputLayout.NumElements > RSP_MAX_INPUTS || !d->VS.BytecodeLength || !d->PS.BytecodeLength)
        return 0;
    r->version = RSP_RECORD_VERSION;
    r->root_hash_lo = (uint32_t)root_hash; r->root_hash_hi = (uint32_t)(root_hash >> 32);
    r->vs = *vs; r->ps = *ps;
    const D3D12_BLEND_DESC* b = &d->BlendState;
    r->blend[0] = (uint32_t)b->AlphaToCoverageEnable; r->blend[1] = (uint32_t)b->IndependentBlendEnable;
    for (int i = 0; i < 8; i++) {
        const D3D12_RENDER_TARGET_BLEND_DESC* t = &b->RenderTarget[i];
        uint32_t* o = &r->blend[2 + i * 10];
        o[0] = (uint32_t)t->BlendEnable; o[1] = (uint32_t)t->LogicOpEnable;
        o[2] = (uint32_t)t->SrcBlend; o[3] = (uint32_t)t->DestBlend; o[4] = (uint32_t)t->BlendOp;
        o[5] = (uint32_t)t->SrcBlendAlpha; o[6] = (uint32_t)t->DestBlendAlpha; o[7] = (uint32_t)t->BlendOpAlpha;
        o[8] = (uint32_t)t->LogicOp; o[9] = (uint32_t)t->RenderTargetWriteMask;
    }
    r->sample_mask = d->SampleMask;
    const D3D12_RASTERIZER_DESC* s = &d->RasterizerState;
    r->raster[0] = (uint32_t)s->FillMode; r->raster[1] = (uint32_t)s->CullMode;
    r->raster[2] = (uint32_t)s->FrontCounterClockwise; r->raster[3] = (uint32_t)s->DepthBias;
    r->raster[4] = rsp_float_bits(s->DepthBiasClamp); r->raster[5] = rsp_float_bits(s->SlopeScaledDepthBias);
    r->raster[6] = (uint32_t)s->DepthClipEnable; r->raster[7] = (uint32_t)s->MultisampleEnable;
    r->raster[8] = (uint32_t)s->AntialiasedLineEnable; r->raster[9] = s->ForcedSampleCount;
    r->raster[10] = (uint32_t)s->ConservativeRaster;
    const D3D12_DEPTH_STENCIL_DESC* z = &d->DepthStencilState;
    r->depth[0] = (uint32_t)z->DepthEnable; r->depth[1] = (uint32_t)z->DepthWriteMask;
    r->depth[2] = (uint32_t)z->DepthFunc; r->depth[3] = (uint32_t)z->StencilEnable;
    r->depth[4] = z->StencilReadMask; r->depth[5] = z->StencilWriteMask;
    r->depth[6] = (uint32_t)z->FrontFace.StencilFailOp; r->depth[7] = (uint32_t)z->FrontFace.StencilDepthFailOp;
    r->depth[8] = (uint32_t)z->FrontFace.StencilPassOp; r->depth[9] = (uint32_t)z->FrontFace.StencilFunc;
    r->depth[10] = (uint32_t)z->BackFace.StencilFailOp; r->depth[11] = (uint32_t)z->BackFace.StencilDepthFailOp;
    r->depth[12] = (uint32_t)z->BackFace.StencilPassOp; r->depth[13] = (uint32_t)z->BackFace.StencilFunc;
    r->input_count = d->InputLayout.NumElements;
    for (uint32_t i = 0; i < r->input_count; i++) {
        const D3D12_INPUT_ELEMENT_DESC* e = &d->InputLayout.pInputElementDescs[i];
        if (!e->SemanticName || strcmp(e->SemanticName, "ATTR") != 0) return 0;
        r->input[i][0] = e->SemanticIndex; r->input[i][1] = (uint32_t)e->Format;
        r->input[i][2] = e->InputSlot; r->input[i][3] = e->AlignedByteOffset;
        r->input[i][4] = (uint32_t)e->InputSlotClass; r->input[i][5] = e->InstanceDataStepRate;
    }
    r->strip_cut = (uint32_t)d->IBStripCutValue; r->topology = (uint32_t)d->PrimitiveTopologyType;
    r->rt_count = d->NumRenderTargets;
    for (int i = 0; i < 8; i++) r->rtv[i] = (uint32_t)d->RTVFormats[i];
    r->dsv = (uint32_t)d->DSVFormat; r->sample_count = d->SampleDesc.Count;
    r->sample_quality = d->SampleDesc.Quality; r->node_mask = d->NodeMask; r->flags = (uint32_t)d->Flags;
    return 1;
}

/* Inverse of rsp_record_from_desc; elements must outlive the descriptor. */
static void rsp_desc_from_record(D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, D3D12_INPUT_ELEMENT_DESC* elements,
                                 const rsp_pso_record* r, ID3D12RootSignature* root,
                                 const void* vs, size_t vs_bytes, const void* ps, size_t ps_bytes)
{
    memset(d, 0, sizeof *d);
    d->pRootSignature = root;
    d->VS.pShaderBytecode = vs; d->VS.BytecodeLength = vs_bytes;
    d->PS.pShaderBytecode = ps; d->PS.BytecodeLength = ps_bytes;
    d->BlendState.AlphaToCoverageEnable = (BOOL)r->blend[0];
    d->BlendState.IndependentBlendEnable = (BOOL)r->blend[1];
    for (int i = 0; i < 8; i++) {
        D3D12_RENDER_TARGET_BLEND_DESC* t = &d->BlendState.RenderTarget[i];
        const uint32_t* o = &r->blend[2 + i * 10];
        t->BlendEnable = (BOOL)o[0]; t->LogicOpEnable = (BOOL)o[1];
        t->SrcBlend = (D3D12_BLEND)o[2]; t->DestBlend = (D3D12_BLEND)o[3]; t->BlendOp = (D3D12_BLEND_OP)o[4];
        t->SrcBlendAlpha = (D3D12_BLEND)o[5]; t->DestBlendAlpha = (D3D12_BLEND)o[6];
        t->BlendOpAlpha = (D3D12_BLEND_OP)o[7]; t->LogicOp = (D3D12_LOGIC_OP)o[8];
        t->RenderTargetWriteMask = (UINT8)o[9];
    }
    d->SampleMask = r->sample_mask;
    D3D12_RASTERIZER_DESC* s = &d->RasterizerState;
    s->FillMode = (D3D12_FILL_MODE)r->raster[0]; s->CullMode = (D3D12_CULL_MODE)r->raster[1];
    s->FrontCounterClockwise = (BOOL)r->raster[2]; s->DepthBias = (INT)r->raster[3];
    s->DepthBiasClamp = rsp_bits_float(r->raster[4]); s->SlopeScaledDepthBias = rsp_bits_float(r->raster[5]);
    s->DepthClipEnable = (BOOL)r->raster[6]; s->MultisampleEnable = (BOOL)r->raster[7];
    s->AntialiasedLineEnable = (BOOL)r->raster[8]; s->ForcedSampleCount = r->raster[9];
    s->ConservativeRaster = (D3D12_CONSERVATIVE_RASTERIZATION_MODE)r->raster[10];
    D3D12_DEPTH_STENCIL_DESC* z = &d->DepthStencilState;
    z->DepthEnable = (BOOL)r->depth[0]; z->DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)r->depth[1];
    z->DepthFunc = (D3D12_COMPARISON_FUNC)r->depth[2]; z->StencilEnable = (BOOL)r->depth[3];
    z->StencilReadMask = (UINT8)r->depth[4]; z->StencilWriteMask = (UINT8)r->depth[5];
    z->FrontFace.StencilFailOp = (D3D12_STENCIL_OP)r->depth[6]; z->FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)r->depth[7];
    z->FrontFace.StencilPassOp = (D3D12_STENCIL_OP)r->depth[8]; z->FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)r->depth[9];
    z->BackFace.StencilFailOp = (D3D12_STENCIL_OP)r->depth[10]; z->BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)r->depth[11];
    z->BackFace.StencilPassOp = (D3D12_STENCIL_OP)r->depth[12]; z->BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)r->depth[13];
    for (uint32_t i = 0; i < r->input_count && i < RSP_MAX_INPUTS; i++) {
        D3D12_INPUT_ELEMENT_DESC* e = &elements[i];
        e->SemanticName = "ATTR"; e->SemanticIndex = r->input[i][0];
        e->Format = (DXGI_FORMAT)r->input[i][1]; e->InputSlot = r->input[i][2];
        e->AlignedByteOffset = r->input[i][3];
        e->InputSlotClass = (D3D12_INPUT_CLASSIFICATION)r->input[i][4];
        e->InstanceDataStepRate = r->input[i][5];
    }
    d->InputLayout.pInputElementDescs = r->input_count ? elements : NULL;
    d->InputLayout.NumElements = r->input_count;
    d->IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)r->strip_cut;
    d->PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)r->topology;
    d->NumRenderTargets = r->rt_count;
    for (int i = 0; i < 8; i++) d->RTVFormats[i] = (DXGI_FORMAT)r->rtv[i];
    d->DSVFormat = (DXGI_FORMAT)r->dsv;
    d->SampleDesc.Count = r->sample_count; d->SampleDesc.Quality = r->sample_quality;
    d->NodeMask = r->node_mask; d->Flags = (D3D12_PIPELINE_STATE_FLAGS)r->flags;
}

/* ------------------------------------------------------------------------
 * Chunk files: inventory (distributed) and observed (local, append-only).
 * header{magic,version,0,0} then chunks {type, bytes, fnv32(payload), payload}.
 * A truncated or corrupt trailing chunk ends the read; earlier chunks stay valid.
 * ----------------------------------------------------------------------*/
typedef struct rsp_root_entry { uint64_t hash; uint32_t bytes; uint8_t data[RSP_MAX_ROOT_BYTES]; } rsp_root_entry;

typedef struct rsp_inventory {
    rsp_root_entry roots[RSP_MAX_ROOTS];
    uint32_t root_count;
    rsp_pso_record* records;
    rsp_key* keys;           /* sorted, unique */
    uint32_t count, capacity;
    uint32_t corrupt_chunks, duplicate_records, unknown_chunks, overflow;
    long last_good_end;      /* offset after the last valid chunk of the last file read */
} rsp_inventory;

static uint32_t rsp_chunk_check(const void* payload, uint32_t bytes)
{
    const uint64_t h = rsp_fnv1a(payload, bytes, RSP_FNV_SEED);
    return (uint32_t)(h ^ (h >> 32));
}

static int rsp_inventory_has(const rsp_inventory* inv, rsp_key key)
{
    if (!inv->count) return 0;
    return bsearch(&key, inv->keys, inv->count, sizeof key, rsp_key_cmp) != NULL;
}

static int rsp_inventory_add_root(rsp_inventory* inv, uint64_t hash, const void* data, uint32_t bytes)
{
    for (uint32_t i = 0; i < inv->root_count; i++)
        if (inv->roots[i].hash == hash) return 1;
    if (inv->root_count >= RSP_MAX_ROOTS || bytes > RSP_MAX_ROOT_BYTES ||
        rsp_fnv1a(data, bytes, RSP_FNV_SEED) != hash) return 0;
    rsp_root_entry* r = &inv->roots[inv->root_count++];
    r->hash = hash; r->bytes = bytes; memcpy(r->data, data, bytes);
    return 1;
}

static const rsp_root_entry* rsp_inventory_root(const rsp_inventory* inv, uint64_t hash)
{
    for (uint32_t i = 0; i < inv->root_count; i++)
        if (inv->roots[i].hash == hash) return &inv->roots[i];
    return NULL;
}

/* Insert keeping keys sorted; records stay parallel to keys. */
static int rsp_inventory_add(rsp_inventory* inv, const rsp_pso_record* rec)
{
    if (rec->version != RSP_RECORD_VERSION || rec->input_count > RSP_MAX_INPUTS) return 0;
    const rsp_key key = rsp_record_key(rec);
    uint32_t lo = 0, hi = inv->count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2u;
        const int c = rsp_key_cmp(&inv->keys[mid], &key);
        if (c == 0) { inv->duplicate_records++; return 1; }
        if (c < 0) lo = mid + 1u; else hi = mid;
    }
    if (inv->count >= RSP_MAX_RECORDS) { inv->overflow++; return 0; }
    if (inv->count == inv->capacity) {
        const uint32_t cap = inv->capacity ? inv->capacity * 2u : 256u;
        rsp_pso_record* r = (rsp_pso_record*)realloc(inv->records, (size_t)cap * sizeof *r);
        if (!r) return 0;
        inv->records = r;
        rsp_key* k = (rsp_key*)realloc(inv->keys, (size_t)cap * sizeof *k);
        if (!k) return 0;
        inv->keys = k; inv->capacity = cap;
    }
    memmove(&inv->keys[lo + 1u], &inv->keys[lo], (size_t)(inv->count - lo) * sizeof *inv->keys);
    memmove(&inv->records[lo + 1u], &inv->records[lo], (size_t)(inv->count - lo) * sizeof *inv->records);
    inv->keys[lo] = key; inv->records[lo] = *rec; inv->count++;
    return 1;
}

static void rsp_inventory_free(rsp_inventory* inv)
{
    free(inv->records); free(inv->keys);
    memset(inv, 0, sizeof *inv);
}

/* Returns number of PSOD chunks read, -1 if the file is absent or not ours. */
static int rsp_inventory_read_file(rsp_inventory* inv, FILE* f)
{
    if (!f) return -1;
    uint32_t header[4];
    inv->last_good_end = 0;
    if (fread(header, sizeof header, 1, f) != 1 || header[0] != RSP_FILE_MAGIC || header[1] != RSP_FILE_VERSION)
        return -1;
    inv->last_good_end = (long)sizeof header;
    int read = 0;
    uint8_t* buffer = (uint8_t*)malloc(RSP_MAX_ROOT_BYTES + 16u > sizeof(rsp_pso_record) ? RSP_MAX_ROOT_BYTES + 16u : sizeof(rsp_pso_record));
    if (!buffer) return -1;
    for (;;) {
        uint32_t chunk[3];
        if (fread(chunk, sizeof chunk, 1, f) != 1) break;
        if (chunk[1] > RSP_MAX_ROOT_BYTES + 8u && chunk[1] != sizeof(rsp_pso_record)) { inv->corrupt_chunks++; break; }
        if (fread(buffer, 1, chunk[1], f) != chunk[1]) { inv->corrupt_chunks++; break; }
        if (rsp_chunk_check(buffer, chunk[1]) != chunk[2]) { inv->corrupt_chunks++; break; }
        if (chunk[0] == RSP_CHUNK_PSOD && chunk[1] == sizeof(rsp_pso_record)) {
            rsp_pso_record rec; memcpy(&rec, buffer, sizeof rec);
            if (rsp_inventory_add(inv, &rec)) read++;
        } else if (chunk[0] == RSP_CHUNK_ROOT && chunk[1] >= 8u) {
            uint64_t hash; memcpy(&hash, buffer, 8);
            if (!rsp_inventory_add_root(inv, hash, buffer + 8, chunk[1] - 8u)) inv->corrupt_chunks++;
        } else {
            inv->unknown_chunks++;
        }
        inv->last_good_end = ftell(f);
    }
    free(buffer);
    return read;
}

static int rsp_write_chunk(FILE* f, uint32_t type, const void* a, uint32_t a_bytes, const void* b, uint32_t b_bytes)
{
    uint8_t* joined = (uint8_t*)malloc((size_t)a_bytes + b_bytes + 1u);
    if (!joined) return 0;
    memcpy(joined, a, a_bytes);
    if (b_bytes) memcpy(joined + a_bytes, b, b_bytes);
    const uint32_t bytes = a_bytes + b_bytes;
    const uint32_t chunk[3] = { type, bytes, rsp_chunk_check(joined, bytes) };
    const int ok = fwrite(chunk, sizeof chunk, 1, f) == 1 && fwrite(joined, 1, bytes, f) == bytes;
    free(joined);
    return ok;
}

static int rsp_write_file_header(FILE* f)
{
    const uint32_t header[4] = { RSP_FILE_MAGIC, RSP_FILE_VERSION, 0, 0 };
    return fwrite(header, sizeof header, 1, f) == 1;
}

/* ------------------------------------------------------------------------
 * Local pipeline library identity (adapter + driver). LUID is per boot and
 * deliberately not part of it.
 * ----------------------------------------------------------------------*/
typedef struct rsp_library_header {
    uint32_t magic, version, header_bytes, record_version;
    uint32_t vendor_id, device_id, subsys_id, revision;
    uint32_t umd_lo, umd_hi, description_hash_lo, description_hash_hi;
    uint32_t entries, blob_bytes, blob_hash_lo, blob_hash_hi;
    uint32_t inventory_hash_lo, inventory_hash_hi, complete, reserved;
} rsp_library_header;

typedef struct rsp_adapter_identity {
    uint32_t vendor_id, device_id, subsys_id, revision;
    uint64_t umd, description_hash;
    wchar_t description[128];
    int valid;
} rsp_adapter_identity;

static rsp_adapter_identity rsp_query_adapter(ID3D12Device* dev)
{
    rsp_adapter_identity id;
    memset(&id, 0, sizeof id);
    if (!dev) return id;
    LUID luid; memset(&luid, 0, sizeof luid);
    dev->lpVtbl->GetAdapterLuid(dev, &luid);
    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory)) || !factory) return id;
    IDXGIAdapter1* adapter = NULL;
    if (SUCCEEDED(factory->lpVtbl->EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1, (void**)&adapter)) && adapter) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
            id.vendor_id = desc.VendorId; id.device_id = desc.DeviceId;
            id.subsys_id = desc.SubSysId; id.revision = desc.Revision;
            memcpy(id.description, desc.Description, sizeof id.description);
            id.description[127] = 0;
            id.description_hash = rsp_fnv1a(id.description, wcslen(id.description) * sizeof(wchar_t), RSP_FNV_SEED);
            LARGE_INTEGER umd; umd.QuadPart = 0;
            if (SUCCEEDED(adapter->lpVtbl->CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd)))
                id.umd = (uint64_t)umd.QuadPart;
            id.valid = 1;
        }
        adapter->lpVtbl->Release(adapter);
    }
    factory->lpVtbl->Release(factory);
    return id;
}

static int rsp_library_supported(ID3D12Device* dev)
{
    D3D12_FEATURE_DATA_SHADER_CACHE cache;
    memset(&cache, 0, sizeof cache);
    if (FAILED(dev->lpVtbl->CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_CACHE, &cache, sizeof cache)))
        return 0;
    return (cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) != 0;
}

/* 1 = header describes this adapter/driver and record format. */
static int rsp_library_identity_matches(const rsp_library_header* h, const rsp_adapter_identity* id)
{
    return h->magic == RSP_LIBRARY_MAGIC && h->version == RSP_LIBRARY_VERSION &&
        h->header_bytes == sizeof *h && h->record_version == RSP_RECORD_VERSION && id->valid &&
        h->vendor_id == id->vendor_id && h->device_id == id->device_id &&
        h->subsys_id == id->subsys_id && h->revision == id->revision &&
        h->umd_lo == (uint32_t)id->umd && h->umd_hi == (uint32_t)(id->umd >> 32) &&
        h->description_hash_lo == (uint32_t)id->description_hash &&
        h->description_hash_hi == (uint32_t)(id->description_hash >> 32);
}

typedef enum {
    RSP_LIB_ABSENT = 0, RSP_LIB_LOADED, RSP_LIB_IDENTITY_MISMATCH, RSP_LIB_CORRUPT,
    RSP_LIB_DRIVER_REJECTED, RSP_LIB_UNSUPPORTED
} rsp_library_status;

static const char* rsp_library_status_name(rsp_library_status s)
{
    switch (s) {
    case RSP_LIB_LOADED: return "loaded";
    case RSP_LIB_IDENTITY_MISMATCH: return "identity-mismatch";
    case RSP_LIB_CORRUPT: return "corrupt";
    case RSP_LIB_DRIVER_REJECTED: return "driver-rejected";
    case RSP_LIB_UNSUPPORTED: return "unsupported";
    default: return "absent";
    }
}

/* Opens a serialized library. On success *blob must stay alive as long as
 * *library does (D3D12 references it). */
static rsp_library_status rsp_library_open(ID3D12Device* dev, const char* path,
                                           const rsp_adapter_identity* id,
                                           ID3D12PipelineLibrary** library, void** blob,
                                           rsp_library_header* header_out, HRESULT* hr_out)
{
    *library = NULL; *blob = NULL;
    if (hr_out) *hr_out = S_OK;
    FILE* f = fopen(path, "rb");
    if (!f) return RSP_LIB_ABSENT;
    rsp_library_header h;
    memset(&h, 0, sizeof h);
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != RSP_LIBRARY_MAGIC) { fclose(f); return RSP_LIB_CORRUPT; }
    if (header_out) *header_out = h;
    if (!rsp_library_identity_matches(&h, id)) { fclose(f); return RSP_LIB_IDENTITY_MISMATCH; }
    if (!h.blob_bytes || h.blob_bytes > 512u * 1024u * 1024u) { fclose(f); return RSP_LIB_CORRUPT; }
    void* data = malloc(h.blob_bytes);
    const int ok = data && fread(data, 1, h.blob_bytes, f) == h.blob_bytes;
    fclose(f);
    const uint64_t hash = ok ? rsp_fnv1a(data, h.blob_bytes, RSP_FNV_SEED) : 0;
    if (!ok || (uint32_t)hash != h.blob_hash_lo || (uint32_t)(hash >> 32) != h.blob_hash_hi) {
        free(data); return RSP_LIB_CORRUPT;
    }
    ID3D12Device1* dev1 = NULL;
    if (FAILED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12Device1, (void**)&dev1)) || !dev1) {
        free(data); return RSP_LIB_UNSUPPORTED;
    }
    HRESULT hr = dev1->lpVtbl->CreatePipelineLibrary(dev1, data, h.blob_bytes, &IID_ID3D12PipelineLibrary, (void**)library);
    dev1->lpVtbl->Release(dev1);
    if (hr_out) *hr_out = hr;
    if (FAILED(hr) || !*library) { *library = NULL; free(data); return RSP_LIB_DRIVER_REJECTED; }
    *blob = data;
    return RSP_LIB_LOADED;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif /* RSX_SHADER_PREP_H */
