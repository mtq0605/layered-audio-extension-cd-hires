// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES file format and decoder (draft v1, 2026-10-28; the encoder side is in laes_file_enc.h, GPL / commercial). An LAES WAV file is an LAE file whose "LAE " chunk carries an LAES payload:
//   RIFF/WAVE: "fmt " (PCM, 2 ch, base rate, 16 bit) | "data" = the core (CD 44.1 kHz or DVD 48 kHz, 16 bit) | "LAE " = payload + CRC-32
// The same "LAE " chunk body followed by the 80-byte track trailer (type 2) is the per-track extension file of a CD-Extra disc (.LAX),
// handled by lae_cdx exactly like an LAE Lossy block. An LAE Lossy decoder rejects the payload (its magic is not "LAE1") and plays the core.
//
// Payload (little-endian):
//   header, 64 bytes: magic "LAE2" | u16 version = 1 | u16 header size = 64 | u32 base rate | u8 core bits (16 / 24 / 32) | u8 channels = 2 |
//                     u8 number of masters (1..16) | u8 core mode (0 = core in the WAV "data" / CD tracks, 1 = core in the FLAC audio frames) |
//                     u64 core frames | 32-byte SHA-256 of the core (16 bit: int16 LE as in "data"; 24 / 32 bit: int32 LE) |
//                     u64 reserved = 0
//   per master, 72-byte record, then its layers:
//     u8 bits (16/24/32) | u8 stages (0..3: rate = base * 2^stages) | u8 unused low bits w | u8 storage (0 = difference to a tier at the base
//     level, 1 = full, 2 = difference to an earlier master at the highest level both have) | u8 reference tier (0 = core, k = master k;
//     storage 0: core or any earlier master; storage 2: an earlier master) | 3 reserved | u32 rate | u32 reserved | u64 frames |
//     i64 T whole | u32 T fraction (2^-32 base sample; T relative to the reference tier) | u32 reserved | 32-byte SHA-256 of the master
//     (samples as int32 LE, L R L R ...)
//     per channel (L, then R), with j = 0 for storage 0 / 1 and j = min(stages, stages of the tier) for storage 2:
//       u32 length + the level-j difference (storage 0 / 2) or the base level (storage 1), coded with laes_codec (block 4096 * 2^j);
//       then for each stage k = j+1..stages: u32 length + octave layer k (block 4096 * 2^(k-1))
//   No index, no seek table, no cue table; nothing interleaved with the core.
// Reconstruction of master m: level j = (storage 1 ? 0 : reference(tier's level j, T * 2^j, grid difference)) + layer; then inv() per stage
// k = j+1..stages; samples << w. A tier's level j comes from its decoded samples (>> w) by fwd(). Each channel only from the same channel.
// grid difference (up) = the master's significant bits (bits - w) minus the tier's (core: its bits; master k: bits_k - w_k); it may be negative.
// One storage decision and one reference tier per master (never per block); no weights or gains are stored.
#pragma once
#include <cstdint>
#include <climits>
#include <cstring>
#include <string>
#include <vector>
#include "laes_codec.h"
#include "laes_lift.h"   // fast forms of fwd / inv (laesfast, bit-identical to laes_math.h)
namespace laes {
// ---- SHA-256 (FIPS 180-4), written for this project
struct Sha256 { uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; uint8_t buf[64]; size_t n = 0; uint64_t len = 0;
    static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const uint8_t* p) { static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64]; for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) { uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i], S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh; }
    void update(const uint8_t* p, size_t k) { len += k; while (k) { size_t t = std::min(k, 64 - n); memcpy(buf + n, p, t); n += t; p += t; k -= t; if (n == 64) { block(buf); n = 0; } } }
    void final(uint8_t out[32]) { uint64_t bits = len * 8; uint8_t pad = 0x80; update(&pad, 1); uint8_t z = 0; while (n != 56) update(&z, 1);
        uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i)); update(L, 8); for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j)); } };
