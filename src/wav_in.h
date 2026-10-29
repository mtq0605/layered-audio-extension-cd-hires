// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise encoder (2026-10-29): the master read piece by piece instead of whole. RIFF/WAVE (as wavio::read), RF64 (ds64 sizes) and
// Sony Wave64; integer PCM 16 / 24 / 32 bit (PCM or WAVE_FORMAT_EXTENSIBLE). The samples become the raw stereo int32 file every module
// reads (24-bit value left-aligned, 32-bit input: the top 24 bits), exactly the values wavio::read gives. While reading, the MD5 of the
// data bytes is computed once and, for 32-bit input, the samples with a non-zero lowest byte are counted.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "enc_io.h"
#include "blk_io.h"
#include "align_hash.h"
namespace wavin {
struct Info { int rate = 0, ch = 0, bits = 0, fmt = 0; long long dataOff = -1, dataBytes = 0; std::string kind; };
inline uint32_t le32(const uint8_t* b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
inline uint64_t le64(const uint8_t* b) { return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32; }
// reads the header chunks; false if the file is not a WAV / RF64 / W64 with a fmt and a data chunk
inline bool probe(const char* path, Info& in) {
    FILE* f = fopen(path, "rb"); if (!f) return false; const long long sz = encio::size(f); bool ok = false;
    auto at = [&](long long p, void* d, size_t n) { return p >= 0 && p + (long long)n <= sz && blkio::seek(f, p) && fread(d, 1, n, f) == n; };
    auto fmtChunk = [&](long long body, uint64_t n) { uint8_t b[26] = {0}; if (n < 16 || !at(body, b, n >= 26 ? 26 : 16)) return false;
        in.fmt = b[0] | b[1] << 8; in.ch = b[2] | b[3] << 8; in.rate = (int)le32(b + 4); in.bits = b[14] | b[15] << 8; if (in.fmt == 0xFFFE && n >= 26) in.fmt = b[24] | b[25] << 8; return true; };
    uint8_t h[40];
    if (sz >= 12 && at(0, h, 12) && (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4)) && !memcmp(h + 8, "WAVE", 4)) {
        const bool rf64 = !memcmp(h, "RF64", 4); in.kind = rf64 ? "RF64" : "WAV"; uint64_t ds64Data = 0; bool haveFmt = false; long long p = 12;
        while (p + 8 <= sz) { uint8_t c[8]; if (!at(p, c, 8)) break; uint64_t n = le32(c + 4); const long long body = p + 8;
            if (rf64 && !memcmp(c, "ds64", 4)) { uint8_t d[24]; if (n >= 24 && at(body, d, 24)) ds64Data = le64(d + 8); }
            if (rf64 && !memcmp(c, "data", 4) && n == 0xFFFFFFFFull) n = ds64Data;
            if ((long long)n > sz - body) n = (uint64_t)(sz - body);   // a truncated last chunk is tolerated (as wavio::read)
            if (!memcmp(c, "fmt ", 4)) { if (!fmtChunk(body, n)) break; haveFmt = true; }
            else if (!memcmp(c, "data", 4)) { in.dataOff = body; in.dataBytes = (long long)n; }
            p = body + (long long)n + (long long)(n & 1); }
        ok = haveFmt && in.dataOff >= 0; }
    else if (sz >= 40 && at(0, h, 40)) {   // Wave64: GUID chunks, 64-bit sizes that include the 24-byte chunk header, 8-byte alignment
        static const uint8_t riff[16] = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
        static const uint8_t tail[12] = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
        if (!memcmp(h, riff, 16) && !memcmp(h + 24, "wave", 4) && !memcmp(h + 28, tail, 12)) { in.kind = "W64"; bool haveFmt = false; long long p = 40;
            while (p + 24 <= sz) { uint8_t c[24]; if (!at(p, c, 24)) break; uint64_t n = le64(c + 16); if (n < 24) break; const long long body = p + 24; uint64_t len = n - 24;
                if ((long long)len > sz - body) len = (uint64_t)(sz - body);
                if (!memcmp(c + 4, tail, 12)) { if (!memcmp(c, "fmt ", 4)) { if (!fmtChunk(body, len)) break; haveFmt = true; } else if (!memcmp(c, "data", 4)) { in.dataOff = body; in.dataBytes = (long long)len; } }
                p = body + (long long)((len + 7) & ~7ull); }
            ok = haveFmt && in.dataOff >= 0; } }
    fclose(f); return ok && in.fmt == 1 && (in.bits == 16 || in.bits == 24 || in.bits == 32); }
// writes the raw stereo int32 file; frames = whole stereo frames; low8 = 32-bit samples with a non-zero lowest byte; md5 of the data bytes
inline void toRaw(const char* path, const Info& in, const char* out, long long& frames, long long& low8, std::string& md5) {
    FILE* f = encio::open(path, "rb"); struct Close { FILE* f; ~Close() { fclose(f); } } cl{f};
    const int bps = in.bits / 8; const long long n = in.dataBytes / bps; frames = n / 2; low8 = 0; alignhash::Md5 m;
    blkio::Writer w(out); std::vector<uint8_t> b; std::vector<int32_t> s; const long long BS = 1LL << 18;   // frames per piece
    if (!blkio::seek(f, in.dataOff)) encio::fail(std::string("seek failed: ") + path);
    for (long long a = 0; a < n; a += 2 * BS) { const long long cnt = std::min(2 * BS, n - a); b.resize((size_t)(cnt * bps));
        if (fread(b.data(), 1, b.size(), f) != b.size()) encio::fail(std::string("read failed: ") + path);
        m.update(b.data(), b.size()); s.resize((size_t)cnt);
        for (long long i = 0; i < cnt; i++) { const uint8_t* q = b.data() + i * bps; int32_t v;
            if (bps == 2) v = (int32_t)((uint32_t)(int32_t)(int16_t)(q[0] | q[1] << 8) << 16);
            else if (bps == 3) v = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24);
            else { if (q[0]) low8++; v = (int32_t)(((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24) & ~0xFFu); }
            s[(size_t)i] = v; }
        const long long whole = (a + cnt) / 2 * 2 - a; w.put(s.data(), (size_t)std::max(0LL, whole)); }   // an odd last sample is not a frame
    if (in.dataBytes % bps) { uint8_t r[4]; size_t k = (size_t)(in.dataBytes % bps); if (fread(r, 1, k, f) == k) m.update(r, k); }
    w.finish(); md5 = m.hex() + ":" + std::to_string(in.rate) + ":" + std::to_string(in.bits); }
// samples of a 32-bit file whose lowest byte is not zero (what reducing it to 24 bit drops), read piece by piece; -1 if unreadable
inline long long low8(const char* path, const Info& in) {
    FILE* f = fopen(path, "rb"); if (!f) return -1; long long cnt = 0; std::vector<uint8_t> b(1 << 20); long long left = in.dataBytes / 4 * 4;
    if (!blkio::seek(f, in.dataOff)) { fclose(f); return -1; }
    while (left > 0) { const size_t k = (size_t)std::min<long long>(left, (long long)b.size()); if (fread(b.data(), 1, k, f) != k) { fclose(f); return -1; }
        for (size_t i = 0; i < k; i += 4) if (b[i]) cnt++; left -= (long long)k; }
    fclose(f); return cnt; }
}
