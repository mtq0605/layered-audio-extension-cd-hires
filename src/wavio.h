// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// WAV reader/writer (RIFF/WAVE, PCM and WAVE_FORMAT_EXTENSIBLE integer PCM).
// Samples are kept as int32 with the 24-bit value left-aligned (value << 8), the layout every LAE module uses.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
namespace wavio {
struct Wav { int rate = 0, ch = 0, bits = 0; std::vector<int32_t> s; std::vector<int16_t> pcm16; std::vector<uint8_t> lae; std::vector<int32_t> raw32; };   // raw32: a 32-bit file's samples with all 32 bits
inline bool read(const char* path, Wav& w) {
    FILE* f = fopen(path, "rb"); if (!f) return false; std::vector<uint8_t> b;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END); long long sz = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);   // 64-bit size: files beyond 2 GB
#else
    fseeko(f, 0, SEEK_END); long long sz = (long long)ftello(f); fseeko(f, 0, SEEK_SET);
#endif
    if (sz < 0 || (unsigned long long)sz > (size_t)-1) { fclose(f); return false; }
    b.resize((size_t)sz); if (fread(b.data(), 1, b.size(), f) != b.size()) { fclose(f); return false; } fclose(f);
    if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int fmt = 0; const uint8_t* data = nullptr; size_t dlen = 0;
    auto u16 = [&](size_t o) { return (int)(b[o] | b[o + 1] << 8); }; auto u32 = [&](size_t o) { return (uint32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24); };
    while (p + 8 <= b.size()) { uint32_t n = u32(p + 4); const char* id = (const char*)&b[p]; size_t body = p + 8;
        if (n > b.size() - body) n = (uint32_t)(b.size() - body);   // tolerate a truncated last chunk (no wrap-around with a 32-bit size_t)
        if (!memcmp(id, "fmt ", 4)) { if (n < 16) return false; fmt = u16(body); w.ch = u16(body + 2); w.rate = (int)u32(body + 4); w.bits = u16(body + 14); if (fmt == 0xFFFE && n >= 26) fmt = u16(body + 24); }
        else if (!memcmp(id, "data", 4)) { data = &b[body]; dlen = n; }
        else if (!memcmp(id, "LAE ", 4)) w.lae.assign(b.begin() + body, b.begin() + body + n);
        p = body + n + (n & 1); }
    if (fmt != 1 || !data || (w.bits != 16 && w.bits != 24 && w.bits != 32)) return false;
    int bps = w.bits / 8; size_t n = dlen / bps; w.s.resize(n); if (w.bits == 16) w.pcm16.resize(n); if (w.bits == 32) w.raw32.resize(n);
    for (size_t i = 0; i < n; i++) { const uint8_t* q = data + i * bps; int32_t v;
        if (bps == 2) { v = (int16_t)(q[0] | q[1] << 8); w.pcm16[i] = (int16_t)v; v = (int32_t)((uint32_t)v << 16); }
        else if (bps == 3) v = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24);
        else { w.raw32[i] = (int32_t)((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24); v = w.raw32[i] & ~0xFF; }   // 32-bit: keep the top 24 bits
        w.s[i] = v; }
    return true; }
inline bool fits32(unsigned long long dataBytes, unsigned long long extra) { return 4 + 24 + 8 + dataBytes + 1 + extra <= 0xFFFFFFFFull; }   // RIFF sizes are 32-bit
inline void hdr(std::vector<uint8_t>& o, int rate, int ch, int bits, size_t dataBytes, size_t extra) {
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }; auto p16 = [&](int v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); };
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); p32((uint32_t)(4 + 24 + 8 + dataBytes + (dataBytes & 1) + extra)); o.insert(o.end(), {'W', 'A', 'V', 'E'});
    o.insert(o.end(), {'f', 'm', 't', ' '}); p32(16); p16(1); p16(ch); p32(rate); p32(rate * ch * bits / 8); p16(ch * bits / 8); p16(bits);
    o.insert(o.end(), {'d', 'a', 't', 'a'}); p32((uint32_t)dataBytes); }
// CD layer as ordinary 16-bit PCM; the extension travels in a "LAE " chunk after the audio (players skip unknown chunks)
inline bool writeCD(const char* path, int rate, const std::vector<int16_t>& cd, const std::vector<uint8_t>& lae) {
    size_t db = cd.size() * 2, extra = lae.empty() ? 0 : 8 + lae.size() + (lae.size() & 1); std::vector<uint8_t> o; hdr(o, rate, 2, 16, db, extra);
    const uint8_t* c = (const uint8_t*)cd.data(); o.insert(o.end(), c, c + db); if (db & 1) o.push_back(0);
    if (!lae.empty()) { o.insert(o.end(), {'L', 'A', 'E', ' '}); uint32_t n = (uint32_t)lae.size(); for (int i = 0; i < 4; i++) o.push_back((uint8_t)(n >> (8 * i))); o.insert(o.end(), lae.begin(), lae.end()); if (n & 1) o.push_back(0); }
    FILE* f = fopen(path, "wb"); if (!f) return false; const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size(); return fclose(f) == 0 && ok; }   // a short write (full disk) is a failure
// 24-bit PCM output (values are plain 24-bit integers, already limited to full scale)
inline bool write24(const char* path, int rate, const std::vector<int32_t>& s) {
    size_t db = s.size() * 3; std::vector<uint8_t> o; hdr(o, rate, 2, 24, db, 0);
    for (int32_t v : s) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)(v >> 16)); } if (db & 1) o.push_back(0);
    FILE* f = fopen(path, "wb"); if (!f) return false; const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size(); return fclose(f) == 0 && ok; }
}
