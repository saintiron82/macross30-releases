/* General raw vertex ABI (LD_VERTEX_RAWN=1): the CPU snapshots each unique
 * vertex's guest attribute bytes unchanged (big-endian, packed per attribute
 * to 4-byte words) and the vertex shader decodes them -- RAW28 generalised to
 * every per-vertex RSX format. An attribute with no array (its RSX default
 * value) is stored as a host float4 and read by the shader as before.
 * Decoding matches rsx_vertex_decode_element bit for bit: the integer
 * normalisations use raw28_norm (correctly rounded x/(2^n-1), what the CPU's
 * float division gives), halves use raw28_half. Instanced attributes
 * (frequency > 1) are rejected; the caller falls back to float4 payloads. */
#ifndef PS3RECOMP_RSX_VERTEX_RAWN_H
#define PS3RECOMP_RSX_VERTEX_RAWN_H
#include "rsx_vertex_raw28.h"
#include <stdio.h>

#define RSX_RAWN_CONST 0xFFu
typedef struct rsx_rawn_attr {
    u8 attr, type, size, words;   /* type RSX_VTX_TYPE_* or RSX_RAWN_CONST */
    u32 offset;                   /* byte offset in the raw vertex */
    const u8* base;               /* stable source range (rsx_vertex_fetch_plan_prepare) */
    u32 first, stride;
    float konst[4];
} rsx_rawn_attr;
typedef struct rsx_rawn_layout {
    u32 count, stride;
    rsx_rawn_attr a[RSX_DSP_NUM_VERTEX_ATTR];
    u64 sig;                      /* shader-visible layout: attrs, types, sizes */
} rsx_rawn_layout;

static u32 rsx_rawn_bytes(u32 type, u32 size)
{
    switch (type) {
    case RSX_VTX_TYPE_FLOAT: return 4u * size;
    case RSX_VTX_TYPE_HALF: case RSX_VTX_TYPE_SNORM16: case RSX_VTX_TYPE_SINT16: return 2u * size;
    case RSX_VTX_TYPE_UNORM8: case RSX_VTX_TYPE_UINT8: return size;
    case RSX_VTX_TYPE_CMP32: return 4u;
    default: return 0;
    }
}

/* Build the layout for these refs (the plan prepared with them), or 0 when
 * any attribute cannot be raw. */
static int rsx_rawn_plan(const rsx_vertex_fetch_plan* p, const rsx_vertex_ref* refs, u32 count,
                         rsx_rawn_layout* out)
{
    if (!p || !refs || !count || !out || p->inline_data) return 0;
    memset(out, 0, sizeof *out);
    u64 sig = 1469598103934665603ull;
    u32 off = 0;
    for (u32 slot = 0; slot < p->layout.count; slot++) {
        const u32 attr = p->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f = &p->attr[attr];
        rsx_rawn_attr* a = &out->a[slot];
        a->attr = (u8)attr; a->offset = off;
        if (!f->desc.type || !f->desc.size) {
            a->type = RSX_RAWN_CONST; a->size = 4; a->words = 4;
            memcpy(a->konst, f->default_value, 16);
        } else {
            const u32 n = f->desc.size < 4u ? f->desc.size : 4u;
            const u32 bytes = rsx_rawn_bytes(f->desc.type, f->desc.type == RSX_VTX_TYPE_CMP32 ? 1u : n);
            if (!bytes || f->desc.frequency > 1 || !f->stable_base) return 0;
            a->type = (u8)f->desc.type; a->size = (u8)n; a->words = (u8)((bytes + 3u) / 4u);
            a->base = f->stable_base; a->first = f->stable_first; a->stride = f->stride;
            /* rsx_vertex_fetch_plan_prepare made [stable_first, stable_last]
             * the element range of these same refs (frequency <= 1), so every
             * ref is inside; no per-ref walk (it was a pass over the draw per
             * attribute). */
        }
        off += 4u * a->words;
        const u32 key[4] = { attr, a->type, a->size, a->words };
        for (u32 k = 0; k < 4; k++) { sig ^= key[k]; sig *= 1099511628211ull; }
    }
    out->count = p->layout.count;
    out->stride = off;
    out->sig = sig;
    return out->count != 0;
}

static void rsx_rawn_copy(const rsx_rawn_layout* L, const rsx_vertex_ref* refs, u32 count, u8* out)
{
    for (u32 slot = 0; slot < L->count; slot++) {
        const rsx_rawn_attr* a = &L->a[slot];
        u8* dst = out + a->offset;
        if (a->type == RSX_RAWN_CONST) {
            for (u32 i = 0; i < count; i++) memcpy(dst + (u64)i * L->stride, a->konst, 16);
            continue;
        }
        const u32 bytes = rsx_rawn_bytes(a->type, a->type == RSX_VTX_TYPE_CMP32 ? 1u : a->size);
        const u32 padded = 4u * a->words;
        /* Constant-size copies inline (a memcpy call per attribute per vertex
         * was ~5% of the renderer thread); same bytes as the general loop. */
        #define RAWN_COPY_FIXED(N) \
            for (u32 i = 0; i < count; i++) \
                memcpy(dst + (u64)i * L->stride, a->base + (u64)(rsx_raw28_element(refs[i]) - a->first) * a->stride, N)
        if (bytes == padded && bytes == 4u) { RAWN_COPY_FIXED(4); continue; }
        if (bytes == padded && bytes == 8u) { RAWN_COPY_FIXED(8); continue; }
        if (bytes == padded && bytes == 12u) { RAWN_COPY_FIXED(12); continue; }
        if (bytes == padded && bytes == 16u) { RAWN_COPY_FIXED(16); continue; }
        #undef RAWN_COPY_FIXED
        for (u32 i = 0; i < count; i++) {
            const u32 e = rsx_raw28_element(refs[i]);
            u8* d = dst + (u64)i * L->stride;
            memcpy(d, a->base + (u64)(e - a->first) * a->stride, bytes);
            if (padded > bytes) memset(d + bytes, 0, padded - bytes);
        }
    }
}

