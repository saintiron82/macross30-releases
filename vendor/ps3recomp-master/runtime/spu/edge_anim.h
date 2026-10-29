/* edge_anim.h -- the Edge Animation SPU job of Macross 30 (image 10006,
 * "Macross edgeAnim captured original job") as host code.
 *
 * Read from the lifted job (native/macross30/spu_gameplay/anim/spu_recomp.c,
 * disassembly native/analysis/anim-ra/anim.s); what the job reads and writes
 * is written down in native/analysis/edge-anim-spec.md.
 *
 * The port keeps the job's local-store picture: a private 256 KB work area
 * with the same buffers at the same addresses spurs_job.c stages them at, so
 * every byte the job PUTs -- including padding joints and the bytes around the
 * completion flag -- comes out as the SPU leaves them. Arithmetic is the
 * SPU's: every float operation goes through spu_helpers.h (truncating, no
 * denormals, saturating, the frest/frsqest tables) in the SPU's order.
 *
 * Header-only: spurs_job.c (shadow / trust path) and
 * tools/tests/edge_anim/harness_main.c (offline differential test) include
 * the same copy. */
#ifndef EDGE_ANIM_H
#define EDGE_ANIM_H
#include <stdint.h>
#include <string.h>
#include "spu_helpers.h"
#ifdef EAN_PROF
#include <intrin.h>
extern unsigned long long ean_prof[8];
#define EAN_P0() const unsigned long long ean_t0_ = __rdtsc()
#define EAN_P1(k) (ean_prof[k] += __rdtsc() - ean_t0_)
#else
#define EAN_P0() ((void)0)
#define EAN_P1(k) ((void)0)
#endif

/* ---- local store ------------------------------------------------------- */
#if defined(_MSC_VER)
#define EAN_TLS __declspec(thread)
#else
#define EAN_TLS _Thread_local
#endif
#define EAN_LS_SIZE 0x40000u
#define EAN_LS_MASK 0x3FFFFu
static inline uint32_t ean_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t ean_be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void ean_wbe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void ean_wbe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
/* word / halfword / byte at an LS address (wrapping like the SPU) */
static inline uint32_t ean_lw(const uint8_t* ls, uint32_t a) { return ean_be32(ls + (a & (EAN_LS_MASK & ~3u))); }
static inline uint32_t ean_lh(const uint8_t* ls, uint32_t a) { return ean_be16(ls + (a & (EAN_LS_MASK & ~1u))); }
static inline void ean_sw(uint8_t* ls, uint32_t a, uint32_t v) { ean_wbe32(ls + (a & (EAN_LS_MASK & ~3u)), v); }
static inline void ean_sh(uint8_t* ls, uint32_t a, uint32_t v) { ean_wbe16(ls + (a & (EAN_LS_MASK & ~1u)), v); }
/* quadword (lqd/stqd: the address is rounded down to 16) */
static inline u128 ean_lq(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & (EAN_LS_MASK & ~15u));
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = ean_be32(p + 4 * i);
    return v;
}
#ifdef EAN_DEBUG_WATCH
void ean_debug_watch(uint32_t a, u128 v, const char* fn);
#define EAN_WATCH(a, v) ean_debug_watch((a), (v), __func__)
#else
#define EAN_WATCH(a, v) ((void)0)
#endif
static inline void ean_sq_(uint8_t* ls, uint32_t a, u128 v)
{
    uint8_t* p = ls + (a & (EAN_LS_MASK & ~15u));
    for (int i = 0; i < 4; i++) ean_wbe32(p + 4 * i, v._u32[i]);
}
#define ean_sq(ls, a, v) do { const uint32_t ean_a_ = (a); const u128 ean_v_ = (v); EAN_WATCH(ean_a_, ean_v_); ean_sq_((ls), ean_a_, ean_v_); } while (0)
static inline u128 ean_splat(uint32_t v) { u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = v; return r; }
#define ean_splat_ ean_splat
static inline u128 ean_q4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }

/* ---- SPU single precision, one lane ------------------------------------ */
static inline uint32_t ean_fa(uint32_t a, uint32_t b) { return spu__fa_any(a, b); }
static inline uint32_t ean_fs(uint32_t a, uint32_t b) { return spu__fa_any(a, b ^ 0x80000000u); }
static inline uint32_t ean_fm(uint32_t a, uint32_t b) { return spu__fm_any(a, b); }
/* spu__fma_any's fast path without the libm fma(): its fma(1.0, p, z) with an
 * exact product p rounds p + z once, which is what the double addition does */
static inline uint32_t ean_fma_any(uint32_t x, uint32_t y, uint32_t z, uint32_t np, uint32_t nc)
{
    if (spu__sf_plain(x) && spu__sf_plain(y) && spu__sf_plain(z)) {
        const double p = (double)spu__sf_f(x ^ np) * (double)spu__sf_f(y);
        const uint32_t f = spu__sf_from_d(p + (double)spu__sf_f(z ^ nc), 1);
        if (f) return f;
    }
    return spu__fma_lane(x, y, z, np, nc);
}
static inline uint32_t ean_fma(uint32_t a, uint32_t b, uint32_t c) { return ean_fma_any(a, b, c, 0, 0); }               /* a*b + c */
static inline uint32_t ean_fms(uint32_t a, uint32_t b, uint32_t c) { return ean_fma_any(a, b, c, 0, 0x80000000u); }     /* a*b - c */
static inline uint32_t ean_fnms(uint32_t a, uint32_t b, uint32_t c) { return ean_fma_any(a, b, c, 0x80000000u, 0); }    /* c - a*b */

/* estimates and conversions: the vector helpers on one lane (the immediate is
 * the instruction's I8 field, as the lifted code passes it) */
/* one lane of spu_frest / spu_frsqest / spu_fi / the conversions / fcgt
 * (spu_helpers.h, same tables and formulas) */
static inline uint32_t ean_frest(uint32_t a)
{
    const uint32_t exp = (a >> 23) & 0xFFu, rexp = exp == 0u ? 255u : exp >= 253u ? 0u : 253u - exp;
    return (a & 0x80000000u) | (rexp << 23) | (spu_frest_fraction_lut[(a >> 18) & 0x1Fu] & 0x7FFFFFu);
}
static inline uint32_t ean_frsqest(uint32_t a)
{
    const uint32_t exp = (a >> 23) & 0xFFu, rexp = exp == 0u ? 0xFFu : 190u - (exp + 1u) / 2u;
    return (rexp << 23) | (spu_frsqest_fraction_lut[(a >> 18) & 0x3Fu] & 0x7FFFFFu);
}
static inline uint32_t ean_fi(uint32_t a, uint32_t est)
{
    int exp = (int)((est >> 23) & 0xFFu);
    if (exp == 0) return 0;
    uint64_t m = ((((uint64_t)1 << 13) | ((est >> 10) & 0x1FFFu)) << 19) - (uint64_t)(est & 0x3FFu) * (a & 0x7FFFFu);
    if (m < ((uint64_t)1 << 32)) { m <<= 1; if (--exp == 0) return 0; }
    return (est & 0x80000000u) | ((uint32_t)exp << 23) | (uint32_t)((m >> 9) & 0x7FFFFFu);
}
static inline uint32_t ean_cuflt(uint32_t a, int i8) { return a ? spu__sf_pack(0, a, -(155 - (i8 & 0xFF))) : 0; }
static inline uint32_t ean_csflt(uint32_t a, int i8)
{
    const int32_t v = (int32_t)a;
    return v ? spu__sf_pack(v < 0 ? 0x80000000u : 0, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, -(155 - (i8 & 0xFF))) : 0;
}
static inline uint32_t ean_cflts(uint32_t a, int i8) { return spu__cfl_lane(a, 173 - (i8 & 0xFF), 0); }
static inline uint32_t ean_cfltu(uint32_t a, int i8) { return spu__cfl_lane(a, 173 - (i8 & 0xFF), 1); }
static inline int ean_fgt(uint32_t a, uint32_t b) { return spu__sf_key(a) > spu__sf_key(b); }   /* fcgt */
/* 1/x as the job refines it: fi(frest) then one Newton step */
static inline uint32_t ean_recip_nr(uint32_t x)
{
    const uint32_t r = ean_fi(x, ean_frest(x));
    return ean_fma(ean_fnms(x, r, 0x3F800000u), r, r);
}

#define EAN_F_HALF 0x3F000000u
#define EAN_F_ONE  0x3F800000u
#define EAN_F_TWO  0x40000000u
#define EAN_F_MONE 0xBF800000u

/* ---- joints -> 4x4 matrices (SPU 0x84E8) --------------------------------
 * in: joint transforms, 48 bytes each (rotation quaternion x y z w,
 * translation x y z w, scale x y z w). out: 64 bytes each, rows
 *   0: (1 - 2(yy+zz), 2(xy+wz), 2(xz-wy), = lane 2) * sx
 *   1: (2(xy-wz), 1 - 2(xx+zz), 2(yz+wx), = lane 2) * sy
 *   2: (2(xz+wy), 2(yz-wx), 1 - 2(xx+yy), = lane 2) * sz
 *   3: (tx, ty, tz, 1)
 * computed as ((0.5 - aa) - bb) * (2s) and (ab +- wc) * (2s); four joints
 * per pass, so align4(n) joints are written. */
static inline void ean_joints_to_matrices(uint8_t* ls, uint32_t out, uint32_t in, uint32_t n)
{
    if (!n) return;
    const uint32_t m = (n + 3u) & ~3u;
    for (uint32_t j = 0; j < m; j++, in += 48u, out += 64u) {
        const u128 q = ean_lq(ls, in), t = ean_lq(ls, in + 16u), s = ean_lq(ls, in + 32u);
        const uint32_t X = q._u32[0], Y = q._u32[1], Z = q._u32[2], W = q._u32[3];
        const uint32_t sx2 = ean_fm(s._u32[0], EAN_F_TWO), sy2 = ean_fm(s._u32[1], EAN_F_TWO), sz2 = ean_fm(s._u32[2], EAN_F_TWO);
        const uint32_t hxx = ean_fnms(X, X, EAN_F_HALF), hyy = ean_fnms(Y, Y, EAN_F_HALF);
        const uint32_t xy = ean_fm(X, Y), xz = ean_fm(X, Z), yz = ean_fm(Y, Z);
        const uint32_t r00 = ean_fm(ean_fnms(Z, Z, hyy), sx2), r01 = ean_fm(ean_fma(W, Z, xy), sx2), r02 = ean_fm(ean_fnms(W, Y, xz), sx2);
        const uint32_t r10 = ean_fm(ean_fnms(W, Z, xy), sy2), r11 = ean_fm(ean_fnms(Z, Z, hxx), sy2), r12 = ean_fm(ean_fma(W, X, yz), sy2);
        const uint32_t r20 = ean_fm(ean_fma(W, Y, xz), sz2), r21 = ean_fm(ean_fnms(W, X, yz), sz2), r22 = ean_fm(ean_fnms(Y, Y, hxx), sz2);
        ean_sq(ls, out, ean_q4(r00, r01, r02, r02));
        ean_sq(ls, out + 16u, ean_q4(r10, r11, r12, r12));
        ean_sq(ls, out + 32u, ean_q4(r20, r21, r22, r22));
        ean_sq(ls, out + 48u, ean_q4(t._u32[0], t._u32[1], t._u32[2], EAN_F_ONE));
    }
}

