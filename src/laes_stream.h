// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES, block-wise processing (2026-10-30): the same values as laes_file.h / laes_lift.h / laes_codec.h, computed piece by piece so that
// memory does not grow with the length. Signals live in files of a work folder; a piece is a whole multiple of the format's coding blocks
// (4096 x 2^k samples) and every filter (lifting P: 40 taps, U: 60 taps, reference: 256 taps) reads a window around it, with the same
// symmetric extension at the ends (laes::mir over the whole length). Every output value is computed with the same integer operations as the
// whole-signal forms: 64-bit sums only where they provably cannot overflow (then they equal the 128-bit sums), 128-bit otherwise.
// Decoder side (Apache-2.0): file helpers, signal sources, lifting / reference pieces, the bit reader over a file and the streaming decoder.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include "laes_file.h"
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#endif
namespace laess {
// ---- exact sums of int32 x int32 products (2026-11-02): when every factor is below 2^31 in magnitude, an int64 product of two int32
// gives the same value as the int64 x int64 product, and the int64 sum is the same (the callers have already proved that the sum fits
// int64). Used on 32-bit x86 (no int64 multiply instruction) and, with AVX2 (4 products per instruction, chosen at run time), on x64 too.
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#define LAESS_AVX2 1
inline bool hasAvx2() { static const bool v = [] { int r[4]; __cpuid(r, 0); if (r[0] < 7) return false; __cpuid(r, 1);
        if (!((r[2] >> 27) & 1) || !((r[2] >> 28) & 1) || (_xgetbv(0) & 6) != 6) return false; __cpuidex(r, 7, 0); return ((r[1] >> 5) & 1) != 0; }(); return v; }
inline __m256i loadH4(const int32_t* p) { return _mm256_cvtepi32_epi64(_mm_loadu_si128((const __m128i*)p)); }
inline __m256i loadH4(const int64_t* p) { return _mm256_loadu_si256((const __m256i*)p); }
inline int64_t sum4(__m256i v) { alignas(32) int64_t s[4]; _mm256_store_si256((__m256i*)s, v); return s[0] + s[1] + s[2] + s[3]; }
#else
inline bool hasAvx2() { return false; }
#endif
inline bool int32Path() { return sizeof(void*) == 4 || hasAvx2(); }   // worth it on 32-bit builds, or with AVX2
using laes::V;
struct Fail : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] inline void fail(const std::string& m) { throw Fail(m); }
inline bool seekTo(FILE* f, long long at) {
#ifdef _WIN32
    return _fseeki64(f, at, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)at, SEEK_SET) == 0;
#endif
}
inline long long sizeOf(FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return -1; const long long n = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END)) return -1; const long long n = (long long)ftello(f);
#endif
    return n; }
inline FILE* openFile(const std::string& p, const char* mode) { FILE* f = fopen(p.c_str(), mode); if (!f) fail("cannot open " + p); return f; }
// a file written in order (a failed write throws; the caller's work folder is removed on failure)
struct Out { FILE* f = nullptr; std::string path; unsigned long long n = 0;
    explicit Out(const std::string& p) : path(p) { f = openFile(p, "wb"); }
    ~Out() { if (f) fclose(f); }
    Out(const Out&) = delete; Out& operator=(const Out&) = delete;
    void put(const void* d, size_t k) { if (k && fwrite(d, 1, k, f) != k) fail("write failed (disk full?): " + path); n += k; }
    void close() { if (!f) return; const bool ok = fclose(f) == 0; f = nullptr; if (!ok) fail("write failed (disk full?): " + path); } };
// a file read by byte ranges
struct In { FILE* f = nullptr; std::string path; long long n = 0;
    explicit In(const std::string& p) : path(p) { f = openFile(p, "rb"); n = sizeOf(f); if (n < 0) fail("cannot read the size of " + p); }
    ~In() { if (f) fclose(f); }
    In(const In&) = delete; In& operator=(const In&) = delete;
    void read(long long at, void* d, size_t k) { if (!k) return; if (at < 0 || at + (long long)k > n || !seekTo(f, at) || fread(d, 1, k, f) != k) fail("read failed: " + path); } };
