// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES normative arithmetic (S1, 2026-10-28). Everything here decides decoded sample values and must give identical results on
// every platform:
//   - 128-bit accumulator; rounding = add 2^(s-1), arithmetic shift right by s (ties towards +infinity, as rq31 of the LAEY SPEC);
//   - whole-sample symmetric extension (period 2(n-1); n = 1 -> index 0);
//   - rate-layer lifting L: d = xo - P(xe), s = xe + U(d)  (P = LAEY half-band table 0, U = kU0, both Q31);
//   - reference taps for the time position T (Q32 fraction of a base-rate sample): 256-tap Kaiser (beta 14) windowed sinc computed
//     with IEEE-754 binary64 +, -, *, / and sqrt only, in the fixed order written below (no library sin/exp, no fused multiply-add),
//     rounded to Q31, then the tap nearest the centre corrects the sum to exactly 2^31.
// Compile the file that uses refTaps with /fp:strict (MSVC) or -ffp-contract=off (GCC/Clang).
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#if defined(_MSC_VER) && defined(_M_X64) && !defined(LAES_PORTABLE_MUL128)
#include <intrin.h>
#endif
#include "halfband.h"
namespace laes {
typedef std::vector<int64_t> V;
// signed 64 x 64 -> 128-bit product: x64 intrinsic, __int128, or four 32 x 32 partial products (identical results everywhere)
inline void mul128(int64_t a, int64_t b, uint64_t& lo, int64_t& hi) {
#if defined(_MSC_VER) && defined(_M_X64) && !defined(LAES_PORTABLE_MUL128)
    lo = (uint64_t)_mul128(a, b, &hi);
#elif defined(__SIZEOF_INT128__) && !defined(LAES_PORTABLE_MUL128)
    __int128 p = (__int128)a * b; lo = (uint64_t)p; hi = (int64_t)(p >> 64);
#else
    const uint64_t ua = (uint64_t)a, ub = (uint64_t)b, a0 = ua & 0xFFFFFFFFu, a1 = ua >> 32, b0 = ub & 0xFFFFFFFFu, b1 = ub >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1, mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    lo = (p00 & 0xFFFFFFFFu) | (mid << 32); uint64_t h = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);   // unsigned high part
    if (a < 0) h -= ub; if (b < 0) h -= ua; hi = (int64_t)h;                                                 // signed correction
#endif
}
struct Acc { uint64_t lo = 0; int64_t hi = 0;
    void mac(int64_t a, int64_t b) { uint64_t l; int64_t h; mul128(a, b, l, h); uint64_t n = lo + l; hi += h + (n < lo ? 1 : 0); lo = n; }
    int64_t shr(int s) const { uint64_t add = 1ull << (s - 1), n = lo + add; int64_t h = hi + (n < lo ? 1 : 0); return (int64_t)((n >> s) | ((uint64_t)h << (64 - s))); } };
inline int64_t mir(int64_t i, int64_t n) { if (n == 1) return 0; int64_t p = 2 * (n - 1); i %= p; if (i < 0) i += p; return i < n ? i : p - i; }
static const int64_t kU0[60] = {683270116, -226970762, 135242270, -95599562, 73323768, -58947533, 48830814, -41276037, 35385490, -30640348, 26720250, -23416981, 20589580, -18139377, 15995343, -14105100, 12429177, -10937236, 9605506, -8415006, 7350282, -6398504, 5548802, -4791782, 4119170, -3523546, 2998153, -2536758, 2133556, -1783104, 1480274, -1220234, 998431, -810587, 652702, -521052, 412202, -322998, 250575, -192354, 146076, -109516, 81159, -59401, 42896, -30529, 21386, -14723, 9943, -6573, 4241, -2663, 1619, -948, 531, -281, 138, -61, 23, -6};
inline int64_t P(const V& xe, int64_t i) { Acc a; int64_t n = (int64_t)xe.size(); for (int t = 0; t < 40; t++) { a.mac(lae::kHalf0[t], xe[mir(i - t, n)]); a.mac(lae::kHalf0[t], xe[mir(i + 1 + t, n)]); } return a.shr(30); }
inline int64_t U(const V& d, int64_t i) { int64_t n = (int64_t)d.size(); if (n == 0) return 0; Acc a; for (int t = 0; t < 60; t++) { a.mac(kU0[t], d[mir(i - 1 - t, n)]); a.mac(kU0[t], d[mir(i + t, n)]); } return a.shr(31); }
inline void fwd(const V& x, V& s, V& d) { int64_t n = (int64_t)x.size(), ne = (n + 1) / 2, no = n / 2; V xe(ne); d.assign(no, 0); s.assign(ne, 0);
    for (int64_t i = 0; i < ne; i++) xe[i] = x[2 * i];
    for (int64_t i = 0; i < no; i++) d[i] = x[2 * i + 1] - P(xe, i);
    for (int64_t i = 0; i < ne; i++) s[i] = xe[i] + U(d, i); }