/* ---- local joints -> world joints (SPU 0x8738) ---------------------------
 * hier: n entries of (u16 joint, u16 parent), 4 per quadword (Edge's SIMD
 * hierarchy). parent & 0x7FFF > 0x3FFF: the parent is the root transform;
 * parent bit 15: the joint does not inherit its parent's scale.
 *   rot   = P.rot * L.rot (quaternion product)
 *   scale = L.scale * (inherit ? P.scale : 1)
 *   trans = P.rot (L.trans * P.scale) P.rot^-1 + P.trans, w = 1 + P.trans.w
 * The SPU reads all four entries of a quadword before writing any of them
 * (rotations, then scales, then translations, each in lane order), so a joint
 * whose parent sits in the same quadword sees the parent's old value. Its
 * software-pipelined loop (more than one quadword) keeps that order: random
 * hierarchies with parents in the same or the previous quadword match this
 * sequential form (tools/tests/edge_anim unit "l2w"). */
static inline void ean_local_to_world(uint8_t* ls, uint32_t world, uint32_t local, uint32_t root, uint32_t hier, uint32_t n)
{
    const uint32_t groups = (n + 3u) >> 2;
    for (uint32_t g = 0; g < groups; g++, hier += 16u) {
        uint32_t j[4], P[4], keep[4];
        for (int k = 0; k < 4; k++) {
            const uint32_t e = ean_lw(ls, hier + 4u * (uint32_t)k), par = e & 0x7FFFu;
            j[k] = (e >> 16) * 48u;
            P[k] = par > 0x3FFFu ? root : world + par * 48u;
            keep[k] = (e & 0x8000u) != 0;   /* no scale inheritance */
        }
        u128 Pq[4], Pt[4], Ps[4], Lq[4], Lt[4], Ls[4];
        for (int k = 0; k < 4; k++) {
            Pq[k] = ean_lq(ls, P[k]); Pt[k] = ean_lq(ls, P[k] + 16u); Ps[k] = ean_lq(ls, P[k] + 32u);
            Lq[k] = ean_lq(ls, local + j[k]); Lt[k] = ean_lq(ls, local + 16u + j[k]); Ls[k] = ean_lq(ls, local + 32u + j[k]);
        }
        u128 Q[4], S[4], T[4];
        for (int k = 0; k < 4; k++) {
            const uint32_t PX = Pq[k]._u32[0], PY = Pq[k]._u32[1], PZ = Pq[k]._u32[2], PW = Pq[k]._u32[3];
            const uint32_t LX = Lq[k]._u32[0], LY = Lq[k]._u32[1], LZ = Lq[k]._u32[2], LW = Lq[k]._u32[3];
            const uint32_t qx = ean_fnms(PZ, LY, ean_fma(PY, LZ, ean_fma(PX, LW, ean_fm(PW, LX))));
            const uint32_t qy = ean_fma(PZ, LX, ean_fma(PY, LW, ean_fnms(PX, LZ, ean_fm(PW, LY))));
            const uint32_t qz = ean_fma(PZ, LW, ean_fnms(PY, LX, ean_fma(PX, LY, ean_fm(PW, LZ))));
            const uint32_t qw = ean_fnms(PZ, LZ, ean_fnms(PY, LY, ean_fnms(PX, LX, ean_fm(PW, LW))));
            Q[k] = ean_q4(qx, qy, qz, qw);
            const u128 ps = keep[k] ? ean_splat(EAN_F_ONE) : Ps[k];
            for (int c = 0; c < 4; c++) S[k]._u32[c] = ean_fm(Ls[k]._u32[c], ps._u32[c]);
            const uint32_t TX = ean_fm(Lt[k]._u32[0], Ps[k]._u32[0]), TY = ean_fm(Lt[k]._u32[1], Ps[k]._u32[1]);
            const uint32_t TZ = ean_fm(Lt[k]._u32[2], Ps[k]._u32[2]);
            const uint32_t uy = ean_fma(PW, TY, ean_fnms(PX, TZ, ean_fm(PZ, TX)));
            const uint32_t ux = ean_fma(PW, TX, ean_fnms(PZ, TY, ean_fm(PY, TZ)));
            const uint32_t uz = ean_fma(PW, TZ, ean_fnms(PY, TX, ean_fm(PX, TY)));
            const uint32_t wz = ean_fma(EAN_F_TWO, ean_fnms(PY, ux, ean_fm(PX, uy)), TZ);
            const uint32_t wy = ean_fma(EAN_F_TWO, ean_fnms(PX, uz, ean_fm(PZ, ux)), TY);
            const uint32_t wx = ean_fma(EAN_F_TWO, ean_fnms(PZ, uy, ean_fm(PY, uz)), TX);
            T[k] = ean_q4(ean_fa(wx, Pt[k]._u32[0]), ean_fa(wy, Pt[k]._u32[1]), ean_fa(wz, Pt[k]._u32[2]), ean_fa(EAN_F_ONE, Pt[k]._u32[3]));
        }
        for (int k = 0; k < 4; k++) ean_sq(ls, world + j[k], Q[k]);
        for (int k = 0; k < 4; k++) ean_sq(ls, world + 32u + j[k], S[k]);
        for (int k = 0; k < 4; k++) ean_sq(ls, world + 16u + j[k], T[k]);
    }
}

/* ---- blending helpers ----------------------------------------------------- */
/* sin(x) ~ x (1 - x^2/6 + x^4/120 - x^6/5040 + x^8/362880) */
static inline uint32_t ean_sinpoly(uint32_t x)
{
    const uint32_t x2 = ean_fm(x, x);
    uint32_t p = ean_fma(0x3638B94Fu, x2, 0xB9500C03u);
    p = ean_fma(p, x2, 0x3C088887u);
    p = ean_fma(p, x2, 0xBE2AAAABu);
    p = ean_fma(p, x2, EAN_F_ONE);
    return ean_fm(p, x);
}
/* slerp weights for |cos| = d >= 0 and blend factor alpha: theta = acos(d)
 * as (pi/2 - d(0.2146 - d(0.0890 - 0.0502 d))) sqrt(1 - d); above d =
 * 0.999 the plain lerp weights. wb weights the first quaternion. */
static inline void ean_slerp_weights(uint32_t d, uint32_t alpha, uint32_t* wb, uint32_t* wc)
{
    const uint32_t omd = ean_fs(EAN_F_ONE, d);
    const uint32_t rs = ean_fi(omd, ean_frsqest(omd));
    const uint32_t poly = ean_fnms(d, ean_fnms(d, ean_fnms(d, ean_fm(d, 0x3D4D8392u), 0x3DB63A9Eu), 0x3E5BBFCAu), 0x3FC90FDAu);
    const uint32_t theta = ean_fm(poly, ean_fnms(rs, d, rs));
    const uint32_t st = ean_sinpoly(theta);
    const uint32_t rst = ean_fi(st, ean_frest(st));
    const int lin = ean_fgt(d, 0x3F7FBE77u);
    const uint32_t oma = ean_fs(EAN_F_ONE, alpha);
    const uint32_t sc = ean_fm(ean_sinpoly(ean_fm(alpha, theta)), rst);
    const uint32_t sb = ean_fm(ean_sinpoly(ean_fm(oma, theta)), rst);
    *wc = lin ? alpha : sc;
    *wb = lin ? oma : sb;
}
/* weight byte -> float: u8 / 256 * 256/255 */
static inline uint32_t ean_wbyte(uint32_t b) { return ean_fm(ean_cuflt(b, 147), 0x3F808081u); }
/* one joint weight from the B/C bytes and the branch factor: the effective
 * blend factor (alpha) and the blended weight, clamped to a byte */
static inline uint32_t ean_blend_alpha(uint32_t b, uint32_t c, uint32_t f, uint32_t* wout)
{
    const int gt = ean_fgt(c, b);
    const uint32_t mx = gt ? c : b;
    const uint32_t num = gt ? ean_fs(ean_fma(f, b, c), b) : ean_fm(f, c);
    const uint32_t alpha = ean_fm(num, ean_recip_nr(mx));
    int32_t w = (int32_t)ean_cflts(ean_fma(ean_fnms(b, alpha, ean_fma(c, alpha, b)), 0x3F7F0000u, 0x3B000000u), 165);
    if (w > 255) w = 255;
    if (w < 0) w = 0;
    *wout = (uint32_t)w;
    return alpha;
}

/* ---- blend two poses, linear (SPU 0x6910: blend op 0) ---------------------
 * out[j] = slerp/lerp(B[j], C[j], alpha_j) for align4(n) joints; alpha from
 * the factor f and the joints' weight bytes (a NULL weight array: 255). A
 * zero weight in B takes C's joint as is; else a zero in C takes B's. The
 * blended weights go to outw (whole words, 4 joints each). out may be B or C. */
