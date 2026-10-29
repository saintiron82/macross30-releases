/* jobcrt_ribbon.h -- SPURS job image 10009 of Macross 30 ("JOBCRT Ver14",
 * jobcrt14_c, binary 0x009E0E80, 9104 bytes) as host code: the effect ribbon /
 * trail vertex generator (beam and smoke tails).
 *
 * Read from the job binary (native/macross30/raw_jobs/job_039ea2388c0b6fcf.bin,
 * disassembly native/analysis/ribbon-ra/jobcrt14c.s) and checked against the
 * lifted job (native/macross30/spu_gameplay/jobcrt14_c/spu_recomp.c) by
 * tools/tests/test_ribbon_native.py. What the job reads and writes, in order:
 * native/analysis/jobcrt10009-spec.md.
 *
 * Arithmetic is the SPU's: every float operation goes through spu_helpers.h
 * (truncation, no denormals, saturation, the frest/frsqest tables and fi, the
 * double-precision fesd/dfs/dfm/frds of the alpha term) in the job's order,
 * so the PUT bytes are the ones the SPU makes.
 *
 * Header-only. Part 1 (always): the port, rbn_run_job(). Part 2 (only when
 * the includer defines RBN_SPURS_GLUE, i.e. runtime/spu/spurs_job.c): the
 * shadow / trust / capture glue, which uses spurs_job.c's own helpers. The
 * offline test (tools/tests/ribbon_native) includes part 1 only. */
#ifndef JOBCRT_RIBBON_H
#define JOBCRT_RIBBON_H
#include <stdint.h>
#include <string.h>
#include "spu_helpers.h"

/* ---- SPU single precision, one lane (the vector helpers on one lane) ---- */
static inline uint32_t rbn_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t rbn_be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void rbn_wbe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint32_t rbn_fa(uint32_t a, uint32_t b) { return spu__fa_any(a, b); }
static inline uint32_t rbn_fs(uint32_t a, uint32_t b) { return spu__fa_any(a, b ^ 0x80000000u); }
static inline uint32_t rbn_fm(uint32_t a, uint32_t b) { return spu__fm_any(a, b); }
static inline uint32_t rbn_fma_any(uint32_t x, uint32_t y, uint32_t z, uint32_t np, uint32_t nc)
{
    /* spu__fma_any's fast path; fma(1.0, p, z) with an exact product p is one rounded addition */
    if (spu__sf_plain(x) && spu__sf_plain(y) && spu__sf_plain(z)) {
        const double p = (double)spu__sf_f(x ^ np) * (double)spu__sf_f(y);
        const uint32_t f = spu__sf_from_d(p + (double)spu__sf_f(z ^ nc), 1);
        if (f) return f;
    }
    return spu__fma_lane(x, y, z, np, nc);
}
static inline uint32_t rbn_fma(uint32_t a, uint32_t b, uint32_t c) { return rbn_fma_any(a, b, c, 0, 0); }             /* a*b + c */
static inline uint32_t rbn_fnms(uint32_t a, uint32_t b, uint32_t c) { return rbn_fma_any(a, b, c, 0x80000000u, 0); }  /* c - a*b */
static inline uint32_t rbn_frest(uint32_t a)
{
    const uint32_t exp = (a >> 23) & 0xFFu, rexp = exp == 0u ? 255u : exp >= 253u ? 0u : 253u - exp;
    return (a & 0x80000000u) | (rexp << 23) | (spu_frest_fraction_lut[(a >> 18) & 0x1Fu] & 0x7FFFFFu);
}
static inline uint32_t rbn_frsqest(uint32_t a)
{
    const uint32_t exp = (a >> 23) & 0xFFu, rexp = exp == 0u ? 0xFFu : 190u - (exp + 1u) / 2u;
    return (rexp << 23) | (spu_frsqest_fraction_lut[(a >> 18) & 0x3Fu] & 0x7FFFFFu);
}
static inline uint32_t rbn_fi(uint32_t a, uint32_t est)
{
    int exp = (int)((est >> 23) & 0xFFu);
    if (exp == 0) return 0;
    uint64_t m = ((((uint64_t)1 << 13) | ((est >> 10) & 0x1FFFu)) << 19) - (uint64_t)(est & 0x3FFu) * (a & 0x7FFFFu);
    if (m < ((uint64_t)1 << 32)) { m <<= 1; if (--exp == 0) return 0; }
    return (est & 0x80000000u) | ((uint32_t)exp << 23) | (uint32_t)((m >> 9) & 0x7FFFFFu);
}
static inline uint32_t rbn_cuflt(uint32_t a) { return a ? spu__sf_pack(0, a, 0) : 0; }                      /* cuflt i8=155 */
static inline uint32_t rbn_csflt(uint32_t a)                                                                 /* csflt i8=155 */
{
    const int32_t v = (int32_t)a;
    return v ? spu__sf_pack(v < 0 ? 0x80000000u : 0, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 0) : 0;
}
static inline uint32_t rbn_cfltu(uint32_t a) { return spu__cfl_lane(a, 0, 1); }                              /* cfltu i8=173 */
static inline int rbn_fgt(uint32_t a, uint32_t b) { return spu__sf_key(a) > spu__sf_key(b); }                /* fcgt */
static inline int rbn_feq(uint32_t a, uint32_t b) { return spu__sf_key(a) == spu__sf_key(b); }               /* fceq */
/* 1/sqrt(x) as the job refines it (0x4F00 / 0xF34): fi(frsqest) and one Newton step */
static inline uint32_t rbn_rsqrt(uint32_t x)
{
    const uint32_t y = rbn_fi(x, rbn_frsqest(x));
    return rbn_fma(rbn_fnms(y, rbn_fm(y, x), 0x3F800001u), rbn_fm(y, 0x3F000000u), y);
}
/* 1/x (0x694): fi(frest) and one Newton step */
static inline uint32_t rbn_recip(uint32_t x)
{
    const uint32_t y = rbn_fi(x, rbn_frest(x));
    return rbn_fma(rbn_fnms(x, y, 0x3F800001u), y, y);
}

