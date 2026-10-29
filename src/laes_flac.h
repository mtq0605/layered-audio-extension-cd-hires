// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// FLAC reader for the LAES FLAC form (the writer is in laes_flac_enc.h, GPL / commercial) (F1 draft, 2026-10-29). Written for this project from the format description in
// RFC 9639 (no libFLAC code). The writer produces only what LAES needs:
//   "fLaC" | STREAMINFO | APPLICATION blocks (ID "LAE2", the LAES payload split into pieces, in order) | audio frames = the core
//   audio frames: fixed block size 4096, two independent channels (channel assignment 0001; no inter-channel decorrelation),
//   subframes CONSTANT / VERBATIM / FIXED 0-4 / LPC (order <= 12, precision <= 15, shift 0..15), residual with 4- or 5-bit Rice
//   parameters (partition order <= 8). At 44.1 / 48 kHz this is the FLAC streamable subset for 16, 24 and 32 bits (RFC 9639 section 7);
//   many older players still do not play a 32-bit stream, so a 32-bit core is not the default.
//   No SEEKTABLE, no CUESHEET; nothing interleaved with the audio frames. STREAMINFO's MD5 covers the core only.
// The reader decodes any stream written this way (and FLAC streams of the same kind from other encoders: wasted bits, every
// fixed / LPC order, both Rice methods); streams using inter-channel decorrelation are reported, not decoded.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "laes_codec.h"
#include "align_hash.h"
namespace laesflac {
using laes::V; using laes::Acc;
static_assert((-3LL >> 1) == -2, "right shift of a negative value must be arithmetic (floor), as in C++20");
inline uint8_t crc8(const uint8_t* p, size_t n) { uint8_t c = 0; for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1); } return c; }
inline uint16_t crc16(const uint8_t* p, size_t n) { uint16_t c = 0; for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(p[i] << 8); for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1); } return c; }
// MD5 of the audio as STREAMINFO defines it: interleaved, each sample little-endian signed in ceil(bits/8) bytes
inline void audioMd5(const V* ch, int64_t n, int bits, uint8_t out[16]) { alignhash::Md5 m; const int by = (bits + 7) / 8; std::vector<uint8_t> b; b.reserve(1 << 16);
    for (int64_t i = 0; i < n; i++) for (int c = 0; c < 2; c++) { uint64_t v = (uint64_t)ch[c][i]; for (int k = 0; k < by; k++) b.push_back((uint8_t)(v >> (8 * k))); if (b.size() >= (1 << 16)) { m.update(b.data(), b.size()); b.clear(); } }
    m.update(b.data(), b.size()); std::string h = m.hex(); for (int i = 0; i < 16; i++) out[i] = (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16); }

inline bool fits32(int64_t r) { return r > -(1LL << 31) && r < (1LL << 31); }   // residuals stay inside the 32-bit signed range (excluding -2^31)
inline int64_t fixedPred(const int64_t* x, long i, int o) { return laes::fixedPred(x, i, o); }

// ---- reader
struct BR { const uint8_t* p; size_t n, pos = 0; bool fail = false;   // MSB-first bit reader over a byte range; pos in bits
    uint64_t get(int w) { if (fail || w < 0 || w > 64 || pos + (size_t)w > n * 8) { fail = true; return 0; } uint64_t v = 0; for (int i = 0; i < w; i++) { v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1); pos++; } return v; }
    int64_t getS(int w) { if (w == 0) return 0; uint64_t v = get(w); if (w < 64 && (v >> (w - 1) & 1)) v |= ~0ull << w; return (int64_t)v; }
    uint64_t unary() { uint64_t q = 0; while (!fail && get(1) == 0) if (++q > (1ull << 33)) fail = true; return q; } };
struct Info { int rate = 0, channels = 0, bps = 0, minBs = 0, maxBs = 0; uint64_t total = 0; uint8_t md5[16] = {0}; };
inline bool readResidual(BR& r, int64_t* out, long blk, int order) {
    const int method = (int)r.get(2); if (method > 1) return false; const int pb = method ? 5 : 4, esc = method ? 31 : 15; const int po = (int)r.get(4);
    if (r.fail || blk % (1L << po) || (blk >> po) <= order) return false; const long plen = blk >> po; long a = 0;   // the first partition is not empty
    for (long q = 0; q < (1L << po); q++) { const long len = q ? plen : plen - order; const int k = (int)r.get(pb);
        if (k == esc) { const int rw = (int)r.get(5); for (long i = 0; i < len; i++) out[a + i] = r.getS(rw); }
        else for (long i = 0; i < len && !r.fail; i++) { const uint64_t q = r.unary(); if (q >> (63 - k)) return false;   // the two reads in order (quotient, then low bits)
            const uint64_t lo = k ? r.get(k) : 0; const int64_t v = laes::unzz((q << k) | lo); if (!fits32(v)) return false; out[a + i] = v; }   // residuals inside +-(2^31 - 1)
        if (r.fail) return false; a += len; } return true; }
