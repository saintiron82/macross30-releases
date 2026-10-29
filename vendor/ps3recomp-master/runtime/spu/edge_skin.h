/* edge_skin.h -- the Edge Geometry job's vertex-output path (skinning schemes 1/2/3,
 * no blend shapes; schemes 2/3 validated for all seven Edge images) as host code, for spurs_job.c's native Edge path and
 * tools/tests/test_edge_skin.py. Header-only so both use one copy.
 *
 * Read from geom2 (image 10005; native/analysis/edge-skin-spec.md):
 *   - weights: per vertex 8 bytes w0 i0 w1 i1 w2 i2 w3 i3; weight = cuflt(u8) *
 *     0x3B808080 (one ulp under 1/255, no normalisation); the index addresses
 *     the concatenated 48-byte palette directly (13230, 0x15A90/0x15AE0);
 *   - blend: per matrix element fm with w0, then fma with w1, w2, w3. The SPU
 *     keeps the matrices diagonally (A784: L0 = m00 m11 m22 t0, L1 = m01 m12
 *     m20 t1, L2 = m02 m10 m21 t2) -- element-wise, so the blend is the same;
 *   - transform: lane i of fma(P, L0|1, T), fma(P.yzx, L1, .), fma(P.zxy, L2, .)
 *     -- the order es_xform_pos spells out; directions start with fm(P, L0);
 *   - X11Y11Z10N: decompress fma(X/1024, 0x3F801002, 0x3A001002) (Z: Z/512,
 *     0x3F802008, 0x3A802008; 6B90, 0x14CB0/0x14CC0); compress
 *     cflts(fma(v, 0x3F7FE000, 0xBA000000) * 2^31) and keep the top 11/11/10
 *     bits (22D0, 0x14B50/0x14B60, rotm 5/2/0 + byte shuffles).
 * Every float operation is the SPU's (truncating, no denormals, saturating:
 * spu_helpers.h) in the SPU's order, so the result is meant to be bit-equal
 * to what the job writes to its ring; spurs_job.c checks that per key. */
#ifndef EDGE_SKIN_H
#define EDGE_SKIN_H
#include <stdint.h>
#include <string.h>
#include "spu_helpers.h"

/* One bound vertex attribute: RSX index, byte offset in the vertex, and the
 * NV4097 vertex format word (stride << 8 | size << 4 | type). */
typedef struct { uint8_t attr, off; uint16_t fmt; } es_attr;

#define ES_WEIGHT_SCALE 0x3B808080u   /* ~1/255 */
#define ES_CMP_DX_MUL   0x3F801002u   /* 2048/2047 */
#define ES_CMP_DX_ADD   0x3A001002u   /* 1/2047    */
#define ES_CMP_DZ_MUL   0x3F802008u   /* 1024/1023 */
#define ES_CMP_DZ_ADD   0x3A802008u   /* 1/1023    */
#define ES_CMP_CX_MUL   0x3F7FE000u   /* 2047/2048 */
#define ES_CMP_CX_ADD   0xBA000000u   /* -1/2048   */
#define ES_CMP_CZ_MUL   0x3F7FC000u   /* 1023/1024 */
#define ES_CMP_CZ_ADD   0xBA800000u   /* -1/1024   */
#define ES_RET          0x00020000u   /* FIFO RETURN */

static inline uint32_t es_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void es_wbe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* cfg's vertex count includes SIMD padding. Only the prefix ending at the
 * largest referenced u16 index needs vertices/weights or output storage.
 * Never hide an invalid index by shortening the vertex count. Zero = reject. */
