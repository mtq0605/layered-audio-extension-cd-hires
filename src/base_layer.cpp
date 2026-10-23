// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Deterministic base (CD) layer derivation. Integer only; tables from halfband.h.
// usage: base_layer <raw s32le stereo, 24-bit left-aligned> <rate> <base_rate> <out s16le raw> [ref s16le raw]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <complex>
#include "fft.h"
#include "halfband.h"
#include "enc_io.h"
using namespace std;
using namespace lae;

static const int32_t* tab(int s, int& half) {
    switch (s) {
        case 0: half = kHalfLen0; return kHalf0;
        case 1: half = kHalfLen1; return kHalf1;
        case 2: half = kHalfLen2; return kHalf2;
        default: half = kHalfLen3; return kHalf3;
    }
}
// one decimate-by-2 stage, stage index counted from the base rate upwards. x in Q4 of the 24-bit domain (28-bit).
static vector<int32_t> decimate(const vector<int32_t>& x, int s) {
    int half; const int32_t* h = tab(s, half);
    long n = (long)x.size(), m = n / 2;
    vector<int32_t> y(m);
    for (long i = 0; i < m; i++) {
        long c = 2 * i;
        int64_t acc = (int64_t)x[c] << 30;      // centre tap 2^30 (Q31 0.5)
        for (int j = 1, t = 0; j <= half; j += 2, t++) {
            long a = c - j, b = c + j;
            long aa = a >= 0 ? a : -a, bb = b < n ? b : 2 * (n - 1) - b; aa = min(max(aa, 0L), n - 1); bb = min(max(bb, 0L), n - 1);
            int64_t sa = x[aa], sb = x[bb];
            acc += (int64_t)h[t] * (sa + sb);
        }
        int64_t r = (acc + (1LL << 30)) >> 31;    // round half up, Q31 -> integer
        if (r > 2147483647LL) r = 2147483647LL; if (r < -2147483647LL) r = -2147483647LL;   // only guards int32 (Q4 has 24 dB headroom): band-limited overshoot beyond full scale is kept for the 24-bit base; the CD16 is limited at the end
        y[i] = (int32_t)r;
    }
    return y;
}
int main(int argc, char** argv) {
    if (argc < 5) return 1;
    int R = atoi(argv[2]), R0 = atoi(argv[3]);
    int stages = 0; while ((R0 << stages) < R) stages++;
    if ((R0 << stages) != R) { fprintf(stderr, "rate %d is not base %d * 2^n\n", R, R0); return 2; }
    vector<int32_t> raw = encio::readAll<int32_t>(argv[1]); long frames = encio::count(raw.size()) / 2;
    vector<vector<int32_t>> ch(2);
    for (int c = 0; c < 2; c++) { ch[c].resize(frames); for (long i = 0; i < frames; i++) ch[c][i] = raw[i * 2 + c] >> 4; }
    for (int c = 0; c < 2; c++)
        for (int s = stages - 1; s >= 0; s--) ch[c] = decimate(ch[c], s);   // highest-rate stage first
    long n = (long)ch[0].size();
    vector<int16_t> out(n * 2);
    // adaptive dither decision per channel per 4096-sample block
    const int DB = 4096; long nbk = (n + DB - 1) / DB; vector<char> dflag(2 * nbk, 0);
    for (int c = 0; c < 2; c++) for (long k = 0; k < nbk; k++) {
        long s0 = k * DB, s1 = min(n, s0 + DB); int P = 16; vector<double> r(P + 1, 0), a(P + 1, 0), t(P + 1); vector<double> xx(s1 - s0);
        for (long i = s0; i < s1; i++) xx[i - s0] = ch[c][i] / 16.0;
        for (int q = 0; q <= P; q++) for (long i = q; i < (long)xx.size(); i++) r[q] += xx[i] * xx[i - q];
        double rms = 0;
        if (r[0] > 0) { r[0] *= 1 + 1e-9; a[0] = 1; double e = r[0];
            for (int i = 1; i <= P; i++) { double acc = r[i]; for (int j = 1; j < i; j++) acc += a[j] * r[i - j]; double kk = -acc / e; for (int j = 1; j < i; j++) t[j] = a[j] + kk * a[i - j]; for (int j = 1; j < i; j++) a[j] = t[j]; a[i] = kk; e *= 1 - kk * kk; }
            double se = 0; for (long i = P; i < (long)xx.size(); i++) { double p = 0; for (int j = 0; j <= P; j++) p += a[j] * xx[i - j]; se += p * p; } rms = sqrt(se / max(1L, (long)xx.size() - P)); }
        dflag[c * nbk + k] = rms < 128.0 && r[0] > 0;   // 0.5 LSB16 in 24-bit units; digital silence (all-zero block) is never dithered: the CD keeps true zeros
    }
    if (getenv("DITHERFLAGS")) encio::writeVec(getenv("DITHERFLAGS"), dflag);
    { long nd = 0; for (char v : dflag) nd += v; printf("adaptive dither: %ld of %ld blocks dithered\n", nd, (long)dflag.size()); }
    uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u};
    for (long i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            uint32_t& s = st[c];
            s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
            int d = dflag[c * nbk + i / DB] ? u1 + u2 - 4096 : 0;
            int64_t v = ((int64_t)ch[c][i] + d + 2048) >> 12;
            if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            out[i * 2 + c] = (int16_t)v;
        }
    encio::writeVec(argv[4], out);
    if (getenv("BASE24")) { vector<int32_t> b24(n * 2); for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) { int64_t v = ((int64_t)ch[c][i] + 8) >> 4; b24[i * 2 + c] = (int32_t)v; }   // plain 24-bit units, NOT left-aligned: band-limited overshoot beyond full scale is kept (depth target carries it; only the final output is limited)
        encio::writeVec(getenv("BASE24"), b24); }
    printf("base layer: %ld frames @ %d Hz, %d stages\n", n, R0, stages);
    return 0;
}
