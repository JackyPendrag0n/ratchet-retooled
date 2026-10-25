// SPU instruction semantics (everything except control flow, channels and stop).
// Each op takes the raw instruction word; with a constant word (recompiled code) the field
// extraction folds away.
#pragma once
#include "spu_core.h"

#include <cmath>
#include <cstdlib>

#define SPU_RT (op & 0x7f)
#define SPU_RA ((op >> 7) & 0x7f)
#define SPU_RB ((op >> 14) & 0x7f)
#define SPU_RC (op & 0x7f)
#define SPU_RT4 ((op >> 21) & 0x7f)
#define SPU_I7 (static_cast<s32>(op << 11) >> 25)
#define SPU_I8 ((op >> 14) & 0xff)
#define SPU_I10 (static_cast<s32>(op << 8) >> 22)
#define SPU_I16 (static_cast<s32>(op << 9) >> 16)
#define SPU_U16 ((op >> 7) & 0xffff)
#define SPU_I18 ((op >> 7) & 0x3ffff)

#define G(n) s.gpr[n]
#define PREF(n) (s.gpr[n].w[3])

namespace spu {

static inline spu_reg splat32(u32 x) { spu_reg r; r.m = _mm_set1_epi32(static_cast<int>(x)); return r; }
static inline spu_reg splat16(u16 x) { spu_reg r; r.m = _mm_set1_epi16(static_cast<short>(x)); return r; }
static inline spu_reg splat8(u8 x) { spu_reg r; r.m = _mm_set1_epi8(static_cast<char>(x)); return r; }
static inline spu_reg pref_only(u32 x) { spu_reg r; r.m = _mm_set_epi32(static_cast<int>(x), 0, 0, 0); return r; }

static inline u128 rotl128(u128 x, unsigned n) { n &= 127; return n ? (x << n) | (x >> (128 - n)) : x; }

// ---------------------------------------------------------------- RRR
static inline __attribute__((always_inline)) void selb(SPUThread& s, u32 op) {
    const __m128i c = G(SPU_RC).m;
    G(SPU_RT4).m = _mm_or_si128(_mm_and_si128(c, G(SPU_RB).m), _mm_andnot_si128(c, G(SPU_RA).m));
}
static inline __attribute__((always_inline)) void shufb(SPUThread& s, u32 op) {
    // Registers hold SPU byte k at host byte 15-k, so selector byte k of a:b is host byte
    // (~sel & 15) of a (sel bit 4 clear) or b. Selectors 10xxxxxx, 110xxxxx, 111xxxxx give
    // 0x00, 0xff and 0x80.
    const __m128i sel = G(SPU_RC).m;
    const __m128i j = _mm_andnot_si128(sel, _mm_set1_epi8(0x0f));
    const __m128i ra = _mm_shuffle_epi8(G(SPU_RA).m, j), rb = _mm_shuffle_epi8(G(SPU_RB).m, j);
    const __m128i useb = _mm_cmpeq_epi8(_mm_and_si128(sel, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    const __m128i r = _mm_blendv_epi8(ra, rb, useb);
    const __m128i top = _mm_and_si128(sel, _mm_set1_epi8(char(0xe0)));
    const __m128i ff = _mm_cmpeq_epi8(top, _mm_set1_epi8(char(0xc0)));
    const __m128i x80 = _mm_and_si128(_mm_cmpeq_epi8(top, _mm_set1_epi8(char(0xe0))), _mm_set1_epi8(char(0x80)));
    // sel >= 0x80 (signed < 0): constant instead of a source byte
    const __m128i special = _mm_cmplt_epi8(sel, _mm_setzero_si128());
    G(SPU_RT4).m = _mm_or_si128(_mm_andnot_si128(special, r), _mm_or_si128(ff, x80));
}
static inline __attribute__((always_inline)) void mpya(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB), c = G(SPU_RC);
    spu_reg& t = G(SPU_RT4);
    for (int i = 0; i < 4; ++i) t.sw[i] = s32(s16(a.w[i])) * s32(s16(b.w[i])) + c.sw[i];
}
// SPU single precision has no Inf/NaN: exponent 255 is an ordinary (huge) number and results
// saturate. Approximate that on x86 by reading exponent-255 inputs as +-FLT_MAX and clamping
// results to +-FLT_MAX, so 0*Inf and Inf-Inf can never turn geometry into NaNs.
// TOD_NO_XFLOAT=1 turns the approximation off (plain IEEE results), for comparisons.
inline const bool g_spu_xfloat = !std::getenv("TOD_NO_XFLOAT");
static inline __m128 xin(__m128 x) {
    if (!g_spu_xfloat) return x;
    const __m128i xi = _mm_castps_si128(x), e = _mm_set1_epi32(0x7f800000);
    const __m128i big = _mm_cmpeq_epi32(_mm_and_si128(xi, e), e);
    const __m128i fmax = _mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32(int(0x80000000))), _mm_set1_epi32(0x7f7fffff));
    return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(big, xi), _mm_and_si128(big, fmax)));
}
static inline __m128 xout(__m128 x) {
    if (!g_spu_xfloat) return x;
    const __m128 m = _mm_set1_ps(3.40282347e38f);
    return _mm_max_ps(_mm_min_ps(x, m), _mm_set1_ps(-3.40282347e38f));
}
static inline __attribute__((always_inline)) void fma(SPUThread& s, u32 op) {
    G(SPU_RT4).mf = xout(_mm_add_ps(xout(_mm_mul_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf))), xin(G(SPU_RC).mf)));
}
static inline __attribute__((always_inline)) void fms(SPUThread& s, u32 op) {
    G(SPU_RT4).mf = xout(_mm_sub_ps(xout(_mm_mul_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf))), xin(G(SPU_RC).mf)));
}
static inline __attribute__((always_inline)) void fnms(SPUThread& s, u32 op) {
    G(SPU_RT4).mf = xout(_mm_sub_ps(xin(G(SPU_RC).mf), xout(_mm_mul_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf)))));
}

