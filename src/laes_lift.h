// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES rate-layer construction L, fast form (2026-10-28). Same results as the normative form (src/laes_math.h fwd / inv), bit for bit:
//   d[i] = xo[i] - P(xe, i),  P = round(sum_t h0[t] (xe[m(i-t)] + xe[m(i+1+t)]) / 2^30)      h0 = lae::kHalf0 (40 taps, Q31)
//   s[i] = xe[i] + U(d, i),   U = round(sum_t u[t]  (d[m(i-1-t)] + d[m(i+t)])  / 2^31)      u  = kU0 (60 taps, Q31)
//   round = add 2^(s-1), arithmetic shift right (ties up); m = symmetric extension, period 2(n-1), m = 0 for n = 1.
// What is faster, without changing a single value:
//   - the symmetric extension is computed only near the two ends; in between the samples are indexed directly;
//   - the two taps of a pair are added first and multiplied once (integer arithmetic: the exact sum does not depend on the order);
//   - 64-bit accumulation when it provably cannot overflow (2 x max|x| x sum|h| + rounding < 2^63, checked per call from the data),
//     otherwise 128-bit accumulation as in the reference;
//   - the samples of one step are split across threads (each output depends only on the input of that step).
#pragma once
#include <cstdint>
#include <vector>
#include <thread>
#include <algorithm>
#include <exception>
#include "halfband.h"
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif
namespace laesfast {   // fast form; the normative definition is laes::fwd / inv (src/laes_math.h)
typedef std::vector<int64_t> V;
static const int64_t kU0[60] = {683270116, -226970762, 135242270, -95599562, 73323768, -58947533, 48830814, -41276037, 35385490, -30640348, 26720250, -23416981, 20589580, -18139377, 15995343, -14105100, 12429177, -10937236, 9605506, -8415006, 7350282, -6398504, 5548802, -4791782, 4119170, -3523546, 2998153, -2536758, 2133556, -1783104, 1480274, -1220234, 998431, -810587, 652702, -521052, 412202, -322998, 250575, -192354, 146076, -109516, 81159, -59401, 42896, -30529, 21386, -14723, 9943, -6573, 4241, -2663, 1619, -948, 531, -281, 138, -61, 23, -6};
// 128-bit accumulator (portable: two's complement in two 64-bit words)
// signed 64 x 64 -> 128-bit product (low word, high word): x64 MSVC intrinsic, __int128 where the compiler has it, otherwise four 32 x 32
// partial products (MSVC x86 / ARM64, 32-bit GCC / Clang); all three give the same two's-complement result
inline void mul128(int64_t a, int64_t b, uint64_t& lo, int64_t& hi) {
#if defined(_MSC_VER) && defined(_M_X64) && !defined(LAESFAST_PORTABLE_MUL128)   // the macro forces the portable form (test)
    lo = (uint64_t)_mul128(a, b, &hi);
#elif defined(__SIZEOF_INT128__) && !defined(LAESFAST_PORTABLE_MUL128)
    __int128 p = (__int128)a * b; lo = (uint64_t)p; hi = (int64_t)(p >> 64);
#else
    const uint64_t ua = (uint64_t)a, ub = (uint64_t)b, a0 = ua & 0xFFFFFFFFu, a1 = ua >> 32, b0 = ub & 0xFFFFFFFFu, b1 = ub >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1, mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    lo = (p00 & 0xFFFFFFFFu) | (mid << 32); uint64_t h = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    if (a < 0) h -= ub; if (b < 0) h -= ua;   // unsigned product -> signed: subtract b * 2^64 for a < 0 and a * 2^64 for b < 0
    hi = (int64_t)h;
#endif
}
struct Acc128 { uint64_t lo = 0; int64_t hi = 0;
    void mac(int64_t a, int64_t b) { int64_t h; uint64_t l; mul128(a, b, l, h); uint64_t n = lo + l; hi += h + (n < lo ? 1 : 0); lo = n; }
    int64_t shr(int s) const { uint64_t add = 1ull << (s - 1), n = lo + add; int64_t h = hi + (n < lo ? 1 : 0); return (int64_t)((n >> s) | ((uint64_t)h << (64 - s))); } };
inline long mir(long i, long n) { if (n == 1) return 0; long p = 2 * (n - 1); i %= p; if (i < 0) i += p; return i < n ? i : p - i; }
inline int64_t maxAbs(const V& x) { int64_t m = 0; for (int64_t v : x) { int64_t a = v < 0 ? -v : v; if (a > m) m = a; } return m; }
template <int NT, class H> inline int64_t sumAbs(const H* h) { int64_t s = 0; for (int t = 0; t < NT; t++) s += h[t] < 0 ? -(int64_t)h[t] : (int64_t)h[t]; return s; }
// one filtered value: sum_t h[t] (x[a(t)] + x[b(t)]) with a(t) = i + oa - t, b(t) = i + ob + t, rounded by 2^sh
template <int NT, bool WIDE, class H> inline int64_t tap(const H* h, const int64_t* x, long n, long i, long oa, long ob, int sh) {
    const long a0 = i + oa - (NT - 1), b1 = i + ob + (NT - 1);
    if (a0 >= 0 && b1 < n) {   // interior: direct indexing
        const int64_t* pa = x + i + oa; const int64_t* pb = x + i + ob;
        if (!WIDE) { int64_t acc = 0; for (int t = 0; t < NT; t++) acc += (int64_t)h[t] * (pa[-t] + pb[t]); return (acc + (1LL << (sh - 1))) >> sh; }
        Acc128 A; for (int t = 0; t < NT; t++) { A.mac((int64_t)h[t], pa[-t]); A.mac((int64_t)h[t], pb[t]); } return A.shr(sh); }
    if (!WIDE) { int64_t acc = 0; for (int t = 0; t < NT; t++) acc += (int64_t)h[t] * (x[mir(i + oa - t, n)] + x[mir(i + ob + t, n)]); return (acc + (1LL << (sh - 1))) >> sh; }
    Acc128 A; for (int t = 0; t < NT; t++) { A.mac((int64_t)h[t], x[mir(i + oa - t, n)]); A.mac((int64_t)h[t], x[mir(i + ob + t, n)]); } return A.shr(sh); }
// 64-bit accumulation is exact when 2 x max|x| x sum|h| + 2^(sh-1) stays below 2^63 (no partial sum can exceed the bound either)
template <int NT, class H> inline bool fits64(const H* h, int64_t mx, int sh) {
    const double b = 2.0 * (double)mx * (double)sumAbs<NT, H>(h) + (double)(1LL << (sh - 1)); return b < 9.0e18; }   // 2^63 = 9.22e18
// splits [0, n) across threads; a chunk whose thread cannot be started runs here; an exception is re-thrown after all chunks have finished
template <class F> inline void parallelFor(long n, int threads, F f) {
    if (threads <= 1 || n < 65536) { f(0, n); return; }
    std::vector<std::thread> pool; std::vector<std::exception_ptr> xs(threads); std::vector<int> here; long step = (n + threads - 1) / threads;
    for (int k = 1; k < threads; k++) { long a = k * step, b = std::min(n, a + step); if (a >= b) continue;
        try { pool.emplace_back([&f, &xs, k, a, b]() { try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }); } catch (...) { here.push_back(k); } }
    try { f(0, std::min(n, step)); } catch (...) { xs[0] = std::current_exception(); }
    for (int k : here) { long a = k * step, b = std::min(n, a + step); try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }
    for (auto& t : pool) t.join(); for (auto& x : xs) if (x) std::rethrow_exception(x); }
inline int defaultThreads() { unsigned h = std::thread::hardware_concurrency(); return h ? (int)h : 1; }
// d[i] = xo[i] - P(xe, i)
inline void predictStep(const V& xe, const int64_t* xo, int64_t* d, long no, int sign, int threads) {
    const long n = (long)xe.size(); const bool w = !fits64<40>(lae::kHalf0, maxAbs(xe), 30);
    parallelFor(no, threads, [&](long a, long b) { for (long i = a; i < b; i++) {
        int64_t p = w ? tap<40, true>(lae::kHalf0, xe.data(), n, i, 0, 1, 30) : tap<40, false>(lae::kHalf0, xe.data(), n, i, 0, 1, 30);
        d[i] = sign > 0 ? xo[i] - p : xo[i] + p; } }); }
// s[i] = xe[i] + U(d, i)
inline void updateStep(const V& d, const int64_t* xe, int64_t* s, long ne, int sign, int threads) {
    const long n = (long)d.size(); if (n == 0) { for (long i = 0; i < ne; i++) s[i] = xe[i]; return; }
    const bool w = !fits64<60>(kU0, maxAbs(d), 31);
    parallelFor(ne, threads, [&](long a, long b) { for (long i = a; i < b; i++) {
        int64_t u = w ? tap<60, true>(kU0, d.data(), n, i, -1, 0, 31) : tap<60, false>(kU0, d.data(), n, i, -1, 0, 31);
        s[i] = sign > 0 ? xe[i] + u : xe[i] - u; } }); }
// one stage 2R -> R: s (low level, ceil(n/2)) and d (detail, floor(n/2))
inline void fwd(const V& x, V& s, V& d, int threads = defaultThreads()) {
    const long n = (long)x.size(), ne = (n + 1) / 2, no = n / 2; V xe(ne), xo(no);
    for (long i = 0; i < ne; i++) xe[i] = x[2 * i]; for (long i = 0; i < no; i++) xo[i] = x[2 * i + 1];
    d.assign(no, 0); s.assign(ne, 0); predictStep(xe, xo.data(), d.data(), no, +1, threads); updateStep(d, xe.data(), s.data(), ne, +1, threads); }
inline V inv(const V& s, const V& d, long n, int threads = defaultThreads()) {
    const long ne = (long)s.size(), no = (long)d.size(); V xe(ne), xo(no), x(n);
    updateStep(d, s.data(), xe.data(), ne, -1, threads); predictStep(xe, d.data(), xo.data(), no, -1, threads);
    for (long i = 0; i < ne; i++) x[2 * i] = xe[i]; for (long i = 0; i < no; i++) x[2 * i + 1] = xo[i]; return x; }
// the core reference on the master's grid (laes::reference in src/laes_math.h), with the taps h = laes::refTaps(frac) given by the caller:
// ref[i] = round(sum_q h[q] core[m(i + whole + q)] / 2^(31 - up)); same values, direct indexing inside, 64-bit when it cannot overflow
inline V reference(const V& core, int64_t whole, const std::vector<int64_t>& h, int up, int threads = defaultThreads()) {
    const long n = (long)core.size(); V r(n); const int s = 31 - up; int64_t sh = 0; for (int64_t v : h) sh += v < 0 ? -v : v;
    const bool w = !((double)maxAbs(core) * (double)sh + (s > 0 ? (double)(1LL << (s - 1)) : 0.0) < 9.0e18);
    parallelFor(n, threads, [&](long a, long b) { for (long i = a; i < b; i++) { const int64_t j0 = (int64_t)i + whole - 127, j1 = (int64_t)i + whole + 128;   // 64-bit: no wrap on 32-bit long
        if (!w) { int64_t acc = 0;
            if (j0 >= 0 && j1 < n) { const int64_t* c = core.data() + j0; for (int q = 0; q < 256; q++) acc += h[q] * c[q]; }
            else for (int q = -127; q <= 128; q++) acc += h[q + 127] * core[mir((long)(i + whole + q), n)];
            r[i] = s > 0 ? (acc + (1LL << (s - 1))) >> s : (int64_t)((uint64_t)acc << (-s)); }
        else { Acc128 A;
            if (j0 >= 0 && j1 < n) { const int64_t* c = core.data() + j0; for (int q = 0; q < 256; q++) A.mac(h[q], c[q]); }
            else for (int q = -127; q <= 128; q++) A.mac(h[q + 127], core[mir((long)(i + whole + q), n)]);
            r[i] = s > 0 ? A.shr(s) : (int64_t)(A.lo << (-s)); } } });
    return r; }
}   // namespace laesfast