// ---- signal sources: values [a, b) of a signal of n samples (int64)
struct Sig { int64_t n = 0; virtual void get(int64_t a, int64_t b, int64_t* out) = 0; virtual ~Sig() {} };
struct MemSig : Sig { const V* v; explicit MemSig(const V& x) : v(&x) { n = (int64_t)x.size(); }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); std::copy(v->begin() + a, v->begin() + b, out); } };
struct FileSig : Sig { In in; explicit FileSig(const std::string& p) : in(p) { if (in.n % 8) fail("bad signal file " + p); n = in.n / 8; }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); in.read(a * 8, out, (size_t)(b - a) * 8); } };
// one channel of an interleaved int32 stereo file (a master), shifted right by w
struct RawSig : Sig { In in; int c, w; std::vector<int32_t> buf;
    RawSig(const std::string& p, int ch, int sh) : in(p), c(ch), w(sh) { if (in.n % 8) fail("bad master file " + p); n = in.n / 8; }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); buf.resize((size_t)(b - a) * 2);
        in.read(a * 8, buf.data(), buf.size() * 4); for (int64_t i = a; i < b; i++) out[i - a] = (int64_t)buf[(size_t)(i - a) * 2 + c] >> w; } };
// int64 values written in order to a file
struct SigOut { Out o; explicit SigOut(const std::string& p) : o(p) {} void put(const int64_t* d, size_t k) { o.put(d, k * 8); } void close() { o.close(); } };
// values at the indices g0..g1 (inclusive) of s, each index mapped by the symmetric extension laes::mir over s.n
inline V window(Sig& s, int64_t g0, int64_t g1) {
    V w((size_t)(g1 - g0 + 1)); if (g1 < g0) return w;
    if (g0 >= 0 && g1 < s.n) { s.get(g0, g1 + 1, w.data()); return w; }
    int64_t lo = s.n, hi = -1; for (int64_t g = g0; g <= g1; g++) { const int64_t m = laes::mir(g, s.n); lo = std::min(lo, m); hi = std::max(hi, m); }
    V r((size_t)(hi - lo + 1)); s.get(lo, hi + 1, r.data()); for (int64_t g = g0; g <= g1; g++) w[(size_t)(g - g0)] = r[(size_t)(laes::mir(g, s.n) - lo)];
    return w; }
// the even / odd samples of x as signals of their own (xe: (n + 1) / 2, xo: n / 2)
struct HalfSig : Sig { Sig* x; int odd; HalfSig(Sig& s, int o) : x(&s), odd(o) { n = o ? s.n / 2 : (s.n + 1) / 2; }
    void get(int64_t a, int64_t b, int64_t* out) override { if (b <= a) return; const int64_t xa = 2 * a + odd, xb = 2 * (b - 1) + odd + 1; V t((size_t)(xb - xa)); x->get(xa, xb, t.data());
        for (int64_t i = a; i < b; i++) out[i - a] = t[(size_t)(2 * (i - a))]; } };
// ---- filter taps on a window (already extended): sum_t h[t] (w[ia - t] + w[ib + t]), rounded by 2^sh (add 2^(sh-1), arithmetic shift)
inline int64_t maxAbsOf(const V& x) { int64_t m = 0; for (int64_t v : x) { const int64_t a = v < 0 ? (v == INT64_MIN ? INT64_MAX : -v) : v;   /* FU13-3: INT64_MIN counts as the largest (wide path) */ if (a > m) m = a; } return m; }
template <int NT, class H> inline bool fits(const H* h, int64_t mx, int sh) { return laesfast::fits64<NT, H>(h, mx, sh); }
template <int NT, bool WIDE, class H> inline int64_t wtap(const H* h, const int64_t* w, int64_t ia, int64_t ib, int sh) {
    if (!WIDE) { int64_t acc = 0; for (int t = 0; t < NT; t++) acc += (int64_t)h[t] * (w[ia - t] + w[ib + t]); return (acc + (1LL << (sh - 1))) >> sh; }
    laesfast::Acc128 A; for (int t = 0; t < NT; t++) { A.mac((int64_t)h[t], w[ia - t]); A.mac((int64_t)h[t], w[ib + t]); } return A.shr(sh); }
// the same sum when every value of the window is below 2^30 in magnitude (the pair sums then fit int32 and so do the taps): products of
// two int32 (a single instruction on 32-bit x86, where an int64 product is a library call); the result equals wtap<NT, false>
template <int NT, class H> inline int64_t wtapN(const H* h, const int64_t* w, int64_t ia, int64_t ib, int sh) {
#ifdef LAESS_AVX2
    if (hasAvx2()) { static_assert(NT % 4 == 0, "tap count"); __m256i acc = _mm256_setzero_si256();
        for (int t = 0; t < NT; t += 4) {   // w[ia - t - 3 .. ia - t] reversed, plus w[ib + t .. ib + t + 3]
            const __m256i lo = _mm256_permute4x64_epi64(_mm256_loadu_si256((const __m256i*)(w + ia - t - 3)), 0x1B), hi = _mm256_loadu_si256((const __m256i*)(w + ib + t));
            acc = _mm256_add_epi64(acc, _mm256_mul_epi32(loadH4(h + t), _mm256_add_epi64(lo, hi))); }
        return (sum4(acc) + (1LL << (sh - 1))) >> sh; }
#endif
    int64_t acc = 0; for (int t = 0; t < NT; t++) acc += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)(w[ia - t] + w[ib + t]); return (acc + (1LL << (sh - 1))) >> sh; }