#define RBN_F_ZERO 0x00000000u
#define RBN_F_HALF 0x3F000000u
#define RBN_F_ONE  0x3F800000u
#define RBN_F_255  0x437F0000u
/* the job's constants (data at 0x1DB0..0x1EDC of the image) */
static const uint32_t rbn_corner[4][2] = {   /* 0x1DB0: quad corners (x, y); z = w = 0 */
    { 0xBF800000u, 0xBF800000u }, { 0x3F800000u, 0xBF800000u }, { 0x3F800000u, 0x3F800000u }, { 0xBF800000u, 0x3F800000u } };
static const uint32_t rbn_jitter[23] = {     /* 0x1E80: copied to the stack per vertex, indexed by seed % 19/17/23/22 */
    0x00000000u, 0x3F800000u, 0xBF000000u, 0xBF333333u, 0xBDCCCCCCu, 0x3F000000u, 0xBF800000u, 0x3F333333u,
    0x3E999999u, 0xBE999999u, 0x3E4CCCCCu, 0x00000000u, 0xBF333333u, 0xBE4CCCCCu, 0x3F800000u, 0xBF000000u,
    0x3F000000u, 0xBF800000u, 0x00000000u, 0x3F333333u, 0x3E999999u, 0xBE999999u, 0x3DCCCCCCu };
/* the alpha term's factor: the first 8 entries of the stack copy after the job
 * overwrote them (0xAA4..0xAF0), indexed by (5 * seed) & 7 */
static const uint32_t rbn_bright[8] = {
    0x00000000u, 0x3F800000u, 0x3E999999u, 0x3F000000u, 0x3E999999u, 0x3F800000u, 0x3F000000u, 0x3DCCCCCCu };

/* ---- the job -------------------------------------------------------------- */
enum {
    RBN_R_OK,
    RBN_R_LAYOUT,    /* descriptor / staging the port does not mirror (DMA list, sizes, unaligned input) */
    RBN_R_ASSERT,    /* the SPU job would halt: output or flag EA not 16-aligned */
    RBN_R_MEMCHECK,  /* descriptor asks for the CRT memory guard check (DEADBEEF guards): not ported */
    RBN_R_SIZE,      /* output does not fit below the job's stack in the local store (or the caller's buffer) */
    RBN_R_COUNT
};
static const char* const rbn_reason_name[RBN_R_COUNT] = { "ok", "layout", "assert", "memcheck", "size" };
#define RBN_MAX_PUTS 40
#define RBN_STACK_NEED 0x600u   /* deepest SPU stack use (frames 0x40 + 0xA0 + 0x370 + 0xA0) with margin */
typedef struct { uint32_t ea, size, cmd, off; } rbn_put;   /* off: byte offset into J->out */
typedef struct {
    /* in */
    const uint8_t* desc; uint32_t dsz;    /* the job descriptor as the stager reads it */
    const uint8_t* param; uint32_t psz;   /* DMA element 0 as staged: the 288-byte parameter block */
    uint8_t* out; uint32_t out_cap;       /* receives the oBuffer bytes the job PUTs: data, then the 16-byte flag quadword */
    /* out */
    uint32_t count, flags, lit;           /* vertices, P+0x108..0x10B as bits 3..0 (0x108 = bit 3), shading path taken */
    uint32_t size;                        /* data PUT size (16-aligned); the flag quadword is out[size..size+15] */
    uint32_t nput; rbn_put put[RBN_MAX_PUTS];
} rbn_job;

/* the shading factor of each quad corner (0x438..0x554): the defaults at P+0xF0,
 * or, when the ribbon segment has a direction that the 3x3 at P+0xB0 does not
 * map into the z = 0 plane, the light direction P+0xE0 against the corner's
 * direction in the plane across the segment. Then x*0.8 + 0.6 clamped to [0,1]. */