inline bool readSubframe(BR& r, int64_t* x, long n, int bps) {
    if (r.get(1) != 0) return false; const int type = (int)r.get(6); int wasted = 0; if (r.get(1)) { wasted = 1; while (!r.fail && r.get(1) == 0) wasted++; }
    const int b = bps - wasted; if (r.fail || b < 1) return false;
    if (type == 0) { int64_t v = r.getS(b); for (long i = 0; i < n; i++) x[i] = v; }
    else if (type == 1) { for (long i = 0; i < n; i++) x[i] = r.getS(b); }
    else if (type >= 8 && type <= 12) { const int o = type - 8; if (o > n) return false; for (int i = 0; i < o; i++) x[i] = r.getS(b);
        std::vector<int64_t> res(n); if (!readResidual(r, res.data() + o, n, o)) return false; const int64_t lim = 1LL << (b - 1);
        for (long i = o; i < n; i++) { x[i] = res[i] + fixedPred(x, i, o); if (x[i] < -lim || x[i] >= lim) return false; } }   // checked as it goes: the sums stay inside int64
    else if (type >= 32) { const int o = type - 31; if (o > n) return false; for (int i = 0; i < o; i++) x[i] = r.getS(b);
        const int prec = (int)r.get(4) + 1; const int64_t sh = r.getS(5); if (prec == 16 || sh < 0) return false; std::vector<int64_t> c(o); for (int j = 0; j < o; j++) c[j] = r.getS(prec);
        std::vector<int64_t> res(n); if (!readResidual(r, res.data() + o, n, o)) return false;
        for (long i = o; i < n; i++) { Acc a; for (int j = 0; j < o; j++) a.mac(c[j], x[i - 1 - j]); int64_t s = (int64_t)a.lo; if (sh) { Acc t = a; t.lo -= (1ull << (sh - 1)); if (t.lo > a.lo) t.hi--; s = t.shr((int)sh); }   // floor
            x[i] = res[i] + s; const int64_t lim = 1LL << (b - 1); if (x[i] < -lim || x[i] >= lim) return false; } }
    else return false;
    if (r.fail) return false;
    const int64_t lim = 1LL << (b - 1); for (long i = 0; i < n; i++) if (x[i] < -lim || x[i] >= lim) return false;   // every sample inside the subframe's bit depth
    if (wasted) for (long i = 0; i < n; i++) x[i] = (int64_t)((uint64_t)x[i] << wasted); return true; }