inline V inv(const V& s, const V& d, int64_t n) { int64_t ne = (int64_t)s.size(), no = (int64_t)d.size(); V xe(ne), x(n);
    for (int64_t i = 0; i < ne; i++) xe[i] = s[i] - U(d, i);
    for (int64_t i = 0; i < ne; i++) x[2 * i] = xe[i];
    for (int64_t i = 0; i < no; i++) x[2 * i + 1] = d[i] + P(xe, i); return x; }
// ---- reference taps. T = whole part (int64) + frac (uint32, units of 2^-32 base-rate sample). The reference value at index i is the
// band-limited core at time i + T: ref[i] = sum_{q=-127..128} h[q] * core[i + whole + q], h[q] ~ sinc(f - q) * kaiser(f - q), f = frac / 2^32.
// All operations: binary64 + - * / sqrt, written out in a fixed order. Returns 256 taps (index q + 127) in Q31 with sum exactly 2^31.
inline double kPi() { return 3.141592653589793115997963468544185161590576171875; }   // binary64 nearest to pi
inline double sinSeries(double x) {   // sin x for |x| <= pi/2: Taylor to x^31, Horner, fixed order
    double x2 = x * x, s = 1.0; for (int k = 15; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k) * (2.0 * k + 1.0)); return x * s; }
inline double I0Series(double x) {   // modified Bessel I0, power series, 80 terms, fixed order
    double y = x * x / 4.0, term = 1.0, s = 1.0; for (int k = 1; k <= 80; k++) { term = term * y / ((double)k * (double)k); s = s + term; } return s; }
inline std::vector<int64_t> refTaps(uint32_t frac) {   // int64: the centre tap reaches 2^31 when frac = 0
    const int H = 128; const double beta = 14.0, f = (double)frac / 4294967296.0;
    const double g = f <= 0.5 ? f : 1.0 - f;                 // sin(pi f) = sin(pi (1 - f)), both exact in binary64
    const double sf = sinSeries(kPi() * g), i0b = I0Series(beta);
    std::vector<int64_t> h(2 * H); int64_t sum = 0; int centre = 0; double best = 1e300;
    for (int q = -H + 1; q <= H; q++) { const double u = f - (double)q;      // exact
        double sinc; if (u == 0.0) sinc = 1.0; else { const double sgn = (q % 2 == 0) ? 1.0 : -1.0; sinc = sgn * sf / (kPi() * u); }   // sin(pi(f-q)) = (-1)^q sin(pi f)
        const double r = u / (double)H; double w = 0.0; if (r > -1.0 && r < 1.0) w = I0Series(beta * std::sqrt(1.0 - r * r)) / i0b;
        const double v = sinc * w * 2147483648.0; const int64_t t = (int64_t)std::floor(v + 0.5);
        h[q + H - 1] = t; sum += t; const double au = u < 0 ? -u : u; if (au < best) { best = au; centre = q + H - 1; } }
    h[centre] += (1LL << 31) - sum;     // DC gain exactly 1
    return h; }
// reference on the master's grid: ref[i] = round( sum_q h[q] core[i + whole + q] / 2^(31 - up) ), up = extra grid bits (may be negative);
// samples outside the core are taken by symmetric extension (same rule as the lifting)
inline V reference(const V& core, int64_t whole, uint32_t frac, int up) {
    std::vector<int64_t> h = refTaps(frac); int64_t n = (int64_t)core.size(); V r(n);
    for (int64_t i = 0; i < n; i++) { Acc a; for (int q = -127; q <= 128; q++) a.mac(h[q + 127], core[mir(i + whole + q, n)]);
        const int s = 31 - up; r[i] = s > 0 ? a.shr(s) : (int64_t)a.lo << (-s); } return r; }
}  // namespace laes
