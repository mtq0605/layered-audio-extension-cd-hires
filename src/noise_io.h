// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Block-wise WAV input for the noise tools (noise_scan0c, lae_noise): any sample range of a stereo PCM 16/24/32 or float32 WAV,
// never the whole file. Values as doubles in [-1, 1) (24-bit: int24 / 2^23 through int32 / 2^31, as readWav in noise_measure.cpp).
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#ifndef _WIN32
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

struct WavStream { FILE* f = nullptr; int fmt = 0, bits = 0, bps = 0; double rate = 0; long long frames = 0, dataPos = 0;
    std::string why;   // FU17-5 (2026-11-14): the reason when open() refuses the file
    bool open(const char* path) {
        auto no = [&](const char* w) { why = w; return false; };
        f = fopen(path, "rb"); if (!f) return no("cannot open the file");
        _fseeki64(f, 0, SEEK_END); const long long fileLen = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
        uint8_t hdr[12]; if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr + 8, "WAVE", 4)) return no("not a WAV file");
        if (!memcmp(hdr, "RF64", 4) || !memcmp(hdr, "BW64", 4)) return no("an RF64 / BW64 file (over 4 GB): the noise tools read only RIFF WAV files up to 4 GB");
        if (memcmp(hdr, "RIFF", 4)) return no("not a WAV file");
        int ch = 0; bool haveFmt = false;
        for (;;) { uint8_t c[8]; if (fread(c, 1, 8, f) != 8) return no("no data chunk"); uint32_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            if (!memcmp(c, "fmt ", 4)) { if (n < 16 || n > 65536) return no("the fmt chunk is damaged (its length is not 16 to 65536 bytes)");
                std::vector<uint8_t> b(n); if (fread(b.data(), 1, n, f) != n) return no("the fmt chunk is cut off");
                fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24; bits = b[14] | b[15] << 8;
                if (fmt == 0xFFFE && n >= 26) fmt = b[24] | b[25] << 8; if (n & 1) _fseeki64(f, 1, SEEK_CUR); haveFmt = true; continue; }
            if (!memcmp(c, "data", 4)) { if (!haveFmt || ch != 2 || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return no("needs stereo PCM 16/24/32 or float32 WAV");
                if (!(rate > 0)) return no("the sample rate is 0");
                bps = bits / 8; frames = n / (2 * bps); dataPos = _ftelli64(f);
                if (dataPos + (long long)n > fileLen) return no("the data chunk runs past the end of the file (the file is cut off)");
                return true; }
            _fseeki64(f, (long long)n + (n & 1), SEEK_CUR); } }

    // samples [a, a + n) as doubles; zero outside the file
    void read(long long a, long long n, std::vector<double>& L, std::vector<double>& R) {
        L.assign((size_t)n, 0.0); R.assign((size_t)n, 0.0); long long ra = std::max(a, 0LL), rb = std::min(a + n, frames); if (ra >= rb) return;
        std::vector<uint8_t> b((size_t)(rb - ra) * 2 * bps); _fseeki64(f, dataPos + ra * 2 * bps, SEEK_SET); if (fread(b.data(), 1, b.size(), f) != b.size()) { fprintf(stderr, "read failed\n"); exit(1); }
        for (long long i = 0; i < (rb - ra) * 2; i++) { const uint8_t* p = &b[(size_t)i * bps]; double v;
            if (fmt == 3) { float fl; memcpy(&fl, p, 4); v = fl; }
            else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0;
            else if (bits == 24) v = (int32_t)((uint32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0;
            else v = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0;
            (i & 1 ? R : L)[(size_t)(ra - a + i / 2)] = v; } }
    // the raw bytes of the data chunk, [off, off + n)
    bool raw(long long off, size_t n, std::vector<uint8_t>& out) { out.resize(n); _fseeki64(f, dataPos + off, SEEK_SET); return fread(out.data(), 1, n, f) == n; }
    ~WavStream() { if (f) fclose(f); } };