static inline void ean_blend_linear(uint8_t* ls, uint32_t out, uint32_t outw, uint32_t B, uint32_t Bw,
                                    uint32_t C, uint32_t Cw, uint32_t f, uint32_t n)
{
    if (!n) return;
    const uint32_t m = (n + 3u) & ~3u;
    for (uint32_t g = 0; g < m; g += 4u) {
        uint32_t wbytes = 0;
        u128 q[4], t[4], s[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t j = g + k;
            const uint32_t wb = Bw ? ls[(Bw + j) & EAN_LS_MASK] : 255u, wc = Cw ? ls[(Cw + j) & EAN_LS_MASK] : 255u;
            uint32_t nw;
            const uint32_t alpha = ean_blend_alpha(ean_wbyte(wb), ean_wbyte(wc), f, &nw);
            wbytes |= nw << (24 - 8 * k);
            const u128 bq = ean_lq(ls, B + 48u * j), bt = ean_lq(ls, B + 48u * j + 16u), bs = ean_lq(ls, B + 48u * j + 32u);
            const u128 cq = ean_lq(ls, C + 48u * j), ct = ean_lq(ls, C + 48u * j + 16u), cs = ean_lq(ls, C + 48u * j + 32u);
            if (wb == 0) { q[k] = cq; t[k] = ct; s[k] = cs; continue; }
            if (wc == 0) { q[k] = bq; t[k] = bt; s[k] = bs; continue; }
            for (int c = 0; c < 4; c++) {
                t[k]._u32[c] = ean_fnms(bt._u32[c], alpha, ean_fma(ct._u32[c], alpha, bt._u32[c]));
                s[k]._u32[c] = ean_fnms(bs._u32[c], alpha, ean_fma(cs._u32[c], alpha, bs._u32[c]));
            }
            const uint32_t dot = ean_fma(bq._u32[3], cq._u32[3], ean_fma(bq._u32[2], cq._u32[2], ean_fma(bq._u32[1], cq._u32[1], ean_fm(bq._u32[0], cq._u32[0]))));
            const uint32_t sgn = ean_fgt(0, dot) ? EAN_F_MONE : EAN_F_ONE;
            uint32_t wqb, wqc;
            ean_slerp_weights(ean_fm(dot, sgn), alpha, &wqb, &wqc);
            for (int c = 0; c < 4; c++) q[k]._u32[c] = ean_fma(ean_fm(cq._u32[c], sgn), wqc, ean_fm(bq._u32[c], wqb));
        }
        for (uint32_t k = 0; k < 4; k++) {
            ean_sq(ls, out + 48u * (g + k), q[k]); ean_sq(ls, out + 48u * (g + k) + 16u, t[k]); ean_sq(ls, out + 48u * (g + k) + 32u, s[k]);
        }
        ean_sw(ls, outw + g, wbytes);
    }
}

/* ---- blend two poses, additive family (SPU 0x7090: blend ops 1..7) -------
 * flags bit 0: C is subtracted (conjugate rotation, negated translation,
 * reciprocal scale -- frest/fi, no refinement); bit 4: full strength
 * (alpha = 1) and a zero B weight takes C's (adjusted) joint. Otherwise
 * alpha = f * weight(C).
 *   Q = B.rot * C'.rot; rot = slerp(B.rot, Q, alpha)
 *   trans = lerp(B.trans, B.trans + C'.trans (w: + 0), alpha)
 *   scale = lerp(B.scale, B.scale * C'.scale, alpha)
 * A zero C weight keeps B. Weights out: min(255, wB + wC), 0 where wB is 0
 * unless bit 4. */
static inline void ean_blend_op(uint8_t* ls, uint32_t out, uint32_t outw, uint32_t B, uint32_t Bw,
                                uint32_t C, uint32_t Cw, uint32_t f, uint32_t flags, uint32_t n)
{
    if (!n) return;
    const int sub = (flags & 1u) != 0, full = (flags & 16u) != 0;
    const uint32_t m = (n + 3u) & ~3u, tsg = sub ? EAN_F_MONE : EAN_F_ONE, qsg = sub ? 0x80000000u : 0u;
    for (uint32_t g = 0; g < m; g += 4u) {
        uint32_t wbytes = 0;
        u128 q[4], t[4], s[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t j = g + k;
            const uint32_t wb = Bw ? ls[(Bw + j) & EAN_LS_MASK] : 255u, wc = Cw ? ls[(Cw + j) & EAN_LS_MASK] : 255u;
            uint32_t nw = wb + wc > 255u ? 255u : wb + wc;
            if (!full && wb == 0) nw = 0;
            wbytes |= nw << (24 - 8 * k);
            const uint32_t alpha = full ? EAN_F_ONE : ean_fm(f, ean_wbyte(wc));
            const u128 bq = ean_lq(ls, B + 48u * j), bt = ean_lq(ls, B + 48u * j + 16u), bs = ean_lq(ls, B + 48u * j + 32u);
            const u128 cq = ean_lq(ls, C + 48u * j), ct = ean_lq(ls, C + 48u * j + 16u), cs = ean_lq(ls, C + 48u * j + 32u);
            const uint32_t cx = cq._u32[0] ^ qsg, cy = cq._u32[1] ^ qsg, cz = cq._u32[2] ^ qsg, cw = cq._u32[3];
            u128 ct2 = ean_splat(0), cs2 = ean_splat(0);
            for (int c = 0; c < 3; c++) ct2._u32[c] = ean_fm(ct._u32[c], tsg);
            ct2._u32[3] = ean_fm(ct._u32[3], 0);
            for (int c = 0; c < 4; c++) cs2._u32[c] = sub ? ean_fi(cs._u32[c], ean_frest(cs._u32[c])) : cs._u32[c];
            if (full && wb == 0) { q[k] = ean_q4(cx, cy, cz, cw); t[k] = ct2; s[k] = cs2; continue; }
            if (wc == 0) { q[k] = bq; t[k] = bt; s[k] = bs; continue; }
            const uint32_t Bx = bq._u32[0], By = bq._u32[1], Bz = bq._u32[2], Bwq = bq._u32[3];
            const uint32_t qx = ean_fnms(By, cz, ean_fma(Bz, cy, ean_fma(Bwq, cx, ean_fm(Bx, cw))));
            const uint32_t qy = ean_fma(Bx, cz, ean_fma(Bwq, cy, ean_fnms(Bz, cx, ean_fm(By, cw))));
            const uint32_t qz = ean_fma(Bwq, cz, ean_fnms(Bx, cy, ean_fma(By, cx, ean_fm(Bz, cw))));
            const uint32_t qw = ean_fnms(Bz, cz, ean_fnms(By, cy, ean_fnms(Bx, cx, ean_fm(Bwq, cw))));
            const uint32_t Q[4] = { qx, qy, qz, qw };
            const uint32_t dot = ean_fma(Bwq, qw, ean_fma(Bz, qz, ean_fma(By, qy, ean_fm(Bx, qx))));
            const uint32_t sgn = ean_fgt(0, dot) ? EAN_F_MONE : EAN_F_ONE;
            uint32_t wqb, wqc;
            ean_slerp_weights(ean_fm(dot, sgn), alpha, &wqb, &wqc);
            for (int c = 0; c < 4; c++) {
                q[k]._u32[c] = ean_fma(ean_fm(Q[c], sgn), wqc, ean_fm(bq._u32[c], wqb));
                t[k]._u32[c] = ean_fnms(bt._u32[c], alpha, ean_fma(ean_fa(ct2._u32[c], bt._u32[c]), alpha, bt._u32[c]));
                s[k]._u32[c] = ean_fnms(bs._u32[c], alpha, ean_fma(ean_fm(cs2._u32[c], bs._u32[c]), alpha, bs._u32[c]));
            }
        }
        for (uint32_t k = 0; k < 4; k++) {
            ean_sq(ls, out + 48u * (g + k), q[k]); ean_sq(ls, out + 48u * (g + k) + 16u, t[k]); ean_sq(ls, out + 48u * (g + k) + 32u, s[k]);
        }
        ean_sw(ls, outw + g, wbytes);
    }
}

/* ---- user channels (SPU 0x7FD0 linear, 0x8260 ops 1..7) ------------------
 * One float per channel, align4(n) channels; weights as for joints. The
 * skeleton's per-channel flag byte (tab, NULL: 0): bit 0 clamps the result to
 * [0, 1]; bit 1 (ops only) blends by max -- min when subtracting -- instead
 * of adding. */
static inline uint32_t ean_clamp01(uint32_t v)
{
    if (ean_fgt(v, EAN_F_ONE)) v = EAN_F_ONE;
    if (ean_fgt(0, v)) v = 0;
    return v;
}
static inline void ean_blend_user_linear(uint8_t* ls, uint32_t out, uint32_t outw, uint32_t B, uint32_t Bw,
                                         uint32_t C, uint32_t Cw, uint32_t tab, uint32_t f, uint32_t n)
{
    if (!n) return;
    const uint32_t m = (n + 3u) & ~3u;
    for (uint32_t g = 0; g < m; g += 4u) {
        uint32_t wbytes = 0, v[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t j = g + k;
            const uint32_t wb = Bw ? ls[(Bw + j) & EAN_LS_MASK] : 255u, wc = Cw ? ls[(Cw + j) & EAN_LS_MASK] : 255u;
            const uint32_t T = tab ? ls[(tab + j) & EAN_LS_MASK] : 0u;
            uint32_t nw;
            const uint32_t alpha = ean_blend_alpha(ean_wbyte(wb), ean_wbyte(wc), f, &nw);
            wbytes |= nw << (24 - 8 * k);
            const uint32_t b = ean_lw(ls, B + 4u * j), c = ean_lw(ls, C + 4u * j);
            uint32_t x = wb == 0 ? c : wc == 0 ? b : ean_fnms(b, alpha, ean_fma(c, alpha, b));
            if (T & 1u) x = ean_clamp01(x);
            v[k] = x;
        }
        ean_sq(ls, out + 4u * g, ean_q4(v[0], v[1], v[2], v[3]));
        ean_sw(ls, outw + g, wbytes);
    }
}
static inline void ean_blend_user_op(uint8_t* ls, uint32_t out, uint32_t outw, uint32_t B, uint32_t Bw,
                                     uint32_t C, uint32_t Cw, uint32_t tab, uint32_t f, uint32_t flags, uint32_t n)
{
    if (!n) return;
    const int sub = (flags & 1u) != 0, full = (flags & 16u) != 0;
    const uint32_t m = (n + 3u) & ~3u;
    for (uint32_t g = 0; g < m; g += 4u) {
        uint32_t wbytes = 0, v[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t j = g + k;
            const uint32_t wb = Bw ? ls[(Bw + j) & EAN_LS_MASK] : 255u, wc = Cw ? ls[(Cw + j) & EAN_LS_MASK] : 255u;
            const uint32_t T = tab ? ls[(tab + j) & EAN_LS_MASK] : 0u;
            uint32_t nw = wb + wc > 255u ? 255u : wb + wc;
            if (!full && wb == 0) nw = 0;
            wbytes |= nw << (24 - 8 * k);
            const uint32_t b = ean_lw(ls, B + 4u * j), c = ean_lw(ls, C + 4u * j);
            const uint32_t cn = c ^ ((sub && !(T & 2u)) ? 0x80000000u : 0u);
            const uint32_t alpha = full ? EAN_F_ONE : ean_fm(f, ean_wbyte(wc));
            const uint32_t mm = (ean_fgt(b, c) != sub) ? b : c;       /* max (min when subtracting) */
            uint32_t x;
            if (full && wb == 0) x = cn;
            else if (wc == 0) x = b;
            else if ((T & 2u) && wb != 0) x = mm;
            else x = ean_fma(cn, alpha, b);
            if (T & 1u) x = ean_clamp01(x);
            v[k] = x;
        }
        ean_sq(ls, out + 4u * g, ean_q4(v[0], v[1], v[2], v[3]));
        ean_sw(ls, outw + g, wbytes);
    }
}

