// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAES encoder, block-wise (2026-10-30): the same payload as laes::encodeLaes (laes_file_enc.h), byte for byte, with the memory of a few
// pieces. Each master is read once into a work folder (int32 samples, SHA-256 and the unused low bits found on the way); its rate levels
// and octave layers are computed piece by piece (laes_stream.h) and do not depend on the file order, so they are made once for both orders
// the tool tries. Every candidate storage of a master is coded to a file; only the cheapest is kept. The payload is written to a file with
// its CRC-32 added up piece by piece.
#pragma once
#include <cmath>
#include "laes_stream.h"
#include "laes_file_enc.h"
namespace laess {
// a layer (one channel) coded in pieces: blocks of B; a piece is a whole number of blocks (only the last piece may end in a short block).
// As laes::encodeLayer: the blocks' parameters are chosen in parallel threads, then written in order; the coded bytes go to a file.
struct LayerOut { long B; laes::BitW w; Out o; int64_t done = 0;
    LayerOut(const std::string& p, long b) : B(b), o(p) {}
    void feed(const int64_t* x, int64_t n) {
        const long nb = (long)((n + B - 1) / B); std::vector<laes::BlockPlan> plans((size_t)std::max(0L, nb));
        auto planRange = [&](long a, long b) { for (long k = a; k < b; k++) { const int64_t s = (int64_t)k * B; plans[(size_t)k] = laes::planBlock(x + s, (long)std::min<int64_t>(B, n - s)); } };
        unsigned hc = std::thread::hardware_concurrency(); int th = (int)std::max(1u, hc); if (nb < 4) th = 1; th = (int)std::min<long>(th, nb > 0 ? nb : 1);
        if (th <= 1) planRange(0, nb);
        else { std::vector<std::thread> pool; std::vector<std::exception_ptr> xs((size_t)th); std::vector<int> here; const long step = (nb + th - 1) / th;
            for (int t = 1; t < th; t++) { const long a = t * step, b = std::min(nb, a + step); if (a >= b) continue;
                try { pool.emplace_back([&planRange, &xs, t, a, b]() { try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }); } catch (...) { here.push_back(t); } }
            try { planRange(0, std::min(nb, step)); } catch (...) { xs[0] = std::current_exception(); }
            for (int t : here) { const long a = t * step, b = std::min(nb, a + step); try { planRange(a, b); } catch (...) { xs[(size_t)t] = std::current_exception(); } }
            for (auto& p : pool) p.join(); for (auto& e : xs) if (e) std::rethrow_exception(e); }
        for (long k = 0; k < nb; k++) { const int64_t s = (int64_t)k * B; laes::writeBlock(w, x + s, (long)std::min<int64_t>(B, n - s), plans[(size_t)k]); }
        o.put(w.b.data(), w.b.size()); w.b.clear(); done += n; }   // the whole bytes so far; the bits of a byte not yet full stay in w
    size_t finish() { const size_t bits = w.bits; w.flush(); o.put(w.b.data(), w.b.size()); w.b.clear(); o.close(); return bits; } };
// copies a byte range of one file into an output, with the payload's running CRC-32
struct CrcOut { Out& o; uint32_t crc = 0xFFFFFFFFu; explicit CrcOut(Out& out) : o(out) {}
    void put(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { crc ^= p[i]; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } o.put(p, n); }
    void u(uint64_t v, int n) { uint8_t t[8]; for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i)); put(t, (size_t)n); }
    void file(const std::string& p) { In in(p); std::vector<uint8_t> b; for (long long a = 0; a < in.n; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, in.n - a)); in.read(a, b.data(), b.size()); put(b.data(), b.size()); } } };
