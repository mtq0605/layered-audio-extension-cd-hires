// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// FLAC writer for the LAES FLAC form (split from laes_flac.h, 2026-10-29; the code is unchanged). Written for this project from RFC 9639
// (no libFLAC code). The reader is in laes_flac.h (Apache-2.0). Here: the subframe encoder (CONSTANT / VERBATIM / FIXED / LPC, wasted
// bits, Rice partitions), the frame writer and writeFlac (STREAMINFO, the LAES payload in APPLICATION blocks, the core in audio frames).
#pragma once
#include <type_traits>
#include "laes_flac.h"
#include "laes_codec_enc.h"
namespace laesflac {
using laes::BitW;
// ---- subframe encoder
const int kMaxLpcOrder = 12, kMaxPrec = 15, kMaxShift = 15;
// cost of one Rice partition with the 5-bit method (parameter 0..30, 31 = escape)
// L5 (2026-11-03): from the zz values, with the same result as the former loops. The cost was the double sum 5 + sum((u >> k) + 1 + k) in order;
// every term and partial sum is an integer, below 2^53 when sum(u) <= 2^52 (n <= 2^16, k <= 30), so it equals the integer sum then (otherwise
// the double loop as before). rw = the largest widthOf of the non-zero values = bitLen(OR of the zz values) (laes::bitLen).
inline int riceParamU(const uint64_t* u, int64_t n, double& cost) {
    uint64_t sum = 0, orv = 0; for (int64_t i = 0; i < n; i++) { sum += u[i]; orv |= u[i]; }
    const bool small = laes::bitLen(orv) + laes::bitLen((uint64_t)n) <= 63 && sum <= (1ull << 52) && n <= 65536;
    double mean = 0; if (small) mean = (double)sum; else for (int64_t i = 0; i < n; i++) mean += (double)u[i];
    mean = n ? mean / n : 0;
    int k0 = mean > 1 ? (int)std::floor(lmath::log2(mean)) : 0; int best = 0; cost = 1e300;
    for (int k = std::max(0, k0 - 2); k <= std::min(30, k0 + 2); k++) { double c;
        if (small) { uint64_t t = 0; for (int64_t i = 0; i < n; i++) t += u[i] >> k; c = (double)(5 + t + (uint64_t)n * (uint64_t)(1 + k)); }
        else { c = 5; for (int64_t i = 0; i < n; i++) c += (double)(u[i] >> k) + 1 + k; }
        if (c < cost) { cost = c; best = k; } }
    const int rw = laes::bitLen(orv);
    if (rw >= 1 && rw <= 31) { double ce = 10 + (double)rw * n; if (ce < cost) { cost = ce; best = 31; } }   // no 0-bit escape: some older decoders mishandle it (an all-zero partition takes k = 0)
    return best; }
inline const uint64_t* zzBuf(const int64_t* r, int64_t n) { thread_local std::vector<uint64_t> ub; if ((int64_t)ub.size() < n) ub.resize((size_t)n); for (int64_t i = 0; i < n; i++) ub[(size_t)i] = laes::zz(r[i]); return ub.data(); }
// partition orders 0..8 with the block length divisible by 2^po and a non-empty first partition (block >> po > order)
inline double residualCost(const int64_t* r, int64_t blk, int order, int& po) { double best = 1e300; po = 0; const uint64_t* u = zzBuf(r, blk - order);
    for (int p = 0; p <= 8; p++) { if (blk % (1LL << p) || (blk >> p) <= order) break; int64_t plen = blk >> p; double c = 2 + 4; int64_t a = 0;
        for (int64_t q = 0; q < (1LL << p); q++) { int64_t len = q ? plen : plen - order; double cc; riceParamU(u + a, len, cc); c += cc; a += len; }
        if (c < best) { best = c; po = p; } } return best; }
inline void putResidual(BitW& w, const int64_t* r, int64_t blk, int order, int po) { w.put(1, 2); w.put(po, 4); int64_t plen = blk >> po, a = 0;   // method 01: 5-bit parameters
    const uint64_t* u = zzBuf(r, blk - order);
    for (int64_t q = 0; q < (1LL << po); q++) { int64_t len = q ? plen : plen - order; double c; int k = riceParamU(u + a, len, c); w.put(k, 5);
        if (k == 31) { int rw = 0; for (int64_t i = a; i < a + len; i++) if (r[i]) rw = std::max(rw, laes::widthOf(r[i])); w.put(rw, 5); if (rw) for (int64_t i = a; i < a + len; i++) w.putS(r[i], rw); }
        else for (int64_t i = a; i < a + len; i++) { const uint64_t qv = u[i] >> k; w.zeros(qv); w.put(1, 1); if (k) w.put(u[i] & ((1ull << k) - 1), k); }
        a += len; } }
inline int64_t lpcPred(const int64_t* x, int64_t i, const std::vector<int64_t>& c, int shift) { int64_t s = 0; for (size_t j = 0; j < c.size(); j++) s += c[j] * x[i - 1 - (int64_t)j]; return s >> shift; }   // |s| < 2^50: int64, arithmetic shift = floor
inline void encodeSubframe(BitW& w, const int64_t* x0, int64_t n, int bps0) {
    bool same = true; for (int64_t i = 1; i < n && same; i++) same = x0[i] == x0[0];
    if (same) { w.put(0, 1); w.put(0, 6); w.put(0, 1); w.putS(x0[0], bps0); return; }
    // wasted bits: low bits that are zero in every sample of the block are left out (subframe header flag + unary count)
    int64_t orv = 0; for (int64_t i = 0; i < n; i++) orv |= x0[i]; int wb = 0; while (wb < bps0 - 1 && !((orv >> wb) & 1)) wb++;
    std::vector<int64_t> xs; const int64_t* x = x0; if (wb) { xs.resize(n); for (int64_t i = 0; i < n; i++) xs[i] = x0[i] >> wb; x = xs.data(); }
    const int bps = bps0 - wb; auto head = [&](int type) { w.put(0, 1); w.put(type, 6); if (wb) { w.put(1, 1); for (int i = 1; i < wb; i++) w.put(0, 1); w.put(1, 1); } else w.put(0, 1); };
    std::vector<int64_t> res(n); double bestBits = 8 + (double)bps * n; int bestType = 1, bestOrd = 0, bestPo = 0, bestShift = 0, bestPrec = 0; std::vector<int64_t> bestC;
    for (int o = 0; o <= 4 && o < n; o++) { bool ok = true; for (int64_t i = o; i < n && ok; i++) { res[i] = x[i] - fixedPred(x, i, o); ok = fits32(res[i]); } if (!ok) continue;
        int po; double c = 8 + (double)bps * o + residualCost(res.data() + o, n, o, po); if (c < bestBits) { bestBits = c; bestType = 2; bestOrd = o; bestPo = po; } }
    const int MAXO = (int)std::min<int64_t>(kMaxLpcOrder, n - 1);
    if (MAXO >= 1) { std::vector<double> xw(n), R(MAXO + 1, 0); for (int64_t i = 0; i < n; i++) { double t = n > 1 ? (double)i / (n - 1) : 0, wv = 1;
            if (t < 0.25) wv = 0.5 * (1 - lmath::cospi(t / 0.25)); else if (t > 0.75) wv = 0.5 * (1 - lmath::cospi((1 - t) / 0.25)); xw[i] = (double)x[i] * wv; }
        for (int l = 0; l <= MAXO; l++) { double s = 0; for (int64_t i = l; i < n; i++) s += xw[i] * xw[i - l]; R[l] = s; }
        if (R[0] > 0) { std::vector<std::vector<double>> A(MAXO + 1); std::vector<double> a(MAXO + 1, 0), tmp; double err = R[0] * (1 + 1e-10);
            for (int m = 1; m <= MAXO; m++) { double acc = R[m]; for (int j = 1; j < m; j++) acc -= a[j] * R[m - j]; double k = acc / err; tmp = a; a[m] = k; for (int j = 1; j < m; j++) a[j] = tmp[j] - k * tmp[m - j];
                err *= (1 - k * k); A[m] = a; if (err <= 0) break; }
            for (int o = 1; o <= MAXO; o++) { if (A[o].empty()) continue;
                for (int prec : {12, 15}) { double mx = 0; for (int j = 1; j <= o; j++) mx = std::max(mx, std::fabs(A[o][j])); if (mx <= 0) continue;
                    const int lg = (int)std::ceil(lmath::log2(mx + 1e-300)); const int shift = std::max(0, std::min(kMaxShift, prec - 1 - lg));
                    std::vector<int64_t> c(o); const int64_t lim = (1LL << (prec - 1)) - 1; for (int j = 0; j < o; j++) c[j] = std::max(-lim - 1, std::min(lim, (int64_t)std::floor(A[o][j + 1] * (double)(1LL << shift) + 0.5)));
                    bool ok = true; for (int64_t i = o; i < n && ok; i++) { res[i] = x[i] - lpcPred(x, i, c, shift); ok = fits32(res[i]); } if (!ok) continue;
                    int po; double cst = 8 + 4 + 5 + (double)prec * o + (double)bps * o + residualCost(res.data() + o, n, o, po);
                    if (cst < bestBits) { bestBits = cst; bestType = 3; bestOrd = o; bestPo = po; bestShift = shift; bestPrec = prec; bestC = c; } } } } }
    if (bestType == 1) { head(1); for (int64_t i = 0; i < n; i++) w.putS(x[i], bps); return; }
    if (bestType == 2) { head(8 + bestOrd); for (int i = 0; i < bestOrd; i++) w.putS(x[i], bps);
        for (int64_t i = bestOrd; i < n; i++) res[i] = x[i] - fixedPred(x, i, bestOrd); putResidual(w, res.data() + bestOrd, n, bestOrd, bestPo); return; }
    head(32 + bestOrd - 1); for (int i = 0; i < bestOrd; i++) w.putS(x[i], bps);
    w.put(bestPrec - 1, 4); w.put(bestShift, 5); for (int j = 0; j < bestOrd; j++) w.putS(bestC[j], bestPrec);
    for (int64_t i = bestOrd; i < n; i++) res[i] = x[i] - lpcPred(x, i, bestC, bestShift); putResidual(w, res.data() + bestOrd, n, bestOrd, bestPo); }

// ---- writer
inline void putUtf8(BitW& w, uint64_t v) {   // the frame number, coded as in UTF-8 (up to 36 bits)
    if (v < 0x80) { w.put(v, 8); return; } int nb = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : v < 0x80000000ull ? 6 : 7;
    w.put((0xFF00 >> nb) & 0xFF | (nb == 7 ? 0 : (v >> (6 * (nb - 1)))), 8); for (int i = nb - 2; i >= 0; i--) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8); }