static inline uint32_t es_index_vertex_span(const uint8_t* idx, uint32_t n_idx, uint32_t cfg_vertices)
{
    uint32_t span = 0;
    if (!idx || !n_idx || !cfg_vertices) return 0;
    for (uint32_t i = 0; i < n_idx; i++) {
        const uint32_t v = (uint32_t)idx[2u * i] << 8 | idx[2u * i + 1u];
        if (v >= cfg_vertices) return 0;
        if (v + 1u > span) span = v + 1u;
    }
    return span;
}
typedef struct { const uint8_t* data; uint32_t size; } es_dma_part;
/* A packed stream may span up to three DMA elements. The SPURS loader aligns
 * each nonempty element to 16 bytes in LS. Reject a prefix that crosses an
 * alignment gap rather than inventing bytes for that gap. Empty slots add no
 * storage. A final partial element is fine when the requested prefix fits. */
static inline int es_dma_prefix_fits(const es_dma_part* parts, uint32_t n, uint32_t need)
{
    if (!parts || !n || n > 3 || !need || !parts[0].data || !parts[0].size) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t size = parts[i].size;
        if (!size) continue;
        if (!parts[i].data) return 0;
        if (size >= need) return 1;
        if (size & 15u) return 0;
        need -= size;
    }
    return 0;
}
static inline int es_copy_dma_prefix(const es_dma_part* parts, uint32_t n, uint32_t need,
                                     uint8_t* out, size_t cap)
{
    if (!out || cap < need || !es_dma_prefix_fits(parts, n, need)) return 0;
    for (uint32_t i = 0; i < n && need; i++) {
        uint32_t size = parts[i].size < need ? parts[i].size : need;
        if (size) { memcpy(out, parts[i].data, size); out += size; need -= size; }
    }
    return 1;
}
static inline uint32_t es_fm(uint32_t a, uint32_t b) { return spu__fm_any(a, b); }
static inline uint32_t es_fma(uint32_t a, uint32_t b, uint32_t c) { return spu__fma_any(a, b, c, 0, 0); }
/* i / 2^sh as an SPU single (exact for the small integers used here). */
static inline uint32_t es_sfix(int32_t i, int sh)
{
    if (!i) return 0;
    return spu__sf_pack(i < 0 ? 0x80000000u : 0u, (uint32_t)(i < 0 ? -i : i), -sh);
}
static inline uint32_t es_weight(uint8_t b) { return b ? es_fm(es_sfix(b, 0), ES_WEIGHT_SCALE) : 0u; }

/* B = sum w_k M[i_k], 12 floats (rows m_r0 m_r1 m_r2 t_r), host-order bits. */
static inline void es_blend(const uint32_t* pal, const uint8_t wi[8], uint32_t B[12])
{
    const uint32_t w0 = es_weight(wi[0]), w1 = es_weight(wi[2]), w2 = es_weight(wi[4]), w3 = es_weight(wi[6]);
    const uint32_t *m0 = pal + 12u * wi[1], *m1 = pal + 12u * wi[3], *m2 = pal + 12u * wi[5], *m3 = pal + 12u * wi[7];
    for (int e = 0; e < 12; e++)
        B[e] = es_fma(w3, m3[e], es_fma(w2, m2[e], es_fma(w1, m1[e], es_fm(w0, m0[e]))));
}
static inline void es_xform_pos(const uint32_t B[12], const uint32_t p[3], uint32_t o[3])
{
    o[0] = es_fma(p[2], B[2], es_fma(p[1], B[1], es_fma(p[0], B[0], B[3])));
    o[1] = es_fma(p[0], B[4], es_fma(p[2], B[6], es_fma(p[1], B[5], B[7])));
    o[2] = es_fma(p[1], B[9], es_fma(p[0], B[8], es_fma(p[2], B[10], B[11])));
}
/* Normal / tangent / binormal: the 3x3 part (scheme 2 normalises afterwards). */
static inline void es_xform_dir(const uint32_t B[12], const uint32_t n[3], uint32_t o[3])
{
    o[0] = es_fma(n[2], B[2], es_fma(n[1], B[1], es_fm(n[0], B[0])));
    o[1] = es_fma(n[0], B[4], es_fma(n[2], B[6], es_fm(n[1], B[5])));
    o[2] = es_fma(n[1], B[9], es_fma(n[0], B[8], es_fm(n[2], B[10])));
}
/* geom2 scheme 2 (12188): x*x, then fused z*z, then fused y*y. The
 * reciprocal square root is fi(frsqest), without a Newton refinement.
 * Changing the summation order changes the packed direction bytes. */