/* ---- animated channels: frame bitmasks ----------------------------------
 * A frame set (a DMA'd block of up to 128 frames after its first) stores,
 * per animated channel, one bit per frame 1..N ("a key at this frame"), the
 * channels' bit runs back to back, MSB first. A channel's keys are packed in
 * frame order; frame 0 comes from the block's first-frame table, frame N+1
 * from its last-frame table (the next block's first frame).
 * The SPU evaluates four channels at a time; past the last channel of a kind
 * the lanes read whatever follows (the next kind's bits and keys) and write
 * it to the padding joint the channel lists point at -- kept here, since the
 * padding joints reach the output matrices. */
typedef struct {
    uint32_t allf[4];   /* the N frame bits of a 128-bit window */
    uint32_t bef[4];    /* the bits of frames 1..f (keys before the evaluated interval) */
    uint32_t f, frac;   /* frame index in the block, fraction (SPU float) */
} ean_frame;
/* 128 bits of local store from bit address b (MSB first), as 4 words */
static inline void ean_bits128(const uint8_t* ls, uint32_t b, uint32_t w[4])
{
    const uint32_t base = (b >> 3) & (EAN_LS_MASK & ~15u), off = b & 127u;
    uint32_t v[9];
    for (int i = 0; i < 8; i++) v[i] = ean_lw(ls, base + 4u * (uint32_t)i);
    v[8] = 0;
    const uint32_t ws = off >> 5, bs = off & 31u;
    for (uint32_t i = 0; i < 4; i++)
        w[i] = bs ? (v[i + ws] << bs) | (v[i + ws + 1] >> (32 - bs)) : v[i + ws];
}
/* 16 bytes at any LS address */
static inline u128 ean_lq_u(const uint8_t* ls, uint32_t a)
{
    const uint32_t q0 = a & (EAN_LS_MASK & ~15u), o = a & 15u;
    uint8_t b[32];
    memcpy(b, ls + q0, 16);
    memcpy(b + 16, ls + ((q0 + 16u) & EAN_LS_MASK), 16);
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = ean_be32(b + o + 4 * i);
    return v;
}
/* lqd + rotqby: the word at a, wrapping inside its quadword */
static inline uint32_t ean_lw_rot(const uint8_t* ls, uint32_t a)
{
    const uint8_t* q = ls + (a & (EAN_LS_MASK & ~15u));
    const uint32_t o = a & 15u;
    return (uint32_t)q[o] << 24 | (uint32_t)q[(o + 1) & 15] << 16 | (uint32_t)q[(o + 2) & 15] << 8 | q[(o + 3) & 15];
}
static inline uint32_t ean_clz(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }
/* per lane: keys before the interval, all keys, and the interval's frames
 * p (last key at or before f; 0: the first-frame table) and n (first key
 * after f; N+1 is the last-frame table) as the SPU forms them */
typedef struct { uint32_t before, total, fp, nf; } ean_keyspan;
static inline ean_keyspan ean_span(const uint32_t W[4], const ean_frame* fr)
{
    ean_keyspan s = { 0, 0, 0, 0 };
    uint32_t c[4], e[4], cn[4];
    for (int w = 0; w < 4; w++) {
        const uint32_t a = W[w] & fr->allf[w], bf = W[w] & fr->bef[w];
        s.total += (uint32_t)__builtin_popcount(a);
        s.before += (uint32_t)__builtin_popcount(bf);
        c[w] = ean_clz(bf & (0u - bf));
        e[w] = c[w] == 32u;
        cn[w] = ean_clz((a & ~fr->bef[w]) | ~fr->allf[w]);
    }
    /* 128 - p: 32 per empty word above the last key, then 31 - its bit position */
    uint32_t r = 0;
    const uint32_t v0 = e[0] ? 32u : 31u - c[0], v1 = e[1] ? 32u : 31u - c[1], v2 = e[2] ? 32u : 31u - c[2], v3 = e[3] ? 32u : 31u - c[3];
    r = v3 + (e[3] ? v2 : 0) + ((e[3] && e[2]) ? v1 : 0) + ((e[3] && e[2] && e[1]) ? v0 : 0);
    s.fp = r - (128u - fr->f);   /* f - p */
    uint32_t k = cn[0];
    if (cn[0] == 32u) k += cn[1];
    if (cn[0] == 32u && cn[1] == 32u) k += cn[2];
    if (cn[0] == 32u && cn[1] == 32u && cn[2] == 32u) k += cn[3];
    s.nf = k + 1u - fr->f;        /* n - f */
    return s;
}
/* interpolation factor (frac + f - p) / ((f - p) + (n - f)), frest/fi */
static inline uint32_t ean_span_t(const ean_keyspan* s, uint32_t frac)
{
    const uint32_t d0 = ean_cuflt(s->fp, 155), d1 = ean_cuflt(s->nf, 155);
    const uint32_t d = ean_fa(d0, d1);
    return ean_fm(ean_fa(frac, d0), ean_fi(d, ean_frest(d)));
}

/* ---- animated translations / scales (SPU 0xA7D0) ------------------------
 * keys: 12 bytes (3 floats, the 16-byte load takes 4 bytes more for w);
 * keybase points one key before this kind's first key. out: the joints'
 * translation (or scale) base; jl: the kind's u16 joint list. Lanes of bits,
 * first and last: the four channels of the first group. */
static inline void ean_anim_lerp(uint8_t* ls, u128 bits, const ean_frame* fr, uint32_t keybase, u128 first, u128 last,
                                 uint32_t out, uint32_t jl, uint32_t count, uint32_t nframes)
{
    const uint32_t groups = (count + 3u) >> 2;
    for (uint32_t g = 0; g < groups; g++) {
        ean_keyspan sp[4];
        for (int L = 0; L < 4; L++) {
            uint32_t W[4];
            ean_bits128(ls, bits._u32[L], W);
            sp[L] = ean_span(W, fr);
        }
        u128 val[4];
        uint32_t pre = 0, sum = 0;
        for (int L = 0; L < 4; L++) {
            const uint32_t idx = pre + (sp[L].before & 255u);
            pre += sp[L].total & 255u;
            sum += sp[L].total & 255u;
            const uint32_t pk = keybase + 12u * idx;
            const uint32_t p0 = sp[L].before ? pk : first._u32[L];
            const uint32_t p1 = sp[L].before == sp[L].total ? last._u32[L] : pk + 12u;
            const u128 v0 = ean_lq_u(ls, p0), v1 = ean_lq_u(ls, p1);
            const uint32_t t = ean_span_t(&sp[L], fr->frac);
            for (int c = 0; c < 4; c++) val[L]._u32[c] = ean_fnms(v0._u32[c], t, ean_fma(t, v1._u32[c], v0._u32[c]));
        }
        for (int L = 0; L < 4; L++) ean_sq(ls, out + 48u * ean_lh(ls, jl + 2u * (uint32_t)L), val[L]);
        keybase += 12u * sum;
        jl += 8u;
        for (int L = 0; L < 4; L++) {
            bits._u32[L] += 4u * nframes; first._u32[L] += 48u; last._u32[L] += 48u;
        }
    }
}

/* ---- 48-bit rotations ----------------------------------------------------
 * 16-bit h, 32-bit l: three components h, l[31:17], l[16:2] as
 * u * sqrt2 / 32768 - sqrt2 / 2, the fourth sqrt(1 - a^2 - b^2 - c^2)
 * (frsqest/fi times the square), placed at l[1:0] (0 x, 1 y, 2 z, 3 w). */
static inline void ean_quat48(uint32_t h, uint32_t l, uint32_t q[4])
{
    const uint32_t s2 = 0x3FB504F3u, off = ean_fm(0xBF000000u, s2);
    const uint32_t c0 = ean_fma(ean_cuflt(h, 140), s2, off);
    const uint32_t c1 = ean_fma(ean_cuflt((l >> 17) & 0x7FFFu, 140), s2, off);
    const uint32_t c2 = ean_fma(ean_cuflt((l >> 2) & 0x7FFFu, 140), s2, off);
    const uint32_t s = ean_fnms(c2, c2, ean_fnms(c1, c1, ean_fnms(c0, c0, EAN_F_ONE)));
    const uint32_t c3 = ean_fm(ean_fi(s, ean_frsqest(s)), s);
    switch (l & 3u) {
    case 0: q[0] = c3; q[1] = c0; q[2] = c1; q[3] = c2; break;
    case 1: q[0] = c0; q[1] = c3; q[2] = c1; q[3] = c2; break;
    case 2: q[0] = c0; q[1] = c1; q[2] = c3; q[3] = c2; break;
    default: q[0] = c0; q[1] = c1; q[2] = c2; q[3] = c3; break;
    }
}
/* q = slerp(a, b, t) as the blend kernels and the key decoder form it */
static inline void ean_slerp(const uint32_t a[4], const uint32_t b[4], uint32_t t, uint32_t q[4])
{
    const uint32_t dot = ean_fma(a[3], b[3], ean_fma(a[2], b[2], ean_fma(a[1], b[1], ean_fm(a[0], b[0]))));
    const uint32_t sgn = ean_fgt(0, dot) ? EAN_F_MONE : EAN_F_ONE;
    uint32_t wa, wb;
    ean_slerp_weights(ean_fm(dot, sgn), t, &wa, &wb);
    for (int c = 0; c < 4; c++) q[c] = ean_fma(ean_fm(b[c], sgn), wb, ean_fm(a[c], wa));
}

/* ---- constant rotations (SPU 0xA520) --------------------------------------
 * n 48-bit rotations packed 6 bytes apart from data (rounded down to 16),
 * written to out + 48 * list[k]; eight at a time, stored in the order
 * 6 5 4 7 0 1 3 2 of each eight (a list may repeat the padding joint). */
