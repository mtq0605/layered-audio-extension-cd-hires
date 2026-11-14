// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Test signal for the LAE Noise Marker platform check (2026-11-06, tests/check_noise_linux.sh): 25 s, 96 kHz, stereo, 24-bit PCM.
// Integers only, so the file is the same on every platform: steady noise in both channels (sum of four uniform values from a
// xorshift generator, about -86 dBFS), plus a fixed line at 1000 Hz in the left channel (a 96-sample table with lmath::sinpi,
// rounded once; the table repeats exactly, so every period has the same samples).
// usage: gen_noise_test <out.wav> [--overwrite]
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include "../src/lae_math.h"
#include "../src/noise_replace.h"   // decision 21: no silent replacement (--overwrite)
int main(int argc, char** argv) {
    nrep::takeOverwrite(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: gen_noise_test <out.wav> [--overwrite]\n"); return 2; }
    if (!nrep::mayReplace(argv[1])) return 1;   // DEC 21 (2026-11-15): an existing output is not replaced silently
    const uint32_t rate = 96000, secs = 25, frames = rate * secs; const int period = 96;   // 1000 Hz
    std::vector<int32_t> tone(period); for (int i = 0; i < period; i++) tone[i] = (int32_t)llround(2000.0 * lmath::sinpi(2.0 * i / period));   // about -72 dBFS
    uint64_t s = 0x2545F4914F6CDD1Dull; auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (int32_t)((s >> 40) & 0xFFF) - 2048; };
    FILE* f = fopen(argv[1], "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    const uint32_t db = frames * 6; auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + db); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(rate); w32(rate * 6); w16(6); w16(24); fwrite("data", 1, 4, f); w32(db);
    std::vector<uint8_t> buf(6 * 4096); size_t k = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = (rnd() + rnd() + rnd() + rnd()) / 4 + tone[i % period], r = (rnd() + rnd() + rnd() + rnd()) / 4;
        for (int32_t v : {l, r}) { buf[k++] = (uint8_t)v; buf[k++] = (uint8_t)(v >> 8); buf[k++] = (uint8_t)(v >> 16); }
        if (k == buf.size()) { fwrite(buf.data(), 1, k, f); k = 0; } }
    fwrite(buf.data(), 1, k, f); return fclose(f) == 0 ? 0 : 1; }