// ---- a master in the work folder: raw int32 L R samples, SHA-256, unused low bits w, its levels 0..stages-1 (int64, samples >> w) and its
// octave layers 1..stages coded (they do not depend on the file order)
struct Prep { std::string path, raw; int bits = 0, rate = 0, stages = 0, w = 0; long frames = 0; uint8_t sha[32]; std::vector<std::string> lev[2], dfile[2]; std::vector<size_t> dbits[2]; };
inline std::unique_ptr<Sig> levelOf(const Prep& m, int c, int k) { if (k >= m.stages) return std::unique_ptr<Sig>(new RawSig(m.raw, c, m.w)); return std::unique_ptr<Sig>(new FileSig(m.lev[c][(size_t)k])); }
// reads a 16 / 24 / 32-bit integer PCM WAV (PCM or EXTENSIBLE) as laes_tool readWav does (all 32 bits of a 32-bit file), header only first
struct WavInfo { int rate = 0, bits = 0; long long dataOff = -1, dataBytes = 0; long frames = 0; };
inline std::string probeWav(const std::string& path, WavInfo& wi, long long* laeOff = nullptr, long long* laeLen = nullptr) {
    In in(path); uint8_t h[12]; if (in.n < 12) return path + ": not RIFF/WAVE"; in.read(0, h, 12); if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return path + ": not RIFF/WAVE";
    long long pos = 12; int tag = 0, ch = 0; if (laeOff) *laeOff = -1;
    while (pos + 8 <= in.n) { uint8_t c[8]; in.read(pos, c, 8); uint64_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint64_t)c[7] << 24; const long long body = pos + 8; if ((long long)n > in.n - body) n = (uint64_t)(in.n - body);
        if (!memcmp(c, "fmt ", 4) && n >= 16) { uint8_t f[26] = {0}; in.read(body, f, n >= 26 ? 26 : 16); tag = f[0] | f[1] << 8; ch = f[2] | f[3] << 8; wi.rate = (int)(f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24); wi.bits = f[14] | f[15] << 8; if (tag == 0xFFFE && n >= 26) tag = f[24] | f[25] << 8; }
        else if (!memcmp(c, "data", 4)) { wi.dataOff = body; wi.dataBytes = (long long)n; }
        else if (!memcmp(c, "LAE ", 4) && laeOff) { *laeOff = body; *laeLen = (long long)n; }
        pos = body + (long long)n + (long long)(n & 1); }
    if (tag != 1 || ch != 2 || (wi.bits != 16 && wi.bits != 24 && wi.bits != 32) || wi.dataOff < 0) return path + ": need 16/24/32-bit integer PCM, 2 channels";
    wi.frames = (long)(wi.dataBytes / (2 * (wi.bits / 8))); return ""; }
// the samples of frames [a, b) of a probed WAV as int32 L R
inline void wavFrames(In& in, const WavInfo& wi, long long a, long long b, std::vector<int32_t>& out) {
    const int by = wi.bits / 8; std::vector<uint8_t> t((size_t)(b - a) * 2 * by); in.read(wi.dataOff + a * 2 * by, t.data(), t.size()); out.resize((size_t)(b - a) * 2);
    for (size_t i = 0; i < out.size(); i++) { const uint8_t* q = t.data() + i * by; uint32_t v = 0; for (int k = 0; k < by; k++) v |= (uint32_t)q[k] << (8 * k);
        if (wi.bits < 32 && (v >> (wi.bits - 1) & 1)) v |= ~0u << wi.bits; out[i] = (int32_t)v; } }
// step 1: the master's samples into the work folder (one pass: raw file, SHA-256 of the samples as int32 LE, unused low bits)
inline std::string prepRead(const std::string& path, const std::string& stem, Prep& m) {
    WavInfo wi; std::string e = probeWav(path, wi); if (!e.empty()) return e;
    m.path = path; m.rate = wi.rate; m.bits = wi.bits; m.frames = wi.frames; m.raw = stem + "_raw.i32";
    In in(path); Out o(m.raw); laes::Sha256 s; int64_t orv = 0; std::vector<int32_t> x; const long long P = 1 << 20;
    for (long long a = 0; a < wi.frames; a += P) { const long long b = std::min<long long>(wi.frames, a + P); wavFrames(in, wi, a, b, x);
        for (int32_t v : x) orv |= (int64_t)v; s.update((const uint8_t*)x.data(), x.size() * 4); o.put(x.data(), x.size() * 4); }   // little-endian hosts
    o.close(); s.final(m.sha);
    m.w = 0; if (orv) { while (m.w < 31 && !((orv >> m.w) & 1)) m.w++; }   // laes::unusedBits
    return ""; }
// step 2: levels and octave layers (stages from the base rate). piece: base-level samples per piece (a multiple of 4096)
inline std::string prepLevels(Prep& m, int baseRate, const std::string& stem, int64_t piece, int threads) {
    int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
    m.stages = st;
    for (int c = 0; c < 2; c++) { m.lev[c].assign((size_t)st, std::string()); m.dfile[c].assign((size_t)st + 1, std::string()); m.dbits[c].assign((size_t)st + 1, 0);
        for (int k = st; k > 0; k--) { std::unique_ptr<Sig> x = levelOf(m, c, k); const int64_t ne = (x->n + 1) / 2;
            const std::string sc = stem + "_c" + std::to_string(c); m.lev[c][(size_t)(k - 1)] = sc + "_L" + std::to_string(k - 1); m.dfile[c][(size_t)k] = sc + "_D" + std::to_string(k);
            SigOut so(m.lev[c][(size_t)(k - 1)]); LayerOut lo(m.dfile[c][(size_t)k], 4096L << (k - 1)); V s, d; const int64_t P = piece << (k - 1);
            for (int64_t a = 0; a < ne; a += P) { fwdPiece(*x, a, std::min(ne, a + P), s, d, threads); so.put(s.data(), s.size()); if (!d.empty()) lo.feed(d.data(), (int64_t)d.size()); }
            so.close(); m.dbits[c][(size_t)k] = lo.finish(); } }
    return ""; }