// reads a whole FLAC file: STREAMINFO, the concatenated "LAE2" APPLICATION data, and the audio (two independent channels).
// Returns "" or the reason. md5ok: the decoded audio matches STREAMINFO's MD5 (an all-zero MD5 counts as "not given").
inline std::string readFlac(const std::vector<uint8_t>& f, Info& in, V ch[2], std::vector<uint8_t>& app, bool& md5ok) {
    app.clear(); md5ok = false; if (f.size() < 8 || memcmp(f.data(), "fLaC", 4)) return "not a FLAC file";
    size_t p = 4; bool last = false, haveSi = false;
    while (!last) { if (p + 4 > f.size()) return "metadata truncated"; last = f[p] & 0x80; const int type = f[p] & 0x7F; const uint32_t len = (uint32_t)f[p + 1] << 16 | f[p + 2] << 8 | f[p + 3]; p += 4;
        if (p + len > f.size()) return "metadata truncated"; if (!haveSi && type != 0) return "STREAMINFO is not the first metadata block";
        if (type == 0) { if (len != 34) return "bad STREAMINFO"; BR r{f.data() + p, 34}; in.minBs = (int)r.get(16); in.maxBs = (int)r.get(16); r.get(24); r.get(24); in.rate = (int)r.get(20); in.channels = (int)r.get(3) + 1; in.bps = (int)r.get(5) + 1; in.total = r.get(36);
            memcpy(in.md5, f.data() + p + 18, 16); haveSi = true; }
        else if (type == 2 && len >= 4 && !memcmp(&f[p], "LAE2", 4)) app.insert(app.end(), f.begin() + p + 4, f.begin() + p + len);
        p += len; }
    if (!haveSi) return "no STREAMINFO"; if (in.total == 0) return "STREAMINFO gives no sample count (an LAES core always has one)"; if (in.maxBs < 16) return "bad STREAMINFO block size"; if (in.channels != 2) return "LAES core must have 2 channels"; if (in.bps != 16 && in.bps != 24 && in.bps != 32) return "core must be 16, 24 or 32 bit";
    ch[0].clear(); ch[1].clear(); uint64_t frameNo = 0;
    while (p < f.size()) { BR r{f.data() + p, f.size() - p};
        if (r.get(15) != 0x7FFC) return "lost frame sync"; const int strategy = (int)r.get(1); const int bsc = (int)r.get(4), src = (int)r.get(4), chc = (int)r.get(4), ssc = (int)r.get(3);
        if (r.get(1)) return "frame header reserved bit set";
        if (chc != 1) return chc <= 7 ? "core frame without 2 channels" : "core frame uses inter-channel decorrelation (not supported by this reader)";
        uint64_t num = r.get(8); { int extra = 0; if (num == 0xFF) return "bad frame number"; if (num >= 0xFE) { if (strategy == 0) return "bad frame number"; extra = 6; } else if (num >= 0xFC) extra = 5; else if (num >= 0xF8) extra = 4; else if (num >= 0xF0) extra = 3; else if (num >= 0xE0) extra = 2; else if (num >= 0xC0) extra = 1; else if (num >= 0x80) return "bad frame number";
            if (extra) { num &= (0x3F >> extra); for (int i = 0; i < extra; i++) { const uint64_t b = r.get(8); if ((b & 0xC0) != 0x80) return "bad frame number"; num = num << 6 | (b & 0x3F); } } }
        long n; if (bsc == 1) n = 192; else if (bsc >= 2 && bsc <= 5) n = 576L << (bsc - 2); else if (bsc == 6) n = (long)r.get(8) + 1; else if (bsc == 7) { n = (long)r.get(16) + 1; if (n == 65536) return "bad block size"; } else if (bsc >= 8) n = 256L << (bsc - 8); else return "bad block size";
        if (n > in.maxBs || (in.total && ch[0].size() + (uint64_t)n > in.total)) return "block larger than STREAMINFO allows";   // also caps memory to what the header declares
        if (src == 12) r.get(8); else if (src == 13 || src == 14) r.get(16); else if (src == 15) return "bad sample rate code";
        const int bps = ssc == 0 ? in.bps : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 4 ? 16 : ssc == 5 ? 20 : ssc == 6 ? 24 : ssc == 7 ? 32 : 0; if (bps != in.bps) return "frame bit depth differs from STREAMINFO";
        if (r.fail || (r.pos & 7)) return "frame header truncated"; const size_t hb = r.pos / 8; const uint8_t c8 = (uint8_t)r.get(8); if (r.fail || c8 != crc8(f.data() + p, hb)) return "frame header CRC-8 mismatch";
        if (strategy == 0 && num != frameNo) return "frame number out of order";
        const size_t base = ch[0].size(); for (int c = 0; c < 2; c++) { ch[c].resize(base + n); if (!readSubframe(r, ch[c].data() + base, n, bps)) return "damaged subframe"; }
        r.pos = (r.pos + 7) & ~(size_t)7; const size_t fb = r.pos / 8; if (fb + 2 > r.n) return "frame truncated";
        if (crc16(f.data() + p, fb) != (uint16_t)(f[p + fb] << 8 | f[p + fb + 1])) return "frame CRC-16 mismatch";
        p += fb + 2; frameNo++; }
    if (in.total && in.total != ch[0].size()) return "sample count differs from STREAMINFO";
    uint8_t m[16]; audioMd5(ch, (int64_t)ch[0].size(), in.bps, m); bool zero = true; for (int i = 0; i < 16; i++) zero &= in.md5[i] == 0; md5ok = !zero && !memcmp(m, in.md5, 16);
    return ""; }
}  // namespace laesflac