// round(2^-sh (ah 2^16 + al)) with the sum taken exactly in 128 bits (as laesfast::Acc128 rounds: add 2^(sh - 1), shift right)
inline int64_t comb16(int64_t ah, int64_t al, int sh) { laesfast::Acc128 A; A.lo = (uint64_t)ah << 16; A.hi = ah >> 48;
    const uint64_t n = A.lo + (uint64_t)al; A.hi += (al >> 63) + (n < A.lo ? 1 : 0); A.lo = n; return A.shr(sh); }
// the same sum for larger values (32-bit masters), every value below 2^45: each pair sum p (below 2^46) is split into p >> 16 and
// p & 0xFFFF, both int32, so h p = (h (p >> 16)) 2^16 + h (p & 0xFFFF) with int32 products; each of the two sums is exact in int64
// (below 2^30 x sum|h| x 2), and they are combined in 128 bits, so the result equals wtap<NT, true> and wtap<NT, false> alike.
template <int NT, class H> inline int64_t wtapS(const H* h, const int64_t* w, int64_t ia, int64_t ib, int sh) {
#ifdef LAESS_AVX2
    if (hasAvx2()) { __m256i ah = _mm256_setzero_si256(), al = _mm256_setzero_si256(); const __m256i m16 = _mm256_set1_epi64x(0xFFFF);
        for (int t = 0; t < NT; t += 4) {
            const __m256i p = _mm256_add_epi64(_mm256_permute4x64_epi64(_mm256_loadu_si256((const __m256i*)(w + ia - t - 3)), 0x1B), _mm256_loadu_si256((const __m256i*)(w + ib + t)));
            const __m256i hv = loadH4(h + t);   // the low 32 bits of p >> 16 (logical) are those of the arithmetic shift: bits 16..47 of p
            ah = _mm256_add_epi64(ah, _mm256_mul_epi32(hv, _mm256_srli_epi64(p, 16))); al = _mm256_add_epi64(al, _mm256_mul_epi32(hv, _mm256_and_si256(p, m16))); }
        return comb16(sum4(ah), sum4(al), sh); }
#endif
    int64_t ah = 0, al = 0; for (int t = 0; t < NT; t++) { const int64_t p = w[ia - t] + w[ib + t];
        ah += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)(p >> 16); al += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)(p & 0xFFFF); }
    return comb16(ah, al, sh); }
// ---- one lifting stage, a piece at a time
// forward (laesfast::fwd): x (n samples) -> s[a, b) (ne = (n + 1) / 2) and d[a, min(b, no)) (no = n / 2)
inline void fwdPiece(Sig& x, int64_t a, int64_t b, V& s, V& d, int threads) {
    const int64_t n = x.n, ne = (n + 1) / 2, no = n / 2; HalfSig xe(x, 0), xo(x, 1); s.assign((size_t)(b - a), 0); const int64_t db = std::min(b, no); d.assign((size_t)std::max<int64_t>(0, db - a), 0);
    if (no == 0) { xe.get(a, b, s.data()); return; }
    // the detail values the update needs: d at mir(g, no) for g in [a - 60, b + 58], and the piece's own d[a, db)
    int64_t lo = no, hi = -1; for (int64_t g = a - 60; g <= b + 58; g++) { const int64_t m = laes::mir(g, no); lo = std::min(lo, m); hi = std::max(hi, m); }
    if (a < db) { lo = std::min(lo, a); hi = std::max(hi, db - 1); }
    // d[lo..hi] = xo - P(xe), P reading xe[mir(i - 39 .. i + 40, ne)]
    const V xeW = window(xe, lo - 39, hi + 40); V xoW((size_t)(hi - lo + 1)); xo.get(lo, hi + 1, xoW.data()); V dv((size_t)(hi - lo + 1));
    { const int64_t mxw_ = maxAbsOf(xeW); const bool wide = !fits<40>(lae::kHalf0, mxw_, 30), narrow = int32Path() && mxw_ < (1LL << 30), split = int32Path() && mxw_ < (1LL << 45); const int64_t* w = xeW.data();
      laesfast::parallelFor((int64_t)(hi - lo + 1), threads, [&](int64_t p, int64_t q) { for (int64_t k = p; k < q; k++) { const int64_t i = lo + k, ia = i - (lo - 39), ib = ia + 1;
          const int64_t pv = !wide && narrow ? wtapN<40>(lae::kHalf0, w, ia, ib, 30) : split ? wtapS<40>(lae::kHalf0, w, ia, ib, 30) : wide ? wtap<40, true>(lae::kHalf0, w, ia, ib, 30) : wtap<40, false>(lae::kHalf0, w, ia, ib, 30); dv[(size_t)k] = xoW[(size_t)k] - pv; } }); }
    for (int64_t i = a; i < db; i++) d[(size_t)(i - a)] = dv[(size_t)(i - lo)];
    // s = xe + U(d), U reading d[mir(i - 60 .. i + 59, no)]
    V dW((size_t)(b + 58 - (a - 60) + 1)); for (int64_t g = a - 60; g <= b + 58; g++) dW[(size_t)(g - (a - 60))] = dv[(size_t)(laes::mir(g, no) - lo)];
    V xeA((size_t)(b - a)); xe.get(a, b, xeA.data());
    const int64_t mxw_ = maxAbsOf(dW); const bool wide = !fits<60>(laesfast::kU0, mxw_, 31), narrow = int32Path() && mxw_ < (1LL << 30), split = int32Path() && mxw_ < (1LL << 45); const int64_t* w = dW.data();
    laesfast::parallelFor((int64_t)(b - a), threads, [&](int64_t p, int64_t q) { for (int64_t k = p; k < q; k++) { const int64_t ia = (a + k) - 1 - (a - 60), ib = ia + 1;
        const int64_t u = !wide && narrow ? wtapN<60>(laesfast::kU0, w, ia, ib, 31) : split ? wtapS<60>(laesfast::kU0, w, ia, ib, 31) : wide ? wtap<60, true>(laesfast::kU0, w, ia, ib, 31) : wtap<60, false>(laesfast::kU0, w, ia, ib, 31); s[(size_t)k] = xeA[(size_t)k] + u; } }); }