// ---- the core derived from a master's base level (laes_tool deriveCore, same values and the same random numbers in the same order)
inline laes::Pcm deriveCoreS(const std::vector<Prep*>& ms, int base, int coreBits, size_t& src, bool& dithered, uint64_t seed) {
    src = 0; for (size_t i = 0; i < ms.size(); i++) if (ms[i]->bits > ms[src]->bits) src = i;
    for (size_t i = 0; i < ms.size(); i++) if (ms[i]->bits == coreBits) { src = i; break; }
    const Prep& m = *ms[src]; struct Rng { uint64_t s; explicit Rng(uint64_t sd) : s(sd * 2654435761ull + 88172645463325252ull) {} uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double uni() { return (double)(next() >> 11) / 9007199254740992.0 - 0.5; } } r(seed);
    laes::Pcm core; core.rate = base; core.bits = coreBits; const int sig = m.bits - m.w, drop = sig - coreBits; const int64_t mx = (1LL << (coreBits - 1)) - 1, mn = -mx - 1; dithered = drop > 0;
    for (int c = 0; c < 2; c++) { std::unique_ptr<Sig> x = levelOf(m, c, 0); core.c[c].resize((size_t)x->n); x->get(0, x->n, core.c[c].data());
        for (size_t i = 0; i < core.c[c].size(); i++) { const int64_t xv = core.c[c][i]; int64_t v;
            if (drop <= 0) v = xv << (-drop); else v = (int64_t)std::floor((double)xv / std::ldexp(1.0, drop) + r.uni() + r.uni() + 0.5);
            core.c[c][i] = std::max(mn, std::min(mx, v)); } }
    return core; }