static inline void rbn_corner_shade(const uint8_t* P, uint32_t L[4], uint32_t* lit)
{
    uint32_t d[3], M[3][3], v[3];
    for (int m = 0; m < 3; m++) d[m] = rbn_fs(rbn_be32(P + 4 * m), rbn_be32(P + 0x40 + 4 * m));
    for (int r = 0; r < 3; r++) for (int m = 0; m < 3; m++) M[r][m] = rbn_be32(P + 0xB0 + 16 * r + 4 * m);
    const uint32_t len2 = rbn_fma(d[2], d[2], rbn_fma(d[1], d[1], rbn_fm(d[0], d[0])));
    for (int j = 0; j < 4; j++) L[j] = rbn_be32(P + 0xF0 + 4 * j);
    *lit = 0;
    if (rbn_fgt(len2, RBN_F_ZERO)) {
        for (int m = 0; m < 3; m++) v[m] = rbn_fma(M[2][m], d[2], rbn_fma(M[1][m], d[1], rbn_fm(M[0][m], d[0])));
        if (!rbn_feq(v[2], RBN_F_ZERO)) {
            *lit = 1;
            const uint32_t rs = rbn_rsqrt(rbn_fma(v[2], v[2], rbn_fma(v[1], v[1], rbn_fm(v[0], v[0]))));
            uint32_t n[3], E[3];
            for (int m = 0; m < 3; m++) { n[m] = rbn_fm(v[m], rs); E[m] = rbn_be32(P + 0xE0 + 4 * m); }
            for (int j = 0; j < 4; j++) {
                const uint32_t c[3] = { rbn_corner[j][0], rbn_corner[j][1], RBN_F_ZERO };
                const uint32_t dot = rbn_fma(n[2], c[2], rbn_fma(n[1], c[1], rbn_fm(c[0], n[0])));
                uint32_t p[3], u[3];
                for (int m = 0; m < 3; m++) p[m] = rbn_fnms(n[m], dot, c[m]);
                const uint32_t r = rbn_rsqrt(rbn_fma(p[2], p[2], rbn_fma(p[1], p[1], rbn_fm(p[0], p[0]))));
                for (int m = 0; m < 3; m++) u[m] = rbn_fm(p[m], r);
                L[j] = rbn_fma(E[2], u[2], rbn_fma(E[1], u[1], rbn_fm(u[0], E[0])));
            }
        }
    }
    for (int j = 0; j < 4; j++) {
        uint32_t x = rbn_fma(L[j], 0x3F4CCCCCu, 0x3F199999u);
        if (rbn_fgt(x, RBN_F_ONE)) x = RBN_F_ONE;
        if (rbn_fgt(RBN_F_ZERO, x)) x = RBN_F_ZERO;
        L[j] = x;
    }
}

/* the quad corner offsets (0x558..0x6EC): inverse of the 3x3 at P+0xB0 (rows
 * B0, C0, D0) applied to the corners (+-1, +-1, 0); lanes x, y, z */
static inline void rbn_corner_offsets(const uint8_t* P, uint32_t Q[4][3])
{
    uint32_t M[3][3];
    for (int r = 0; r < 3; r++) for (int m = 0; m < 3; m++) M[r][m] = rbn_be32(P + 0xB0 + 16 * r + 4 * m);
    uint32_t C01[3], C12[3], C20[3];
    for (int m = 0; m < 3; m++) {
        const int a = (m + 1) % 3, b = (m + 2) % 3;   /* .yzx / .zxy lanes */
        C01[m] = rbn_fnms(M[0][b], M[1][a], rbn_fm(M[0][a], M[1][b]));
        C12[m] = rbn_fnms(M[1][b], M[2][a], rbn_fm(M[1][a], M[2][b]));
        C20[m] = rbn_fnms(M[2][b], M[0][a], rbn_fm(M[2][a], M[0][b]));
    }
    const uint32_t det = rbn_fma(M[2][2], C01[2], rbn_fma(M[2][1], C01[1], rbn_fm(C01[0], M[2][0])));
    const uint32_t rd = rbn_recip(det);
    uint32_t I0[3], I1[3], I2[3];   /* rows: (C12.k, C20.k, C01.k) / det */
    I0[0] = rbn_fm(C12[0], rd); I0[1] = rbn_fm(C20[0], rd); I0[2] = rbn_fm(C01[0], rd);
    I1[0] = rbn_fm(C12[1], rd); I1[1] = rbn_fm(C20[1], rd); I1[2] = rbn_fm(C01[1], rd);
    I2[0] = rbn_fm(C12[2], rd); I2[1] = rbn_fm(C20[2], rd); I2[2] = rbn_fm(C01[2], rd);
    for (int j = 0; j < 4; j++)
        for (int m = 0; m < 3; m++)
            Q[j][m] = rbn_fma(I2[m], RBN_F_ZERO, rbn_fma(I1[m], rbn_corner[j][1], rbn_fm(I0[m], rbn_corner[j][0])));
}

/* unsigned float -> integer, low byte (cfltu then a byte insert) */
static inline uint8_t rbn_byte255(uint32_t x) { return (uint8_t)rbn_cfltu(rbn_fm(x, RBN_F_255)); }

/* The job (main 0xFE8 + body 0x330). 0: done, J->out / J->put hold what the
 * SPU would PUT; else a reason and the SPU must run the job. */