// ---- types
struct Pcm { int rate = 0, bits = 0; V c[2]; long frames() const { return (long)c[0].size(); } };
inline void hashPcm(const Pcm& p, uint8_t out[32]) { Sha256 s; std::vector<uint8_t> b; b.reserve(1 << 16);
    for (long i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { uint32_t v = (uint32_t)(int32_t)p.c[c][i]; for (int k = 0; k < 4; k++) b.push_back((uint8_t)(v >> (8 * k))); if (b.size() >= (1 << 16)) { s.update(b.data(), b.size()); b.clear(); } }
    s.update(b.data(), b.size()); s.final(out); }
inline void hashCore(const Pcm& p, uint8_t out[32]) { Sha256 s; std::vector<uint8_t> b; b.reserve(1 << 16);   // int16 LE, as in the "data" chunk
    for (long i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { uint16_t v = (uint16_t)(int16_t)p.c[c][i]; b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); if (b.size() >= (1 << 16)) { s.update(b.data(), b.size()); b.clear(); } }
    s.update(b.data(), b.size()); s.final(out); }
struct MasterInfo { int bits = 0, stages = 0, w = 0, storage = 0, refTier = 0, rate = 0; long frames = 0; int64_t whole = 0; uint32_t frac = 0; uint8_t sha[32]; double layerBits = 0; };
inline long levLen(long n, int k) { while (k-- > 0) n = (n + 1) / 2; return n; }
inline int unusedBits(const Pcm& p) { int64_t o = 0; for (int c = 0; c < 2; c++) for (int64_t v : p.c[c]) o |= v; if (!o) return 0; int w = 0; while (w < 31 && !((o >> w) & 1)) w++; return w; }
// the reference of a tier on the master's base grid, nOut values: r[i] = round(sum_q h[q] src[m(i + whole + q)] / 2^(31 - up)), h = refTaps(frac).
// Normative form: laes::reference (laes_math.h). This is its fast form (same values; laesfast technique, 2026-10-29): direct indexing away from
// the ends, 64-bit accumulation when 2 x max|src| x sum|h| + rounding stays below 2^63 (then the 64-bit sum equals the 128-bit one),
// otherwise 128-bit; indices stay int64 as in the normative form; the output is split across threads.
inline V refLen(const V& src, int64_t nOut, int64_t whole, uint32_t frac, int up) {
    const std::vector<int64_t> h = refTaps(frac); const int64_t n = (int64_t)src.size(); V r(nOut); const int s = 31 - up;
    int64_t sh = 0; for (int64_t v : h) sh += v < 0 ? -v : v;
    const bool wide = !((double)laesfast::maxAbs(src) * (double)sh + (s > 0 ? (double)(1LL << (s - 1)) : 0.0) < 9.0e18);
    laesfast::parallelFor((long)nOut, laesfast::defaultThreads(), [&](long a, long b) { for (int64_t i = a; i < b; i++) { const int64_t j0 = i + whole - 127, j1 = i + whole + 128;
        if (!wide) { int64_t acc = 0;
            if (j0 >= 0 && j1 < n) { const int64_t* c = src.data() + j0; for (int q = 0; q < 256; q++) acc += h[q] * c[q]; }
            else for (int q = -127; q <= 128; q++) acc += h[q + 127] * src[mir(i + whole + q, n)];
            r[i] = s > 0 ? (acc + (1LL << (s - 1))) >> s : (int64_t)((uint64_t)acc << (-s)); }
        else { Acc A;
            if (j0 >= 0 && j1 < n) { const int64_t* c = src.data() + j0; for (int q = 0; q < 256; q++) A.mac(h[q], c[q]); }
            else for (int q = -127; q <= 128; q++) A.mac(h[q + 127], src[mir(i + whole + q, n)]);
            r[i] = s > 0 ? A.shr(s) : (int64_t)((uint64_t)A.lo << (-s)); } } });
    return r; }
struct BRd2 { const uint8_t* p; size_t n, pos = 0; bool fail = false; uint64_t u(int k) { if (pos + k > n) { fail = true; return 0; } uint64_t v = 0; for (int i = 0; i < k; i++) v |= (uint64_t)p[pos + i] << (8 * i); pos += k; return v; }
    const uint8_t* take(size_t k) { if (pos + k > n) { fail = true; return nullptr; } const uint8_t* r = p + pos; pos += k; return r; } };
inline void hashAnyCore(const Pcm& p, uint8_t out[32]) { if (p.bits == 16) hashCore(p, out); else hashPcm(p, out); }
// ---- decoder: "LAE " chunk body + core -> masters. Every field and layer is checked; returns "" or the reason.
inline bool isLaes(const std::vector<uint8_t>& body) { return body.size() >= 8 && !memcmp(body.data(), "LAE2", 4); }
inline std::string decodeLaes(const std::vector<uint8_t>& body, const Pcm& core, int baseRate, std::vector<Pcm>& out, std::vector<MasterInfo>& info, std::vector<bool>& shaOk) {
    out.clear(); info.clear(); shaOk.clear(); if (body.size() < 64 + 4) return "LAES block too short";
    uint32_t crc = 0xFFFFFFFFu; for (size_t i = 0; i + 4 < body.size(); i++) { crc ^= body[i]; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } crc ^= 0xFFFFFFFFu;
    uint32_t stored = body[body.size() - 4] | body[body.size() - 3] << 8 | body[body.size() - 2] << 16 | (uint32_t)body[body.size() - 1] << 24; if (crc != stored) return "LAES block damaged (CRC-32)";
    BRd2 r{body.data(), body.size() - 4};
    uint64_t magic = r.u(4), ver = r.u(2), hs = r.u(2), base = r.u(4), cbits = r.u(1), ch = r.u(1), nm = r.u(1), cmode = r.u(1), nc = r.u(8); const uint8_t* csha = r.take(32); r.u(8);
    if (r.fail || magic != 0x3245414Cull) return "not an LAES block"; if (ver != 1 || hs != 64) return "unsupported LAES version";
    if ((base != 44100 && base != 48000) || (int)base != baseRate || (int)cbits != core.bits || ch != 2 || cmode > 1 || nm < 1 || nm > 16) return "invalid LAES header";
    if (nc != (uint64_t)core.frames()) return "core length differs from the LAES header";   // compared as uint64: nothing truncated
    { uint8_t h[32]; hashAnyCore(core, h); if (memcmp(h, csha, 32)) return "core differs from the one this LAES block was made for (SHA-256)"; }
    std::vector<std::vector<std::vector<V>>> levs;   // levs[m][c][k]: level k of decoded master m (samples >> w)
    for (uint64_t mi = 0; mi < nm; mi++) { MasterInfo I;
        I.bits = (int)r.u(1); I.stages = (int)r.u(1); I.w = (int)r.u(1); I.storage = (int)r.u(1); I.refTier = (int)r.u(1); r.u(3); I.rate = (int)r.u(4); r.u(4); const uint64_t F64 = r.u(8);
        I.whole = (int64_t)r.u(8); I.frac = (uint32_t)r.u(4); r.u(4); const uint8_t* sha = r.take(32);
        if (r.fail) return "LAES block truncated";
        if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int)mi) || (I.storage == 2 && I.refTier < 1)
            || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 < 1 || ((F64 + (1ull << I.stages) - 1) >> I.stages) > nc + (1ull << 24)   // all in uint64
            || I.whole <= -(1LL << 40) || I.whole >= (1LL << 40)) return "invalid master record";
        if (F64 > (uint64_t)LONG_MAX) return "master too long for this decoder"; I.frames = (long)F64;
        memcpy(I.sha, sha, 32); const int j = I.storage == 2 ? std::min(I.stages, info[I.refTier - 1].stages) : 0; const long nj = levLen(I.frames, I.stages - j);
        Pcm m; m.rate = I.rate; m.bits = I.bits; std::vector<V> top(2);
        for (int c = 0; c < 2; c++) {
            uint32_t eb = (uint32_t)r.u(4); const uint8_t* ep = r.take(eb); if (r.fail) return "LAES block truncated";
            bool ok; V x = decodeLayer(std::vector<uint8_t>(ep, ep + eb), nj, 4096L << j, ok); if (!ok) return "damaged base layer";
            if (I.storage != 1) { const V* src; int up; if (I.refTier == 0) { src = &core.c[c]; up = (I.bits - I.w) - core.bits; } else { src = &levs[I.refTier - 1][c][j]; up = (I.bits - I.w) - (info[I.refTier - 1].bits - info[I.refTier - 1].w); }
                const int64_t wj = I.whole * (1LL << j) + (j ? (int64_t)(I.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)I.frac << j);   // T in level-j samples
                V rr = refLen(*src, nj, wj, fj, up); for (long i = 0; i < nj; i++) x[i] += rr[i]; }
            for (int k = j + 1; k <= I.stages; k++) { long nh = levLen(I.frames, I.stages - k); uint32_t db = (uint32_t)r.u(4); const uint8_t* dp = r.take(db); if (r.fail) return "LAES block truncated";
                V d = decodeLayer(std::vector<uint8_t>(dp, dp + db), nh / 2, 4096L << (k - 1), ok); if (!ok) return "damaged octave layer"; x = laesfast::inv(x, d, nh); }
            top[c] = x; m.c[c].resize(I.frames); for (long i = 0; i < I.frames; i++) m.c[c][i] = x[i] << I.w; }
        std::vector<std::vector<V>> lv(2, std::vector<V>(I.stages + 1));   // every level of this master, for later masters that refer to it
        for (int c = 0; c < 2; c++) { lv[c][I.stages] = top[c]; for (int k = I.stages; k > 0; k--) { V d; laesfast::fwd(lv[c][k], lv[c][k - 1], d); } }
        uint8_t h[32]; hashPcm(m, h); shaOk.push_back(memcmp(h, I.sha, 32) == 0); levs.push_back(lv); info.push_back(I); out.push_back(m); }
    if (r.pos != r.n) return "unexpected bytes after the last master";
    return ""; }
}  // namespace laes