static inline void ean_const_rot(uint8_t* ls, uint32_t out, uint32_t data, uint32_t n, uint32_t list)
{
    static const uint8_t order[8] = { 6, 5, 4, 7, 0, 1, 3, 2 };
    if (!n) return;
    data &= EAN_LS_MASK & ~15u;
    list &= EAN_LS_MASK & ~15u;
    for (uint32_t g = 0; g < (n + 7u) >> 3; g++, data += 48u, list += 16u) {
        uint32_t q[8][4], j[8];
        for (uint32_t i = 0; i < 8; i++) {
            const uint8_t* p = ls + ((data + 6u * i) & EAN_LS_MASK);
            ean_quat48(ean_be16(p), ean_be32(p + 2), q[i]);
            j[i] = ean_lh(ls, list + 2u * i);
        }
        for (uint32_t k = 0; k < 8; k++) {
            const uint32_t i = order[k];
            ean_sq(ls, out + 48u * j[i], ean_q4(q[i][0], q[i][1], q[i][2], q[i][3]));
        }
    }
}

/* ---- animated rotations (SPU 0x98D8) ---------------------------------------
 * As ean_anim_lerp with 6-byte (48-bit) keys and a slerp between the two
 * keys around the evaluated time; stored per group in lane order 2 3 0 1. */
static inline void ean_anim_slerp(uint8_t* ls, u128 bits, const ean_frame* fr, uint32_t keybase, u128 first, u128 last,
                                  uint32_t out, uint32_t jl, uint32_t count, uint32_t nframes)
{
    static const uint8_t order[4] = { 2, 3, 0, 1 };
    const uint32_t groups = (count + 3u) >> 2;
    for (uint32_t g = 0; g < groups; g++) {
        ean_keyspan sp[4];
        for (int L = 0; L < 4; L++) {
            uint32_t W[4];
            ean_bits128(ls, bits._u32[L], W);
            sp[L] = ean_span(W, fr);
        }
        uint32_t q[4][4], pre = 0, sum = 0;
        for (int L = 0; L < 4; L++) {
            const uint32_t idx = pre + (sp[L].before & 255u);
            pre += sp[L].total & 255u;
            sum += sp[L].total & 255u;
            const uint32_t pk = keybase + 6u * idx;
            const uint32_t p0 = sp[L].before ? pk : first._u32[L];
            const uint32_t p1 = sp[L].before == sp[L].total ? last._u32[L] : pk + 6u;
            const u128 k0 = ean_lq_u(ls, p0), k1 = ean_lq_u(ls, p1);
            uint32_t a[4], b[4];
            ean_quat48(k0._u32[0] >> 16, k0._u32[0] << 16 | k0._u32[1] >> 16, a);
            ean_quat48(k1._u32[0] >> 16, k1._u32[0] << 16 | k1._u32[1] >> 16, b);
            ean_slerp(a, b, ean_span_t(&sp[L], fr->frac), q[L]);
        }
        for (int k = 0; k < 4; k++) {
            const uint32_t L = order[k];
            ean_sq(ls, out + 48u * ean_lh(ls, jl + 2u * L), ean_q4(q[L][0], q[L][1], q[L][2], q[L][3]));
        }
        keybase += 6u * sum;
        jl += 8u;
        for (int L = 0; L < 4; L++) {
            bits._u32[L] += 4u * nframes; first._u32[L] += 24u; last._u32[L] += 24u;
        }
    }
}

/* ---- animated user channels (SPU 0xACD8) --------------------------------
 * As ean_anim_lerp with one float per key and channel: out + 4 * list[k],
 * one word written per channel (lanes in order). */
static inline void ean_anim_user(uint8_t* ls, u128 bits, const ean_frame* fr, uint32_t keybase, u128 first, u128 last,
                                 uint32_t out, uint32_t jl, uint32_t count, uint32_t nframes)
{
    const uint32_t groups = (count + 3u) >> 2;
    for (uint32_t g = 0; g < groups; g++) {
        ean_keyspan sp[4];
        for (int L = 0; L < 4; L++) {
            uint32_t W[4];
            ean_bits128(ls, bits._u32[L], W);
            sp[L] = ean_span(W, fr);
        }
        uint32_t val[4], pre = 0, sum = 0;
        for (int L = 0; L < 4; L++) {
            const uint32_t idx = pre + (sp[L].before & 255u);
            pre += sp[L].total & 255u;
            sum += sp[L].total & 255u;
            const uint32_t pk = keybase + 4u * idx;
            const uint32_t p0 = sp[L].before ? pk : first._u32[L];
            const uint32_t p1 = sp[L].before == sp[L].total ? last._u32[L] : pk + 4u;
            const uint32_t v0 = ean_lw_rot(ls, p0), v1 = ean_lw_rot(ls, p1);
            const uint32_t t = ean_span_t(&sp[L], fr->frac);
            val[L] = ean_fnms(v0, t, ean_fma(t, v1, v0));
        }
        for (int L = 0; L < 4; L++) ean_sw(ls, out + 4u * ean_lh(ls, jl + 2u * (uint32_t)L), val[L]);
        keybase += 4u * sum;
        jl += 8u;
        for (int L = 0; L < 4; L++) {
            bits._u32[L] += 4u * nframes; first._u32[L] += 16u; last._u32[L] += 16u;
        }
    }
}

/* ---- evaluate one animation at a frame (SPU 0x8FD8) ------------------------
 * hdr: the EA01 header in local store; blk: the frame set block; nf: frames
 * in the block after its first (bits per channel); f: frame index in the
 * block (f <= nf, the caller checks), frac: fraction (SPU float).
 * Header counts (u16): +16 const rot, +18 const trans, +1A const scale,
 * +1C const user, +1E/+20/+22/+24 the animated ones. Self-relative offsets:
 * +3C const rotations (48-bit), +40 const translations (12 B), +44 const
 * scales, +48 const user floats. Joint/channel lists (u16) from +50:
 * const rot (align8), const trans, const scale, const user, anim rot, anim
 * trans, anim scale, anim user (align4 each).
 * Block: first-frame tables (rot 6 B, trans 12 B, scale 12 B, align4, user
 * 4 B), key counts (u16 rot, trans, scale, user), the channel bit runs (nf
 * bits each, rot..user), keys (rot 6 B, trans 12 B, scale 12 B, align4,
 * user 4 B), align16, last-frame tables (as the first-frame ones).
 * As on the SPU: a constant translation's w is the next channel's x (lane
 * 0) or 0; constant scales land in the TRANSLATION slot (+16) and every
 * group takes the first group's four values. */
static inline void ean_evaluate(uint8_t* ls, uint32_t joints, uint32_t user, uint32_t hdr, uint32_t blk,
                                uint32_t nf, uint32_t f, uint32_t frac)
{
#define EAN_AL4(x) (((x) + 3u) & ~3u)
    const uint32_t nCR = ean_lh(ls, hdr + 0x16), nCT = ean_lh(ls, hdr + 0x18), nCS = ean_lh(ls, hdr + 0x1A), nCU = ean_lh(ls, hdr + 0x1C);
    const uint32_t nAR = ean_lh(ls, hdr + 0x1E), nAT = ean_lh(ls, hdr + 0x20), nAS = ean_lh(ls, hdr + 0x22), nAU = ean_lh(ls, hdr + 0x24);
    uint32_t off;
    off = ean_lw(ls, hdr + 0x3C); const uint32_t cR = off ? hdr + 0x3Cu + off : 0;
    off = ean_lw(ls, hdr + 0x40); const uint32_t cT = off ? hdr + 0x40u + off : 0;
    off = ean_lw(ls, hdr + 0x44); const uint32_t cS = off ? hdr + 0x44u + off : 0;
    off = ean_lw(ls, hdr + 0x48); const uint32_t cU = off ? hdr + 0x48u + off : 0;
    const uint32_t lR = hdr + 0x50u, lT = lR + 2u * ((nCR + 7u) & ~7u), lS = lT + 2u * EAN_AL4(nCT), lU = lS + 2u * EAN_AL4(nCS);
    const uint32_t laR = lU + 2u * EAN_AL4(nCU), laT = laR + 2u * EAN_AL4(nAR), laS = laT + 2u * EAN_AL4(nAT), laU = laS + 2u * EAN_AL4(nAS);
    const uint32_t fR = blk, fT = fR + 6u * nAR, fS = fT + 12u * nAT, fU = EAN_AL4(fS + 12u * nAS);
    const uint32_t cnt = fU + 4u * nAU;
    const uint32_t nRk = ean_lh(ls, cnt), nTk = ean_lh(ls, cnt + 2u), nSk = ean_lh(ls, cnt + 4u), nUk = ean_lh(ls, cnt + 6u);
    const uint32_t bits = cnt + 8u, nbits = nf * (nAR + nAT + nAS + nAU);
    const uint32_t kR = bits + ((nbits + 7u) >> 3), kT = kR + 6u * nRk, kS = kT + 12u * nTk, kU = EAN_AL4(kS + 12u * nSk);
    const uint32_t gR = (kU + 4u * nUk + 15u) & ~15u, gT = gR + 6u * nAR, gS = gT + 12u * nAT, gU = EAN_AL4(gS + 12u * nAS);
    ean_frame fr;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t a = nf > 32u * i ? nf - 32u * i : 0u, b = f > 32u * i ? f - 32u * i : 0u;
        fr.allf[i] = a >= 32u ? 0xFFFFFFFFu : a ? ~(0xFFFFFFFFu >> a) : 0u;
        fr.bef[i] = b >= 32u ? 0xFFFFFFFFu : b ? ~(0xFFFFFFFFu >> b) : 0u;
    }
    fr.f = f; fr.frac = frac;
    const uint32_t bR = bits * 8u, bT = bR + nf * nAR, bS = bT + nf * nAT, bU = bS + nf * nAS;
    /* constants */
    ean_const_rot(ls, joints, cR, nCR, lR);
    for (uint32_t g = 0, d = cT, l = lT; g < (nCT + 3u) >> 2; g++, d += 48u, l += 8u) {
        const u128 a = ean_lq(ls, d), b = ean_lq(ls, d + 16u), c = ean_lq(ls, d + 32u);
        const u128 v[4] = { a, ean_q4(a._u32[3], b._u32[0], b._u32[1], 0), ean_q4(b._u32[2], b._u32[3], c._u32[0], 0),
                            ean_q4(c._u32[1], c._u32[2], c._u32[3], 0) };
        for (uint32_t k = 0; k < 4; k++) ean_sq(ls, joints + 48u * ean_lh(ls, l + 2u * k) + 16u, v[k]);
    }
    if ((nCS + 3u) >> 2) {
        const u128 a = ean_lq(ls, cS), b = ean_lq(ls, cS + 16u), c = ean_lq(ls, cS + 32u);
        const u128 v[4] = { a, ean_q4(a._u32[3], b._u32[0], b._u32[1], 0), ean_q4(b._u32[2], b._u32[3], c._u32[0], 0),
                            ean_q4(c._u32[1], c._u32[2], c._u32[3], 0) };
        for (uint32_t g = 0, l = lS; g < (nCS + 3u) >> 2; g++, l += 8u)
            for (uint32_t k = 0; k < 4; k++) ean_sq(ls, joints + 48u * ean_lh(ls, l + 2u * k) + 16u, v[k]);
    }
    for (uint32_t k = 0; k < nCU; k++)
        ean_sw(ls, user + 4u * ean_lh(ls, lU + 2u * k), ean_lw_rot(ls, cU + 4u * k));
    /* animated */
