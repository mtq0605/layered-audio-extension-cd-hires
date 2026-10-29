// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Range coding of partitioned layers: coder + the symbol walk shared by part_coder (verification) and lae_dec (standalone decoder).
// See part_coder.cpp for the symbol layout.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <type_traits>
#include "partition_format.h"
using namespace std;

// Binary range coder. Interval [low, low + width) on 32 bits; a symbol splits the width at split = (width >> 11) * p0, where p0 is the
// 11-bit probability of a 0 (adapted by 1/32 of the distance to 0 or 2048). The width is renormalised to >= 2^24 one byte at a time.
// Stream: a leading 0x00 byte, then one byte per renormalisation, then 4 bytes at the end.
static const uint32_t RC_TOP = 1u << 24;
inline void rcAdapt(uint16_t& p0, int b) { if (b) p0 = (uint16_t)(p0 - (p0 >> 5)); else p0 = (uint16_t)(p0 + ((2048 - p0) >> 5)); }
struct Enc { vector<uint8_t> out{ 0 }; uint32_t low = 0, width = 0xFFFFFFFFu;
    // adding to low can overflow 2^32: the overflow is added to the bytes already written (a run of 0xFF bytes becomes 0x00)
    void add(uint32_t v) { uint32_t before = low; low += v; if (low < before) { size_t k = out.size(); while (out[--k] == 0xFF) out[k] = 0; out[k]++; } }
    void normalise() { while (width < RC_TOP) { out.push_back((uint8_t)(low >> 24)); low <<= 8; width <<= 8; } }
    void bit(uint16_t& p0, int b) { uint32_t split = (width >> 11) * p0;
        if (b) { add(split); width -= split; } else width = split;
        rcAdapt(p0, b); normalise(); }
    void direct(uint32_t v, int n) { while (n-- > 0) { width >>= 1; if ((v >> n) & 1) add(width); normalise(); } }
    void flush() { for (int k = 0; k < 4; k++) { out.push_back((uint8_t)(low >> 24)); low <<= 8; } } };
struct Dec { const uint8_t* p; size_t n, i = 0; uint32_t width = 0xFFFFFFFFu, code = 0;   // code = value - low
    uint8_t next() { return i < n ? p[i++] : 0; }
    void init() { for (int k = 0; k < 5; k++) code = (code << 8) | next(); }
    void normalise() { while (width < RC_TOP) { width <<= 8; code = (code << 8) | next(); } }
    int bit(uint16_t& p0) { uint32_t split = (width >> 11) * p0; int b = code >= split;
        if (b) { code -= split; width -= split; } else width = split;
        rcAdapt(p0, b); normalise(); return b; }
    uint32_t direct(int n) { uint32_t v = 0; while (n-- > 0) { width >>= 1; int b = code >= width; if (b) code -= width; v = (v << 1) | (uint32_t)b; normalise(); } return v; } };
static int blen(uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }
struct UModel { vector<uint16_t> tree, mant; UModel() : tree(64, 1024), mant(64, 1024) {} };
template <class C> static void codeU(C& c, UModel& m, uint32_t& v, bool enc) {
    int len = enc ? blen(v) : 0; int node = 1;
    for (int b = 4; b >= 0; b--) { int bit = enc ? (len >> b) & 1 : 0; if constexpr (is_same<C, Enc>::value) c.bit(m.tree[node], bit); else bit = c.bit(m.tree[node]); node = node * 2 + bit; }
    if (!enc) len = node - 32;
    if (len <= 1) { if (!enc) v = len; return; }
    int top = enc ? (v >> (len - 2)) & 1 : 0;
    if constexpr (is_same<C, Enc>::value) c.bit(m.mant[len], top); else top = c.bit(m.mant[len]);
    uint32_t rest = 0; int nr = len - 2;
    if constexpr (is_same<C, Enc>::value) { rest = v & ((1u << nr) - 1); if (nr) c.direct(rest, nr); } else { if (nr) rest = c.direct(nr); v = (1u << (len - 1)) | ((uint32_t)top << nr) | rest; }
}
static thread_local double g_tierBytes[4]; static thread_local long g_tierCoef[4];   // statistics: bytes and coefficients per block tier (encoder side; per thread)
static thread_local bool g_streamErr = false;   // decoder: set when a decoded symbol is out of range (corrupt stream); values are clamped, memory is never overrun; per thread (a thread decodes one stream at a time)
// CRC-32 (IEEE 802.3 polynomial, reflected, the common public-domain table algorithm) for the LAE chunk integrity check
inline uint32_t crc32(const uint8_t* p, size_t n) { struct Table { uint32_t t[256]; Table() {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } } };
    static const Table tab; const uint32_t* t = tab.t;   // built once, thread-safe (C++11 static initialisation)
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
struct Hdr { long K; int N, k0, k1, rate; long L; int cw, lv, eq; };   // bins k0..k1-1 of an N-bin plane; lv = mode index (part::MODES)

