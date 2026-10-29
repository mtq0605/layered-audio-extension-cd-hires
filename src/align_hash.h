// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Shared by the encoder's alignment check and the planned CD-Extra reader:
//  - MD5 (algorithm of RFC 1321, implemented from its description) over the CD layer as FLAC defines its audio MD5:
//    every stereo frame as interleaved little-endian 16-bit samples (ncd x 4 bytes);
//  - digital-silence edges: number of leading / trailing frames whose samples are all exactly zero.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace alignhash {
struct Md5 {
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}; uint64_t bytes = 0; uint8_t buf[64]; size_t fill = 0;
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
        uint32_t m[16]; for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) { uint32_t f; int g, r = i / 16;
            if (r == 0) { f = (b & c) | (~b & d); g = i; } else if (r == 1) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (r == 2) { f = b ^ c ^ d; g = (3 * i + 5) % 16; } else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t t = d; d = c; c = b; b = b + rotl(a + f + K[i] + m[g], S[r * 4 + i % 4]); a = t; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; }
    void update(const void* data, size_t n) { const uint8_t* p = (const uint8_t*)data; bytes += n;
        while (n) { size_t k = 64 - fill < n ? 64 - fill : n; memcpy(buf + fill, p, k); fill += k; p += k; n -= k; if (fill == 64) { block(buf); fill = 0; } } }
    std::string hex() {   // finalises; call once
        uint64_t bits = bytes * 8; uint8_t pad = 0x80; update(&pad, 1); uint8_t z = 0; while (fill != 56) update(&z, 1);
        uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i)); update(len, 8);
        static const char* hx = "0123456789abcdef"; std::string s;
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { uint8_t v = (uint8_t)(h[i] >> (8 * k)); s += hx[v >> 4]; s += hx[v & 15]; }
        return s; } };
// MD5 of a CD layer given as interleaved stereo int16 (host order must be little-endian, as on every supported platform)
inline std::string cdMd5(const int16_t* s, size_t frames) {
    Md5 m; for (size_t i = 0; i < frames * 2; i++) { uint8_t b[2] = {(uint8_t)s[i], (uint8_t)((uint16_t)s[i] >> 8)}; m.update(b, 2); } return m.hex(); }
inline bool md5SelfTest() {   // the 7 values of the RFC 1321 test suite (the 62-byte string pads into an extra block)
    auto h = [](const char* t) { Md5 m; m.update(t, strlen(t)); return m.hex(); };
    return h("") == "d41d8cd98f00b204e9800998ecf8427e" && h("a") == "0cc175b9c0f1b6a831c399e269772661" && h("abc") == "900150983cd24fb0d6963f7d28e17f72"
        && h("message digest") == "f96b697d7cb7938d525a2f31aaf161d0" && h("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b"
        && h("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f"
        && h("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a"; }
// digital-silence edges of an interleaved stereo signal: frames at the start / end whose two samples are exactly zero
// (S: a pointer, or any type with operator[] over the interleaved samples, such as the encoder's window reader of a master file)
template <class S> void silenceEdges(const S& s, long frames, long& lead, long& tail) {
    lead = 0; while (lead < frames && s[(size_t)lead * 2] == 0 && s[(size_t)lead * 2 + 1] == 0) lead++;
    tail = 0; while (tail < frames - lead && s[(size_t)(frames - 1 - tail) * 2] == 0 && s[(size_t)(frames - 1 - tail) * 2 + 1] == 0) tail++; }
template <class S> bool allZero(const S& s, long a, long b) { for (long i = a; i < b; i++) if (s[(size_t)i * 2] || s[(size_t)i * 2 + 1]) return false; return true; }
}