#define EAN_LANES(base, step) ean_q4((base), (base) + (step), (base) + 2u * (step), (base) + 3u * (step))
    ean_anim_slerp(ls, EAN_LANES(bR, nf), &fr, kR - 6u, EAN_LANES(fR, 6u), EAN_LANES(gR, 6u), joints, laR, nAR, nf);
    ean_anim_lerp(ls, EAN_LANES(bT, nf), &fr, kT - 12u, EAN_LANES(fT, 12u), EAN_LANES(gT, 12u), joints + 16u, laT, nAT, nf);
    ean_anim_lerp(ls, EAN_LANES(bS, nf), &fr, kS - 12u, EAN_LANES(fS, 12u), EAN_LANES(gS, 12u), joints + 32u, laS, nAS, nf);
    ean_anim_user(ls, EAN_LANES(bU, nf), &fr, kU - 4u, EAN_LANES(fU, 4u), EAN_LANES(gU, 4u), user, laU, nAU, nf);
#undef EAN_LANES
#undef EAN_AL4
}

/* ===========================================================================
 * The job (SPU main 0x30, body 0x578, init 0x4E90, blend tree 0x6720/0x4CA8,
 * list 0x59C8, pose stack 0x5158/0x57E0/0x55F8, blend dispatch 0x5278).
 * ========================================================================= */
/* Why a job is left to the SPU (also counted by spurs_job.c). */
enum {
    EAN_OK = 0,
    EAN_R_LAYOUT,    /* descriptor/staging the port does not mirror (io/cache counts, sizes) */
    EAN_R_ASSERT,    /* the SPU would stop: an Edge assert (alignment, sizes, scratch, tags) */
    EAN_R_TREE,      /* blend tree index out of range / bad flags / list overflow, op > 7 */
    EAN_R_ANIM,      /* not an EA01 animation, or a frame set the port does not take */
    EAN_R_SPILL,     /* pose stack deeper than local store: external pose buffer traffic */
    EAN_R_POST,      /* post-process pass (input 2 + user word 0x38): not ported */
    EAN_R_STACK,     /* SPU stack too small for the tree: it would overrun the scratch */
    EAN_R_MEM,       /* a guest address the port cannot read */
    EAN_R_COUNT
};
static const char* const ean_reason_name[EAN_R_COUNT] = {
    "ok", "layout", "assert", "tree", "anim", "spill", "post", "stack", "mem"
};
#define EAN_MAX_PUTS 16
typedef struct { uint32_t ea, size, cmd, lsa; } ean_put;
typedef struct {
    /* in */
    const uint8_t* desc; uint32_t dsz, job_ea, spu_num;
    int image;                   /* 10006 (default, 0) or 10002: the smaller edgeAnim build (see below) */
    const uint8_t* const* seg;   /* staged copies of the input DMA elements (snapshot), or NULL entries */
    uint32_t nseg;
    /* read guest memory (GET, staging); 0 on success */
    int (*get)(void* u, uint32_t ea, uint8_t* dst, uint32_t size);
    void* u;
    uint8_t* ls;                 /* 256 KB work area (the job's local store) */
    /* out: the job's PUTs in order (data = ls + lsa, valid until the next run) */
    ean_put put[EAN_MAX_PUTS]; uint32_t nput;
    uint32_t gets, rejected_gets, leaves, branches, njoints;
    uint32_t opmask;     /* blend ops used (bit op) */
    uint32_t chanmask;   /* channel kinds evaluated: bits 0-7 const/anim rot trans scale user, 8 joint weights, 9 user channels */
} ean_job;

/* the pose stack context (the SPU keeps it at sp+128) */
typedef struct {
    uint32_t spu, buf[3], bufsz, base, max, ext_ea, ext_n, entry, jsize, usize, nj4, depth, top;
} ean_ctx;
typedef struct { uint32_t joints, w1, user, w3, hdr; } ean_pose;

static inline int ean_mfc_get(ean_job* J, uint32_t lsa, uint32_t ea, uint32_t size)
{
    lsa &= EAN_LS_MASK;
    /* rejected exactly as runtime/spu/spu_dma.h: no data moves */
    if (size == 0 || size > 0x4000u || (size >= 16u && (size & 15u)) || ((lsa ^ ea) & 15u) || lsa + size > EAN_LS_SIZE) {
        J->rejected_gets++;
        return 0;
    }
    J->gets++;
    return J->get(J->u, ea, J->ls + lsa, size) ? -1 : 0;
}
static inline int ean_mfc_put(ean_job* J, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t cmd)
{
    lsa &= EAN_LS_MASK;
    if (size == 0 || size > 0x4000u || (size >= 16u && (size & 15u)) || ((lsa ^ ea) & 15u) || ea < 0x10000u || lsa + size > EAN_LS_SIZE)
        return -1;   /* the SPU's PUT would be dropped: leave such a job to the SPU */
    if (J->nput >= EAN_MAX_PUTS) return -1;
    ean_put* p = &J->put[J->nput++];
    p->ea = ea; p->size = size; p->cmd = cmd; p->lsa = lsa;
    return 0;
}
static inline ean_pose ean_get_pose(const uint8_t* ls, const ean_ctx* c, uint32_t idx)
{
    const uint32_t slot = idx > c->top ? c->top + c->max - idx : c->top - idx;
    ean_pose p;
    p.joints = c->base + slot * c->entry;
    p.user = p.joints + c->jsize;
    p.hdr = p.user + c->usize;
    p.w1 = p.hdr + 16u;
    p.w3 = p.w1 + c->nj4;
    (void)ls;
    return p;
}
/* push/pop; a depth that reaches the local-store capacity would move poses
 * to the external buffer: refused before the run (EAN_R_SPILL) */
static inline void ean_push(ean_ctx* c) { c->top = c->top == c->max - 1u ? 0 : c->top + 1u; c->depth++; }
static inline void ean_pop(ean_ctx* c) { c->top = c->top ? c->top - 1u : c->max - 1u; c->depth--; }

/* 0x4BE0: n quadwords */
static inline void ean_copy_quads(uint8_t* ls, uint32_t dst, uint32_t src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) ean_sq(ls, dst + 16u * i, ean_lq(ls, src + 16u * i));
}
/* 0x420: a float in (0.99999, 1.00001) (compared in double) becomes 1.0 */
static inline void ean_snap_one(uint8_t* ls, uint32_t a)
{
    const uint32_t w = ean_lw_rot(ls, a), be = (w >> 23) & 0xFFu;
    if (be == 0 || be == 255) return;
    uint64_t u = (uint64_t)(w >> 31) << 63 | (uint64_t)(be - 127u + 1023u) << 52 | (uint64_t)(w & 0x7FFFFFu) << 29;
    double d, lo, hi;
    const uint64_t ulo = 0x3FEFFFEB074A771Dull, uhi = 0x3FF0000A7C5AC472ull;
    memcpy(&d, &u, 8); memcpy(&lo, &ulo, 8); memcpy(&hi, &uhi, 8);
    if (d > lo && d < hi) ean_sw(ls, a, EAN_F_ONE);
}

/* blend tree flatten (0x4CA8): post-order {type, LS address}; 1 leaf, 2 branch */
typedef struct { uint32_t type, ptr; } ean_item;
static int ean_flatten(const uint8_t* ls, ean_item* it, uint32_t* cur, uint32_t end, uint32_t idx,
                       uint32_t branches, uint32_t nb, uint32_t leaves, uint32_t nl, int depth)
{
    if (!(end > *cur)) return 0;
    if (depth > 64) return -1;
    const uint32_t bi = idx & 0xFFFFBFFFu;
    if (bi >= nb) return -1;
    const uint32_t br = branches + 16u * bi;
    for (int side = 0; side < 2; side++) {
        const uint32_t ch = ean_lh(ls, br + 2u + 2u * (uint32_t)side);
        if (ch & 0x4000u) {
            if (ean_flatten(ls, it, cur, end, ch & 0xFFFFBFFFu, branches, nb, leaves, nl, depth + 1)) return -1;
        } else if (ch & 0x8000u) {
            if ((ch & 0x7FFFu) >= nl) return -1;
            it[*cur].type = 1; it[*cur].ptr = leaves + 16u * (ch & 0x7FFFu); (*cur)++;
        } else return -1;
        if (!(end > *cur)) return 0;
    }
    it[*cur].type = 2; it[*cur].ptr = br; (*cur)++;
    return 0;
}

/* 0x5278: blend pose idx b and c into pose idx out (always 1) */
static inline void ean_blend_poses(uint8_t* ls, const ean_ctx* c, uint32_t b, uint32_t cc, uint32_t op,
                                   uint32_t factor, uint32_t skel)
{
    const ean_pose A = ean_get_pose(ls, c, 1), P = ean_get_pose(ls, c, b), Q = ean_get_pose(ls, c, cc);
    const uint32_t tabo = ean_lw(ls, skel + 0x28), tab = tabo ? skel + 40u + tabo : 0;
    const uint32_t nj = ean_lh(ls, skel + 12), nu = ean_lh(ls, skel + 14);
    const int fb = ean_lw(ls, P.hdr) & 1u, fc = ean_lw(ls, Q.hdr) & 1u;
    if (op == 0 || op > 7) {
        ean_blend_linear(ls, A.joints, A.w1, P.joints, fb ? P.w1 : 0, Q.joints, fc ? Q.w1 : 0, factor, nj);
        if (nu) ean_blend_user_linear(ls, A.user, A.w3, P.user, fb ? P.w3 : 0, Q.user, fc ? Q.w3 : 0, tab, factor, nu);
    } else {
        static const uint8_t fl[8] = { 0, 0, 0, 1, 1, 16, 17, 17 };
        ean_blend_op(ls, A.joints, A.w1, P.joints, fb ? P.w1 : 0, Q.joints, fc ? Q.w1 : 0, factor, fl[op], nj);
        if (nu) ean_blend_user_op(ls, A.user, A.w3, P.user, fb ? P.w3 : 0, Q.user, fc ? Q.w3 : 0, tab, factor, fl[op], nu);
    }
    ean_sw(ls, A.hdr, ean_lw(ls, A.hdr) | 1u);
}