static int rbn_run_job(rbn_job* J)
{
    const uint8_t* D = J->desc;
    J->nput = 0; J->size = 0; J->count = J->flags = J->lit = 0;
    if (J->dsz < 0x38u) return RBN_R_LAYOUT;
    const uint32_t size_bin = rbn_be16(D + 0x08) << 4, n_dma = rbn_be16(D + 0x0A) / 8u;
    const uint32_t size_io = rbn_be32(D + 0x14), size_out = rbn_be32(D + 0x18);
    uint32_t size_stk = rbn_be16(D + 0x1C) << 4;
    const uint32_t size_scr = rbn_be16(D + 0x1E) << 4;
    if (!size_stk) size_stk = 8192u;   /* spu_run_spurs_job's JOB_STACK_DEFAULT */
    if (!size_bin || size_bin > 0x40000u || n_dma != 1u) return RBN_R_LAYOUT;
    const uint32_t sz = rbn_be32(D + 0x30) & 0x7FFFu, ea_in = rbn_be32(D + 0x34);
#define RBN_AL1K(x) (((uint64_t)(x) + 1023u) & ~(uint64_t)1023u)
    /* the block staged at the start of ioBuffer (entry 0 LS pointer = ioBuffer | (ea & 15)) */
    if (!ea_in || (ea_in & 15u) || sz < 0x120u || sz > RBN_AL1K(size_io) || !J->param || J->psz < 0x120u) return RBN_R_LAYOUT;
    if (D[0x2C] & 2u) return RBN_R_MEMCHECK;
    const uint64_t io_ls = RBN_AL1K(size_bin), out_ls = io_ls + RBN_AL1K(size_io), s_ls = out_ls + RBN_AL1K(size_out);
    const uint64_t stack_top = s_ls + RBN_AL1K((uint64_t)size_scr + size_stk);
#undef RBN_AL1K
    const uint8_t* P = J->param;
    const uint32_t count = rbn_be32(P + 0x100), seed = rbn_be32(P + 0x104);
    const int rev = P[0x108] != 0, quadcol = P[0x109] != 0, vcol = P[0x10A] != 0, point = P[0x10B] != 0;
    J->count = count;
    J->flags = (uint32_t)rev << 3 | (uint32_t)quadcol << 2 | (uint32_t)vcol << 1 | (uint32_t)point;
    /* output layout (0x5A8..0x6A0) and the PUT size (main 0x10D8 / 0x1238) */
    const uint64_t vbytes = (uint64_t)count * (point ? 16u : 48u);
    const uint64_t qbase = vbytes, cbase = qbase + (quadcol ? (uint64_t)count * 16u : 16u);
    const uint64_t total = cbase + (vcol ? (uint64_t)count * 4u : 4u), size = (total + 15u) & ~(uint64_t)15u;
    if (size + 16u > J->out_cap || out_ls + size + 16u + RBN_STACK_NEED > stack_top || stack_top > 0x3FB80u) return RBN_R_SIZE;
    const uint32_t ea_out = rbn_be32(P + 0x114), ea_flag = rbn_be32(P + 0x118);
    if ((ea_out & 15u) || (ea_flag & 15u)) return RBN_R_ASSERT;   /* main 0x1154 / 0x11E0: heqi */
    if ((uint64_t)ea_out + size > 0xFFFFFFFFull) return RBN_R_LAYOUT;
    uint8_t* const o = J->out;
    memset(o, 0, (size_t)size + 16u);

    uint32_t L[4], Q[4][3];
    rbn_corner_shade(P, L, &J->lit);
    rbn_corner_offsets(P, Q);

    if (count) {
        uint32_t a[4][4], b[4][4], ab[4][4], X[3][3];
        for (int r = 0; r < 4; r++)
            for (int m = 0; m < 4; m++) {
                a[r][m] = rbn_be32(P + 16 * r + 4 * m); b[r][m] = rbn_be32(P + 0x40 + 16 * r + 4 * m);
                ab[r][m] = rbn_fs(b[r][m], a[r][m]);
            }
        for (int r = 0; r < 3; r++) for (int m = 0; m < 3; m++) X[r][m] = rbn_be32(P + 0x80 + 16 * r + 4 * m);
        const uint32_t cf = rbn_cuflt(count), rc = rbn_fi(cf, rbn_frest(cf));
        const uint32_t s3 = seed - 3u * (seed / 3u), mul = 18u * s3 + 1u;
        uint32_t k = rev ? count - 1u : 0u;
        const uint32_t step = rev ? 0xFFFFFFFFu : 1u;
        uint8_t* pv = o;
        for (uint32_t i = 0; i < count; i++, k += step) {
            const uint32_t s = seed + mul * k;   /* 0x7A0..0x808, mod 2^32 */
            /* t = (k + 0.5) / count (0x98C..0x9F4) */
            const uint32_t kh = rbn_fa(rbn_csflt(k), RBN_F_HALF), q0 = rbn_fm(kh, rc);
            const uint32_t t = rbn_fma(rbn_fnms(q0, cf, kh), rc, q0);
            /* jitter (0x9C8..0x9F8) */
            const uint32_t jx = rbn_jitter[s % 19u], jy = rbn_jitter[s % 17u];
            const uint32_t jz = rbn_fm(rbn_jitter[s % 23u], 0x3DCCCCCCu), jw = rbn_fma(rbn_jitter[s % 22u], 0x3ECCCCCCu, RBN_F_ONE);
            /* the two keys interpolated (0x180): row 0 position, 1 and 2 colours, 3 (size, jitter scale) */
            uint32_t row[4][4];
            for (int m = 0; m < 4; m++) row[0][m] = rbn_fma(ab[0][m], t, a[0][m]);
            for (int r = 1; r < 3; r++) for (int m = 0; m < 4; m++) row[r][m] = rbn_fma(t, ab[r][m], a[r][m]);
            row[3][0] = rbn_fma(t, ab[3][0], a[3][0]); row[3][1] = rbn_fma(t, ab[3][1], a[3][1]);
            /* per-corner colours (0xA70..0xC2C): every vertex with P+0x109, else vertex 0 only */
            if (quadcol || i == 0) {
                uint8_t* q = o + qbase + 16u * (uint64_t)i;
                uint32_t dc[4];
                for (int m = 0; m < 4; m++) dc[m] = rbn_fs(row[2][m], row[1][m]);
                for (int j = 0; j < 4; j++) {
                    q[4 * j + 0] = rbn_byte255(rbn_fma(L[j], dc[0], row[1][0]));
                    q[4 * j + 1] = rbn_byte255(rbn_fma(L[j], dc[1], row[1][1]));
                    q[4 * j + 2] = rbn_byte255(rbn_fma(L[j], dc[2], row[1][2]));
                    q[4 * j + 3] = rbn_byte255(rbn_fma(dc[3], L[j], row[1][3]));
                }
                /* alpha = w * (1 - (1 - w) * B/2) in double precision, clamped, into all four corners */
                u128 w, h, one;
                memset(&w, 0, sizeof w); memset(&h, 0, sizeof h); memset(&one, 0, sizeof one);
                w._u32[0] = row[1][3];
                h._u32[0] = rbn_fm(rbn_bright[(5u * s) & 7u], RBN_F_HALF);
                one._u32[0] = 0x3FF00000u;   /* 1.0 in doubleword 0 (word 0 = high half) */
                const u128 wd = spu_fesd(w);
                const u128 al = spu_frds(spu_dfm(wd, spu_dfs(one, spu_dfm(spu_dfs(one, wd), spu_fesd(h)))));
                uint32_t av = al._u32[0];
                if (rbn_fgt(av, RBN_F_ONE)) av = RBN_F_ONE;
                if (rbn_fgt(RBN_F_ZERO, av)) av = RBN_F_ZERO;
                const uint8_t ab8 = rbn_byte255(av);
                q[15] = ab8; q[11] = ab8; q[7] = ab8; q[3] = ab8;
            }
            /* vertex colour (0xC50..0xCBC): every vertex with P+0x10A, else vertex 0 only */
            if (vcol || i == 0) {
                uint8_t* c = o + cbase + 4u * (uint64_t)i;
                for (int m = 0; m < 4; m++) c[m] = rbn_byte255(row[2][m]);
            }
            /* position (0xCC0..0xD0C) and size */
            uint32_t pos[3];
            for (int m = 0; m < 3; m++)
                pos[m] = rbn_fma(rbn_fma(X[2][m], jz, rbn_fma(X[1][m], jy, rbn_fm(X[0][m], jx))), row[3][1], row[0][m]);
            const uint32_t sz2 = rbn_fm(row[3][0], jw);
            if (point) {   /* 0xD18: x y z, 2*size */
                rbn_wbe32(pv, pos[0]); rbn_wbe32(pv + 4, pos[1]); rbn_wbe32(pv + 8, pos[2]); rbn_wbe32(pv + 12, rbn_fa(sz2, sz2));
                pv += 16;
            } else {       /* 0xD98: four corners, x y z each */
                for (int j = 0; j < 4; j++)
                    for (int m = 0; m < 3; m++) rbn_wbe32(pv + 12 * j + 4 * m, rbn_fma(Q[j][m], sz2, pos[m]));
                pv += 48;
            }
        }
    }
    /* main 0x1170..0x1204: the completion byte after the data, PUT in 16 KB pieces (0x19C8), then PUTF */
    o[size] = 1;
    J->size = (uint32_t)size;
    for (uint32_t off = 0; off < (uint32_t)size; ) {
        const uint32_t left = (uint32_t)size - off, n = left > 16383u ? 16384u : left;   /* multiples of 16 */
        if (J->nput >= RBN_MAX_PUTS - 1u) return RBN_R_SIZE;
        J->put[J->nput].ea = ea_out + off; J->put[J->nput].size = n; J->put[J->nput].cmd = 0x20u; J->put[J->nput].off = off;
        J->nput++; off += n;
    }
    J->put[J->nput].ea = ea_flag; J->put[J->nput].size = 16u; J->put[J->nput].cmd = 0x22u; J->put[J->nput].off = (uint32_t)size;
    J->nput++;
    return RBN_R_OK;
}

