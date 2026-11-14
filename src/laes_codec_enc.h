// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAES layer coder, encoder side (split from laes_codec.h, 2026-10-29; the code is unchanged). The block syntax and the decoder are in
// laes_codec.h (Apache-2.0). Here: the bit writer, the residual cost and Rice parameter search, the block plan (predictor, order,
// precision, shift, partition), the block writer and encodeLayer (block parameters chosen in parallel threads, written in order).
#pragma once
#include "laes_codec.h"
#include "lae_math.h"   // log2 / cospi identical on every platform (the coder's choices follow from them; 2026-11-03)
#if defined(_MSC_VER)
#include <intrin.h>
#endif
namespace laes {
struct BitW { std::vector<uint8_t> b; uint64_t acc = 0; int n = 0; size_t bits = 0;
    void put(uint64_t v, int w) {   // the w low bits of v, most significant first; w <= 64
        if (w <= 0) return; bits += (size_t)w;
        while (w > 0) { const int take = std::min(w, 8 - n); acc = (acc << take) | ((v >> (w - take)) & ((1u << take) - 1)); n += take; w -= take;
            if (n == 8) { b.push_back((uint8_t)acc); acc = 0; n = 0; } } }
    void zeros(uint64_t q) { while (q > 0) { const int t = (int)std::min<uint64_t>(q, 32); put(0, t); q -= (uint64_t)t; } }
    void putS(int64_t v, int w) { put((uint64_t)v & ((1ull << w) - 1), w); }   // w <= 63
    void flush() { while (n) put(0, 1); } };
// ---- residual coding
// bit length of u (0 for 0). widthOf(v) == max(1, bitLen(zz(v))) for every int64 v, so the width of a partition is max(1, bitLen(OR of its zz values))
inline int bitLen(uint64_t u) {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long i; return _BitScanReverse64(&i, u) ? (int)i + 1 : 0;
#elif defined(__GNUC__) || defined(__clang__)
    return u ? 64 - __builtin_clzll(u) : 0;
#else
    int b = 0; while (u) { b++; u >>= 1; } return b;
#endif
}
inline double riceCostU(const uint64_t* u, int64_t n, int k) { double b = 6; for (int64_t i = 0; i < n; i++) b += (double)(u[i] >> k) + 1 + k; return b; }
// the Rice parameter of one partition, from its zz values: start from the mean, then test neighbours exactly; escape compared too.
// L5 (2026-11-03): the same choice and cost as before, computed faster. The mean is the double sum of the values in order; while every
// partial sum is an integer below 2^53 that sum is exact and equals the integer sum, so the integer sum is used then (otherwise the double
// loop as before). The five costs 6 + sum(u >> k) + n (1 + k) are exact integers; sum(u >> k) <= sum(u) >> klo, so when the integer sum is at
// most 2^52 the former check "s[0] > 2^52" can never fire and the branch-free loop gives the same sums (otherwise the former loop runs).
inline int bestKu(const uint64_t* u, int64_t n, double& cost) {
    uint64_t sum = 0, orv = 0; for (int64_t i = 0; i < n; i++) { sum += u[i]; orv |= u[i]; }
    const int rw = std::max(1, bitLen(orv)); const uint64_t lim = 1ull << 52;
    const bool small = rw + bitLen((uint64_t)n) <= 63 && sum <= lim;   // no wrap-around in sum, and sum <= 2^52 < 2^53
    double mean = 0; if (small) mean = (double)sum; else for (int64_t i = 0; i < n; i++) mean += (double)u[i];
    mean = n ? mean / n : 0;
    int k0 = mean > 1 ? (int)std::floor(lmath::log2(mean)) : 0; int best = 0; cost = 1e300;
    const int klo = std::max(0, k0 - 2), khi = std::min(62, k0 + 2);
    uint64_t s[5] = {0, 0, 0, 0, 0}; bool exact = true;
    if (small) { const int nk = khi - klo + 1; for (int k = 0; k < nk; k++) { const int sh = klo + k; uint64_t t = 0; for (int64_t i = 0; i < n; i++) t += u[i] >> sh; s[k] = t; } }
    else for (int64_t i = 0; i < n && exact; i++) { for (int k = klo; k <= khi; k++) { s[k - klo] += u[i] >> k; } if (s[0] > lim) exact = false; }
    for (int k = klo; k <= khi; k++) { double c;
        if (exact) c = (double)(6 + s[k - klo] + (uint64_t)n * (uint64_t)(1 + k)); else c = riceCostU(u, n, k);
        if (c < cost) { cost = c; best = k; } }
    double ce = 12 + (double)rw * n; if (rw <= 63 && ce < cost) { cost = ce; best = 63; } return best; }
inline double residualCost(const int64_t* r, int64_t n, int& po) { double best = 1e300; po = 0;
    thread_local std::vector<uint64_t> ub; if ((int64_t)ub.size() < n) ub.resize((size_t)n); uint64_t* u = ub.data(); for (int64_t i = 0; i < n; i++) u[i] = zz(r[i]);
    for (int p = 0; p <= 8; p++) { int64_t parts = 1LL << p; if (parts > n) break; int64_t len = n / parts; double c = 4;
        for (int64_t q = 0; q < parts; q++) { int64_t a = q * len, e = (q == parts - 1) ? n : a + len; double cc; (void)bestKu(u + a, e - a, cc); c += cc; }
        if (c < best) { best = c; po = p; } } return best; }
inline void putResidual(BitW& w, const int64_t* r, int64_t n, int po) { w.put(po, 4); int64_t parts = 1LL << po, len = n / parts;
    thread_local std::vector<uint64_t> ub; if ((int64_t)ub.size() < n) ub.resize((size_t)n); uint64_t* u = ub.data(); for (int64_t i = 0; i < n; i++) u[i] = zz(r[i]);
    for (int64_t q = 0; q < parts; q++) { int64_t a = q * len, e = (q == parts - 1) ? n : a + len; double c; int k = bestKu(u + a, e - a, c); w.put(k, 6);
        if (k == 63) { int rw = 1; for (int64_t i = a; i < e; i++) rw = std::max(rw, widthOf(r[i])); w.put(rw, 6); for (int64_t i = a; i < e; i++) w.putS(r[i], rw); }
        else for (int64_t i = a; i < e; i++) { uint64_t u = zz(r[i]), qv = u >> k; w.zeros(qv); w.put(1, 1); if (k) w.put(u & ((1ull << k) - 1), k); } } }
inline void lpcResidual(const int64_t* x, int64_t n, const std::vector<int64_t>& c, int shift, int64_t* res, uint64_t maxAbsX) {
    const int o = (int)c.size();
    if (lpcFits64(c, maxAbsX)) { for (int64_t i = o; i < n; i++) res[i] = x[i] - lpcPred64(x, i, c.data(), o, shift); }
    else for (int64_t i = o; i < n; i++) res[i] = x[i] - lpcPred(x, i, c, shift); }
// ---- encoder for one block: the choice (expensive, independent per block) and the writing
struct BlockPlan { int W = 1; bool same = false; int type = 1, ord = 0, po = 0, shift = 0, prec = 0; std::vector<int64_t> c; };
inline BlockPlan planBlock(const int64_t* x, int64_t n) {   // throws if a sample needs more than kMaxW bits (LAES layers stay below about 2^41)
    BlockPlan P; uint64_t orv = 0; bool same = true; for (int64_t i = 0; i < n; i++) { orv |= zz(x[i]); if (x[i] != x[0]) same = false; } const int W = std::max(1, bitLen(orv));   // = max widthOf
    if (W > kMaxW) throw std::runtime_error("laes: sample wider than 62 bits");
    P.W = W; P.same = same; if (same) { P.type = 0; return P; }
    std::vector<int64_t> res(n); double bestBits = 2 + 6 + (double)W * n; const uint64_t mx = absMax(x, n);
    // fixed orders
    for (int o = 0; W <= kMaxWFixed && o <= 4 && o < n; o++) { for (int64_t i = o; i < n; i++) res[i] = x[i] - fixedPred(x, i, o); int po; double c = 2 + 6 + 3 + (double)W * o + residualCost(res.data() + o, n - o, po);
        if (c < bestBits) { bestBits = c; P.type = 2; P.ord = o; P.po = po; } }
    // LPC: Tukey(0.5) window, autocorrelation, Levinson; orders 1..32 (fewer than n)
    const int MAXO = W <= kMaxWLpc ? (int)std::min<int64_t>(32, n - 1) : 0;
    if (MAXO >= 1) { std::vector<double> xw(n), R(MAXO + 1, 0); for (int64_t i = 0; i < n; i++) { double t = (double)i / (n - 1), wv = 1;
            if (t < 0.25) wv = 0.5 * (1 - lmath::cospi(t / 0.25)); else if (t > 0.75) wv = 0.5 * (1 - lmath::cospi((1 - t) / 0.25)); xw[i] = (double)x[i] * wv; }
        for (int l = 0; l <= MAXO; l++) { double s = 0; for (int64_t i = l; i < n; i++) s += xw[i] * xw[i - l]; R[l] = s; }
        if (R[0] > 0) { std::vector<std::vector<double>> A(MAXO + 1); std::vector<double> a(MAXO + 1, 0), tmp(MAXO + 1); double err = R[0] * (1 + 1e-10);
            for (int m = 1; m <= MAXO; m++) { double acc = R[m]; for (int j = 1; j < m; j++) acc -= a[j] * R[m - j]; double k = acc / err; tmp = a; a[m] = k; for (int j = 1; j < m; j++) a[j] = tmp[j] - k * tmp[m - j];
                err *= (1 - k * k); A[m] = a; if (err <= 0) break; }
            const int orders[] = {1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
            for (int o : orders) { if (o > MAXO || A[o].empty()) continue;
                for (int prec : {12, 15}) { double mxc = 0; for (int j = 1; j <= o; j++) mxc = std::max(mxc, std::fabs(A[o][j])); if (mxc <= 0) continue;
                    int lg = (int)std::ceil(lmath::log2(mxc + 1e-300)); int shift = prec - 1 - lg; shift = std::max(0, std::min(31, shift));
                    std::vector<int64_t> c(o); const int64_t lim = (1LL << (prec - 1)) - 1; for (int j = 0; j < o; j++) { double q = std::floor(A[o][j + 1] * (double)(1LL << shift) + 0.5); c[j] = std::max(-lim - 1, std::min(lim, (int64_t)q)); }
                    lpcResidual(x, n, c, shift, res.data(), mx); int po;
                    double cst = 2 + 6 + 5 + 4 + 5 + (double)prec * o + (double)W * o + residualCost(res.data() + o, n - o, po);
                    if (cst < bestBits) { bestBits = cst; P.type = 3; P.ord = o; P.po = po; P.shift = shift; P.prec = prec; P.c = c; } } } } }
    return P; }
inline void writeBlock(BitW& w, const int64_t* x, int64_t n, const BlockPlan& P) {
    if (P.same) { w.put(0, 2); w.put(P.W, 6); w.putS(x[0], P.W); return; }
    w.put(P.type, 2); w.put(P.W, 6);
    if (P.type == 1) { for (int64_t i = 0; i < n; i++) w.putS(x[i], P.W); return; }
    std::vector<int64_t> res(n);
    if (P.type == 2) { w.put(P.ord, 3); for (int i = 0; i < P.ord; i++) w.putS(x[i], P.W); for (int64_t i = P.ord; i < n; i++) res[i] = x[i] - fixedPred(x, i, P.ord); putResidual(w, res.data() + P.ord, n - P.ord, P.po); return; }
    w.put(P.ord - 1, 5); w.put(P.prec - 1, 4); w.put(P.shift, 5); for (int j = 0; j < P.ord; j++) w.putS(P.c[j], P.prec);
    for (int i = 0; i < P.ord; i++) w.putS(x[i], P.W); lpcResidual(x, n, P.c, P.shift, res.data(), absMax(x, n)); putResidual(w, res.data() + P.ord, n - P.ord, P.po); }
inline void encodeBlock(BitW& w, const int64_t* x, int64_t n) { writeBlock(w, x, n, planBlock(x, n)); }
// ---- a whole layer (one channel): blocks of length B. The blocks' parameters are chosen in parallel threads (each block on its own), then written in order.
inline std::vector<uint8_t> encodeLayer(const V& x, int64_t B, size_t& bits) {
    const int64_t N = (int64_t)x.size(); const int64_t nb = (int64_t)((N + B - 1) / B); std::vector<BlockPlan> plans((size_t)std::max<int64_t>(0, nb));
    auto planRange = [&](int64_t a, int64_t b) { for (int64_t k = a; k < b; k++) { const int64_t s = (int64_t)k * B; plans[(size_t)k] = planBlock(x.data() + s, (int64_t)std::min<int64_t>(B, N - s)); } };
    unsigned hc = std::thread::hardware_concurrency(); int th = (int)std::max(1u, hc); if (nb < 4) th = 1; th = (int)std::min<int64_t>(th, nb > 0 ? nb : 1);
    if (th <= 1) planRange(0, nb);
    else { std::vector<std::thread> pool; std::vector<std::exception_ptr> xs((size_t)th); std::vector<int> here; const int64_t step = (nb + th - 1) / th;
        for (int t = 1; t < th; t++) { const int64_t a = t * step, b = std::min(nb, a + step); if (a >= b) continue;
            try { pool.emplace_back([&planRange, &xs, t, a, b]() { try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }); } catch (...) { here.push_back(t); } }
        try { planRange(0, std::min(nb, step)); } catch (...) { xs[0] = std::current_exception(); }
        for (int t : here) { const int64_t a = t * step, b = std::min(nb, a + step); try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }
        for (auto& p : pool) p.join(); for (auto& e : xs) if (e) std::rethrow_exception(e); }
    BitW w; for (int64_t k = 0; k < nb; k++) { const int64_t s = (int64_t)k * B; writeBlock(w, x.data() + s, (int64_t)std::min<int64_t>(B, N - s), plans[(size_t)k]); }
    bits = w.bits; w.flush(); return w.b; }
}  // namespace laes