static inline void es_normalize_dir(uint32_t v[3])
{
    const uint32_t length2 = es_fma(v[1], v[1], es_fma(v[2], v[2], es_fm(v[0], v[0])));
    const u128 d = spu_splat_u32(length2);
    const uint32_t scale = spu_fi(d, spu_frsqest(d))._u32[0];
    for (int c = 0; c < 3; c++) v[c] = es_fm(v[c], scale);
}
static inline uint32_t es_fms(uint32_t a, uint32_t b, uint32_t c)
{
    return spu__fma_any(a, b, c, 0, 0x80000000u);
}
/* Scheme 3 handles nonuniform scale with the cofactor of the blended 3x3.
 * Normalization removes its magnitude; preserve the original determinant
 * sign instead of introducing a host inverse or dividing by a determinant. */
static inline void es_cofactor(const uint32_t B[12], uint32_t C[12])
{
    C[0] = es_fms(B[5], B[10], es_fm(B[6], B[9]));
    C[1] = es_fms(B[6], B[8], es_fm(B[4], B[10]));
    C[2] = es_fms(B[4], B[9], es_fm(B[5], B[8]));
    C[4] = es_fms(B[2], B[9], es_fm(B[1], B[10]));
    C[5] = es_fms(B[0], B[10], es_fm(B[2], B[8]));
    C[6] = es_fms(B[1], B[8], es_fm(B[0], B[9]));
    C[8] = es_fms(B[1], B[6], es_fm(B[2], B[5]));
    C[9] = es_fms(B[2], B[4], es_fm(B[0], B[6]));
    C[10] = es_fms(B[0], B[5], es_fm(B[1], B[4]));
    C[3] = C[7] = C[11] = 0;
}
/* RSX CMP (X11Y11Z10N): X bits 0-10, Y 11-21, Z 22-31, two's complement. */
static inline void es_cmp_decode(uint32_t v, uint32_t f[3])
{
    const int32_t X = (int32_t)(v << 21) >> 21, Y = (int32_t)(v << 10) >> 21, Z = (int32_t)v >> 22;
    f[0] = es_fma(es_sfix(X, 10), ES_CMP_DX_MUL, ES_CMP_DX_ADD);
    f[1] = es_fma(es_sfix(Y, 10), ES_CMP_DX_MUL, ES_CMP_DX_ADD);
    f[2] = es_fma(es_sfix(Z, 9), ES_CMP_DZ_MUL, ES_CMP_DZ_ADD);
}
static inline uint32_t es_cmp_encode(const uint32_t f[3])
{
    const uint32_t x = spu__cfl_lane(es_fma(f[0], ES_CMP_CX_MUL, ES_CMP_CX_ADD), 31, 0);
    const uint32_t y = spu__cfl_lane(es_fma(f[1], ES_CMP_CX_MUL, ES_CMP_CX_ADD), 31, 0);
    const uint32_t z = spu__cfl_lane(es_fma(f[2], ES_CMP_CZ_MUL, ES_CMP_CZ_ADD), 31, 0);
    return (z & 0xFFC00000u) | (y >> 21) << 11 | x >> 21;
}

/* What the output path does with an attribute: position (attr 0, F32x3 at 0),
 * a skinned direction (CMP on 2/14/15), a byte copy (half, UB: decompress +
 * compress leaves them as they are), or something not seen yet. */
