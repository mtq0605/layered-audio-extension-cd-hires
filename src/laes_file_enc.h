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
struct MasterIn { Pcm m; int storage = -1; int refTier = 0; int64_t whole = 0; uint32_t frac = 0; std::string name; int flags = 0; int menu = 0; };   // name, flags, menu position: payload version 2 (laes_file.h)
struct BWr { std::vector<uint8_t> b; void u(uint64_t v, int n) { for (int i = 0; i < n; i++) b.push_back((uint8_t)(v >> (8 * i))); } void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); } };
// ---- encoder: core (16 / 24 / 32 bit) + masters -> "LAE " chunk body (payload + CRC-32); coreMode 0 = WAV / CD, 1 = FLAC audio frames
inline std::vector<uint8_t> encodeLaes(const Pcm& core, int baseRate, const std::vector<MasterIn>& ms, std::vector<MasterInfo>& info, std::string& err, int coreMode = 0, const Head& head = Head()) {
    err.clear(); info.assign(ms.size(), MasterInfo()); BWr o; uint8_t sh[32]; hashAnyCore(core, sh);
    if (core.bits != 16 && core.bits != 24 && core.bits != 32) { err = "core master word length must be 16, 24 or 32"; return {}; } if (coreMode != 0 && coreMode != 1) { err = "core master mode must be 0 or 1"; return {}; }
    if (ms.empty() || ms.size() > 255) { err = "1 to 255 masters"; return {}; }
    int ver = ms.size() > 16 ? 2 : 1; for (const MasterIn& x : ms) { if (!validMasterName(x.name)) { err = "a master name must be UTF-8 of at most 255 bytes without control characters"; return {}; } if (x.flags & ~kNoDerive) { err = "unknown master flag"; return {}; } if (!x.name.empty() || x.flags || x.menu) ver = 2; }
    { std::vector<MasterInfo> t(ms.size()); for (size_t i = 0; i < ms.size(); i++) t[i].menu = ms[i].menu; if (!menuOk(t)) { err = "menu positions must be 1 ... M, each once"; return {}; } }
    if (head.def < 0 || head.def > (int)ms.size()) { err = "the default master must be 0 (the core master) or 1 ... M"; return {}; } if (head.coreFlags & ~kNoDerive) { err = "unknown core master flag"; return {}; }
    if (head.def || head.coreFlags) ver = 2;
    o.u(0x3245414Cull, 4); o.u((uint64_t)ver, 2); o.u(64, 2); o.u((uint64_t)baseRate, 4); o.u((uint64_t)core.bits, 1); o.u(2, 1); o.u(ms.size(), 1); o.u((uint64_t)coreMode, 1); o.u((uint64_t)core.frames(), 8); o.raw(sh, 32);
    { uint8_t hb[8]; headTo(ver, head, hb); o.raw(hb, 8); }
    std::vector<std::vector<std::vector<V>>> lev(ms.size());   // lev[m][c][k]: level k of master m (samples >> w), k = 0..stages
    for (size_t mi = 0; mi < ms.size(); mi++) { const Pcm& m = ms[mi].m; MasterInfo& I = info[mi];
        if (m.bits != 16 && m.bits != 24 && m.bits != 32) { err = "master word length must be 16, 24 or 32"; return {}; }
        int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) { err = "master rate must be the base rate times 1, 2, 4 or 8"; return {}; }
        // FU12-5 (2026-11-14): input this format cannot hold is refused here, not first by the decoder's check of the written file
        const std::string mn = "master " + std::to_string(mi + 1);
        if (m.frames() < 1 || (int64_t)m.c[1].size() != m.frames()) { err = mn + (m.frames() < 1 ? " has no samples" : ": the two channels differ in length"); return {}; }
        if ((uint64_t)levLen(m.frames(), st) > (uint64_t)core.frames() + (1ull << 24)) { err = mn + " is too long: its base level may exceed the core master by at most 2^24 samples"; return {}; }
        { const int64_t lim = 1LL << (m.bits - 1); for (int c = 0; c < 2; c++) for (int64_t v : m.c[c]) if (v < -lim || v >= lim) { err = mn + ": a sample does not fit its word length"; return {}; } }
        if (ms[mi].storage >= 0 && (ms[mi].whole <= -(1LL << 40) || ms[mi].whole >= (1LL << 40))) { err = mn + ": T must lie within +-2^40 base samples"; return {}; }
        I.bits = m.bits; I.stages = st; I.w = unusedBits(m); I.rate = m.rate; I.frames = m.frames(); hashPcm(m, I.sha);
        std::vector<std::vector<uint8_t>> d[2]; std::vector<std::vector<size_t>> dbits(2, std::vector<size_t>(st + 1, 0)); lev[mi].assign(2, std::vector<V>(st + 1));
        for (int c = 0; c < 2; c++) { V x(I.frames); for (int64_t i = 0; i < I.frames; i++) x[i] = m.c[c][i] >> I.w; std::vector<V>& s = lev[mi][c]; std::vector<V> dd(st + 1); s[st] = x;
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
        if (!autoSel && (ms[mi].refTier < 0 || ms[mi].refTier > (int)mi || ms[mi].storage > 2 || (ms[mi].storage == 2 && ms[mi].refTier == 0))) { err = "reference tier must be the core master or an earlier master (storage 2: an earlier master)"; return {}; }
        int best = -1;
        for (size_t q = 0; q < cs.size(); q++) { Cand& C = cs[q]; C.bits = above(C.j);
            if (C.storage == 1) { for (int c = 0; c < 2; c++) { size_t b; C.e[c] = encodeLayer(lev[mi][c][0], 4096, b); C.bits += (double)b; } }
            else { if (!relT(C.tier, C.whole, C.frac)) continue; const int j = C.j; const int64_t nj = levLen(I.frames, st - j);
                int up; if (C.tier == 0) up = (m.bits - I.w) - core.bits; else up = (m.bits - I.w) - (info[C.tier - 1].bits - info[C.tier - 1].w);
                const int64_t wj = C.whole * (1LL << j) + (j ? (int64_t)(C.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)C.frac << j);   // T in level-j samples
                for (int c = 0; c < 2; c++) { const V& srcv = C.tier == 0 ? core.c[c] : lev[C.tier - 1][c][j]; V r = refLen(srcv, nj, wj, fj, up), e(nj);
                    for (int64_t i = 0; i < nj; i++) e[i] = lev[mi][c][j][i] - r[i]; size_t b; C.e[c] = encodeLayer(e, 4096L << j, b); C.bits += (double)b; } }
            if (best < 0 || C.bits < cs[best].bits) best = (int)q; }
        if (best < 0) { err = "no usable storage for master " + std::to_string(mi + 1); return {}; }
        Cand& B = cs[best];
        for (int c = 0; c < 2; c++) { bool big = B.e[c].size() > 0xFFFFFFFFull; for (int k = B.j + 1; k <= st; k++) big |= d[c][k].size() > 0xFFFFFFFFull;   // the u32 layer length (FU12-5)
            if (big) { err = mn + ": a layer is larger than 4 GiB, more than an LAES layer can hold"; return {}; } }
        I.storage =B.storage; I.refTier = B.storage == 1 ? 0 : B.tier; I.whole = B.storage == 1 ? ms[mi].whole : B.whole; I.frac = B.storage == 1 ? ms[mi].frac : B.frac; I.layerBits = B.bits;   // DEC 15: a master stored in full keeps its T (relative to the core master)
        I.name = ms[mi].name; I.flags = ms[mi].flags; I.menu = ms[mi].menu;
        o.u(I.bits, 1); o.u(I.stages, 1); o.u(I.w, 1); o.u(I.storage, 1); o.u(I.refTier, 1); o.u(ver == 2 ? I.name.size() : 0, 1); o.u(ver == 2 ? (uint64_t)I.flags : 0, 1); o.u(ver == 2 ? (uint64_t)I.menu : 0, 1); o.u((uint64_t)I.rate, 4); o.u(0, 4); o.u((uint64_t)I.frames, 8);
        o.u((uint64_t)I.whole, 8); o.u(I.frac, 4); o.u(0, 4); o.raw(I.sha, 32); if (ver == 2) o.raw((const uint8_t*)I.name.data(), I.name.size());
        for (int c = 0; c < 2; c++) { o.u(B.e[c].size(), 4); o.raw(B.e[c].data(), B.e[c].size()); for (int k = B.j + 1; k <= st; k++) { o.u(d[c][k].size(), 4); o.raw(d[c][k].data(), d[c][k].size()); } } }
    uint32_t crc = 0xFFFFFFFFu; for (uint8_t by : o.b) { crc ^= by; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } crc ^= 0xFFFFFFFFu; o.u(crc, 4);
    return o.b; }
}  // namespace laes