// ch: int64 or int32 samples (int32: the encoder's core, memory 2026-11-05; the frame's samples are widened first, the bytes are the same)
template <class Vec> inline std::vector<uint8_t> encodeFrame(const Vec* ch, int64_t at, int64_t n, uint64_t frameNo, int rate, int bps) {
    BitW w; w.put(0xFFF8, 16);   // sync 0b111111111111100, blocking strategy 0 (fixed block size)
    const int bsCode = n == 4096 ? 12 : (n <= 256 ? 6 : 7), ssCode = bps == 16 ? 4 : bps == 24 ? 6 : 7;
    const int srCode = rate == 88200 ? 1 : rate == 176400 ? 2 : rate == 192000 ? 3 : rate == 8000 ? 4 : rate == 16000 ? 5 : rate == 22050 ? 6 : rate == 24000 ? 7 : rate == 32000 ? 8
                     : rate == 44100 ? 9 : rate == 48000 ? 10 : rate == 96000 ? 11 : 0;   // 0 = as in STREAMINFO
    w.put(bsCode, 4); w.put(srCode, 4); w.put(1, 4); w.put(ssCode, 3); w.put(0, 1); putUtf8(w, frameNo); if (bsCode == 6) w.put(n - 1, 8); else if (bsCode == 7) w.put(n - 1, 16);
    w.put(crc8(w.b.data(), w.b.size()), 8);
    for (int c = 0; c < 2; c++) { V wide; const int64_t* px;
        if constexpr (std::is_same<typename Vec::value_type, int64_t>::value) px = ch[c].data() + at; else { wide.assign(ch[c].begin() + at, ch[c].begin() + at + n); px = wide.data(); }
        encodeSubframe(w, px, n, bps); }
    w.flush(); uint16_t cr = crc16(w.b.data(), w.b.size()); w.b.push_back((uint8_t)(cr >> 8)); w.b.push_back((uint8_t)cr); return w.b; }