/* ===== part 2: SPURS glue (runtime/spu/spurs_job.c only) =====================
 *
 * Shadow first: the first RBN_TRUST_AFTER jobs of every key (the flag bytes,
 * the shading path, a multi-piece PUT, count 0) still run on the SPU; the
 * native PUTs are built aside and compared byte for byte with the PUTs the SPU
 * makes (spurs_job_cap_put -> edge_anim_rec_put, the recorder the Edge
 * Animation shadow uses; one job per thread at a time). RBN_TRUST_AFTER
 * matches take the key native, one mismatch keeps it on the SPU for the rest
 * of the run. Jobs the port does not take (rbn reasons) run on the SPU.
 * Off: RIBBON_NATIVE=0, or a no_ribbon_native file in the working directory
 * (checked each second). RIBBON_NATIVE_VERIFY=1: compare every job, never
 * native. SPURS_JOB_DIRECT_STAGE=1 (unzeroed local store) keeps it off.
 * Stats every 10 s: [ribbon-native] native= shadowed= compared= match=
 * mismatch= not-compared= | spu: <gates and reasons>; each key's first
 * mismatch is logged with the first differing offset.
 * Capture: a ribbon_cap file -> the next 500 jobs run on the SPU and are
 * written to ribboncap/10009_<n>.bin (RIBNCAP1: descriptor, staged block, the
 * SPU's PUTs; read by tools/tests/ribbon_native). */
