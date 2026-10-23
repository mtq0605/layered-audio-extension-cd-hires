// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE encapsulation (spec/cdx/SPEC.md, section 3): the per-track extension file (.LAX) = the v5 "LAE " chunk body
// followed by an 80-byte track trailer; the trailer's whole-track MD5, the content checksums S1/S2 (sliding, for the per-track
// content search) and the leading/trailing digital-silence counts lead0/tail0 (coarse location).
// Shared by the packaging tool, the reader and future player plugins.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "align_hash.h"
namespace cdx {
static const uint64_t M = (1ULL << 61) - 1;
static const size_t TRAILER = 80;
inline uint64_t modM(uint64_t v) { v = (v & M) + (v >> 61); return v >= M ? v - M : v; }   // v < 2^64
inline uint32_t crc32(const uint8_t* p, size_t n) {   // reflected 0xEDB88320, init and final xor 0xFFFFFFFF (= SPEC 3.3)
    struct Table { uint32_t t[256]; Table() { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } } };
    static const Table tab; const uint32_t* t = tab.t;   // built once, thread-safe
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
// frame word of one stereo frame: (L mod 2^16) + 2^16 (R mod 2^16), i.e. its 4 bytes in the MD5 byte stream read little-endian
inline uint32_t word(int16_t L, int16_t R) { return (uint32_t)(uint16_t)L | (uint32_t)(uint16_t)R << 16; }

struct Trailer {
    uint16_t version = 1, type = 1, track = 1; uint32_t rate = 44100, ncd = 0, lead0 = 0, tail0 = 0; uint8_t md5[16] = {0}; uint64_t s1 = 0, s2 = 0;
    std::string md5hex() const { static const char* h = "0123456789abcdef"; std::string s; for (int i = 0; i < 16; i++) { s += h[md5[i] >> 4]; s += h[md5[i] & 15]; } return s; } };

// trailer fields of a CD layer given as interleaved int16 (ncd frames)
inline Trailer compute(const int16_t* cd, uint32_t ncd, uint32_t rate, uint16_t track, uint16_t type = 1) {
    Trailer t; t.rate = rate; t.ncd = ncd; t.track = track; t.type = type;
    alignhash::Md5 m; uint64_t a = 0, b = 0;
    for (uint32_t i = 0; i < ncd; i++) { uint8_t by[4] = {(uint8_t)cd[i * 2], (uint8_t)((uint16_t)cd[i * 2] >> 8), (uint8_t)cd[i * 2 + 1], (uint8_t)((uint16_t)cd[i * 2 + 1] >> 8)}; m.update(by, 4);
        uint64_t w = word(cd[i * 2], cd[i * 2 + 1]); a = modM(a + w); b = modM(b + modM((uint64_t)(i + 1) * w)); }
    std::string hx = m.hex(); for (int i = 0; i < 16; i++) t.md5[i] = (uint8_t)std::stoul(hx.substr(i * 2, 2), nullptr, 16);
    t.s1 = a; t.s2 = b; long l0, t0; alignhash::silenceEdges(cd, (long)ncd, l0, t0); t.lead0 = (uint32_t)l0; t.tail0 = (uint32_t)(l0 == (long)ncd ? ncd : t0); return t; }

inline std::vector<uint8_t> serialize(const Trailer& t) {
    std::vector<uint8_t> o(TRAILER, 0); auto p16 = [&](size_t at, uint16_t v) { o[at] = (uint8_t)v; o[at + 1] = (uint8_t)(v >> 8); };
    auto p32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) o[at + i] = (uint8_t)(v >> (8 * i)); }; auto p64 = [&](size_t at, uint64_t v) { for (int i = 0; i < 8; i++) o[at + i] = (uint8_t)(v >> (8 * i)); };
    memcpy(&o[0], "LTVT", 4); p16(4, t.version); p16(6, (uint16_t)TRAILER); p16(8, t.type); p16(10, t.track); p32(12, t.rate); p32(16, t.ncd);
    memcpy(&o[20], t.md5, 16); p64(36, t.s1); p64(44, t.s2); p32(52, t.lead0); p32(56, t.tail0); p32(76, crc32(o.data(), 76)); return o; }

// the reading checks of X7, in order; returns "" when the file is usable, otherwise the reason for the report.
// cd: the file belongs to a CD (track 1-98 and a CD layer at 44100 Hz, X2); otherwise track >= 1 and 44100 or 48000 Hz
inline std::string parse(const std::vector<uint8_t>& file, Trailer& t, size_t& blockLen, bool cd = false) {
    if (file.size() < 8 + TRAILER || memcmp(&file[file.size() - TRAILER], "LTVT", 4)) return "no trailer";
    const uint8_t* r = &file[file.size() - TRAILER]; auto g16 = [&](size_t at) { return (uint16_t)(r[at] | r[at + 1] << 8); };
    auto g32 = [&](size_t at) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | r[at + i]; return v; }; auto g64 = [&](size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | r[at + i]; return v; };
    if (g32(76) != crc32(r, 76)) return "trailer damaged";
    t.version = g16(4); if (t.version != 1 || g16(6) != TRAILER) return "unknown trailer version";
    t.type = g16(8); if (t.type == 2) return "LAES extension block, not supported by this reader"; if (t.type != 1) return "unknown extension block type";
    t.track = g16(10); t.rate = g32(12); t.ncd = g32(16); memcpy(t.md5, r + 20, 16); t.s1 = g64(36); t.s2 = g64(44); t.lead0 = g32(52); t.tail0 = g32(56);
    bool ok = t.track >= 1 && (!cd || t.track <= 98) && (t.rate == 44100 || (!cd && t.rate == 48000)) && t.ncd >= 1 && t.ncd < (1u << 28) && t.s1 < M && t.s2 < M && t.lead0 <= t.ncd && t.tail0 <= t.ncd
              && ((t.lead0 == t.ncd && t.tail0 == t.ncd) || (uint64_t)t.lead0 + t.tail0 < t.ncd);
    if (!ok) return "invalid trailer field"; blockLen = file.size() - TRAILER; return ""; }

// sliding content checksums over a stream of frame words x[0..len): for each start t in [t0, t1] (window [t, t+n) inside the
// stream) calls hit(t) when A(t) = S1 and B(t) = S2 (X4). Positions outside the stream count as 0 (R5).
template <class X, class Hit> void slide(const X& x, long long len, uint32_t n, long long t0, long long t1, uint64_t S1, uint64_t S2, Hit hit) {
    auto at = [&](long long j) -> uint64_t { return j >= 0 && j < len ? (uint64_t)x(j) : 0; };
    if (t1 < t0) return; uint64_t A = 0, B = 0;
    for (uint32_t i = 0; i < n; i++) { uint64_t w = at(t0 + i); A = modM(A + w); B = modM(B + modM((uint64_t)(i + 1) * w)); }
    for (long long t = t0;; t++) { if (A == S1 && B == S2) hit(t); if (t == t1) break;
        uint64_t xin = at(t + n), xout = at(t); B = modM(B + (M - A) + modM((uint64_t)n * xin)); A = modM(A + (M - xout) + xin); } }
}