/* The SPU runs the list as a three-stage pipeline, one entry per stage per
 * pass: A (header GET) for entry k+2, B (frame set pick, block GET) for
 * k+1, C (evaluate, or blend) for k, each leaf in the work buffer
 * (2 + k) % 3. A padding lane of the evaluation may read past its block into
 * the other buffers, so the order of the GETs is kept, not just their data. */
typedef struct { uint32_t hdr, blk, start, nfr; } ean_leafinfo;
static inline int ean_leaf_a(ean_job* J, const ean_ctx* c, uint32_t leaf, uint32_t buf)
{
    uint8_t* ls = J->ls;
    const uint32_t aea = ean_lw(ls, leaf), hsz = ean_lw(ls, leaf + 4u);
    if (hsz > c->bufsz) return EAN_R_ASSERT;
    const uint32_t hdr = buf + (aea & 127u);
    if ((hdr & 15u) || (aea & 15u) || (hsz & 15u) || hsz > 16384u) return EAN_R_ASSERT;
    return ean_mfc_get(J, hdr, aea, hsz) ? EAN_R_MEM : 0;
}
static inline int ean_leaf_b(ean_job* J, const ean_ctx* c, uint32_t leaf, uint32_t buf, ean_leafinfo* li)
{
    uint8_t* ls = J->ls;
    const uint32_t aea = ean_lw(ls, leaf), hdr = buf + (aea & 127u);
    if (ean_lw(ls, hdr) != 0x45413031u) return EAN_R_ANIM;   /* "EA01"; the SPU prints and stops */
    const uint32_t x = ean_fm(ean_lw(ls, leaf + 8u), ean_lw(ls, hdr + 8u));
    uint32_t frame = ean_cfltu(x, 173);
    if (ean_fgt(0, x)) frame = 0;
    frame &= 0xFFFFu;
    const uint32_t nsets = ean_lh(ls, hdr + 0x12);
    const uint32_t to = ean_lw(ls, hdr + 0x38), T = to ? hdr + 0x38u + to : 0;
    uint32_t best = 0, lo = 1, hi = nsets - 1u;
    while (hi > lo) {
        const uint32_t mid = (best + hi) >> 1;
        if (ean_lh(ls, T + 4u * mid) > frame) hi = mid;
        else { lo = mid + 1u; best = mid; }
    }
    li->hdr = hdr;
    li->start = ean_lh(ls, T + 4u * best);
    li->nfr = ean_lh(ls, T + 4u * best + 2u);
    const uint32_t dofs = ean_lw(ls, hdr + 0x34), D = dofs ? hdr + 0x34u + dofs : 0;
    const uint32_t bsize = ean_lw(ls, D + 8u * best) & 0x7FFFu, bea = ean_lw(ls, D + 8u * best + 4u) + aea;
    li->blk = D + ((bea - D) & 127u);
    if ((uint64_t)bsize + (uint32_t)(li->blk - buf) > c->bufsz) return EAN_R_ASSERT;
    if ((li->blk & 15u) || (bea & 15u) || (bsize & 15u) || bsize > 16384u) return EAN_R_ASSERT;
    if (li->nfr > 128u) return EAN_R_ANIM;   /* bit windows are 128 frames */
    return ean_mfc_get(J, li->blk, bea, bsize) ? EAN_R_MEM : 0;
}
static inline int ean_leaf_c(ean_job* J, ean_ctx* c, uint32_t leaf, const ean_leafinfo* li, uint32_t skel, int callback)
{
    uint8_t* ls = J->ls;
    const uint32_t aea = ean_lw(ls, leaf), hdr = li->hdr;
    ean_push(c);
    const ean_pose P = ean_get_pose(ls, c, 0);
    for (uint32_t k = 0; k < 8; k++) if (ean_lh(ls, hdr + 0x16 + 2u * k)) J->chanmask |= 1u << k;
    const uint32_t wsz = ean_lw(ls, hdr + 0x28);
    if (wsz) J->chanmask |= 1u << 8;
    if (wsz) {
        ean_sw(ls, P.hdr, ean_lw(ls, P.hdr) | 1u);
        const uint32_t w2c = ean_lw(ls, hdr + 0x2C), wea = w2c ? w2c : ean_lw(ls, hdr + 0x30) + aea;
        if ((wea & 15u) || (P.w1 & 15u) || wsz > 16384u || (wsz & 15u)) return EAN_R_ASSERT;
        if (ean_mfc_get(J, P.w1, wea, wsz)) return EAN_R_MEM;
    } else ean_sw(ls, P.hdr, ean_lw(ls, P.hdr) & ~1u);
    const uint32_t nj = ean_lh(ls, skel + 12), nu = ean_lh(ls, skel + 14);
    const uint32_t bpo = ean_lw(ls, skel + 0x14);
    ean_copy_quads(ls, P.joints, bpo ? skel + 20u + bpo : 0, (nj * 48u) >> 4);
    for (uint32_t k = 0; k < nu; k++) ean_sw(ls, P.user + 4u * k, 0);
    const uint32_t xs = ean_fm(ean_lw(ls, leaf + 8u), ean_lw(ls, hdr + 8u));
    const uint32_t x0 = ean_fgt(0, xs) ? 0 : xs;
    const uint32_t xr = ean_fs(x0, ean_cuflt(li->start, 155));
    uint32_t f = ean_cfltu(xr, 173), frac;
    if (f > li->nfr) { f = li->nfr; frac = EAN_F_ONE; }
    else frac = ean_fs(xr, ean_cuflt(f, 155));
    { EAN_P0(); ean_evaluate(ls, P.joints, P.user, hdr, li->blk, li->nfr, f, frac); EAN_P1(0); }
    if (callback && (ean_lw(ls, leaf + 12u) & 1u))
        for (uint32_t k = 0; k < nj; k++) ls[(P.w1 + k) & EAN_LS_MASK] = 0xFF;
    J->leaves++;
    return 0;
}

/* SPU stack bytes the job needs (frames of 0x30, 0x578, 0x6720 + its list,
 * 0x59C8, 0x8FD8 and the deepest kernel's save area), with margin */
static inline uint32_t ean_stack_need(uint32_t nb, uint32_t nl)
{
    return 304u + 384u + 160u + ((8u * (nb + nl + 2u) + 30u) & ~15u) + 384u + 800u + 0x300u + 512u;
}

/* Run the job natively. 0: done (J->put lists the PUTs: the joint matrix /
 * user channel block, then the 16-byte completion flag PUTF); else a reason
 * (the SPU must run it). The whole local store is rebuilt the way
 * spu_run_spurs_job stages it (zeroed, binary at 0, inputs, context). */
static int ean_run_job(ean_job* J)
{
    uint8_t* ls = J->ls;
    const uint8_t* D = J->desc;
    J->nput = J->gets = J->rejected_gets = J->leaves = J->branches = J->njoints = J->opmask = J->chanmask = 0;
    if (J->dsz < 0x30) return EAN_R_LAYOUT;
    const uint32_t ea_bin = ean_be32(D + 4) & ~1u, size_bin = ean_be16(D + 0x08) << 4, n_dma = ean_be16(D + 0x0A) / 8u;
    const uint32_t use_io = ean_be32(D + 0x10), size_io = ean_be32(D + 0x14), size_out = ean_be32(D + 0x18);
    uint32_t size_stk = (uint32_t)ean_be16(D + 0x1C) << 4;
    const uint32_t size_scr = (uint32_t)ean_be16(D + 0x1E) << 4;
    uint32_t n_cache = ean_be32(D + 0x24) / 8u;
    if (n_cache > 4) n_cache = 4;
    if (!size_stk) size_stk = 8192;
    if (!size_bin || size_bin > EAN_LS_SIZE || ean_be32(D) != 0) return EAN_R_LAYOUT;
    if (J->dsz < 0x30u + 8u * (n_dma + n_cache) + 64u) return EAN_R_LAYOUT;
    EAN_P0();
    memset(ls, 0, EAN_LS_SIZE);
    if (J->get(J->u, ea_bin, ls, size_bin)) return EAN_R_MEM;
#define EAN_AL1K(x) (((x) + 1023u) & ~1023u)
    uint32_t p = EAN_AL1K(size_bin);
    const uint32_t io_ls = p; p += EAN_AL1K(size_io);
    uint32_t out_ls = p; p += EAN_AL1K(size_out);
    if (use_io && !out_ls) out_ls = io_ls;
    const uint32_t ss = EAN_AL1K(size_scr + size_stk), s_ls = p;
    p += ss;
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        const uint8_t* e = D + 0x30 + n_dma * 8u + i * 8u;
        const uint32_t sz = ean_be32(e), eal = ean_be32(e + 4);
        if (!sz || !eal) continue;
        if (p + sz > EAN_LS_SIZE) break;
        cache_ls[i] = p;
        if (J->get(J->u, eal, ls + p, sz)) return EAN_R_MEM;
        p += EAN_AL1K(sz);
    }
    uint32_t io_cur = io_ls;
    for (uint32_t i = 0; i < n_dma; i++) {
        const uint8_t* e = D + 0x30 + i * 8u;
        const uint32_t sz = ean_be32(e) & 0x7FFFu, eal = ean_be32(e + 4);
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > EAN_LS_SIZE) break;
        if (J->seg && i < J->nseg && J->seg[i]) memcpy(ls + io_cur, J->seg[i], sz);
        else if (J->get(J->u, eal, ls + io_cur, sz)) return EAN_R_MEM;
        io_cur += (sz + 15u) & ~15u;
    }