#ifdef RBN_SPURS_GLUE
#define RBN_TRUST_AFTER 3
#define RBN_OUT_CAP ((512u << 10) + 64u)
enum { RBG_OFF, RBG_DIRECT, RBG_UNTRUSTED, RBG_TABLE, RBG_CAPTURE, RBG_MEM, RBG_COUNT };
static volatile LONG s_rb_native, s_rb_shadow, s_rb_compared, s_rb_match, s_rb_mismatch, s_rb_skipped, s_rb_last_diff = -1;
static volatile LONG s_rb_reason[RBN_R_COUNT], s_rb_gate[RBG_COUNT];
static ea_trust_slot s_rb_trust[256];
static ea_trust_slot* rb_trust(uint32_t key32)
{
    const LONG key = (LONG)(key32 | 0x80000000u);
    uint32_t h = ((uint32_t)key * 2654435761u) >> 24;
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & 255u) {
        if (s_rb_trust[h].key == key) return &s_rb_trust[h];
        if (s_rb_trust[h].key == 0 && InterlockedCompareExchange(&s_rb_trust[h].key, key, 0) == 0) return &s_rb_trust[h];
        if (s_rb_trust[h].key == key) return &s_rb_trust[h];
    }
    return NULL;
}
/* the job in flight on this thread: native result (shadow) and capture file */
typedef struct { int active; uint32_t key, job_ea, nput; rbn_put put[RBN_MAX_PUTS]; ea_trust_slot* slot; FILE* cap; uint8_t out[RBN_OUT_CAP]; } rb_state;
static __declspec(thread) rb_state* t_rbs;
static void ribbon_stats(void)
{
    static volatile ULONGLONG last;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    char t[512];
    int l = snprintf(t, sizeof t, "[ribbon-native] native=%ld shadowed=%ld compared=%ld match=%ld mismatch=%ld not-compared=%ld",
                     (long)s_rb_native, (long)s_rb_shadow, (long)s_rb_compared, (long)s_rb_match, (long)s_rb_mismatch, (long)s_rb_skipped);
    if (s_rb_last_diff >= 0) l += snprintf(t + l, sizeof t - l, " last-mismatch-at=+0x%lX", (unsigned long)s_rb_last_diff);
    l += snprintf(t + l, sizeof t - l, " | spu:");
    static const char* const gn[RBG_COUNT] = { "off", "direct-stage", "untrusted", "trust-table-full", "capture", "mem" };
    for (int k = 0; k < RBG_COUNT; k++) if (s_rb_gate[k]) l += snprintf(t + l, sizeof t - l, " %s=%ld", gn[k], (long)s_rb_gate[k]);
    for (int k = 1; k < RBN_R_COUNT; k++) if (s_rb_reason[k]) l += snprintf(t + l, sizeof t - l, " %s=%ld", rbn_reason_name[k], (long)s_rb_reason[k]);
    fprintf(stderr, "%s\n", t);
}
/* RIBNCAP1 header: descriptor and the input elements as the stager sees them */
static FILE* ribbon_cap_open(uint32_t job_ea, const uint8_t* D, uint32_t dsz, const sj_snapshot* snap)
{
    static volatile LONG left, seq; static volatile ULONGLONG chk;
    if (left <= 0) {
        const ULONGLONG now = GetTickCount64();
        if (now - chk < 1000) return NULL;
        chk = now;
        if (GetFileAttributesA("ribbon_cap") == INVALID_FILE_ATTRIBUTES) return NULL;
        DeleteFileA("ribbon_cap"); CreateDirectoryA("ribboncap", NULL); left = 500;
        fprintf(stderr, "[ribbon-native] capturing 500 jobs to ribboncap/\n");
    }
    if (InterlockedDecrement(&left) < 0) return NULL;
    char path[96];
    snprintf(path, sizeof path, "ribboncap/10009_%05ld.bin", InterlockedIncrement(&seq));
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    uint32_t nd = rbn_be16(D + 0x0A) / 8u;
    if (nd > 32u || 0x30u + 8u * nd > dsz) nd = 0;
    fwrite("RIBNCAP1", 1, 8, f);
    fwrite(&job_ea, 4, 1, f); fwrite(&g_spurs_job_spu_num, 4, 1, f); fwrite(&dsz, 4, 1, f); fwrite(D, 1, dsz, f);
    static const uint8_t zero[0x8000];
    fwrite(&nd, 4, 1, f);
    for (uint32_t i = 0; i < nd; i++) {
        const uint32_t sz = rbn_be32(D + 0x30 + 8u * i) & 0x7FFFu, ea = rbn_be32(D + 0x34 + 8u * i);
        const uint8_t* src = (snap && i < snap->n_dma && snap->seg[i]) ? snap->seg[i]
                           : (ea && (uint64_t)ea + sz < 0xF0000000ull) ? vm_base + ea : zero;
        fwrite(&sz, 4, 1, f); fwrite(&ea, 4, 1, f); fwrite(src, 1, sz, f);
    }
    const uint32_t ncache = 0;
    fwrite(&ncache, 4, 1, f);
    return f;
}
/* 1: done natively. 2: runs on the SPU, ribbon_verify afterwards (shadow
 * compare and/or capture). 0: runs on the SPU. */