// ---- the payload (laes::encodeLaes, block-wise). ms: the masters in file order with their storage choice (MasterIn without samples).
struct MasterSel { int storage = -1, refTier = 0; int64_t whole = 0; uint32_t frac = 0; };
inline std::string encodeLaesS(const laes::Pcm& core, int baseRate, const std::vector<const Prep*>& ms, const std::vector<MasterSel>& sel, std::vector<laes::MasterInfo>& info,
                               const std::string& bodyPath, const std::string& stem, int coreMode, int64_t piece, int threads) {
    using namespace laes; info.assign(ms.size(), MasterInfo()); uint8_t sh[32]; hashAnyCore(core, sh);
    if (core.bits != 16 && core.bits != 24 && core.bits != 32) return "core word length must be 16, 24 or 32"; if (coreMode != 0 && coreMode != 1) return "core mode must be 0 or 1";
    Out bo(bodyPath); CrcOut o(bo);
    o.u(0x3245414Cull, 4); o.u(1, 2); o.u(64, 2); o.u((uint64_t)baseRate, 4); o.u((uint64_t)core.bits, 1); o.u(2, 1); o.u(ms.size(), 1); o.u((uint64_t)coreMode, 1); o.u((uint64_t)core.frames(), 8); o.put(sh, 32); o.u(0, 8);
    if (ms.empty() || ms.size() > 16) return "1 to 16 masters";
    for (size_t mi = 0; mi < ms.size(); mi++) { const Prep& m = *ms[mi]; MasterInfo& I = info[mi];
        if (m.bits != 16 && m.bits != 24 && m.bits != 32) return "master word length must be 16, 24 or 32";
        int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
        I.bits = m.bits; I.stages = st; I.w = m.w; I.rate = m.rate; I.frames = m.frames; memcpy(I.sha, m.sha, 32);
        auto above = [&](int j) { double b = 0; for (int c = 0; c < 2; c++) for (int k = j + 1; k <= st; k++) b += (double)m.dbits[c][(size_t)k]; return b; };
        struct Cand { int storage, tier, j; int64_t whole; uint32_t frac; double bits; std::string e[2]; };
        std::vector<Cand> cs; const bool autoSel = sel[mi].storage < 0;
        auto relT = [&](int tier, int64_t& wh, uint32_t& fr) {
            if (!autoSel) { wh = sel[mi].whole; fr = sel[mi].frac; return true; }
            const int64_t lim = 1LL << 30; auto X = [&](size_t k) { return sel[k].whole * 4294967296LL + (int64_t)sel[k].frac; };
            if (sel[mi].whole <= -lim || sel[mi].whole >= lim || (tier && (sel[(size_t)tier - 1].whole <= -lim || sel[(size_t)tier - 1].whole >= lim))) return false;
            const int64_t x = X(mi) - (tier ? X((size_t)tier - 1) : 0); wh = x >> 32; fr = (uint32_t)(x & 0xFFFFFFFFLL); return true; };
        if (autoSel || sel[mi].storage == 1) cs.push_back({1, 0, 0, 0, 0, 0, {}});
        for (int t = 0; t <= (int)mi; t++) {
            if (!autoSel && t != sel[mi].refTier) continue; if (t > (int)mi) break;
            if (autoSel || sel[mi].storage == 0) cs.push_back({0, t, 0, 0, 0, 0, {}});
            if (t >= 1 && (autoSel || sel[mi].storage == 2)) { const int j = std::min(st, info[(size_t)t - 1].stages); if (j >= 1 || !autoSel) cs.push_back({2, t, j, 0, 0, 0, {}}); } }
        if (!autoSel && (sel[mi].refTier < 0 || sel[mi].refTier > (int)mi || sel[mi].storage > 2 || (sel[mi].storage == 2 && sel[mi].refTier == 0))) return "reference tier must be the core or an earlier master (storage 2: an earlier master)";
        int best = -1;
        for (size_t q = 0; q < cs.size(); q++) { Cand& C = cs[q]; C.bits = above(C.j);
            for (int c = 0; c < 2; c++) C.e[c] = stem + "_m" + std::to_string(mi) + "_k" + std::to_string(q) + "_c" + std::to_string(c) + ".lay";
            if (C.storage == 1) { for (int c = 0; c < 2; c++) { std::unique_ptr<Sig> x = levelOf(m, c, 0); LayerOut lo(C.e[c], 4096); V v; const int64_t P = piece;
                    for (int64_t a = 0; a < x->n; a += P) { const int64_t b = std::min(x->n, a + P); v.resize((size_t)(b - a)); x->get(a, b, v.data()); lo.feed(v.data(), (int64_t)v.size()); }
                    C.bits += (double)lo.finish(); } }
            else { if (!relT(C.tier, C.whole, C.frac)) { C.e[0].clear(); continue; } const int j = C.j; const long nj = levLen(I.frames, st - j);
                int up; if (C.tier == 0) up = (m.bits - I.w) - core.bits; else up = (m.bits - I.w) - (info[(size_t)C.tier - 1].bits - info[(size_t)C.tier - 1].w);
                const int64_t wj = C.whole * (1LL << j) + (j ? (int64_t)(C.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)C.frac << j);
                const std::vector<int64_t> h = refTaps(fj);
                for (int c = 0; c < 2; c++) { std::unique_ptr<Sig> src; if (C.tier == 0) src.reset(new MemSig(core.c[c])); else src = levelOf(*ms[(size_t)C.tier - 1], c, j);
                    std::unique_ptr<Sig> x = levelOf(m, c, j); if (x->n != nj) return "internal error: level length";
                    LayerOut lo(C.e[c], 4096L << j); V v, r; const int64_t P = piece << j;
                    for (int64_t a = 0; a < nj; a += P) { const int64_t b = std::min<int64_t>(nj, a + P); v.resize((size_t)(b - a)); x->get(a, b, v.data()); refPiece(*src, h, wj, up, a, b, r, threads);
                        for (size_t i = 0; i < v.size(); i++) v[i] -= r[i]; lo.feed(v.data(), (int64_t)v.size()); }
                    C.bits += (double)lo.finish(); } }
            if (best < 0 || C.bits < cs[(size_t)best].bits) { if (best >= 0) for (int c = 0; c < 2; c++) remove(cs[(size_t)best].e[c].c_str()); best = (int)q; }
            else for (int c = 0; c < 2; c++) remove(C.e[c].c_str()); }
        if (best < 0) return "no usable storage for master " + std::to_string(mi + 1);
        Cand& B = cs[(size_t)best]; I.storage = B.storage; I.refTier = B.storage == 1 ? 0 : B.tier; I.whole = B.storage == 1 ? 0 : B.whole; I.frac = B.storage == 1 ? 0 : B.frac; I.layerBits = B.bits;
        o.u(I.bits, 1); o.u(I.stages, 1); o.u(I.w, 1); o.u(I.storage, 1); o.u(I.refTier, 1); o.u(0, 3); o.u((uint64_t)I.rate, 4); o.u(0, 4); o.u((uint64_t)I.frames, 8);
        o.u((uint64_t)I.whole, 8); o.u(I.frac, 4); o.u(0, 4); o.put(I.sha, 32);
        for (int c = 0; c < 2; c++) { { In e(B.e[c]); o.u((uint64_t)e.n, 4); } o.file(B.e[c]); remove(B.e[c].c_str());
            for (int k = B.j + 1; k <= st; k++) { { In d(m.dfile[c][(size_t)k]); o.u((uint64_t)d.n, 4); } o.file(m.dfile[c][(size_t)k]); } } }
    const uint32_t crc = o.crc ^ 0xFFFFFFFFu; uint8_t t[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)}; bo.put(t, 4); bo.close();
    return ""; }
}  // namespace laess