// inverse (laesfast::inv): s (ne) and d (no), n = ne + no -> x[2a, min(2b, n)) for a piece [a, b) of the s indices
inline void invPiece(Sig& s, Sig& d, int64_t n, int64_t a, int64_t b, V& x, int threads) {
    const int64_t ne = s.n, no = d.n; const int64_t ob = std::min(b, no), x0 = 2 * a, x1 = std::min(2 * b, n); x.assign((size_t)std::max<int64_t>(0, x1 - x0), 0);
    if (no == 0) { V t((size_t)(b - a)); s.get(a, b, t.data()); for (int64_t i = a; i < b; i++) if (2 * i < n) x[(size_t)(2 * i - x0)] = t[(size_t)(i - a)]; return; }
    // xe at the indices the odd samples need: mir(g, ne) for g in [a - 39, ob - 1 + 40], and the piece's own xe[a, b)
    int64_t lo = ne, hi = -1; for (int64_t g = a - 39; g <= ob + 39; g++) { const int64_t m = laes::mir(g, ne); lo = std::min(lo, m); hi = std::max(hi, m); }
    lo = std::min(lo, a); hi = std::max(hi, b - 1);
    // xe[lo..hi] = s - U(d), U reading d[mir(i - 60 .. i + 59, no)]
    const V dW = window(d, lo - 60, hi + 59); V sW((size_t)(hi - lo + 1)); s.get(lo, hi + 1, sW.data()); V xev((size_t)(hi - lo + 1));
    { const int64_t mxw_ = maxAbsOf(dW); const bool wide = !fits<60>(laesfast::kU0, mxw_, 31), narrow = int32Path() && mxw_ < (1LL << 30), split = int32Path() && mxw_ < (1LL << 45); const int64_t* w = dW.data();
      laesfast::parallelFor((int64_t)(hi - lo + 1), threads, [&](int64_t p, int64_t q) { for (int64_t k = p; k < q; k++) { const int64_t i = lo + k, ia = i - 1 - (lo - 60), ib = ia + 1;
          const int64_t u = !wide && narrow ? wtapN<60>(laesfast::kU0, w, ia, ib, 31) : split ? wtapS<60>(laesfast::kU0, w, ia, ib, 31) : wide ? wtap<60, true>(laesfast::kU0, w, ia, ib, 31) : wtap<60, false>(laesfast::kU0, w, ia, ib, 31); xev[(size_t)k] = sW[(size_t)k] - u; } }); }
    for (int64_t i = a; i < b; i++) x[(size_t)(2 * i - x0)] = xev[(size_t)(i - lo)];
    if (a >= ob) return;
    // xo = d + P(xe), P reading xe[mir(i - 39 .. i + 40, ne)]
    V xeW((size_t)(ob + 39 - (a - 39) + 1)); for (int64_t g = a - 39; g <= ob + 39; g++) xeW[(size_t)(g - (a - 39))] = xev[(size_t)(laes::mir(g, ne) - lo)];
    V dA((size_t)(ob - a)); d.get(a, ob, dA.data());
    const int64_t mxw_ = maxAbsOf(xeW); const bool wide = !fits<40>(lae::kHalf0, mxw_, 30), narrow = int32Path() && mxw_ < (1LL << 30), split = int32Path() && mxw_ < (1LL << 45); const int64_t* w = xeW.data();
    laesfast::parallelFor((int64_t)(ob - a), threads, [&](int64_t p, int64_t q) { for (int64_t k = p; k < q; k++) { const int64_t ia = (a + k) - (a - 39), ib = ia + 1;
        const int64_t pv = !wide && narrow ? wtapN<40>(lae::kHalf0, w, ia, ib, 30) : split ? wtapS<40>(lae::kHalf0, w, ia, ib, 30) : wide ? wtap<40, true>(lae::kHalf0, w, ia, ib, 30) : wtap<40, false>(lae::kHalf0, w, ia, ib, 30); x[(size_t)(2 * (a + k) + 1 - x0)] = dA[(size_t)k] + pv; } }); }