/* CPU decode of one attribute of one raw vertex (diagnostics, float restore). */
static int rsx_rawn_decode_attr(const rsx_rawn_layout* L, const u8* vertex, u32 attr, float out[4])
{
    for (u32 slot = 0; slot < L->count; slot++) {
        const rsx_rawn_attr* a = &L->a[slot];
        if (a->attr != attr) continue;
        if (a->type == RSX_RAWN_CONST) { memcpy(out, vertex + a->offset, 16); return 1; }
        return rsx_vertex_decode_element(a->type, a->type == RSX_VTX_TYPE_CMP32 ? 1u : a->size,
                                         vertex + a->offset, out);
    }
    return 0;
}

/* HLSL expression decoding input aN (a uintK of raw little-endian-loaded words). */
static int rsx_rawn_expr(const rsx_rawn_attr* a, char* s, size_t cap)
{
    static const char* sw = "xyzw";
    const u32 N = a->attr, n = a->size;
    char c[4][96];
    for (u32 k = 0; k < 4; k++) snprintf(c[k], sizeof c[k], "%s", k == 3 ? "1.0" : "0.0");
    switch (a->type) {
    case RSX_VTX_TYPE_FLOAT:
        for (u32 k = 0; k < n; k++) snprintf(c[k], sizeof c[k], "asfloat(raw28_be(input.a%u.%c))", N, sw[k]);
        break;
    case RSX_VTX_TYPE_HALF:
        for (u32 k = 0; k < n; k++)
            snprintf(c[k], sizeof c[k], "raw28_half((raw28_be(input.a%u.%c)%s)&65535u)", N, sw[k / 2], (k & 1) ? "" : ">>16");
        break;
    case RSX_VTX_TYPE_SNORM16: case RSX_VTX_TYPE_SINT16:
        for (u32 k = 0; k < n; k++) {
            char v[64];
            snprintf(v, sizeof v, "((int)((raw28_be(input.a%u.%c)%s)<<16)>>16)", N, sw[k / 2], (k & 1) ? "" : ">>16");
            if (a->type == RSX_VTX_TYPE_SNORM16) snprintf(c[k], sizeof c[k], "raw28_norm(%s,15u)", v);
            else snprintf(c[k], sizeof c[k], "(float)%s", v);
        }
        break;
    case RSX_VTX_TYPE_UNORM8: case RSX_VTX_TYPE_UINT8:
        for (u32 k = 0; k < n; k++) {
            char v[48];
            snprintf(v, sizeof v, "((input.a%u.%c>>%uu)&255u)", N, sw[k / 4], 8u * (k & 3u));
            if (a->type == RSX_VTX_TYPE_UNORM8) snprintf(c[k], sizeof c[k], "raw28_norm((int)%s,8u)", v);
            else snprintf(c[k], sizeof c[k], "(float)%s", v);
        }
        break;
    case RSX_VTX_TYPE_CMP32:
        return snprintf(s, cap, "raw28_cmp(input.a%u.x)", N) < (int)cap;
    default:
        return 0;
    }
    return snprintf(s, cap, "float4(%s,%s,%s,%s)", c[0], c[1], c[2], c[3]) < (int)cap;
}

/* Rewrite the decompiled vertex shader's inputs for this layout. */
static int rsx_rawn_shader(char* text, u32 cap, const rsx_rawn_layout* L)
{
    const size_t helpers = sizeof(rsx_raw28_hlsl_helpers) - 1;
    char* copy = (char*)malloc(cap);
    if (!copy) return 0;
    memcpy(copy, text, strlen(text) + 1);
    int ok = 1;
    for (u32 slot = 0; slot < L->count && ok; slot++) {
        const rsx_rawn_attr* a = &L->a[slot];
        if (a->type == RSX_RAWN_CONST) continue;   /* stays float4 */
        char from[64], to[64], use_from[64], use_to[512], expr[480];
        static const char* const ty[5] = { "", "uint", "uint2", "uint3", "uint4" };
        snprintf(from, sizeof from, "    float4 a%u:ATTR%u;\n", a->attr, a->attr);
        snprintf(to, sizeof to, "    %s a%u:ATTR%u;\n", ty[a->words], a->attr, a->attr);
        snprintf(use_from, sizeof use_from, "    v[%u]=input.a%u;\n", a->attr, a->attr);
        if (!rsx_rawn_expr(a, expr, sizeof expr)) { ok = 0; break; }
        snprintf(use_to, sizeof use_to, "    v[%u]=%s;\n", a->attr, expr);
        ok = rsx_raw28_replace(copy, cap, from, to) && rsx_raw28_replace(copy, cap, use_from, use_to);
    }
    if (ok) {
        const size_t n = strlen(copy);
        if (n + helpers + 1 > cap) ok = 0;
        else { memcpy(text, rsx_raw28_hlsl_helpers, helpers); memcpy(text + helpers, copy, n + 1); }
    }
    free(copy);
    return ok;
}
#endif