enum { ES_POS, ES_DIR, ES_COPY, ES_BAD };
static inline int es_kind(const es_attr* a)
{
    const uint32_t t = a->fmt & 0xFFu;
    if (a->attr == 0) return (t == 0x32u && a->off == 0) ? ES_POS : ES_BAD;
    if (t == 0x16u) return (a->attr == 2 || a->attr == 14 || a->attr == 15) ? ES_DIR : ES_BAD;
    if ((t & 0xFu) == 3u || (t & 0xFu) == 4u) return ES_COPY;
    return ES_BAD;
}
/* Byte size of one attribute (RSX types 1 S1, 2 F32, 3 F16, 4 UB, 5 S32K,
 * 6 CMP, 7 UB256). */
static inline uint32_t es_attr_bytes(uint16_t fmt)
{
    static const uint8_t tb[8] = { 0, 2, 4, 2, 1, 2, 4, 1 };
    const uint32_t t = fmt & 7u, n = (fmt >> 4) & 0xFu;
    return t == 6u ? 4u * n : tb[t] * n;
}
/* A layout the output path can take: attr 0 first, every attribute inside the
 * vertex and of a known kind. pos_only: only the position matters. */
static inline int es_layout_ok(const es_attr* a, uint32_t n_attr, uint32_t stride, int pos_only)
{
    if (!n_attr || !stride || es_kind(&a[0]) != ES_POS || stride < 12u) return 0;
    for (uint32_t k = pos_only ? n_attr : 1u; k < n_attr; k++)
        if (es_kind(&a[k]) == ES_BAD || a[k].off + es_attr_bytes(a[k].fmt) > stride) return 0;
    return 1;
}
/* Output vertices: each a copy of its input, then (skin) the position and
 * every direction attribute through the blended palette. Unskinned, a CMP
 * attribute still goes through decompress + compress, which is not an
 * identity (both constants are truncated: X = 1 comes back as 0); F32, half
 * and UB come back as they were. pos_only: the position alone. Returns 0, or
 * -1 for a weighted bone index outside the palette (an index with weight 0
 * contributes +0 on the SPU whatever it reads, so it is taken as matrix 0). */
static inline int es_skin_mesh_scheme(const uint8_t* vtx, uint8_t* out, uint32_t n_vtx, uint32_t stride,
                               const es_attr* a, uint32_t n_attr, int pos_only,
                               const uint8_t* wts, const uint32_t* pal, uint32_t n_mat, uint32_t scheme)
{
    if (scheme > 3u) return -1;
    memcpy(out, vtx, (size_t)n_vtx * stride);
    const int skin = wts && pal && n_mat;
    if (!skin && pos_only) return 0;
    const uint32_t na = pos_only ? 1u : n_attr;
    /* Job-local only: palettes and schemes can change on the very next job.
     * Reusing exact blended bits preserves SPU operation order, including
     * zero-weight lanes. A full key check makes hash collisions harmless. */
    struct { uint64_t key; uint32_t B[12], C[12]; } cache[16];
    uint32_t valid = 0;
    for (uint32_t v = 0; v < n_vtx; v++) {
        const uint32_t *B = NULL, *C = NULL;
        uint32_t in[3], o[3];
        if (skin) {
            uint8_t wi[8];
            memcpy(wi, wts + 8u * v, 8);
            for (int k = 1; k < 8; k += 2)
                if (wi[k] >= n_mat) { if (wi[k - 1]) return -1; wi[k] = 0; }
            uint64_t key;
            memcpy(&key, wi, sizeof key);
            uint64_t hash = (key ^ (key >> 33)) * UINT64_C(0xff51afd7ed558ccd);
            const unsigned slot = (unsigned)(hash ^ (hash >> 33)) & 15u;
            if (!(valid & (1u << slot)) || cache[slot].key != key) {
                es_blend(pal, wi, cache[slot].B);
                if (scheme == 3u && !pos_only) es_cofactor(cache[slot].B, cache[slot].C);
                cache[slot].key = key;
                valid |= 1u << slot;
            }
            B = cache[slot].B; C = cache[slot].C;
        }
        uint8_t* d = out + (size_t)stride * v;
        for (uint32_t k = 0; k < na; k++) {
            const int kind = es_kind(&a[k]);
            uint8_t* p = d + a[k].off;
            if (kind == ES_POS && skin) {
                for (int c = 0; c < 3; c++) in[c] = es_be32(p + 4 * c);
                es_xform_pos(B, in, o);
                for (int c = 0; c < 3; c++) es_wbe32(p + 4 * c, o[c]);
            } else if (kind == ES_DIR) {
                es_cmp_decode(es_be32(p), in);
                if (skin) es_xform_dir(scheme == 3u ? C : B, in, o); else memcpy(o, in, sizeof o);
                if (skin && scheme >= 2u) es_normalize_dir(o);
                es_wbe32(p, es_cmp_encode(o));
            }
        }
    }
    return 0;
}
static inline int es_skin_mesh(const uint8_t* vtx, uint8_t* out, uint32_t n_vtx, uint32_t stride,
                               const es_attr* a, uint32_t n_attr, int pos_only,
                               const uint8_t* wts, const uint32_t* pal, uint32_t n_mat)
{
    return es_skin_mesh_scheme(vtx, out, n_vtx, stride, a, n_attr, pos_only, wts, pal, n_mat, 1u);
}