// the reference (laes::refLen) for the output indices [a, b): r[i] = round(sum_q h[q] src[mir(i + whole + q, n)] / 2^(31 - up))
inline void refPiece(Sig& src, const std::vector<int64_t>& h, int64_t whole, int up, int64_t a, int64_t b, V& r, int threads) {
    r.assign((size_t)(b - a), 0); if (b <= a) return; const int64_t g0 = a + whole - 127; const V w = window(src, g0, b - 1 + whole + 128);
    // a whole-sample offset (frac = 0): the taps are 2^31 at one place and 0 elsewhere, so r[i] = round(src[i + whole] * 2^up), computed
    // directly (the same value as the sum below: 2^31 * v is a multiple of 2^(31 - up) when up >= 0)
    { int c = -1, nz = 0; for (int t = 0; t < 256; t++) if (h[(size_t)t]) { nz++; c = t; }
      if (nz == 1 && h[(size_t)c] == (1LL << 31)) { for (int64_t k = 0; k < b - a; k++) { const int64_t v = w[(size_t)(k + c)];
              r[(size_t)k] = up >= 0 ? (int64_t)((uint64_t)v << up) : (v + (1LL << (-up - 1))) >> (-up); }
          return; } }
    const int s = 31 - up; int64_t sh = 0; for (int64_t v : h) sh += v < 0 ? -v : v;
    const int64_t mxw = maxAbsOf(w); const bool wide = !((double)mxw * (double)sh + (s > 0 ? (double)(1LL << (s - 1)) : 0.0) < 9.0e18);
    int64_t mh = 0; for (int64_t v : h) mh = std::max(mh, v < 0 ? -v : v);
    const bool n32 = int32Path() && mxw < (1LL << 31) && mh < (1LL << 31);   // every factor fits int32: int32 products (AVX2 when present)
    // larger samples (below 2^47, e.g. a 32-bit master's levels): each split into c >> 16 and c & 0xFFFF (both int32); the two sums are
    // exact in int64 (sum|h| x 2^31 at most) and combined in 128 bits, so the result equals both the 64-bit and the 128-bit form below
    const bool s48 = !(!wide && n32) && int32Path() && mxw < (1LL << 47) && mh < (1LL << 31) && (double)sh * (double)((mxw >> 16) + 1) < 9.0e18;
    laesfast::parallelFor((int64_t)(b - a), threads, [&](int64_t p, int64_t q) { for (int64_t k = p; k < q; k++) { const int64_t* c = w.data() + k;   // c[q + 127] = src at (a + k) + whole + q
        if (s48) { int64_t ah = 0, al = 0;
#ifdef LAESS_AVX2
            if (hasAvx2()) { __m256i vh = _mm256_setzero_si256(), vl = _mm256_setzero_si256(); const __m256i m16 = _mm256_set1_epi64x(0xFFFF);
                for (int t = 0; t < 256; t += 4) { const __m256i hv = loadH4(h.data() + t), x = _mm256_loadu_si256((const __m256i*)(c + t));
                    vh = _mm256_add_epi64(vh, _mm256_mul_epi32(hv, _mm256_srli_epi64(x, 16))); vl = _mm256_add_epi64(vl, _mm256_mul_epi32(hv, _mm256_and_si256(x, m16))); }
                ah = sum4(vh); al = sum4(vl); }
            else
#endif
            for (int t = 0; t < 256; t++) { ah += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)(c[t] >> 16); al += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)(c[t] & 0xFFFF); }
            laes::Acc A; A.lo = (uint64_t)ah << 16; A.hi = ah >> 48; const uint64_t nlo = A.lo + (uint64_t)al; A.hi += (al >> 63) + (nlo < A.lo ? 1 : 0); A.lo = nlo;
            r[(size_t)k] = s > 0 ? A.shr(s) : (int64_t)((uint64_t)A.lo << (-s)); }
        else if (!wide) { int64_t acc = 0;
            if (n32) {
#ifdef LAESS_AVX2
                if (hasAvx2()) { __m256i v = _mm256_setzero_si256(); for (int t = 0; t < 256; t += 4) v = _mm256_add_epi64(v, _mm256_mul_epi32(loadH4(h.data() + t), _mm256_loadu_si256((const __m256i*)(c + t)))); acc = sum4(v); }
                else
#endif
                for (int t = 0; t < 256; t++) acc += (int64_t)(int32_t)h[t] * (int64_t)(int32_t)c[t]; }
            else for (int t = 0; t < 256; t++) acc += h[t] * c[t];
            r[(size_t)k] = s > 0 ? (acc + (1LL << (s - 1))) >> s : (int64_t)((uint64_t)acc << (-s)); }
        else { laes::Acc A; for (int t = 0; t < 256; t++) A.mac(h[t], c[t]); r[(size_t)k] = s > 0 ? A.shr(s) : (int64_t)((uint64_t)A.lo << (-s)); } } }); }
