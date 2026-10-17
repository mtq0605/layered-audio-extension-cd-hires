// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Cut an excerpt from a PCM WAV (24-bit output), optionally with a gain and a hard clip at full scale (loud-master test material).
// usage: cut_wav <in.wav> <out.wav> <start s> <length s> [gain dB]
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include "wavio.h"
int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: cut_wav <in.wav> <out.wav> <start s> <length s> [gain dB]\n"); return 1; }
    wavio::Wav w; if (!wavio::read(argv[1], w) || w.ch != 2) { printf("cannot read %s\n", argv[1]); return 1; }
    long n = (long)w.s.size() / 2, a = (long)(atof(argv[3]) * w.rate), len = (long)(atof(argv[4]) * w.rate);
    if (a < 0 || a + len > n) { printf("excerpt outside the file\n"); return 1; }
    double g = argc > 5 ? pow(10.0, atof(argv[5]) / 20) : 1.0; long nclip = 0;
    std::vector<int32_t> o((size_t)len * 2); for (long i = 0; i < len * 2; i++) { double v = (w.s[(size_t)a * 2 + i] >> 8) * g; long long q = llround(v);
        if (q > 8388607) { q = 8388607; nclip++; } if (q < -8388608) { q = -8388608; nclip++; } o[i] = (int32_t)q; }   // plain 24-bit values
    if (g != 1.0) printf("gain %.1f dB, %ld samples hard-clipped\n", atof(argv[5]), nclip);
    if (!wavio::write24(argv[2], w.rate, o)) { printf("cannot write %s\n", argv[2]); return 1; }
    printf("cut %s: %.1f s from %.1f s -> %s\n", argv[1], len / (double)w.rate, a / (double)w.rate, argv[2]); return 0; }
