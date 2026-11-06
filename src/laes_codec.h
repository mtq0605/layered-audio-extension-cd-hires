// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES layer coder, block syntax and decoder (S2 draft, 2026-10-28; the encoder side is in laes_codec_enc.h, GPL / commercial). One channel of one layer = a sequence of blocks; each block is coded on its own:
//   block header (fixed-length fields only):
//     type 2 bits: 0 = constant, 1 = verbatim, 2 = fixed predictor, 3 = LPC
//     W 6 bits: sample width (two's complement bits needed for every sample of the block, 1..62; fixed only if W <= 58, LPC only if W <= 44)
//     constant: one W-bit value
//     verbatim: n W-bit values
//     fixed:    order 3 bits (0..4); order W-bit warm-up samples; residual
//     LPC:      order-1 5 bits (1..32); precision-1 4 bits (coefficient bits 1..16); shift 5 bits (0..31);
//               order coefficients (precision bits, two's complement); order W-bit warm-up samples; residual
//   residual: partition order 4 bits (0..8, 2^po partitions; block length must be divisible or the last partition takes the rest);
//             per partition a 6-bit Rice parameter k (0..62); k = 63 = escape (all fields fixed-length): 6-bit raw width r, then r-bit two's complement values;
//             Rice: zigzag u = 2v (v >= 0) or -2v-1 (v < 0); unary q = u >> k (q zeros then a one), then k low bits.
//   prediction: fixed (FLAC orders 0..4) or LPC: pred = floor( sum_j c[j] * x[i-1-j] / 2^shift ) (128-bit accumulation), residual = x - pred.
// Every channel is coded on its own (no inter-channel decorrelation).
// Speed-up (2026-10-29): same choices and the same bits as before (tests/laes/laes_codec_test.cpp compares with the former coder):
//   - bit writer / reader move up to 8 bits at a time instead of one;
//   - the Rice parameter search computes the five candidate costs in one pass (integer sums: the former double sums of integers were exact);
//   - LPC prediction in 64 bits when sum|c| x max|x| proves the sum cannot overflow, otherwise the 128-bit accumulator as before;
//   - encodeLayer chooses the parameters of the blocks in parallel threads and writes them in order.
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <exception>
#include "laes_math.h"
namespace laes {
struct BitR { const std::vector<uint8_t>* b = nullptr; size_t pos = 0; bool fail = false;
    size_t left() const { return b->size() * 8 - pos; }
    uint64_t get(int w) { if (fail || w < 0 || w > 64 || (size_t)w > left()) { fail = true; return 0; } uint64_t v = 0;
        while (w > 0) { const int bp = (int)(pos & 7), avail = 8 - bp, take = std::min(w, avail);
            v = (take == 64 ? 0 : (v << take)) | (uint64_t)(((*b)[pos >> 3] >> (avail - take)) & ((1u << take) - 1)); pos += (size_t)take; w -= take; }
        return v; }
    int64_t getS(int w) { if (w < 1 || w > 63) { fail = true; return 0; } uint64_t v = get(w); if (v >> (w - 1) & 1) v |= ~0ull << w; return (int64_t)v; } };
// value ranges (normative): W (sample width) 1..62; fixed predictors only for W <= 58, LPC only for W <= 44 (no int64 overflow anywhere);
// Rice parameter 6 bits 0..62, 63 = escape; escape width 1..63; partition order 0..8 with 2^po <= number of residuals
const int kMaxW = 62, kMaxWFixed = 58, kMaxWLpc = 44;
inline int widthOf(int64_t v) { int w = 1; while (w < 64 && !(v >= -(1LL << (w - 1)) && v < (1LL << (w - 1)))) w++; return w; }
inline uint64_t zz(int64_t v) { return v >= 0 ? (uint64_t)v << 1 : (((uint64_t)(-(v + 1))) << 1) | 1; }
inline int64_t unzz(uint64_t u) { return (u & 1) ? -(int64_t)(u >> 1) - 1 : (int64_t)(u >> 1); }
// R: BitR, or any reader with get / getS / fail (laes_stream.h, 2026-10-30)
// the quotient of a Rice code: the zeros before the next 1 (consumed too); fails when it exceeds qmax or the data ends. A reader with a
// unary(qmax) of its own (laesp::BitRS: a word at a time) uses that; others read bit by bit. Both give the same value and position.
template <class R> inline auto unaryOf(R& rd, uint64_t qmax, int) -> decltype(rd.unary(qmax)) { return rd.unary(qmax); }
template <class R> inline uint64_t unaryOf(R& rd, uint64_t qmax, int64_t) { uint64_t qv = 0; while (!rd.fail && rd.get(1) == 0) { if (++qv > qmax) rd.fail = true; } return qv; }
template <class R> inline void getResidual(R& rd, int64_t* r, int64_t n) { int po = (int)rd.get(4); if (n < 1 || po > 8 || (1LL << po) > n) { rd.fail = true; return; } int64_t parts = 1LL << po, len = n / parts;
    for (int64_t q = 0; q < parts && !rd.fail; q++) { int64_t a = q * len, e = (q == parts - 1) ? n : a + len; int k = (int)rd.get(6);
        if (k == 63) { int rw = (int)rd.get(6); if (rw < 1 || rw > 63) { rd.fail = true; return; } for (int64_t i = a; i < e; i++) r[i] = rd.getS(rw); }
        else for (int64_t i = a; i < e && !rd.fail; i++) { uint64_t qv = 0; const uint64_t qmax = k ? (~0ull >> k) : ~0ull;   // u = q * 2^k + low must fit 64 bits
            qv = unaryOf(rd, qmax, 0);
            uint64_t u = (qv << k) | (k ? rd.get(k) : 0); r[i] = unzz(u); } } }
// ---- predictors (ranges above keep every intermediate inside int64)
inline int64_t fixedPred(const int64_t* x, int64_t i, int ord) { switch (ord) { case 0: return 0; case 1: return x[i - 1]; case 2: return 2 * x[i - 1] - x[i - 2];
        case 3: return 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; default: return 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; } }
inline int64_t lpcPred(const int64_t* x, int64_t i, const std::vector<int64_t>& c, int shift) { Acc a; for (size_t j = 0; j < c.size(); j++) a.mac(c[j], x[i - 1 - (int64_t)j]);
    if (shift == 0) return (int64_t)a.lo; Acc b = a; b.lo -= (1ull << (shift - 1)); if (b.lo > a.lo) b.hi--; return b.shr(shift); }   // floor: shr rounds, so subtract half first
// the same value in 64 bits; only when sum|c| x max|x| < 2^62, so no partial sum overflows (then floor = arithmetic shift right)
inline int64_t lpcPred64(const int64_t* x, int64_t i, const int64_t* c, int o, int shift) { int64_t a = 0; for (int j = 0; j < o; j++) a += c[j] * x[i - 1 - j]; return a >> shift; }
// the same prediction from int32 x int32 products when every coefficient and every sample is below 2^31 in magnitude (each product is
// then the same int64 value; the sum is the int64 sum of lpcPred64): one instruction per product on 32-bit x86 instead of a library call
inline int64_t lpcPred32(const int64_t* x, int64_t i, const int64_t* c, int o, int shift) { int64_t a = 0; for (int j = 0; j < o; j++) a += (int64_t)(int32_t)c[j] * (int64_t)(int32_t)x[i - 1 - j]; return a >> shift; }
// the same for samples below 2^47: each split into x >> 16 and x & 0xFFFF (both int32), the two sums combined modulo 2^64 (exact, as the
// int64 sum of lpcPred64 fits); 32-bit builds
inline int64_t lpcPredS(const int64_t* x, int64_t i, const int64_t* c, int o, int shift) { uint64_t ah = 0, al = 0;
    for (int j = 0; j < o; j++) { const int64_t v = x[i - 1 - j]; ah += (uint64_t)((int64_t)(int32_t)c[j] * (int64_t)(int32_t)(v >> 16)); al += (uint64_t)((int64_t)(int32_t)c[j] * (int64_t)(int32_t)(v & 0xFFFF)); }
    return (int64_t)((ah << 16) + al) >> shift; }
inline bool lpcFits64(const std::vector<int64_t>& c, uint64_t maxAbsX) { double s = 0; for (int64_t v : c) s += std::fabs((double)v); return s * (double)maxAbsX < 4.0e18; }   // 2^62 = 4.61e18
inline uint64_t absMax(const int64_t* x, int64_t n) { uint64_t m = 0; for (int64_t i = 0; i < n; i++) { const uint64_t a = x[i] < 0 ? (uint64_t)(-(x[i] + 1)) + 1 : (uint64_t)x[i]; m = std::max(m, a); } return m; }
template <class R> inline bool decodeBlock(R& rd, int64_t* x, int64_t n) {   // returns false on any value outside the normative ranges or a short buffer
    int type = (int)rd.get(2), W = (int)rd.get(6); if (rd.fail || W < 1 || W > kMaxW) return false;
    if (type == 0) { int64_t v = rd.getS(W); for (int64_t i = 0; i < n; i++) x[i] = v; return !rd.fail; }
    if (type == 1) { for (int64_t i = 0; i < n && !rd.fail; i++) x[i] = rd.getS(W); return !rd.fail; }
    if (type == 2) { int o = (int)rd.get(3); if (rd.fail || W > kMaxWFixed || o > 4 || o >= n) return false;
        for (int i = 0; i < o; i++) x[i] = rd.getS(W); std::vector<int64_t> r(n); getResidual(rd, r.data() + o, n - o); if (rd.fail) return false;
        for (int64_t i = o; i < n; i++) x[i] = r[i] + fixedPred(x, i, o); return true; }
    int o = (int)rd.get(5) + 1, prec = (int)rd.get(4) + 1, shift = (int)rd.get(5); if (rd.fail || W > kMaxWLpc || o >= n) return false;
    std::vector<int64_t> c(o); for (int j = 0; j < o; j++) c[j] = rd.getS(prec);
    for (int i = 0; i < o; i++) x[i] = rd.getS(W); std::vector<int64_t> r(n); getResidual(rd, r.data() + o, n - o); if (rd.fail) return false;
    // 64-bit prediction only while sum|c| x (largest |x| decoded so far) proves it exact; the bound is updated sample by sample
    double sc = 0; for (int64_t v : c) sc += std::fabs((double)v); uint64_t mx = absMax(x, o);
    const bool c32 = sizeof(void*) == 4 && [&] { for (int64_t v : c) if (v <= -(1LL << 31) || v >= (1LL << 31)) return false; return true; }();
    for (int64_t i = o; i < n; i++) { x[i] = r[i] + ((sc * (double)mx < 4.0e18) ? (c32 && mx < (1ull << 31) ? lpcPred32(x, i, c.data(), o, shift) : c32 && mx < (1ull << 47) ? lpcPredS(x, i, c.data(), o, shift) : lpcPred64(x, i, c.data(), o, shift)) : lpcPred(x, i, c, shift));
        const uint64_t a = x[i] < 0 ? (uint64_t)(-(x[i] + 1)) + 1 : (uint64_t)x[i]; if (a > mx) mx = a; }
    return true; }
inline V decodeLayer(const std::vector<uint8_t>& b, int64_t n, int64_t B, bool& ok) { V x(n); BitR rd; rd.b = &b; ok = true;
    for (int64_t a = 0; a < n && ok; a += B) ok = decodeBlock(rd, x.data() + a, (int64_t)std::min<int64_t>(B, n - a)); return x; }
}  // namespace laes