static int ribbon_job(uint32_t job_ea, uint32_t job_desc_size, const sj_snapshot* snap)
{
    static LONG on = -1, verify = -1;
    if (on < 0) { const char* e = getenv("RIBBON_NATIVE"); on = !(e && *e == '0');
                  if (!on) fprintf(stderr, "[ribbon-native] off (RIBBON_NATIVE=0): image 10009 runs on the SPU\n"); }
    if (verify < 0) { const char* e = getenv("RIBBON_NATIVE_VERIFY"); verify = e && *e && *e != '0';
                      if (verify) fprintf(stderr, "[ribbon-native] verify: every job runs on the SPU, native compared\n"); }
    {   static volatile ULONGLONG checked; static volatile LONG file_off;
        const ULONGLONG now = GetTickCount64();
        if (now - checked > 1000) {
            checked = now;
            ribbon_stats(); /* expose off/direct-stage early-return gates */
            const LONG off = GetFileAttributesA("no_ribbon_native") != INVALID_FILE_ATTRIBUTES;
            if (off != file_off) fprintf(stderr, "[ribbon-native] native %s by no_ribbon_native file\n", off ? "OFF" : "ON");
            file_off = off;
        }
        if (file_off || !on) { InterlockedIncrement(&s_rb_gate[RBG_OFF]); return 0; }
    }
    if (direct_stage_enabled()) { InterlockedIncrement(&s_rb_gate[RBG_DIRECT]); return 0; }
    if (!t_rbs) t_rbs = (rb_state*)VirtualAlloc(NULL, sizeof *t_rbs, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t_ea_rec) t_ea_rec = (ea_rec*)VirtualAlloc(NULL, sizeof *t_ea_rec, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t_rbs || !t_ea_rec) return 0;
    t_rbs->active = 0; t_ea_rec->on = 0;
    if (t_rbs->cap) { fclose(t_rbs->cap); t_rbs->cap = NULL; }   /* left over from a job that never ran */
    uint32_t dsz = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    const uint8_t* D = (snap && dsz <= snap->dsz) ? snap->desc : vm_base + job_ea;
    rbn_job J;
    memset(&J, 0, sizeof J);
    J.desc = D; J.dsz = dsz; J.out = t_rbs->out; J.out_cap = RBN_OUT_CAP;
    if (dsz >= 0x38u) {
        const uint32_t sz = rbn_be32(D + 0x30) & 0x7FFFu, ea = rbn_be32(D + 0x34);
        if (snap && snap->n_dma > 0 && snap->seg[0]) { J.param = snap->seg[0]; J.psz = sz; }
        else if (ea && (uint64_t)ea + sz < 0xF0000000ull) { J.param = vm_base + ea; J.psz = sz; }
        else { InterlockedIncrement(&s_rb_gate[RBG_MEM]); return 0; }
    }
    t_rbs->cap = ribbon_cap_open(job_ea, D, dsz, snap);
    const int r = rbn_run_job(&J);
    if (r) {
        InterlockedIncrement(&s_rb_reason[r < RBN_R_COUNT ? r : 0]);
        ribbon_stats();
        if (!t_rbs->cap) return 0;
        t_ea_rec->n = t_ea_rec->bytes = 0; t_ea_rec->overflow = 0; t_ea_rec->on = 1;   /* capture the SPU's PUTs only */
        return 2;
    }
    /* key: the code paths the job took */
    uint32_t key = 2166136261u;
    key = ej_fnv(key, 10009u); key = ej_fnv(key, J.flags); key = ej_fnv(key, J.lit);
    key = ej_fnv(key, (J.nput > 2u) | (J.count == 0u) << 1);
    ea_trust_slot* const slot = verify ? NULL : rb_trust(key);
    if (!verify && !slot) { InterlockedIncrement(&s_rb_gate[RBG_TABLE]); if (t_rbs->cap) { fclose(t_rbs->cap); t_rbs->cap = NULL; } return 0; }
    if (!verify && slot->bad) {
        InterlockedIncrement(&s_rb_gate[RBG_UNTRUSTED]); ribbon_stats();
        if (!t_rbs->cap) return 0;
        t_ea_rec->n = t_ea_rec->bytes = 0; t_ea_rec->overflow = 0; t_ea_rec->on = 1;
        return 2;
    }
    if (!verify && !t_rbs->cap && slot->ok >= RBN_TRUST_AFTER) {
        for (uint32_t i = 0; i < J.nput; i++) {
            if (i + 1 == J.nput) MemoryBarrier();   /* vertices before the completion flag (the PUTF fence) */
            ea_guest_put(J.put[i].ea, t_rbs->out + J.put[i].off, J.put[i].size);
        }
        InterlockedIncrement(&s_rb_native);
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            fprintf(stderr, "[ribbon-native] native job=%08X count=%u flags=%X lit=%u key=%08X puts=%u size=%u\n",
                    job_ea, J.count, J.flags, J.lit, key, J.nput, J.size);
        ribbon_stats();
        return 1;
    }
    if (t_rbs->cap) InterlockedIncrement(&s_rb_gate[RBG_CAPTURE]);
    /* shadow: keep the native PUTs (t_rbs->out), record the SPU's */
    t_rbs->nput = J.nput;
    memcpy(t_rbs->put, J.put, sizeof J.put[0] * J.nput);
    t_rbs->key = key; t_rbs->job_ea = job_ea; t_rbs->slot = slot; t_rbs->active = 1;
    t_ea_rec->n = t_ea_rec->bytes = 0; t_ea_rec->overflow = 0; t_ea_rec->on = 1;
    InterlockedIncrement(&s_rb_shadow);
    return 2;
}
static void ribbon_verify(int failed)
{
    if (!t_rbs || !t_ea_rec) return;
    t_ea_rec->on = 0;
    if (t_rbs->cap) {   /* RIBNCAP1 records: the SPU's PUTs, then the end mark */
        FILE* f = t_rbs->cap;
        t_rbs->cap = NULL;
        if (!failed && !t_ea_rec->overflow)
            for (uint32_t i = 0; i < t_ea_rec->n; i++) {
                fputc('P', f); fwrite(&t_ea_rec->ea[i], 4, 1, f); fwrite(&t_ea_rec->size[i], 4, 1, f);
                fwrite(t_ea_rec->data + t_ea_rec->off[i], 1, t_ea_rec->size[i], f);
            }
        fputc('E', f);
        fclose(f);
    }
    if (!t_rbs->active) return;
    t_rbs->active = 0;
    if (failed || t_ea_rec->overflow) { InterlockedIncrement(&s_rb_skipped); ribbon_stats(); return; }
    char why[256] = "";
    int bad = 0;
    if (t_ea_rec->n != t_rbs->nput) { snprintf(why, sizeof why, "%u PUTs native, %u SPU", t_rbs->nput, t_ea_rec->n); bad = 1; }
    for (uint32_t i = 0; i < t_rbs->nput && i < t_ea_rec->n && !bad; i++) {
        const rbn_put* p = &t_rbs->put[i];
        if (p->ea != t_ea_rec->ea[i] || p->size != t_ea_rec->size[i]) {
            snprintf(why, sizeof why, "PUT %u: native %08X/%u, SPU %08X/%u", i, p->ea, p->size, t_ea_rec->ea[i], t_ea_rec->size[i]);
            bad = 1; break;
        }
        const uint8_t *a = t_rbs->out + p->off, *b = t_ea_rec->data + t_ea_rec->off[i];
        for (uint32_t o = 0; o < p->size; o++)
            if (a[o] != b[o]) {
                const uint32_t q = o & ~3u;
                InterlockedExchange(&s_rb_last_diff, (LONG)(p->off + o));
                snprintf(why, sizeof why, "PUT %u (ea %08X, %u bytes) first differs at +0x%X (output +0x%X): native %08X SPU %08X",
                         i, p->ea, p->size, o, p->off + o, rbn_be32(a + q), rbn_be32(b + q));
                bad = 1; break;
            }
    }
    InterlockedIncrement(&s_rb_compared);
    InterlockedIncrement(bad ? &s_rb_mismatch : &s_rb_match);
    ea_trust_slot* s = t_rbs->slot;
    if (s) {
        if (!bad) {
            const LONG ok = InterlockedIncrement(&s->ok);
            if (ok == RBN_TRUST_AFTER)
                fprintf(stderr, "[ribbon-native] key %08X matched the SPU %d times -- native from now on\n", t_rbs->key, RBN_TRUST_AFTER);
        } else if (InterlockedIncrement(&s->bad) == 1)
            fprintf(stderr, "[ribbon-native] key %08X MISMATCH job=%08X -- kept on the SPU: %s\n", t_rbs->key, t_rbs->job_ea, why);
    } else if (bad) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 16)
            fprintf(stderr, "[ribbon-native] verify MISMATCH key %08X job=%08X: %s\n", t_rbs->key, t_rbs->job_ea, why);
    }
    ribbon_stats();
}
#endif /* RBN_SPURS_GLUE */
#endif /* JOBCRT_RIBBON_H */
