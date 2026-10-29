// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise base_layer.cpp (2026-10-29): the same CD layer, 24-bit base and dither flags, computed one base-rate block at a time.
// Every decimation stage is evaluated over the window its block needs (with the same mirrored edges, which depend only on the stage's
// length); the dither decision is made per 4096-sample block as before, and the random numbers are drawn in the same order.
// usage and environment as base_layer.cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <memory>
#include "halfband.h"
#include "blk_io.h"
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
struct Cascade { int stages = 0; vector<long> len; blkio::Reader<int32_t, 2>* raw = nullptr;   // len[k]: length of level k (level stages = the input >> 4)
    // level k (k = stages: the input in Q4; k < stages: after decimation stage k) over [a, b), channel c
    blkio::Win<int32_t> eval(int k, long a, long b, int c) {
        blkio::Win<int32_t> w; w.base = a; w.d.resize((size_t)(b - a));
        if (k == stages) { vector<int32_t> x; raw->read(a, b, x); for (long i = a; i < b; i++) w.d[(size_t)(i - a)] = x[(size_t)(i - a) * 2 + c] >> 4; return w; }
        const int s = k; int half; const int32_t* h = tab(s, half); const long n = len[k + 1];
        long lo, hi; blkio::cover(2 * a - half, 2 * (b - 1) + half, n, lo, hi);
        const blkio::Win<int32_t> x = eval(k + 1, lo, hi + 1, c);
        for (long i = a; i < b; i++) {   // decimate() of base_layer.cpp
            long cc = 2 * i;
            int64_t acc = (int64_t)x.at(cc) << 30;
            for (int j = 1, t = 0; j <= half; j += 2, t++) {
                long aa0 = cc - j, bb0 = cc + j;
                long aa = aa0 >= 0 ? aa0 : -aa0, bb = bb0 < n ? bb0 : 2 * (n - 1) - bb0; aa = min(max(aa, 0L), n - 1); bb = min(max(bb, 0L), n - 1);
                int64_t sa = x.at(aa), sb = x.at(bb);
                acc += (int64_t)h[t] * (sa + sb);
            }
            int64_t r = (acc + (1LL << 30)) >> 31;
            if (r > 2147483647LL) r = 2147483647LL; if (r < -2147483647LL) r = -2147483647LL;
            w.d[(size_t)(i - a)] = (int32_t)r;
        }
        return w; } };
int main(int argc, char** argv) {
    if (argc < 5) return 1;
    int R = atoi(argv[2]), R0 = atoi(argv[3]);
    int stages = 0; while ((R0 << stages) < R) stages++;
    if ((R0 << stages) != R) { fprintf(stderr, "rate %d is not base %d * 2^n\n", R, R0); return 2; }
    blkio::Reader<int32_t, 2> raw(argv[1]); Cascade C; C.stages = stages; C.raw = &raw; C.len.assign(stages + 1, 0); C.len[stages] = raw.frames;
    for (int k = stages - 1; k >= 0; k--) C.len[k] = C.len[k + 1] / 2;
    const long n = C.len[0];
    const int DB = 4096; long nbk = (n + DB - 1) / DB; vector<char> dflag(2 * nbk, 0);
    const char* b24p = getenv("BASE24"); blkio::Writer out(argv[4]); std::unique_ptr<blkio::Writer> o24; if (b24p) o24.reset(new blkio::Writer(b24p));
    // each CD sample needs only the dither flag of its own 4096-block, decided within the same base block; the flag file is written at the end
    uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u};
    const long BB = 64L * DB;   // base samples per block (a multiple of the dither block)
    vector<int16_t> o16; vector<int32_t> ob24;
    for (long a0 = 0; a0 < n; a0 += BB) { const long a1 = min(n, a0 + BB);
        blkio::Win<int32_t> ch[2] = {C.eval(0, a0, a1, 0), C.eval(0, a0, a1, 1)};
        for (int c = 0; c < 2; c++) for (long k = a0 / DB; k < (a1 + DB - 1) / DB; k++) {   // adaptive dither decision (base_layer.cpp)
            long s0 = k * DB, s1 = min(n, s0 + DB); int P = 16; vector<double> r(P + 1, 0), a(P + 1, 0), t(P + 1); vector<double> xx(s1 - s0);
            for (long i = s0; i < s1; i++) xx[i - s0] = ch[c].at(i) / 16.0;
            for (int q = 0; q <= P; q++) for (long i = q; i < (long)xx.size(); i++) r[q] += xx[i] * xx[i - q];
            double rms = 0;
            if (r[0] > 0) { r[0] *= 1 + 1e-9; a[0] = 1; double e = r[0];
                for (int i = 1; i <= P; i++) { double acc = r[i]; for (int j = 1; j < i; j++) acc += a[j] * r[i - j]; double kk = -acc / e; for (int j = 1; j < i; j++) t[j] = a[j] + kk * a[i - j]; for (int j = 1; j < i; j++) a[j] = t[j]; a[i] = kk; e *= 1 - kk * kk; }
                double se = 0; for (long i = P; i < (long)xx.size(); i++) { double p = 0; for (int j = 0; j <= P; j++) p += a[j] * xx[i - j]; se += p * p; } rms = sqrt(se / max(1L, (long)xx.size() - P)); }
            dflag[c * nbk + k] = rms < 128.0 && r[0] > 0; }
        o16.resize((size_t)(a1 - a0) * 2);
        for (long i = a0; i < a1; i++)
            for (int c = 0; c < 2; c++) {
                uint32_t& s = st[c];
                s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF;
                s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
                int d = dflag[c * nbk + i / DB] ? u1 + u2 - 4096 : 0;
                int64_t v = ((int64_t)ch[c].at(i) + d + 2048) >> 12;
                if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                o16[(size_t)(i - a0) * 2 + c] = (int16_t)v;
            }
        out.put(o16);
        if (o24) { ob24.resize((size_t)(a1 - a0) * 2); for (long i = a0; i < a1; i++) for (int c = 0; c < 2; c++) { int64_t v = ((int64_t)ch[c].at(i) + 8) >> 4; ob24[(size_t)(i - a0) * 2 + c] = (int32_t)v; } o24->put(ob24); } }
    if (getenv("DITHERFLAGS")) encio::writeVec(getenv("DITHERFLAGS"), dflag);
    { long nd = 0; for (char v : dflag) nd += v; printf("adaptive dither: %ld of %ld blocks dithered\n", nd, (long)dflag.size()); }
    out.finish(); if (o24) o24->finish();
    printf("base layer: %ld frames @ %d Hz, %d stages\n", n, R0, stages);
    return 0;
}