inline void putBlockHeader(std::vector<uint8_t>& o, bool last, int type, uint32_t len) { o.push_back((uint8_t)((last ? 0x80 : 0) | type)); o.push_back((uint8_t)(len >> 16)); o.push_back((uint8_t)(len >> 8)); o.push_back((uint8_t)len); }
// core: two channels of n samples at bps bits (16 / 24 / 32), rate 44100 or 48000; app: the LAES payload (may be empty)
// the STREAMINFO MD5 of int64 or int32 samples (as audioMd5)
template <class Vec> inline void audioMd5T(const Vec* ch, int64_t n, int bits, uint8_t out[16]) { alignhash::Md5 m; const int by = (bits + 7) / 8; std::vector<uint8_t> b; b.reserve(1 << 16);
    for (int64_t i = 0; i < n; i++) for (int c = 0; c < 2; c++) { uint64_t v = (uint64_t)(int64_t)ch[c][(size_t)i]; for (int k = 0; k < by; k++) b.push_back((uint8_t)(v >> (8 * k))); if (b.size() >= (1 << 16)) { m.update(b.data(), b.size()); b.clear(); } }
    m.update(b.data(), b.size()); std::string h = m.hex(); for (int i = 0; i < 16; i++) out[i] = (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16); }
template <class Vec> inline std::vector<uint8_t> writeFlac(const Vec* ch, int rate, int bps, const std::vector<uint8_t>& app) {   // throws on input it cannot write exactly
    const int64_t n = (int64_t)ch[0].size(); const int64_t B = 4096;
    if (bps != 16 && bps != 24 && bps != 32) throw std::runtime_error("laesflac: the core master must be 16, 24 or 32 bit");
    if (rate < 1 || rate >= (1 << 20) || (int64_t)ch[1].size() != n || n >= (1LL << 36)) throw std::runtime_error("laesflac: bad rate or length");
    { const int64_t lim = 1LL << (bps - 1); for (int c = 0; c < 2; c++) for (int64_t v : ch[c]) if (v < -lim || v >= lim) throw std::runtime_error("laesflac: a sample does not fit the bit depth"); } std::vector<std::vector<uint8_t>> fr; uint32_t fmin = 0xFFFFFF, fmax = 0;
    {   // the frames are independent: coded in parallel threads (L5, 2026-11-03), then taken in order
        const int64_t nf = (n + B - 1) / B; fr.resize((size_t)nf);
        auto range = [&](int64_t a, int64_t b) { for (int64_t k = a; k < b; k++) fr[(size_t)k] = encodeFrame(ch, k * B, (int64_t)std::min<int64_t>(B, n - k * B), (uint64_t)k, rate, bps); };
        int th = (int)std::max(1u, std::thread::hardware_concurrency()); if (nf < 4) th = 1; th = (int)std::min<int64_t>(th, std::max<int64_t>(1, nf));
        const int64_t step = (nf + th - 1) / th; std::vector<std::thread> pool; std::vector<std::exception_ptr> xs((size_t)th); std::vector<int> here;
        for (int t = 1; t < th; t++) { const int64_t a = t * step, b = std::min(nf, a + step); if (a >= b) continue;
            try { pool.emplace_back([&range, &xs, t, a, b]() { try { range(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }); } catch (...) { here.push_back(t); } }
        try { range(0, std::min(nf, step)); } catch (...) { xs[0] = std::current_exception(); }
        for (int t : here) { const int64_t a = t * step, b = std::min(nf, a + step); try { range(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }
        for (auto& p : pool) p.join(); for (auto& e : xs) if (e) std::rethrow_exception(e);
        for (auto& f : fr) { fmin = std::min<uint32_t>(fmin, (uint32_t)f.size()); fmax = std::max<uint32_t>(fmax, (uint32_t)f.size()); } }
    if (fr.empty()) fmin = 0;
    std::vector<uint8_t> o = {'f', 'L', 'a', 'C'}; const size_t piece = (1u << 24) - 1 - 4; const size_t napp = app.empty() ? 0 : (app.size() + piece - 1) / piece;
    putBlockHeader(o, napp == 0, 0, 34); const uint32_t bs = n >= B ? (uint32_t)B : (uint32_t)std::max<int64_t>(16, n);
    BitW si; si.put(bs, 16); si.put(bs, 16); si.put(fmin, 24); si.put(fmax, 24); si.put((uint64_t)rate, 20); si.put(1, 3); si.put((uint64_t)(bps - 1), 5); si.put((uint64_t)n, 36);
    uint8_t md5[16]; audioMd5T(ch, n, bps, md5); o.insert(o.end(), si.b.begin(), si.b.end()); o.insert(o.end(), md5, md5 + 16);
    for (size_t i = 0; i < napp; i++) { const size_t a = i * piece, len = std::min(piece, app.size() - a); putBlockHeader(o, i + 1 == napp, 2, (uint32_t)(len + 4));
        o.insert(o.end(), {'L', 'A', 'E', '2'}); o.insert(o.end(), app.begin() + a, app.begin() + a + len); }
    for (auto& f : fr) o.insert(o.end(), f.begin(), f.end()); return o; }
}  // namespace laesflac