/* The draw the hole (or a CALLed list) carries: invalidate (1710, 1714 x3),
 * format/offset per attribute at vtx_io, 1714, index array (u16, main memory),
 * the host-only culling marker (NOP 0x0100 = 0xED6E0000 | Edge cull mode, as
 * the culling-only native path), BEGIN triangles, DRAW_INDEX_ARRAY in runs of
 * 256, END. Host-order words; returns the count, 0 when cap is too small. */
typedef struct {
    const es_attr* a; uint32_t n_attr, stride; int pos_only;
    uint32_t vtx_io, idx_io, n_idx, cull;
} es_draw;
static inline uint32_t es_draw_words(const es_draw* d, uint32_t* w, uint32_t cap)
{
    const uint32_t n_attr = d->pos_only ? 1u : d->n_attr, draws = (d->n_idx + 255u) / 256u;
    const uint32_t need = 8u + 4u * n_attr + 2u + 3u + 2u + 2u + (draws ? 1u + draws : 0u) + 2u;
    if (need > cap) return 0;
    uint32_t n = 0;
    w[n++] = 0x00041710u; w[n++] = 0;
    for (int k = 0; k < 3; k++) { w[n++] = 0x00041714u; w[n++] = 0; }
    for (uint32_t k = 0; k < n_attr; k++) {
        const es_attr* a = &d->a[k];
        w[n++] = 0x00041740u + 4u * a->attr; w[n++] = d->pos_only ? (d->stride << 8 | 0x32u) : a->fmt;
        w[n++] = 0x00041680u + 4u * a->attr; w[n++] = 0x80000000u | (d->vtx_io + a->off);
    }
    w[n++] = 0x00041714u; w[n++] = 0;
    w[n++] = 0x0008181Cu; w[n++] = d->idx_io; w[n++] = 0x00000011u;
    w[n++] = 0x00040100u; w[n++] = 0xED6E0000u | d->cull;
    w[n++] = 0x00041808u; w[n++] = 5u;
    if (draws) {
        w[n++] = 0x40001824u | (draws << 18);
        for (uint32_t k = 0, first = 0; k < draws; k++, first += 256u) {
            const uint32_t c = d->n_idx - first < 256u ? d->n_idx - first : 256u;
            w[n++] = ((c - 1u) << 24) | first;
        }
    }
    w[n++] = 0x00041808u; w[n++] = 0;
    return n;
}
/* FIFO CALL to an IO offset (the walker returns on ES_RET). */
static inline uint32_t es_call(uint32_t io) { return (io & 0x1FFFFFFCu) | 2u; }
#endif