// ---------------------------------------------------------------- immediates
static inline __attribute__((always_inline)) void ila(SPUThread& s, u32 op) { G(SPU_RT) = splat32(SPU_I18); }
static inline __attribute__((always_inline)) void il(SPUThread& s, u32 op) { G(SPU_RT) = splat32(u32(SPU_I16)); }
static inline __attribute__((always_inline)) void ilh(SPUThread& s, u32 op) { G(SPU_RT) = splat16(u16(SPU_U16)); }
static inline __attribute__((always_inline)) void ilhu(SPUThread& s, u32 op) { G(SPU_RT) = splat32(SPU_U16 << 16); }
static inline __attribute__((always_inline)) void iohl(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RT).m, _mm_set1_epi32(int(SPU_U16))); }
static inline __attribute__((always_inline)) void fsmbi(SPUThread& s, u32 op) {
    const u32 m = SPU_U16;
    spu_reg t;
    for (int i = 0; i < 16; ++i) t.b[i] = (m >> i) & 1 ? 0xff : 0;  // bit 15 = byte element 0 = b[15]
    G(SPU_RT) = t;
}

// ---------------------------------------------------------------- RI10 arithmetic / logic
static inline __attribute__((always_inline)) void ai(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_add_epi32(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void ahi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_add_epi16(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void sfi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_sub_epi32(_mm_set1_epi32(SPU_I10), G(SPU_RA).m); }
static inline __attribute__((always_inline)) void sfhi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_sub_epi16(_mm_set1_epi16(short(SPU_I10)), G(SPU_RA).m); }
static inline __attribute__((always_inline)) void andi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_and_si128(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void andhi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_and_si128(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void andbi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_and_si128(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }
static inline __attribute__((always_inline)) void ori(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void orhi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void orbi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }
static inline __attribute__((always_inline)) void xori(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void xorhi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void xorbi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }
static inline __attribute__((always_inline)) void mpyi(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] = s32(s16(a.w[i])) * s32(s16(SPU_I10));
}
static inline __attribute__((always_inline)) void mpyui(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = u32(u16(a.w[i])) * u32(u16(SPU_I10));
}
static inline __attribute__((always_inline)) void cgti(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi32(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void cgthi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi16(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void cgtbi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi8(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }
static inline __attribute__((always_inline)) void ceqi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi32(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void ceqhi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi16(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void ceqbi(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi8(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }
static inline __m128i cmpgtu32(__m128i a, __m128i b) {
    const __m128i bias = _mm_set1_epi32(int(0x80000000));
    return _mm_cmpgt_epi32(_mm_xor_si128(a, bias), _mm_xor_si128(b, bias));
}
static inline __m128i cmpgtu16(__m128i a, __m128i b) {
    const __m128i bias = _mm_set1_epi16(short(0x8000));
    return _mm_cmpgt_epi16(_mm_xor_si128(a, bias), _mm_xor_si128(b, bias));
}
static inline __m128i cmpgtu8(__m128i a, __m128i b) {
    const __m128i bias = _mm_set1_epi8(char(0x80));
    return _mm_cmpgt_epi8(_mm_xor_si128(a, bias), _mm_xor_si128(b, bias));
}
static inline __attribute__((always_inline)) void clgti(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu32(G(SPU_RA).m, _mm_set1_epi32(SPU_I10)); }
static inline __attribute__((always_inline)) void clgthi(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu16(G(SPU_RA).m, _mm_set1_epi16(short(SPU_I10))); }
static inline __attribute__((always_inline)) void clgtbi(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu8(G(SPU_RA).m, _mm_set1_epi8(char(SPU_I10))); }

// ---------------------------------------------------------------- RR integer
static inline __attribute__((always_inline)) void a(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_add_epi32(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void ah(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_add_epi16(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void sf(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_sub_epi32(G(SPU_RB).m, G(SPU_RA).m); }
static inline __attribute__((always_inline)) void sfh(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_sub_epi16(G(SPU_RB).m, G(SPU_RA).m); }
static inline __attribute__((always_inline)) void and_(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_and_si128(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void andc(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_andnot_si128(G(SPU_RB).m, G(SPU_RA).m); }
static inline __attribute__((always_inline)) void or_(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void orc(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_or_si128(G(SPU_RA).m, _mm_xor_si128(G(SPU_RB).m, _mm_set1_epi32(-1))); }
static inline __attribute__((always_inline)) void xor_(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void nand(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(_mm_and_si128(G(SPU_RA).m, G(SPU_RB).m), _mm_set1_epi32(-1)); }
static inline __attribute__((always_inline)) void nor(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(_mm_or_si128(G(SPU_RA).m, G(SPU_RB).m), _mm_set1_epi32(-1)); }
static inline __attribute__((always_inline)) void eqv(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_xor_si128(_mm_xor_si128(G(SPU_RA).m, G(SPU_RB).m), _mm_set1_epi32(-1)); }
static inline __attribute__((always_inline)) void orx(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA);
    G(SPU_RT) = pref_only(a.w[0] | a.w[1] | a.w[2] | a.w[3]);
}
static inline __attribute__((always_inline)) void cg(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = (u64(a.w[i]) + b.w[i]) >> 32;
}
static inline __attribute__((always_inline)) void bg(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = b.w[i] >= a.w[i] ? 1 : 0;
}
static inline __attribute__((always_inline)) void addx(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = a.w[i] + b.w[i] + (t.w[i] & 1);
}
static inline __attribute__((always_inline)) void sfx(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = b.w[i] + ~a.w[i] + (t.w[i] & 1);
}
static inline __attribute__((always_inline)) void cgx(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = (u64(a.w[i]) + b.w[i] + (t.w[i] & 1)) >> 32;
}
static inline __attribute__((always_inline)) void bgx(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = (s64(u64(b.w[i])) - s64(u64(a.w[i])) - s64(1 - (t.w[i] & 1))) >= 0 ? 1 : 0;
}
static inline __attribute__((always_inline)) void absdb(SPUThread& s, u32 op) {
    const __m128i a = G(SPU_RA).m, b = G(SPU_RB).m;
    G(SPU_RT).m = _mm_sub_epi8(_mm_max_epu8(a, b), _mm_min_epu8(a, b));
}
static inline __attribute__((always_inline)) void avgb(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_avg_epu8(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void sumb(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg t;
    for (int i = 0; i < 4; ++i) {
        const u32 x = a.w[i], y = b.w[i];
        const u16 sa = u16((x & 0xff) + ((x >> 8) & 0xff) + ((x >> 16) & 0xff) + (x >> 24));
        const u16 sb = u16((y & 0xff) + ((y >> 8) & 0xff) + ((y >> 16) & 0xff) + (y >> 24));
        t.w[i] = (u32(sb) << 16) | sa;
    }
    G(SPU_RT) = t;
}
static inline __attribute__((always_inline)) void clz(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = a.w[i] ? __builtin_clz(a.w[i]) : 32;
}
static inline __attribute__((always_inline)) void cntb(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 16; ++i) t.b[i] = u8(__builtin_popcount(a.b[i]));
}
static inline __attribute__((always_inline)) void xsbh(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 8; ++i) t.sh[i] = s16(s8(a.h[i]));
}
static inline __attribute__((always_inline)) void xshw(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] = s32(s16(a.w[i]));
}
static inline __attribute__((always_inline)) void xswd(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 2; ++i) t.sd[i] = s64(s32(u32(a.d[i])));
}
static inline __attribute__((always_inline)) void cgt(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi32(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void cgth(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi16(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void cgtb(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpgt_epi8(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void clgt(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu32(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void clgth(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu16(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void clgtb(SPUThread& s, u32 op) { G(SPU_RT).m = cmpgtu8(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void ceq(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi32(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void ceqh(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi16(G(SPU_RA).m, G(SPU_RB).m); }
static inline __attribute__((always_inline)) void ceqb(SPUThread& s, u32 op) { G(SPU_RT).m = _mm_cmpeq_epi8(G(SPU_RA).m, G(SPU_RB).m); }

// multiplies (16 x 16 on the low or high halfword of each word)
static inline __attribute__((always_inline)) void mpy(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] = s32(s16(a.w[i])) * s32(s16(b.w[i]));
}
static inline __attribute__((always_inline)) void mpyu(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = u32(u16(a.w[i])) * u32(u16(b.w[i]));
}
static inline __attribute__((always_inline)) void mpyh(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = ((a.w[i] >> 16) * u32(u16(b.w[i]))) << 16;
}
static inline __attribute__((always_inline)) void mpys(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] = (s32(s16(a.w[i])) * s32(s16(b.w[i]))) >> 16;
}
static inline __attribute__((always_inline)) void mpyhh(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] = s32(s16(a.w[i] >> 16)) * s32(s16(b.w[i] >> 16));
}
static inline __attribute__((always_inline)) void mpyhhu(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] = (a.w[i] >> 16) * (b.w[i] >> 16);
}
static inline __attribute__((always_inline)) void mpyhha(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.sw[i] += s32(s16(a.w[i] >> 16)) * s32(s16(b.w[i] >> 16));
}
static inline __attribute__((always_inline)) void mpyhhau(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) t.w[i] += (a.w[i] >> 16) * (b.w[i] >> 16);
}

// ---------------------------------------------------------------- shifts and rotates (words / halfwords)
static inline __attribute__((always_inline)) void rot(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) { const u32 n = b.w[i] & 31; t.w[i] = n ? (a.w[i] << n) | (a.w[i] >> (32 - n)) : a.w[i]; }
}
static inline __attribute__((always_inline)) void rotm(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) { const u32 n = (0 - b.w[i]) & 0x3f; t.w[i] = n < 32 ? a.w[i] >> n : 0; }
}
static inline __attribute__((always_inline)) void rotma(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) { const u32 n = (0 - b.w[i]) & 0x3f; t.sw[i] = a.sw[i] >> (n < 32 ? n : 31); }
}
static inline __attribute__((always_inline)) void shl(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 4; ++i) { const u32 n = b.w[i] & 0x3f; t.w[i] = n < 32 ? a.w[i] << n : 0; }
}
static inline __attribute__((always_inline)) void roth(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 8; ++i) { const u32 n = b.h[i] & 15; t.h[i] = u16(n ? (a.h[i] << n) | (a.h[i] >> (16 - n)) : a.h[i]); }
}
static inline __attribute__((always_inline)) void rothm(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 8; ++i) { const u32 n = (0 - b.h[i]) & 0x1f; t.h[i] = n < 16 ? u16(a.h[i] >> n) : 0; }
}
static inline __attribute__((always_inline)) void rotmah(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 8; ++i) { const u32 n = (0 - b.h[i]) & 0x1f; t.sh[i] = s16(a.sh[i] >> (n < 16 ? n : 15)); }
}
static inline __attribute__((always_inline)) void shlh(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA), b = G(SPU_RB); spu_reg& t = G(SPU_RT);
    for (int i = 0; i < 8; ++i) { const u32 n = b.h[i] & 0x1f; t.h[i] = n < 16 ? u16(a.h[i] << n) : 0; }
}
static inline __attribute__((always_inline)) void roti(SPUThread& s, u32 op) {
    const u32 n = SPU_I7 & 31;
    const __m128i a = G(SPU_RA).m;
    G(SPU_RT).m = n ? _mm_or_si128(_mm_slli_epi32(a, int(n)), _mm_srli_epi32(a, int(32 - n))) : a;
}
static inline __attribute__((always_inline)) void rotmi(SPUThread& s, u32 op) {
    const u32 n = (0 - SPU_I7) & 0x3f;
    G(SPU_RT).m = n < 32 ? _mm_srli_epi32(G(SPU_RA).m, int(n)) : _mm_setzero_si128();
}
static inline __attribute__((always_inline)) void rotmai(SPUThread& s, u32 op) {
    const u32 n = (0 - SPU_I7) & 0x3f;
    G(SPU_RT).m = _mm_srai_epi32(G(SPU_RA).m, int(n < 32 ? n : 31));
}
static inline __attribute__((always_inline)) void shli(SPUThread& s, u32 op) {
    const u32 n = SPU_I7 & 0x3f;
    G(SPU_RT).m = n < 32 ? _mm_slli_epi32(G(SPU_RA).m, int(n)) : _mm_setzero_si128();
}
static inline __attribute__((always_inline)) void rothi(SPUThread& s, u32 op) {
    const u32 n = SPU_I7 & 15;
    const __m128i a = G(SPU_RA).m;
    G(SPU_RT).m = n ? _mm_or_si128(_mm_slli_epi16(a, int(n)), _mm_srli_epi16(a, int(16 - n))) : a;
}
static inline __attribute__((always_inline)) void rothmi(SPUThread& s, u32 op) {
    const u32 n = (0 - SPU_I7) & 0x1f;
    G(SPU_RT).m = n < 16 ? _mm_srli_epi16(G(SPU_RA).m, int(n)) : _mm_setzero_si128();
}
static inline __attribute__((always_inline)) void rotmahi(SPUThread& s, u32 op) {
    const u32 n = (0 - SPU_I7) & 0x1f;
    G(SPU_RT).m = _mm_srai_epi16(G(SPU_RA).m, int(n < 16 ? n : 15));
}
static inline __attribute__((always_inline)) void shlhi(SPUThread& s, u32 op) {
    const u32 n = SPU_I7 & 0x1f;
    G(SPU_RT).m = n < 16 ? _mm_slli_epi16(G(SPU_RA).m, int(n)) : _mm_setzero_si128();
}

// ---------------------------------------------------------------- quadword shifts and rotates
static inline __attribute__((always_inline)) void rotqby_n(SPUThread& s, u32 op, u32 n) { G(SPU_RT).q = rotl128(G(SPU_RA).q, (n & 15) * 8); }
static inline __attribute__((always_inline)) void rotqmby_n(SPUThread& s, u32 op, u32 n) {  // n already negated and masked to 0..31
    G(SPU_RT).q = n < 16 ? G(SPU_RA).q >> (n * 8) : 0;
}
static inline __attribute__((always_inline)) void shlqby_n(SPUThread& s, u32 op, u32 n) { G(SPU_RT).q = n < 16 ? G(SPU_RA).q << (n * 8) : 0; }
static inline __attribute__((always_inline)) void rotqby(SPUThread& s, u32 op) { rotqby_n(s, op, PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void rotqbybi(SPUThread& s, u32 op) { rotqby_n(s, op, PREF(SPU_RB) >> 3); }
static inline __attribute__((always_inline)) void rotqbyi(SPUThread& s, u32 op) { rotqby_n(s, op, u32(SPU_I7)); }
static inline __attribute__((always_inline)) void rotqmby(SPUThread& s, u32 op) { rotqmby_n(s, op, (0 - PREF(SPU_RB)) & 0x1f); }
static inline __attribute__((always_inline)) void rotqmbybi(SPUThread& s, u32 op) { rotqmby_n(s, op, (0 - (PREF(SPU_RB) >> 3)) & 0x1f); }
static inline __attribute__((always_inline)) void rotqmbyi(SPUThread& s, u32 op) { rotqmby_n(s, op, (0 - u32(SPU_I7)) & 0x1f); }
static inline __attribute__((always_inline)) void shlqby(SPUThread& s, u32 op) { shlqby_n(s, op, PREF(SPU_RB) & 0x1f); }
static inline __attribute__((always_inline)) void shlqbybi(SPUThread& s, u32 op) { shlqby_n(s, op, (PREF(SPU_RB) >> 3) & 0x1f); }
static inline __attribute__((always_inline)) void shlqbyi(SPUThread& s, u32 op) { shlqby_n(s, op, u32(SPU_I7) & 0x1f); }
static inline __attribute__((always_inline)) void rotqbi(SPUThread& s, u32 op) { G(SPU_RT).q = rotl128(G(SPU_RA).q, PREF(SPU_RB) & 7); }
static inline __attribute__((always_inline)) void rotqbii(SPUThread& s, u32 op) { G(SPU_RT).q = rotl128(G(SPU_RA).q, u32(SPU_I7) & 7); }
static inline __attribute__((always_inline)) void rotqmbi(SPUThread& s, u32 op) { G(SPU_RT).q = G(SPU_RA).q >> ((0 - PREF(SPU_RB)) & 7); }
static inline __attribute__((always_inline)) void rotqmbii(SPUThread& s, u32 op) { G(SPU_RT).q = G(SPU_RA).q >> ((0 - u32(SPU_I7)) & 7); }
static inline __attribute__((always_inline)) void shlqbi(SPUThread& s, u32 op) { G(SPU_RT).q = G(SPU_RA).q << (PREF(SPU_RB) & 7); }
static inline __attribute__((always_inline)) void shlqbii(SPUThread& s, u32 op) { G(SPU_RT).q = G(SPU_RA).q << (u32(SPU_I7) & 7); }

// ---------------------------------------------------------------- insertion masks
static inline spu_reg insert_base() {
    spu_reg r;
    for (int i = 0; i < 16; ++i) r.b[15 - i] = u8(0x10 + i);
    return r;
}
static inline __attribute__((always_inline)) void cbx_t(SPUThread& s, u32 op, u32 t) { spu_reg r = insert_base(); r.b[15 - (t & 15)] = 3; G(SPU_RT) = r; }
static inline __attribute__((always_inline)) void chx_t(SPUThread& s, u32 op, u32 t) {
    spu_reg r = insert_base(); const u32 i = t & 14; r.b[15 - i] = 2; r.b[14 - i] = 3; G(SPU_RT) = r;
}
static inline __attribute__((always_inline)) void cwx_t(SPUThread& s, u32 op, u32 t) {
    spu_reg r = insert_base(); const u32 i = (t & 12) / 4; r.w[3 - i] = 0x00010203; G(SPU_RT) = r;
}
static inline __attribute__((always_inline)) void cdx_t(SPUThread& s, u32 op, u32 t) {
    spu_reg r = insert_base(); const u32 i = (t & 8) / 8; r.d[1 - i] = 0x0001020304050607ull; G(SPU_RT) = r;
}
static inline __attribute__((always_inline)) void cbx(SPUThread& s, u32 op) { cbx_t(s, op, PREF(SPU_RA) + PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void chx(SPUThread& s, u32 op) { chx_t(s, op, PREF(SPU_RA) + PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void cwx(SPUThread& s, u32 op) { cwx_t(s, op, PREF(SPU_RA) + PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void cdx(SPUThread& s, u32 op) { cdx_t(s, op, PREF(SPU_RA) + PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void cbd(SPUThread& s, u32 op) { cbx_t(s, op, PREF(SPU_RA) + u32(SPU_I7)); }
static inline __attribute__((always_inline)) void chd(SPUThread& s, u32 op) { chx_t(s, op, PREF(SPU_RA) + u32(SPU_I7)); }
static inline __attribute__((always_inline)) void cwd(SPUThread& s, u32 op) { cwx_t(s, op, PREF(SPU_RA) + u32(SPU_I7)); }
static inline __attribute__((always_inline)) void cdd(SPUThread& s, u32 op) { cdx_t(s, op, PREF(SPU_RA) + u32(SPU_I7)); }

// ---------------------------------------------------------------- gather / form select mask
static inline __attribute__((always_inline)) void gb(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA);
    u32 r = 0;
    for (int i = 0; i < 4; ++i) r |= (a.w[i] & 1) << i;  // w[3] = element 0 -> bit 3
    G(SPU_RT) = pref_only(r);
}
static inline __attribute__((always_inline)) void gbh(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA);
    u32 r = 0;
    for (int i = 0; i < 8; ++i) r |= (a.h[i] & 1u) << i;
    G(SPU_RT) = pref_only(r);
}
static inline __attribute__((always_inline)) void gbb(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA);
    u32 r = 0;
    for (int i = 0; i < 16; ++i) r |= (a.b[i] & 1u) << i;
    G(SPU_RT) = pref_only(r);
}
static inline __attribute__((always_inline)) void fsm(SPUThread& s, u32 op) {
    const u32 m = PREF(SPU_RA); spu_reg t;
    for (int i = 0; i < 4; ++i) t.w[i] = (m >> i) & 1 ? ~0u : 0;
    G(SPU_RT) = t;
}
static inline __attribute__((always_inline)) void fsmh(SPUThread& s, u32 op) {
    const u32 m = PREF(SPU_RA); spu_reg t;
    for (int i = 0; i < 8; ++i) t.h[i] = (m >> i) & 1 ? 0xffff : 0;
    G(SPU_RT) = t;
}
static inline __attribute__((always_inline)) void fsmb(SPUThread& s, u32 op) {
    const u32 m = PREF(SPU_RA); spu_reg t;
    for (int i = 0; i < 16; ++i) t.b[i] = (m >> i) & 1 ? 0xff : 0;
    G(SPU_RT) = t;
}

// ---------------------------------------------------------------- loads and stores
static inline __attribute__((always_inline)) void lqd(SPUThread& s, u32 op) { G(SPU_RT) = ls_read128(s, PREF(SPU_RA) + (u32(SPU_I10) << 4)); }
static inline __attribute__((always_inline)) void stqd(SPUThread& s, u32 op) { ls_write128(s, PREF(SPU_RA) + (u32(SPU_I10) << 4), G(SPU_RT)); }
static inline __attribute__((always_inline)) void lqx(SPUThread& s, u32 op) { G(SPU_RT) = ls_read128(s, PREF(SPU_RA) + PREF(SPU_RB)); }
static inline __attribute__((always_inline)) void stqx(SPUThread& s, u32 op) { ls_write128(s, PREF(SPU_RA) + PREF(SPU_RB), G(SPU_RT)); }
static inline __attribute__((always_inline)) void lqa(SPUThread& s, u32 op) { G(SPU_RT) = ls_read128(s, u32(SPU_I16) << 2); }
static inline __attribute__((always_inline)) void stqa(SPUThread& s, u32 op) { ls_write128(s, u32(SPU_I16) << 2, G(SPU_RT)); }
static inline __attribute__((always_inline)) void lqr(SPUThread& s, u32 op, u32 pc) { G(SPU_RT) = ls_read128(s, pc + (u32(SPU_I16) << 2)); }
static inline __attribute__((always_inline)) void stqr(SPUThread& s, u32 op, u32 pc) { ls_write128(s, pc + (u32(SPU_I16) << 2), G(SPU_RT)); }

// ---------------------------------------------------------------- single precision
static inline __attribute__((always_inline)) void fa(SPUThread& s, u32 op) { G(SPU_RT).mf = xout(_mm_add_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf))); }
static inline __attribute__((always_inline)) void fs(SPUThread& s, u32 op) { G(SPU_RT).mf = xout(_mm_sub_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf))); }
static inline __attribute__((always_inline)) void fm(SPUThread& s, u32 op) { G(SPU_RT).mf = xout(_mm_mul_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf))); }
static inline __attribute__((always_inline)) void fcgt(SPUThread& s, u32 op) { G(SPU_RT).mf = _mm_cmpgt_ps(xin(G(SPU_RA).mf), xin(G(SPU_RB).mf)); }
static inline __attribute__((always_inline)) void fceq(SPUThread& s, u32 op) { G(SPU_RT).mf = _mm_cmpeq_ps(G(SPU_RA).mf, G(SPU_RB).mf); }
static inline __m128 fabs4(__m128 x) { return _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))); }
static inline __attribute__((always_inline)) void fcmgt(SPUThread& s, u32 op) { G(SPU_RT).mf = _mm_cmpgt_ps(fabs4(G(SPU_RA).mf), fabs4(G(SPU_RB).mf)); }
static inline __attribute__((always_inline)) void fcmeq(SPUThread& s, u32 op) { G(SPU_RT).mf = _mm_cmpeq_ps(fabs4(G(SPU_RA).mf), fabs4(G(SPU_RB).mf)); }
static inline __attribute__((always_inline)) void frest(SPUThread& s, u32 op) { G(SPU_RT).mf = xout(_mm_div_ps(_mm_set1_ps(1.0f), xin(G(SPU_RA).mf))); }
static inline __attribute__((always_inline)) void frsqest(SPUThread& s, u32 op) {
    G(SPU_RT).mf = xout(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(fabs4(xin(G(SPU_RA).mf)))));
}
static inline __attribute__((always_inline)) void fi(SPUThread& s, u32 op) { G(SPU_RT) = G(SPU_RB); }  // estimates are already exact
static inline s32 sat_s32(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return 0x7fffffff;
    if (v <= -2147483648.0) return s32(0x80000000);
    return s32(v);
}
static inline u32 sat_u32(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 4294967295.0) return 0xffffffff;
    return u32(v);
}
static inline __attribute__((always_inline)) void cflts(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    const double scale = std::ldexp(1.0, 173 - s32(SPU_I8));
    for (int i = 0; i < 4; ++i) t.sw[i] = sat_s32(double(a.f[i]) * scale);
}
static inline __attribute__((always_inline)) void cfltu(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    const double scale = std::ldexp(1.0, 173 - s32(SPU_I8));
    for (int i = 0; i < 4; ++i) t.w[i] = sat_u32(double(a.f[i]) * scale);
}
static inline __attribute__((always_inline)) void csflt(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    const double scale = std::ldexp(1.0, s32(SPU_I8) - 155);
    for (int i = 0; i < 4; ++i) t.f[i] = float(double(a.sw[i]) * scale);
}
static inline __attribute__((always_inline)) void cuflt(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    const double scale = std::ldexp(1.0, s32(SPU_I8) - 155);
    for (int i = 0; i < 4; ++i) t.f[i] = float(double(a.w[i]) * scale);
}

// ---------------------------------------------------------------- double precision
static inline __attribute__((always_inline)) void dfa(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_add_pd(G(SPU_RA).md, G(SPU_RB).md); }
static inline __attribute__((always_inline)) void dfs(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_sub_pd(G(SPU_RA).md, G(SPU_RB).md); }
static inline __attribute__((always_inline)) void dfm(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_mul_pd(G(SPU_RA).md, G(SPU_RB).md); }
static inline __attribute__((always_inline)) void dfma(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_add_pd(_mm_mul_pd(G(SPU_RA).md, G(SPU_RB).md), G(SPU_RT).md); }
static inline __attribute__((always_inline)) void dfms(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_sub_pd(_mm_mul_pd(G(SPU_RA).md, G(SPU_RB).md), G(SPU_RT).md); }
static inline __attribute__((always_inline)) void dfnms(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_sub_pd(G(SPU_RT).md, _mm_mul_pd(G(SPU_RA).md, G(SPU_RB).md)); }
static inline __attribute__((always_inline)) void dfnma(SPUThread& s, u32 op) {
    G(SPU_RT).md = _mm_xor_pd(_mm_add_pd(_mm_mul_pd(G(SPU_RA).md, G(SPU_RB).md), G(SPU_RT).md), _mm_set1_pd(-0.0));
}
static inline __attribute__((always_inline)) void dfcgt(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_cmpgt_pd(G(SPU_RA).md, G(SPU_RB).md); }
static inline __attribute__((always_inline)) void dfceq(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_cmpeq_pd(G(SPU_RA).md, G(SPU_RB).md); }
static inline __m128d dabs(__m128d x) { return _mm_and_pd(x, _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffll))); }
static inline __attribute__((always_inline)) void dfcmgt(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_cmpgt_pd(dabs(G(SPU_RA).md), dabs(G(SPU_RB).md)); }
static inline __attribute__((always_inline)) void dfcmeq(SPUThread& s, u32 op) { G(SPU_RT).md = _mm_cmpeq_pd(dabs(G(SPU_RA).md), dabs(G(SPU_RB).md)); }
static inline __attribute__((always_inline)) void fesd(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    t.df[1] = a.f[3];  // element 0 <- word element 0
    t.df[0] = a.f[1];  // element 1 <- word element 2
}
static inline __attribute__((always_inline)) void frds(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg t;
    t.f[3] = float(a.df[1]); t.w[2] = 0;
    t.f[1] = float(a.df[0]); t.w[0] = 0;
    G(SPU_RT) = t;
}
static inline __attribute__((always_inline)) void dftsv(SPUThread& s, u32 op) {
    const spu_reg a = G(SPU_RA); spu_reg& t = G(SPU_RT);
    const u32 m = SPU_I7 & 0x7f;
    for (int i = 0; i < 2; ++i) {
        const double v = a.df[i];
        const bool neg = std::signbit(v);
        bool hit = false;
        if (std::isnan(v)) hit = m & 0x40;
        else if (std::isinf(v)) hit = neg ? (m & 0x10) : (m & 0x20);
        else if (v == 0.0) hit = neg ? (m & 0x04) : (m & 0x08);
        else if (std::fpclassify(v) == FP_SUBNORMAL) hit = neg ? (m & 0x01) : (m & 0x02);
        t.d[i] = hit ? ~0ull : 0;
    }
}
static inline __attribute__((always_inline)) void fscrrd(SPUThread& s, u32 op) { G(SPU_RT).q = 0; }

}  // namespace spu