// ---- the bit reader of laes_codec.h over a byte range of a file, read through a window
struct BitRF { In* in = nullptr; long long off = 0, len = 0; uint64_t pos = 0;   // bit counts in 64 bits: a layer may hold 4 GiB (FU13-1)
    bool fail = false; std::vector<uint8_t> w; long long wb = -1;
    uint64_t left() const { return (uint64_t)len * 8 - pos; }
    uint8_t byteAt(uint64_t k) { if ((long long)k < wb || (long long)k >= wb + (long long)w.size()) { wb = (long long)k; const long long e = std::min(len, wb + (1LL << 22)); w.resize((size_t)(e - wb)); in->read(off + wb, w.data(), w.size()); }
        return w[(size_t)(k - (uint64_t)wb)]; }
    uint64_t get(int n) { if (fail || n < 0 || n > 64 || (uint64_t)n > left()) { fail = true; return 0; } uint64_t v = 0;
        while (n > 0) { const int bp = (int)(pos & 7), avail = 8 - bp, take = std::min(n, avail);
            v = (take == 64 ? 0 : (v << take)) | (uint64_t)((byteAt(pos >> 3) >> (avail - take)) & ((1u << take) - 1)); pos += (uint64_t)take; n -= take; }
        return v; }
    int64_t getS(int n) { if (n < 1 || n > 63) { fail = true; return 0; } uint64_t v = get(n); if (v >> (n - 1) & 1) v |= ~0ull << n; return (int64_t)v; } };
// a layer (n values, blocks of B) decoded from a byte range of a file into a signal file
inline bool decodeLayerToFile(In& in, long long off, long long len, int64_t n, int64_t B, const std::string& out) {
    BitRF rd; rd.in = &in; rd.off = off; rd.len = len; SigOut o(out); V x;
    for (int64_t a = 0; a < n; a += B) { const int64_t k = (int64_t)std::min<int64_t>(B, n - a); x.resize((size_t)k); if (!laes::decodeBlock(rd, x.data(), k)) return false; o.put(x.data(), x.size()); }
    o.close(); return true; }
// ---- block-wise decoder (laes::decodeLaes, piece by piece): the payload is a file; every decoded master is written as two channel files
// of its samples >> w (top level) and its lower levels (for the masters that refer to it). piece = base-level samples per piece (a multiple
// of 4096; level k uses piece << k).
struct DecMaster { laes::MasterInfo I; std::string top[2]; std::vector<std::string> lev[2]; bool shaOk = false; };
inline std::unique_ptr<Sig> levelOf(const DecMaster& m, int c, int k) { return std::unique_ptr<Sig>(new FileSig(k >= m.I.stages ? m.top[c] : m.lev[c][(size_t)k])); }
// SHA-256 of a master given as two channel files of samples >> w (as laes::hashPcm of the samples); returns false when a sample lies
// outside the master's word length (bits), which fails the master whatever the hash says (FU12-7 / FU13-4)
inline bool hashFiles(const std::string& c0, const std::string& c1, int w, int bits, uint8_t out[32]) {
    FileSig a(c0), b(c1); laes::Sha256 s; V x, y; std::vector<uint8_t> buf; const int64_t P = 1 << 20; bool inw = true;
    for (int64_t i = 0; i < a.n; i += P) { const int64_t e = std::min(a.n, i + P); x.resize((size_t)(e - i)); y.resize((size_t)(e - i)); a.get(i, e, x.data()); b.get(i, e, y.data()); buf.clear();
        for (int64_t k = 0; k < e - i; k++) for (const int64_t v0 : {x[(size_t)k], y[(size_t)k]}) { inw = inw && laes::inWord(v0 << w, bits); const uint32_t v = (uint32_t)(int32_t)(v0 << w); for (int q = 0; q < 4; q++) buf.push_back((uint8_t)(v >> (8 * q))); }
        s.update(buf.data(), buf.size()); }
    s.final(out); return inw; }
