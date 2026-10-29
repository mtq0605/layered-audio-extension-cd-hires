// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAES payload, encoder side (split from laes_file.h, 2026-10-29; the code is unchanged). The format, the types the decoder needs and
// decodeLaes are in laes_file.h (Apache-2.0). Here: the encoder input (MasterIn), the byte writer and encodeLaes (storage 0 / 1 / 2 and
// the reference tier chosen by size, once per master).
#pragma once
#include "laes_file.h"
#include "laes_codec_enc.h"
namespace laes {
// storage -1 = the encoder chooses storage and reference tier (refTier is then ignored and whole / frac are T relative to the core);
// storage 0 / 1 / 2 with refTier given: whole / frac are T relative to that tier
struct MasterIn { Pcm m; int storage = -1; int refTier = 0; int64_t whole = 0; uint32_t frac = 0; };
struct BWr { std::vector<uint8_t> b; void u(uint64_t v, int n) { for (int i = 0; i < n; i++) b.push_back((uint8_t)(v >> (8 * i))); } void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); } };
// ---- encoder: core (16 / 24 / 32 bit) + masters -> "LAE " chunk body (payload + CRC-32); coreMode 0 = WAV / CD, 1 = FLAC audio frames
inline std::vector<uint8_t> encodeLaes(const Pcm& core, int baseRate, const std::vector<MasterIn>& ms, std::vector<MasterInfo>& info, std::string& err, int coreMode = 0) {
    err.clear(); info.assign(ms.size(), MasterInfo()); BWr o; uint8_t sh[32]; hashAnyCore(core, sh);
    if (core.bits != 16 && core.bits != 24 && core.bits != 32) { err = "core word length must be 16, 24 or 32"; return {}; } if (coreMode != 0 && coreMode != 1) { err = "core mode must be 0 or 1"; return {}; }
    o.u(0x3245414Cull, 4); o.u(1, 2); o.u(64, 2); o.u((uint64_t)baseRate, 4); o.u((uint64_t)core.bits, 1); o.u(2, 1); o.u(ms.size(), 1); o.u((uint64_t)coreMode, 1); o.u((uint64_t)core.frames(), 8); o.raw(sh, 32); o.u(0, 8);
    if (ms.empty() || ms.size() > 16) { err = "1 to 16 masters"; return {}; }
    std::vector<std::vector<std::vector<V>>> lev(ms.size());   // lev[m][c][k]: level k of master m (samples >> w), k = 0..stages
    for (size_t mi = 0; mi < ms.size(); mi++) { const Pcm& m = ms[mi].m; MasterInfo& I = info[mi];
        if (m.bits != 16 && m.bits != 24 && m.bits != 32) { err = "master word length must be 16, 24 or 32"; return {}; }
        int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) { err = "master rate must be the base rate times 1, 2, 4 or 8"; return {}; }
        I.bits = m.bits; I.stages = st; I.w = unusedBits(m); I.rate = m.rate; I.frames = m.frames(); hashPcm(m, I.sha);
        std::vector<std::vector<uint8_t>> d[2]; std::vector<std::vector<size_t>> dbits(2, std::vector<size_t>(st + 1, 0)); lev[mi].assign(2, std::vector<V>(st + 1));
        for (int c = 0; c < 2; c++) { V x(I.frames); for (long i = 0; i < I.frames; i++) x[i] = m.c[c][i] >> I.w; std::vector<V>& s = lev[mi][c]; std::vector<V> dd(st + 1); s[st] = x;
            for (int k = st; k > 0; k--) laesfast::fwd(s[k], s[k - 1], dd[k]); d[c].resize(st + 1); for (int k = 1; k <= st; k++) d[c][k] = encodeLayer(dd[k], 4096L << (k - 1), dbits[c][k]); }
        auto above = [&](int j) { double b = 0; for (int c = 0; c < 2; c++) for (int k = j + 1; k <= st; k++) b += (double)dbits[c][k]; return b; };   // octave layers kept
        // candidates: (storage, tier, level j). Auto: storage 1; storage 0 against the core and every earlier master; storage 2 against every
        // earlier master that has a level above the base. Given: only that one. The cheapest (layer bits) wins, one decision per master.
        struct Cand { int storage, tier, j; int64_t whole; uint32_t frac; double bits; std::vector<uint8_t> e[2]; };
        std::vector<Cand> cs; const bool autoSel = ms[mi].storage < 0;
        auto relT = [&](int tier, int64_t& wh, uint32_t& fr) {   // T relative to the tier, from the absolute T of both (2^-32 base samples, int64)
            if (!autoSel) { wh = ms[mi].whole; fr = ms[mi].frac; return true; }
            const int64_t lim = 1LL << 30; auto X = [&](size_t k) { return ms[k].whole * 4294967296LL + (int64_t)ms[k].frac; };
            if (ms[mi].whole <= -lim || ms[mi].whole >= lim || (tier && (ms[tier - 1].whole <= -lim || ms[tier - 1].whole >= lim))) return false;
            const int64_t x = X(mi) - (tier ? X(tier - 1) : 0); wh = x >> 32; fr = (uint32_t)(x & 0xFFFFFFFFLL); return true; };
        if (autoSel || ms[mi].storage == 1) cs.push_back({1, 0, 0, 0, 0, 0});
        for (int t = 0; t <= (int)mi; t++) {
            if (!autoSel && t != ms[mi].refTier) continue; if (t > (int)mi) break;
            if (autoSel || ms[mi].storage == 0) cs.push_back({0, t, 0, 0, 0, 0});
            if (t >= 1 && (autoSel || ms[mi].storage == 2)) { const int j = std::min(st, info[t - 1].stages); if (j >= 1 || !autoSel) cs.push_back({2, t, j, 0, 0, 0}); } }
        if (!autoSel && (ms[mi].refTier < 0 || ms[mi].refTier > (int)mi || ms[mi].storage > 2 || (ms[mi].storage == 2 && ms[mi].refTier == 0))) { err = "reference tier must be the core or an earlier master (storage 2: an earlier master)"; return {}; }
        int best = -1;
        for (size_t q = 0; q < cs.size(); q++) { Cand& C = cs[q]; C.bits = above(C.j);
            if (C.storage == 1) { for (int c = 0; c < 2; c++) { size_t b; C.e[c] = encodeLayer(lev[mi][c][0], 4096, b); C.bits += (double)b; } }
            else { if (!relT(C.tier, C.whole, C.frac)) continue; const int j = C.j; const long nj = levLen(I.frames, st - j);
                int up; if (C.tier == 0) up = (m.bits - I.w) - core.bits; else up = (m.bits - I.w) - (info[C.tier - 1].bits - info[C.tier - 1].w);
                const int64_t wj = C.whole * (1LL << j) + (j ? (int64_t)(C.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)C.frac << j);   // T in level-j samples
                for (int c = 0; c < 2; c++) { const V& srcv = C.tier == 0 ? core.c[c] : lev[C.tier - 1][c][j]; V r = refLen(srcv, nj, wj, fj, up), e(nj);
                    for (long i = 0; i < nj; i++) e[i] = lev[mi][c][j][i] - r[i]; size_t b; C.e[c] = encodeLayer(e, 4096L << j, b); C.bits += (double)b; } }
            if (best < 0 || C.bits < cs[best].bits) best = (int)q; }
        if (best < 0) { err = "no usable storage for master " + std::to_string(mi + 1); return {}; }
        Cand& B = cs[best]; I.storage = B.storage; I.refTier = B.storage == 1 ? 0 : B.tier; I.whole = B.storage == 1 ? 0 : B.whole; I.frac = B.storage == 1 ? 0 : B.frac; I.layerBits = B.bits;
        o.u(I.bits, 1); o.u(I.stages, 1); o.u(I.w, 1); o.u(I.storage, 1); o.u(I.refTier, 1); o.u(0, 3); o.u((uint64_t)I.rate, 4); o.u(0, 4); o.u((uint64_t)I.frames, 8);
        o.u((uint64_t)I.whole, 8); o.u(I.frac, 4); o.u(0, 4); o.raw(I.sha, 32);
        for (int c = 0; c < 2; c++) { o.u(B.e[c].size(), 4); o.raw(B.e[c].data(), B.e[c].size()); for (int k = B.j + 1; k <= st; k++) { o.u(d[c][k].size(), 4); o.raw(d[c][k].data(), d[c][k].size()); } } }
    uint32_t crc = 0xFFFFFFFFu; for (uint8_t by : o.b) { crc ^= by; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } crc ^= 0xFFFFFFFFu; o.u(crc, 4);
    return o.b; }
}  // namespace laes
