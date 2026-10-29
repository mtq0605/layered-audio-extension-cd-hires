// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAES layer coder, encoder side (split from laes_codec.h, 2026-10-29; the code is unchanged). The block syntax and the decoder are in
// laes_codec.h (Apache-2.0). Here: the bit writer, the residual cost and Rice parameter search, the block plan (predictor, order,
// precision, shift, partition), the block writer and encodeLayer (block parameters chosen in parallel threads, written in order).
#pragma once
#include "laes_codec.h"
namespace laes {
struct BitW { std::vector<uint8_t> b; uint64_t acc = 0; int n = 0; size_t bits = 0;
    void put(uint64_t v, int w) {   // the w low bits of v, most significant first; w <= 64
        if (w <= 0) return; bits += (size_t)w;
        while (w > 0) { const int take = std::min(w, 8 - n); acc = (acc << take) | ((v >> (w - take)) & ((1u << take) - 1)); n += take; w -= take;
            if (n == 8) { b.push_back((uint8_t)acc); acc = 0; n = 0; } } }
    void zeros(uint64_t q) { while (q > 0) { const int t = (int)std::min<uint64_t>(q, 32); put(0, t); q -= (uint64_t)t; } }
    void putS(int64_t v, int w) { put((uint64_t)v & ((1ull << w) - 1), w); }   // w <= 63
    void flush() { while (n) put(0, 1); } };
// reader with a sticky failure flag: every read is bounds-checked; on failure it returns 0 and decoding stops
// ---- residual coding
inline double riceCost(const int64_t* r, long n, int k) { double b = 6; for (long i = 0; i < n; i++) b += (double)(zz(r[i]) >> k) + 1 + k; return b; }
inline int bestK(const int64_t* r, long n, double& cost) {   // start from the mean, then test neighbours exactly; escape compared too
    double mean = 0; int rw = 1; for (long i = 0; i < n; i++) { mean += (double)zz(r[i]); rw = std::max(rw, widthOf(r[i])); } mean = n ? mean / n : 0;
    int k0 = mean > 1 ? (int)std::floor(std::log2(mean)) : 0; int best = 0; cost = 1e300;
    const int klo = std::max(0, k0 - 2), khi = std::min(62, k0 + 2);
    // the five costs in one pass: 6 + sum(u >> k) + n (1 + k), exact in integers; the former double sum of the same integers is exact while it stays
    // below 2^53, which the bound below guarantees (otherwise the former per-k double sum is used)
    uint64_t s[5] = {0, 0, 0, 0, 0}; bool exact = true; const uint64_t lim = 1ull << 52;
    for (long i = 0; i < n && exact; i++) { const uint64_t u = zz(r[i]); for (int k = klo; k <= khi; k++) { s[k - klo] += u >> k; } if (s[0] > lim) exact = false; }
    for (int k = klo; k <= khi; k++) { double c;
        if (exact) c = (double)(6 + s[k - klo] + (uint64_t)n * (uint64_t)(1 + k)); else c = riceCost(r, n, k);
        if (c < cost) { cost = c; best = k; } }
    double ce = 12 + (double)rw * n; if (rw <= 63 && ce < cost) { cost = ce; best = 63; } return best; }
inline double residualCost(const int64_t* r, long n, int& po) { double best = 1e300; po = 0;
    for (int p = 0; p <= 8; p++) { long parts = 1L << p; if (parts > n) break; long len = n / parts; double c = 4;
        for (long q = 0; q < parts; q++) { long a = q * len, e = (q == parts - 1) ? n : a + len; double cc; (void)bestK(r + a, e - a, cc); c += cc; }
        if (c < best) { best = c; po = p; } } return best; }
inline void putResidual(BitW& w, const int64_t* r, long n, int po) { w.put(po, 4); long parts = 1L << po, len = n / parts;
    for (long q = 0; q < parts; q++) { long a = q * len, e = (q == parts - 1) ? n : a + len; double c; int k = bestK(r + a, e - a, c); w.put(k, 6);
        if (k == 63) { int rw = 1; for (long i = a; i < e; i++) rw = std::max(rw, widthOf(r[i])); w.put(rw, 6); for (long i = a; i < e; i++) w.putS(r[i], rw); }
        else for (long i = a; i < e; i++) { uint64_t u = zz(r[i]), qv = u >> k; w.zeros(qv); w.put(1, 1); if (k) w.put(u & ((1ull << k) - 1), k); } } }
inline void lpcResidual(const int64_t* x, long n, const std::vector<int64_t>& c, int shift, int64_t* res, uint64_t maxAbsX) {
    const int o = (int)c.size();
    if (lpcFits64(c, maxAbsX)) { for (long i = o; i < n; i++) res[i] = x[i] - lpcPred64(x, i, c.data(), o, shift); }
    else for (long i = o; i < n; i++) res[i] = x[i] - lpcPred(x, i, c, shift); }
// ---- encoder for one block: the choice (expensive, independent per block) and the writing
struct BlockPlan { int W = 1; bool same = false; int type = 1, ord = 0, po = 0, shift = 0, prec = 0; std::vector<int64_t> c; };
inline BlockPlan planBlock(const int64_t* x, long n) {   // throws if a sample needs more than kMaxW bits (LAES layers stay below about 2^41)
    BlockPlan P; int W = 1; bool same = true; for (long i = 0; i < n; i++) { W = std::max(W, widthOf(x[i])); if (x[i] != x[0]) same = false; }
    if (W > kMaxW) throw std::runtime_error("laes: sample wider than 62 bits");
    P.W = W; P.same = same; if (same) { P.type = 0; return P; }
    std::vector<int64_t> res(n); double bestBits = 2 + 6 + (double)W * n; const uint64_t mx = absMax(x, n);
    // fixed orders
    for (int o = 0; W <= kMaxWFixed && o <= 4 && o < n; o++) { for (long i = o; i < n; i++) res[i] = x[i] - fixedPred(x, i, o); int po; double c = 2 + 6 + 3 + (double)W * o + residualCost(res.data() + o, n - o, po);
        if (c < bestBits) { bestBits = c; P.type = 2; P.ord = o; P.po = po; } }
    // LPC: Tukey(0.5) window, autocorrelation, Levinson; orders 1..32 (fewer than n)
    const int MAXO = W <= kMaxWLpc ? (int)std::min<long>(32, n - 1) : 0;
    if (MAXO >= 1) { std::vector<double> xw(n), R(MAXO + 1, 0); for (long i = 0; i < n; i++) { double t = (double)i / (n - 1), wv = 1;
            if (t < 0.25) wv = 0.5 * (1 - std::cos(3.14159265358979323846 * t / 0.25)); else if (t > 0.75) wv = 0.5 * (1 - std::cos(3.14159265358979323846 * (1 - t) / 0.25)); xw[i] = (double)x[i] * wv; }
        for (int l = 0; l <= MAXO; l++) { double s = 0; for (long i = l; i < n; i++) s += xw[i] * xw[i - l]; R[l] = s; }
        if (R[0] > 0) { std::vector<std::vector<double>> A(MAXO + 1); std::vector<double> a(MAXO + 1, 0), tmp(MAXO + 1); double err = R[0] * (1 + 1e-10);
            for (int m = 1; m <= MAXO; m++) { double acc = R[m]; for (int j = 1; j < m; j++) acc -= a[j] * R[m - j]; double k = acc / err; tmp = a; a[m] = k; for (int j = 1; j < m; j++) a[j] = tmp[j] - k * tmp[m - j];
                err *= (1 - k * k); A[m] = a; if (err <= 0) break; }
            const int orders[] = {1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
            for (int o : orders) { if (o > MAXO || A[o].empty()) continue;
                for (int prec : {12, 15}) { double mxc = 0; for (int j = 1; j <= o; j++) mxc = std::max(mxc, std::fabs(A[o][j])); if (mxc <= 0) continue;
                    int lg = (int)std::ceil(std::log2(mxc + 1e-300)); int shift = prec - 1 - lg; shift = std::max(0, std::min(31, shift));
                    std::vector<int64_t> c(o); const int64_t lim = (1LL << (prec - 1)) - 1; for (int j = 0; j < o; j++) { double q = std::floor(A[o][j + 1] * (double)(1LL << shift) + 0.5); c[j] = std::max(-lim - 1, std::min(lim, (int64_t)q)); }
                    lpcResidual(x, n, c, shift, res.data(), mx); int po;
                    double cst = 2 + 6 + 5 + 4 + 5 + (double)prec * o + (double)W * o + residualCost(res.data() + o, n - o, po);
                    if (cst < bestBits) { bestBits = cst; P.type = 3; P.ord = o; P.po = po; P.shift = shift; P.prec = prec; P.c = c; } } } } }
    return P; }
inline void writeBlock(BitW& w, const int64_t* x, long n, const BlockPlan& P) {
    if (P.same) { w.put(0, 2); w.put(P.W, 6); w.putS(x[0], P.W); return; }
    w.put(P.type, 2); w.put(P.W, 6);
    if (P.type == 1) { for (long i = 0; i < n; i++) w.putS(x[i], P.W); return; }
    std::vector<int64_t> res(n);
    if (P.type == 2) { w.put(P.ord, 3); for (int i = 0; i < P.ord; i++) w.putS(x[i], P.W); for (long i = P.ord; i < n; i++) res[i] = x[i] - fixedPred(x, i, P.ord); putResidual(w, res.data() + P.ord, n - P.ord, P.po); return; }
    w.put(P.ord - 1, 5); w.put(P.prec - 1, 4); w.put(P.shift, 5); for (int j = 0; j < P.ord; j++) w.putS(P.c[j], P.prec);
    for (int i = 0; i < P.ord; i++) w.putS(x[i], P.W); lpcResidual(x, n, P.c, P.shift, res.data(), absMax(x, n)); putResidual(w, res.data() + P.ord, n - P.ord, P.po); }
inline void encodeBlock(BitW& w, const int64_t* x, long n) { writeBlock(w, x, n, planBlock(x, n)); }
// ---- a whole layer (one channel): blocks of length B. The blocks' parameters are chosen in parallel threads (each block on its own), then written in order.
inline std::vector<uint8_t> encodeLayer(const V& x, long B, size_t& bits) {
    const int64_t N = (int64_t)x.size(); const long nb = (long)((N + B - 1) / B); std::vector<BlockPlan> plans((size_t)std::max(0L, nb));
    auto planRange = [&](long a, long b) { for (long k = a; k < b; k++) { const int64_t s = (int64_t)k * B; plans[(size_t)k] = planBlock(x.data() + s, (long)std::min<int64_t>(B, N - s)); } };
    unsigned hc = std::thread::hardware_concurrency(); int th = (int)std::max(1u, hc); if (nb < 4) th = 1; th = (int)std::min<long>(th, nb > 0 ? nb : 1);
    if (th <= 1) planRange(0, nb);
    else { std::vector<std::thread> pool; std::vector<std::exception_ptr> xs((size_t)th); std::vector<int> here; const long step = (nb + th - 1) / th;
        for (int t = 1; t < th; t++) { const long a = t * step, b = std::min(nb, a + step); if (a >= b) continue;
            try { pool.emplace_back([&planRange, &xs, t, a, b]() { try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }); } catch (...) { here.push_back(t); } }
        try { planRange(0, std::min(nb, step)); } catch (...) { xs[0] = std::current_exception(); }
        for (int t : here) { const long a = t * step, b = std::min(nb, a + step); try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }
        for (auto& p : pool) p.join(); for (auto& e : xs) if (e) std::rethrow_exception(e); }
    BitW w; for (long k = 0; k < nb; k++) { const int64_t s = (int64_t)k * B; writeBlock(w, x.data() + s, (long)std::min<int64_t>(B, N - s), plans[(size_t)k]); }
    bits = w.bits; w.flush(); return w.b; }
}  // namespace laes