#undef EAN_AL1K
    EAN_P1(4);
    const uint32_t desc_ls = EAN_LS_SIZE - 0x400u, ctx_ls = desc_ls - 0x40u;
    uint32_t dsz = J->dsz > 0x400u ? 0x400u : J->dsz;
    memcpy(ls + desc_ls, D, dsz);
    ean_sw(ls, ctx_ls + 0x00, io_ls);
    for (uint32_t i = 0; i < 4; i++) ean_sw(ls, ctx_ls + 0x04 + 4u * i, cache_ls[i]);
    {   /* the current-SPU helper (lqa r3,0x1c0; rotqbyi r3,r3,8; bi r0) reads LS 0x1C8 */
        static const uint8_t svc[12] = { 0x30,0x80,0x38,0x03, 0x3F,0x82,0x01,0x83, 0x35,0x00,0x00,0x00 };
        for (uint32_t o = 0; o + 12 <= size_bin; o += 4)
            if (!memcmp(ls + o, svc, 12)) { ean_sw(ls, 0x1C8, J->spu_num); break; }
    }
    ean_sw(ls, ctx_ls + 0x14, (dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28);
    ean_sh(ls, ctx_ls + 0x18, n_dma);
    ean_sh(ls, ctx_ls + 0x1A, n_cache);
    ean_sw(ls, ctx_ls + 0x1C, out_ls);
    ean_sw(ls, ctx_ls + 0x20, s_ls);
    ean_sw(ls, ctx_ls + 0x24, 20u);
    ean_sw(ls, ctx_ls + 0x2C, J->job_ea);

    /* ---- main (0x30) */
    const uint32_t numio = ean_lh(ls, ctx_ls + 0x18), numc = ean_lh(ls, ctx_ls + 0x1A);
    if (numio > 4 || numc > 2) return EAN_R_ASSERT;
    if (numio < 2 || numc < 2) return EAN_R_LAYOUT;   /* the body reads pointers 0/1 and both cache buffers */
    uint32_t ptr[4] = { 0, 0, 0, 0 };
    {
        const uint32_t n = ean_lh(ls, desc_ls + 0x0A) >> 3;
        uint32_t cur = io_ls;
        for (uint32_t i = 0; i < n && i < 4; i++) {
            const uint32_t sz = ean_lw(ls, desc_ls + 0x30 + 8u * i) & 0x7FFFu, ea = ean_lw(ls, desc_ls + 0x34 + 8u * i);
            ptr[i] = cur | (ea & 15u);
            cur += (sz + 15u) & ~15u;
        }
        if (n < numio) return EAN_R_LAYOUT;
    }
    if (numio <= 2) ptr[2] = 0;
    const uint32_t cb0 = cache_ls[0], skel = cache_ls[1], obuf = out_ls, sbuf = s_ls;
    /* 10002: user data right after the lists, no completion flag, no post-process,
     * no leaf callback, no near-1 snap; the rest is the same library */
    const int v2 = J->image == 10002;
    const uint32_t ud = desc_ls + 0x30u + 8u * (v2 ? numio + numc : (numio + numc + 1u) & ~1u);
    const uint32_t cnt = ean_lw(ls, ud + 48), out_ea = ean_lw(ls, ud + 52), ud56 = ean_lw(ls, ud + 56), flag_ea = ean_lw(ls, ud + 60);
    const uint32_t spu = ean_lw(ls, 0x1C8), scr = (uint32_t)ean_lh(ls, desc_ls + 0x1E) << 4;
    if (!cb0 || !skel) return EAN_R_LAYOUT;
    const uint32_t nb = cnt >> 16, nl = cnt & 0xFFFFu;
    if (size_stk < ean_stack_need(nb, nl)) return EAN_R_STACK;

    /* ---- init (0x4E90) */
    ean_ctx c;
    const uint32_t nj = ean_lh(ls, skel + 12), nu = ean_lh(ls, skel + 14);
    J->njoints = nj;
    if (sbuf & 15u) return EAN_R_ASSERT;
    if (scr < 3u * 8448u) return EAN_R_ASSERT;
    c.spu = spu; c.bufsz = 8448u;
    for (uint32_t i = 0; i < 3; i++) c.buf[i] = sbuf + 8448u * i;
    c.jsize = 48u * ((nj + 4u) & ~3u);
    c.usize = 4u * ((nu + 4u) & ~3u);
    c.nj4 = (nj + 3u) & ~3u;
    c.entry = c.jsize + c.usize + ((((nj + 3u) & ~3u) + ((nu + 3u) & ~3u) + 15u) & ~15u) + 16u;
    c.base = sbuf + 3u * 8448u;
    c.max = (scr - 3u * 8448u) / c.entry;
    if (c.max <= 2) return EAN_R_ASSERT;
    c.ext_ea = c.ext_n = 0;
    if (spu <= 5) { c.ext_ea = ean_lw(ls, cb0 + 8u * spu + 4u); c.ext_n = ean_lw(ls, cb0 + 8u * spu) / c.entry; }
    c.depth = 0; c.top = c.max - 1u;

    /* ---- blend tree (0x6720): post-order list, then the leaf pipeline (0x59C8) */
    const int callback = nb != 0 && !v2;
    const uint32_t branches = ptr[0], leaves = ptr[1];
    enum { EAN_MAX_ITEMS = 4096 };   /* the SPU keeps this list on its stack */
    static EAN_TLS ean_item items[EAN_MAX_ITEMS];
    const uint32_t end = nb + nl + 2u;
    if (end > EAN_MAX_ITEMS) return EAN_R_STACK;
    uint32_t ni = 0;
    if (nb) {
        /* the root is the last branch; 10002 roots the tree at branch 0 */
        if (ean_flatten(ls, items, &ni, end, v2 ? 0x4000u : (nb - 1u) | 0x4000u, branches, nb, leaves, nl, 0)) return EAN_R_TREE;
    } else {
        if (nl == 0) return EAN_R_TREE;   /* the SPU asserts root leaf 0 < leaves */
        items[0].type = 1; items[0].ptr = leaves; ni = 1;
    }
    if (!(end > ni)) return EAN_R_TREE;   /* the SPU asserts room for the end marker */
    /* pose stack depth: a leaf pushes, a branch pops, one more push at the end;
     * a depth that would move poses to/from the external buffer goes to the SPU */
    {
        uint32_t d = 0;
        for (uint32_t k = 0; k < ni; k++) {
            if (items[k].type == 1) {
                if (!(c.max + c.ext_n > d)) return EAN_R_ASSERT;
                d++;
                if (d >= c.max && c.ext_ea && c.ext_n > d - c.max) return EAN_R_SPILL;
            } else {
                if (d == 0) return EAN_R_ASSERT;
                if (d > c.max && c.ext_ea && c.ext_n > d - c.max - 1u) return EAN_R_SPILL;
                d--;
            }
        }
        if (!(c.max + c.ext_n > d)) return EAN_R_ASSERT;
        d++;
        if (d >= c.max && c.ext_ea && c.ext_n > d - c.max) return EAN_R_SPILL;
    }
    ean_leafinfo li[3];
    for (uint32_t i = 0; i < ni + 2u; i++) {
        int r;
        if (i < ni && items[i].type == 1 && (r = ean_leaf_a(J, &c, items[i].ptr, c.buf[(2u + i) % 3u]))) return r;
        if (i >= 1 && i - 1u < ni && items[i - 1u].type == 1) {
            const uint32_t k = i - 1u;
            if ((r = ean_leaf_b(J, &c, items[k].ptr, c.buf[(2u + k) % 3u], &li[(2u + k) % 3u]))) return r;
        }
        if (i >= 2) {
            const uint32_t k = i - 2u;
            if (items[k].type == 1) {
                if ((r = ean_leaf_c(J, &c, items[k].ptr, &li[(2u + k) % 3u], skel, callback))) return r;
            } else {
                const uint32_t br = items[k].ptr, op = ean_lh(ls, br);
                if (op > 7u) return EAN_R_TREE;
                const int swap = op == 1u || op == 3u || op == 6u;
                { EAN_P0(); ean_blend_poses(ls, &c, swap ? 0u : 1u, swap ? 1u : 0u, op, ean_lw(ls, br + 8u), skel); EAN_P1(1); }
                ean_pop(&c);
                J->branches++;
                J->opmask |= 1u << op;
            }
        }
    }
    /* ---- post-process (0x2AA0): input 2 and user word 0x38 both set */
    if (!v2 && ptr[2] && ud56) return EAN_R_POST;
    /* ---- world joints, user channels, output */
    ean_push(&c);
    const ean_pose L = ean_get_pose(ls, &c, 1), W = ean_get_pose(ls, &c, 0);
    const uint32_t nj4 = (nj + 3u) & ~3u, nu4 = (nu + 3u) & ~3u;
    if (nu) J->chanmask |= 1u << 9;
    if (nu) memcpy(ls + obuf + nj4 * 64u, ls + L.user, 4u * nu);
    for (uint32_t j = 0; j < nj && !v2; j++) {
        const uint32_t a = L.joints + 48u * j;
        ean_snap_one(ls, a + 12u);
        ean_snap_one(ls, a + 32u); ean_snap_one(ls, a + 36u); ean_snap_one(ls, a + 40u); ean_snap_one(ls, a + 44u);
    }
    { EAN_P0(); ean_local_to_world(ls, W.joints, L.joints, ud, skel + 48u, 4u * ean_lh(ls, skel + 0x10)); EAN_P1(2); }
    { EAN_P0(); ean_joints_to_matrices(ls, obuf, W.joints, nj); EAN_P1(3); }
    ean_pop(&c); ean_pop(&c);
    const uint32_t osize = (16u * nj4 + nu4) * 4u;
    if ((out_ea & 15u) || (obuf & 15u)) return EAN_R_ASSERT;
    for (uint32_t lsa = obuf, ea = out_ea, left = osize; left;) {
        const uint32_t ch = left > 16383u ? 16384u : left > 15u ? left & ~15u : left > 7u ? 8u : left > 3u ? 4u : left > 1u ? 2u : 1u;
        if (ean_mfc_put(J, lsa, ea, ch, 0x20u)) return EAN_R_ASSERT;
        lsa += ch; ea += ch; left -= ch;
    }
    if (v2) return EAN_OK;
    ls[(obuf + osize) & EAN_LS_MASK] = 1;
    if ((flag_ea & 15u) || ((obuf + osize) & 15u)) return EAN_R_ASSERT;
    if (ean_mfc_put(J, obuf + osize, flag_ea, 16u, 0x22u)) return EAN_R_ASSERT;
    return EAN_OK;
}

#endif /* EDGE_ANIM_H */