// the levels below the top of one channel: lev[k] for k = stages - 1 .. 0, each from the level above by the forward stage
inline void levelsDown(const std::string& top, int stages, std::vector<std::string>& lev, const std::string& stem, int64_t piece, int threads) {
    lev.assign((size_t)stages, std::string());
    for (int k = stages; k > 0; k--) { std::unique_ptr<Sig> x(new FileSig(k == stages ? top : lev[(size_t)k])); const int64_t ne = (x->n + 1) / 2; lev[(size_t)(k - 1)] = stem + "_L" + std::to_string(k - 1);
        SigOut o(lev[(size_t)(k - 1)]); V s, d; const int64_t P = piece << (k - 1);
        for (int64_t a = 0; a < ne; a += P) { fwdPiece(*x, a, std::min(ne, a + P), s, d, threads); o.put(s.data(), s.size()); } o.close(); } }
inline std::string decodeLaesS(const std::string& bodyPath, const laes::Pcm& core, int baseRate, const std::string& wd, std::vector<DecMaster>& out, int64_t piece, int threads, laes::Head* head = nullptr) {
    using namespace laes; out.clear(); if (head) *head = Head(); In body(bodyPath); if (body.n < 64 + 4) return "LAES block too short";
    { uint32_t crc = 0xFFFFFFFFu; std::vector<uint8_t> b; const long long last = body.n - 4;
      for (long long a = 0; a < last; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, last - a)); body.read(a, b.data(), b.size()); for (uint8_t by : b) { crc ^= by; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } }
      crc ^= 0xFFFFFFFFu; uint8_t t[4]; body.read(last, t, 4); const uint32_t stored = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24; if (crc != stored) return "LAES block damaged (CRC-32)"; }
    const long long end = body.n - 4; long long pos = 0; bool trunc = false;
    auto u = [&](int k) -> uint64_t { if (pos + k > end) { trunc = true; return 0; } uint8_t t[8]; body.read(pos, t, (size_t)k); pos += k; uint64_t v = 0; for (int i = 0; i < k; i++) v |= (uint64_t)t[i] << (8 * i); return v; };
    uint64_t magic = u(4), ver = u(2), hs = u(2), base = u(4), cbits = u(1), ch = u(1), nm = u(1), cmode = u(1), nc = u(8); uint8_t csha[32] = {0}; if (pos + 32 <= end) { body.read(pos, csha, 32); pos += 32; } else trunc = true;
    uint8_t hb[8] = {0}; if (pos + 8 <= end) { body.read(pos, hb, 8); pos += 8; } else trunc = true;
    if (trunc || magic != 0x3245414Cull) return "not an LAES block"; if ((ver != 1 && ver != 2) || hs != 64) return "unsupported LAES version";
    Head hd; if ((base != 44100 && base != 48000) || (int)base != baseRate || (int)cbits != core.bits || ch != 2 || cmode > 1 || !headerCountOk(ver, nm) || !headFrom(ver, hb, nm, hd)) return "invalid LAES header";
    if (head) *head = hd;
    if (nc != (uint64_t)core.frames()) return "core master length differs from the LAES header";
    { uint8_t h[32]; hashAnyCore(core, h); if (memcmp(h, csha, 32)) return "core master differs from the one this LAES block was made for (SHA-256)"; }
    for (uint64_t mi = 0; mi < nm; mi++) { DecMaster D; MasterInfo& I = D.I;
        I.bits = (int)u(1); I.stages = (int)u(1); I.w = (int)u(1); I.storage = (int)u(1); I.refTier = (int)u(1);
        const uint64_t nl = u(1), fl = u(1), mp = u(1); I.rate = (int)u(4); u(4); const uint64_t F64 = u(8);
        I.whole = (int64_t)u(8); I.frac = (uint32_t)u(4); u(4); uint8_t sha[32] = {0}; if (pos + 32 <= end) { body.read(pos, sha, 32); pos += 32; } else trunc = true;
        if (trunc) return "LAES block truncated";
        if (ver == 2) { if (pos + (long long)nl > end) return "LAES block truncated"; std::string nm2((size_t)nl, '\0'); if (nl) body.read(pos, &nm2[0], (size_t)nl); pos += (long long)nl;
            if (fl & ~(uint64_t)kNoDerive) return "invalid master record"; if (!validMasterName((const uint8_t*)nm2.data(), nm2.size())) return "invalid master name"; I.name = nm2; I.flags = (int)fl; I.menu = (int)mp; }
        if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int)mi) || (I.storage == 2 && I.refTier < 1)
            || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 < 1 || ((F64 + (1ull << I.stages) - 1) >> I.stages) > nc + (1ull << 24)
            || I.whole <= -(1LL << 40) || I.whole >= (1LL << 40)) return "invalid master record";
        if (F64 > (uint64_t)INT64_MAX) return "master too long for this decoder"; I.frames = (int64_t)F64;   // frame counts are int64_t on every platform (32 and 64 bit)
        memcpy(I.sha, sha, 32); const int j = I.storage == 2 ? std::min(I.stages, out[(size_t)I.refTier - 1].I.stages) : 0; const int64_t nj = levLen(I.frames, I.stages - j);
        const std::string stem = wd + "/d" + std::to_string(mi);
        for (int c = 0; c < 2; c++) { const std::string sc = stem + "_c" + std::to_string(c);
            const uint32_t eb = (uint32_t)u(4); if (trunc || pos + eb > end) return "LAES block truncated";
            const std::string xj = sc + "_x" + std::to_string(j);
            if (!decodeLayerToFile(body, pos, eb, nj, 4096L << j, I.storage != 1 ? sc + "_e" : xj)) return "damaged base layer"; pos += eb;
            if (I.storage != 1) {   // level j = layer + the reference of the tier (laes::refLen)
                std::unique_ptr<Sig> src; int up; if (I.refTier == 0) { src.reset(new MemSig(core.c[c])); up = (I.bits - I.w) - core.bits; }
                else { src = levelOf(out[(size_t)I.refTier - 1], c, j); const MasterInfo& T = out[(size_t)I.refTier - 1].I; up = (I.bits - I.w) - (T.bits - T.w); }
                const int64_t wj = I.whole * (1LL << j) + (j ? (int64_t)(I.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)I.frac << j);
                const std::vector<int64_t> h = refTaps(fj); { FileSig e(sc + "_e"); SigOut o(xj); V ev, rv; const int64_t P = piece << j;
                    for (int64_t a = 0; a < nj; a += P) { const int64_t b = std::min<int64_t>(nj, a + P); ev.resize((size_t)(b - a)); e.get(a, b, ev.data()); refPiece(*src, h, wj, up, a, b, rv, threads);
                        for (size_t i = 0; i < ev.size(); i++) ev[i] += rv[i]; o.put(ev.data(), ev.size()); } o.close(); }
                remove((sc + "_e").c_str()); }
            std::string cur = xj;
            for (int k = j + 1; k <= I.stages; k++) { const int64_t nh = levLen(I.frames, I.stages - k); const uint32_t db = (uint32_t)u(4); if (trunc || pos + db > end) return "LAES block truncated";
                const std::string dk = sc + "_d" + std::to_string(k); if (!decodeLayerToFile(body, pos, db, nh / 2, 4096L << (k - 1), dk)) return "damaged octave layer"; pos += db;
                const std::string nx = sc + "_x" + std::to_string(k);
                { FileSig s(cur), d(dk); if (s.n + d.n != nh) return "damaged octave layer"; SigOut o(nx); V x; const int64_t P = piece << (k - 1);
                  for (int64_t a = 0; a < s.n; a += P) { invPiece(s, d, nh, a, std::min(s.n, a + P), x, threads); o.put(x.data(), x.size()); } o.close(); }
                remove(dk.c_str()); remove(cur.c_str()); cur = nx; }
            D.top[c] = cur; }
        uint8_t h[32]; const bool inw = hashFiles(D.top[0], D.top[1], I.w, I.bits, h); D.shaOk = inw && memcmp(h, I.sha, 32) == 0;
        for (int c = 0; c < 2; c++) levelsDown(D.top[c], I.stages, D.lev[c], stem + "_c" + std::to_string(c) + "_lv", piece, threads);
        out.push_back(D); }
    { std::vector<laes::MasterInfo> mi; for (const auto& D : out) mi.push_back(D.I); if (!laes::menuOk(mi)) return "invalid menu positions"; }
    if (pos != end) return "unexpected bytes after the last master";
    return ""; }
}  // namespace laess