// Walks the trees of the roots, one row of roots (ROOTT frames) per call, so a decoder can stream: the state (adaptive models,
// previous tier and exponent, next row) persists between calls. enc: reads tokens from s and codes them; dec: decodes tokens into s.
// The plane is written at frame index (m - frame0): frame0 = 0 for a whole-track plane, frame0 = first frame of the row for a row buffer.
template <class C> struct Walker {
    const Hdr& h; int nc; part::Plane P; long M0 = 0;
    vector<UModel> coefM, expM; vector<uint16_t> tierP, zlP, expZero, expSign; size_t pos = 0; int prevTier = 0, prevE = 12;
    Walker(const Hdr& hh) : h(hh), nc(part::numCells(hh.k0, hh.k1)), P{nullptr, hh.K, hh.N, hh.k0, hh.k1, part::numCells(hh.k0, hh.k1)},
        coefM(3 * 13), expM(4), tierP(4 * 4, 1024), zlP(4 * 8, 1024), expZero(4, 1024), expSign(4, 1024) {}
    // the state of another walker of the same stream, bound to this stream's header hh (h is a reference: the default copy keeps the other's)
    Walker(const Hdr& hh, const Walker& o) : h(hh), nc(o.nc), P(o.P), M0(o.M0), coefM(o.coefM), expM(o.expM), tierP(o.tierP), zlP(o.zlP),
        expZero(o.expZero), expSign(o.expSign), pos(o.pos), prevTier(o.prevTier), prevE(o.prevE) {}
    bool done() const { return M0 >= h.K; }
    int rowFrames() const { return (int)min<long>(part::ROOTT, h.K - M0); }
    // S: the token store: a vector (whole stream), or any type with operator[] and push_back read / written in order (block-wise part coder)
    template <class S> void row(C& c, S& s, bool enc, vector<int32_t>& plane, long frame0) {
    auto tok = [&](int32_t v) -> int32_t { if (enc) return s[pos++]; s.push_back(v); pos++; return v; };
    auto bits = [&](int32_t v, int n) -> int32_t { if constexpr (is_same<C, Enc>::value) { c.direct((uint32_t)v, n); return v; } else return (int32_t)c.direct(n); };
    auto tree = [&](uint16_t* pr, int v, int nb) { int node = 1; for (int b = nb - 1; b >= 0; b--) { int bit = enc ? (v >> b) & 1 : 0;
            if constexpr (is_same<C, Enc>::value) c.bit(pr[node], bit); else bit = c.bit(pr[node]); node = node * 2 + bit; } return node - (1 << nb); };
    for (int C0 = 0; C0 < nc; C0 += part::ROOTF) {
        int rf = min(part::ROOTF, nc - C0), rt = (int)min<long>(part::ROOTT, h.K - M0);
        function<void(int, int, int, int)> node = [&](int f0, int f1, int t0, int t1) {
            auto cf = part::cuts(f1 - f0, h.eq), ct = part::cuts(t1 - t0, h.eq);
            int split = 0;
            if (!cf.empty() || !ct.empty()) split = tok(bits(enc ? s[pos] : 0, 1));
            if (split) { int dir = ct.empty() ? 0 : (cf.empty() ? 1 : tok(bits(enc ? s[pos] : 0, 1)));
                const auto& cc = dir ? ct : cf; int nb = part::posBits(cc.size()); int pi = tok(nb ? bits(enc ? s[pos] : 0, nb) : 0); if (pi >= (int)cc.size()) { g_streamErr = true; pi = (int)cc.size() - 1; } int p = cc[pi];
                if (!dir) { node(f0, f0 + p, t0, t1); node(f0 + p, f1, t0, t1); } else { node(f0, f1, t0, t0 + p); node(f0, f1, t0 + p, t1); }
                return; }
            size_t outAt = 0; if constexpr (is_same<C, Enc>::value) outAt = c.out.size();
            int tier = tok(tree(&tierP[prevTier * 4], enc ? s[pos] : 0, 2)); prevTier = tier;
            if (!tier) return;
            // exponent: delta to the previous coded leaf
            int E = enc ? s[pos] : 0; int d = E - prevE, z = d == 0;
            if constexpr (is_same<C, Enc>::value) c.bit(expZero[tier], z); else z = c.bit(expZero[tier]);
            if (!z) { int sg = enc ? d < 0 : 0; if constexpr (is_same<C, Enc>::value) c.bit(expSign[tier], sg); else sg = c.bit(expSign[tier]);
                uint32_t a = enc ? (uint32_t)(abs(d) - 1) : 0; codeU(c, expM[tier], a, enc); if (!enc) d = sg ? -(int)(a + 1) : (int)(a + 1); } else d = 0;
            if (!enc && (prevE + d < 0 || prevE + d > 34)) { g_streamErr = true; d = max(0, min(34, prevE + d)) - prevE; }   // exponents beyond 24-bit range: corrupt
            E = tok(prevE + d); prevE = E;
            int zl = tok(tree(&zlP[tier * 8], enc ? s[pos] : 0, 3));
            int sh = part::shiftOfR(E, tier, (h.cw >> 8) & 255); double st = ldexp(1.0, sh); UModel& cm = coefM[(tier - 1) * 13 + min(12, max(0, E / 2))];
            for (int m = M0 + t0; m < M0 + t1; m++) for (int cc = C0 + f0; cc < C0 + f1; cc++) { int kb; int nb = part::cellBins(P, cc, kb);
                for (int j = 0; j < nb; j++) { int32_t q = enc ? s[pos] : 0; uint32_t a = (uint32_t)abs(q); codeU(c, cm, a, enc);
                    if (a) { int sg = enc ? q < 0 : 0; if constexpr (is_same<C, Enc>::value) c.direct(sg, 1); else sg = c.direct(1); if (!enc) q = sg ? -(int32_t)a : (int32_t)a; } else q = 0;
                    tok(q); size_t ix = (size_t)(m - frame0) * h.N + kb + j;
                    int zs = 0; if (!q && zl) zs = tok(bits(enc ? s[pos] : 0, 1));   // zero level: the true sign of a zero coefficient (bypass bit)
                    double v = q ? part::recon(q, st) : (zl ? (zs ? -1.0 : 1.0) * part::ZC[zl] * st : 0.0);
                    if (v > 1073741823.0 || v < -1073741823.0) { g_streamErr = true; v = v > 0 ? 1073741823.0 : -1073741823.0; }   // beyond any 24-bit signal: corrupt
                    plane[ix] = (int32_t)llround(v); } }
            if constexpr (is_same<C, Enc>::value) { g_tierBytes[tier] += (double)(c.out.size() - outAt); g_tierCoef[tier] += (long)(t1 - t0) * (f1 - f0) * part::CW; } };
        node(0, rf, 0, rt); }
        M0 += part::ROOTT; }
};
// whole stream in one go (encoder, verification tool)
template <class C> static void run(C& c, const Hdr& h, vector<int32_t>& s, bool enc, vector<int32_t>& plane) {
    Walker<C> w(h); while (!w.done()) w.row(c, s, enc, plane, 0); }
