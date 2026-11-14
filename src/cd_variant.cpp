// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Test material for the alignment check: derives altered CDs (and shifted or 32-bit masters) from 16 / 24-bit WAV files.
// Every operation is exact and documented, so the expected measurement is known in advance.
// usage: cd_variant extract <lae.wav> <cd.wav>                CD layer of an LAE file as a plain 16-bit WAV
//        cd_variant shift   <cd.wav> <out.wav> <k>             same length, content moved later by k samples (k < 0: earlier), zero fill
//        cd_variant pad     <cd.wav> <out.wav> <lead s> <tail s>   digital silence added at the start / end
//        cd_variant cut     <cd.wav> <out.wav> <start s>       the first <start> seconds removed
//        cd_variant gain    <cd.wav> <out.wav> <dB>            gain, rounded, hard-clipped at full scale
//        cd_variant invert  <cd.wav> <out.wav> L|LR            polarity inverted on the left or on both channels
//        cd_variant swap    <cd.wav> <out.wav>                 left and right exchanged
//        cd_variant fade    <cd.wav> <out.wav> <s>             linear fade to zero over the last <s> seconds
//        cd_variant splice  <cd.wav> <out.wav> <at s> <n>      n samples removed at <at>, n zero frames appended (same length)
//        cd_variant noise   <cd.wav> <out.wav> <from s> <to s> <dBFS>   deterministic white noise of that rms added in [from, to)
//        cd_variant mshift  <master.wav> <out.wav> <r>         master (24-bit) moved later by r samples, same length, zero fill
//        cd_variant to32    <master.wav> <out.wav>             24-bit master written as 32-bit with a non-zero lowest byte
// Operations on the 24-bit master (the CD is then derived from the result by the encoder, as a mastering chain would make it):
//        cd_variant meq     <master.wav> <out.wav> <low|high|peak>[lin] <f Hz> <dB> [Q]   one biquad (RBJ "Audio EQ Cookbook"
//                           formulas; shelves with slope 1, peak Q default 1); "lin" = zero phase (dB/2 run forward, then backward)
//        cd_variant mlimit  <master.wav> <out.wav> <drive dB>   peak brought to -0.3 dBFS + drive, then a linked look-ahead limiter
//                           (ceiling -0.3 dBFS, 1 ms look-ahead, 50 ms release; offline, so it adds no delay)
//        cd_variant mfdelay <master.wav> <out.wav> <d>         delayed by d master samples (any real d), Kaiser-windowed sinc
//        cd_variant mdrift  <master.wav> <out.wav> <ppm>       speed changed by ppm around the middle (offset 0 there, +-ppm/2 at the ends)
//        cd_variant mgate   <master.wav> <out.wav> <dBFS> <hold ms>   silencing: digital zero wherever the peak (both channels) stays below
//                           dBFS for the hold time on both sides (lead-in / gaps / tail muted the way a mastering engineer does it)
//        cd_variant mdenoise <master.wav> <out.wav> <dBFS> <max cut dB>   noise reduction: a linked downward expander (ratio 2:1 below dBFS,
//                           at most max cut dB of gain reduction; rms detector, 5 ms attack, 100 ms release)
//        cd_variant cat     <out.wav> <a.wav> <b.wav> ...      files of one format (16- or 24-bit, same rate) one after the other
//        cd_variant crop    <in.wav> <out.wav> <first> <frames>   frames [first, first + frames) of a 16- or 24-bit file (zero outside)
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include "wavio.h"
#include "lae_math.h"   // the move kernel's sinpi, the same as the encoder's (cd_align.cpp moveKern, 2026-11-03)
static bool w16(const char* p, int rate, const std::vector<int16_t>& s) { return wavio::writeCD(p, rate, s, {}); }
static int16_t clip16(double v) { long long q = llround(v); if (q > 32767) q = 32767; if (q < -32768) q = -32768; return (int16_t)q; }
// 24-bit master helpers: x[c][i] in 24-bit units (double); written back rounded and hard-clipped at 24-bit full scale
static void toCh(const wavio::Wav& w, std::vector<double>* x) { long n = (long)w.s.size() / 2; for (int c = 0; c < 2; c++) { x[c].resize(n); for (long i = 0; i < n; i++) x[c][i] = (double)(w.s[(size_t)i * 2 + c] >> 8); } }
static bool w24(const char* p, int rate, const std::vector<double>* x, long& nclip) { long n = (long)x[0].size(); std::vector<int32_t> o((size_t)n * 2); nclip = 0;
    for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) { long long q = llround(x[c][i]); if (q > 8388607) { q = 8388607; nclip++; } if (q < -8388608) { q = -8388608; nclip++; } o[(size_t)i * 2 + c] = (int32_t)q; }
    return wavio::write24(p, rate, o); }
static double besselI0(double v) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; if (t < 1e-17 * s) break; } return s; }
static const int kH = 64; static const double kBeta = 10.0;   // windowed-sinc half length (master samples) and Kaiser beta
static double kern(double u) { if (std::fabs(u) >= kH) return 0; double s = u == 0 ? 1.0 : lmath::sinpi(u) / (M_PI * u); double r = u / kH; return s * besselI0(kBeta * std::sqrt(1 - r * r)) / besselI0(kBeta); }
static double at(const std::vector<double>& x, double p, const double* k = nullptr) {   // band-limited value of x at real position p (zero outside); k = precomputed taps for frac(p)
    long i0 = (long)std::floor(p); double f = p - i0, s = 0; long n = (long)x.size();
    for (int m = -kH + 1; m <= kH; m++) { long j = i0 + m; if (j < 0 || j >= n) continue; s += x[j] * (k ? k[m + kH - 1] : kern(f - m)); }
    return s; }
int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: see the header of cd_variant.cpp\n"); return 1; }
    if (!strcmp(argv[1], "cat")) {   // cat <out> <in...>: the output name comes first
        std::vector<int16_t> s16; std::vector<int32_t> s24; int rate = 0, bits = 0; long frames = 0;
        for (int i = 3; i < argc; i++) { wavio::Wav a; if (!wavio::read(argv[i], a) || a.ch != 2) { printf("cannot read %s\n", argv[i]); return 1; }
            if (i == 3) { rate = a.rate; bits = a.bits == 16 ? 16 : 24; } else if (a.rate != rate || (a.bits == 16) != (bits == 16)) { printf("%s: another format\n", argv[i]); return 1; }
            if (bits == 16) s16.insert(s16.end(), a.pcm16.begin(), a.pcm16.end()); else for (int32_t v : a.s) s24.push_back(v >> 8);
            frames += (long)(bits == 16 ? a.pcm16.size() : a.s.size()) / 2; }
        if (!(bits == 16 ? w16(argv[2], rate, s16) : wavio::write24(argv[2], rate, s24))) return 2; printf("%d files, %ld frames -> %s\n", argc - 3, frames, argv[2]); return 0; }
    std::string op = argv[1]; wavio::Wav w; if (!wavio::read(argv[2], w) || w.ch != 2) { printf("cannot read %s\n", argv[2]); return 1; }
    if (op == "crop" && argc > 5) { const long long f0 = atoll(argv[4]), nf = atoll(argv[5]); if (nf <= 0) return 1;
        if (w.bits == 16) { std::vector<int16_t> o((size_t)nf * 2, 0); const long long n16 = (long long)w.pcm16.size() / 2;
            for (long long i = 0; i < nf; i++) if (f0 + i >= 0 && f0 + i < n16) { o[(size_t)i * 2] = w.pcm16[(size_t)(f0 + i) * 2]; o[(size_t)i * 2 + 1] = w.pcm16[(size_t)(f0 + i) * 2 + 1]; }
            if (!w16(argv[3], w.rate, o)) return 2; }
        else { std::vector<int32_t> o((size_t)nf * 2, 0); const long long n = (long long)w.s.size() / 2;
            for (long long i = 0; i < nf; i++) if (f0 + i >= 0 && f0 + i < n) { o[(size_t)i * 2] = w.s[(size_t)(f0 + i) * 2] >> 8; o[(size_t)i * 2 + 1] = w.s[(size_t)(f0 + i) * 2 + 1] >> 8; }
            if (!wavio::write24(argv[3], w.rate, o)) return 2; }
        printf("frames %lld-%lld -> %s\n", f0, f0 + nf, argv[3]); return 0; }
    long n = (long)w.s.size() / 2; const char* out = argv[3]; std::vector<int16_t> s = w.pcm16;
    auto need16 = [&]() { if (w.bits != 16) { printf("%s needs a 16-bit WAV\n", op.c_str()); exit(1); } };
    if (op == "extract") { need16(); if (!w16(out, w.rate, s)) return 2; printf("CD layer: %ld frames -> %s\n", n, out); return 0; }
    if (op == "shift" && argc > 4) { need16(); long k = atol(argv[4]); std::vector<int16_t> o((size_t)n * 2, 0);
        for (long i = 0; i < n; i++) { long j = i - k; if (j >= 0 && j < n) { o[i * 2] = s[j * 2]; o[i * 2 + 1] = s[j * 2 + 1]; } }
        if (!w16(out, w.rate, o)) return 2; printf("shift %+ld samples -> %s\n", k, out); return 0; }
    if (op == "pad" && argc > 5) { need16(); long a = lround(atof(argv[4]) * w.rate), b = lround(atof(argv[5]) * w.rate);
        if (a < 0 || b < 0) { printf("pad: the lead and the tail must not be negative\n"); return 1; } std::vector<int16_t> o((size_t)(n + a + b) * 2, 0);
        for (long i = 0; i < n * 2; i++) o[(size_t)a * 2 + i] = s[i]; if (!w16(out, w.rate, o)) return 2; printf("pad %ld + %ld frames of digital silence -> %s\n", a, b, out); return 0; }
    if (op == "cut" && argc > 4) { need16(); long a = lround(atof(argv[4]) * w.rate); if (a >= n) return 1; if (a < 0) { printf("cut: the start must not be negative\n"); return 1; } std::vector<int16_t> o(s.begin() + (size_t)a * 2, s.end());
        if (!w16(out, w.rate, o)) return 2; printf("cut the first %ld frames -> %s\n", a, out); return 0; }
    if (op == "gain" && argc > 4) { need16(); double g = pow(10.0, atof(argv[4]) / 20); long nc = 0; std::vector<int16_t> o(s.size());
        for (size_t i = 0; i < s.size(); i++) { double v = s[i] * g; o[i] = clip16(v); if (fabs(v) > 32767) nc++; }
        if (!w16(out, w.rate, o)) return 2; printf("gain %s dB, %ld samples clipped -> %s\n", argv[4], nc, out); return 0; }
    if (op == "invert" && argc > 4) { need16(); bool both = !strcmp(argv[4], "LR"); std::vector<int16_t> o(s);
        for (long i = 0; i < n; i++) { o[i * 2] = clip16(-(double)s[i * 2]); if (both) o[i * 2 + 1] = clip16(-(double)s[i * 2 + 1]); }
        if (!w16(out, w.rate, o)) return 2; printf("polarity inverted (%s) -> %s\n", both ? "L and R" : "L", out); return 0; }
    if (op == "swap") { need16(); std::vector<int16_t> o(s.size()); for (long i = 0; i < n; i++) { o[i * 2] = s[i * 2 + 1]; o[i * 2 + 1] = s[i * 2]; }
        if (!w16(out, w.rate, o)) return 2; printf("channels swapped -> %s\n", out); return 0; }
    if (op == "fade" && argc > 4) { need16(); long f = lround(atof(argv[4]) * w.rate); std::vector<int16_t> o(s);
        for (long i = std::max(0L, n - f); i < n; i++) { double g = (double)(n - i) / f; o[i * 2] = clip16(s[i * 2] * g); o[i * 2 + 1] = clip16(s[i * 2 + 1] * g); }
        if (!w16(out, w.rate, o)) return 2; printf("fade over the last %ld frames -> %s\n", f, out); return 0; }
    if (op == "splice" && argc > 5) { need16(); long a = lround(atof(argv[4]) * w.rate), k = atol(argv[5]); if (a + k > n) return 1; if (a < 0 || k < 0) { printf("splice: the position and the count must not be negative\n"); return 1; } std::vector<int16_t> o;
        o.insert(o.end(), s.begin(), s.begin() + (size_t)a * 2); o.insert(o.end(), s.begin() + (size_t)(a + k) * 2, s.end()); o.resize((size_t)n * 2, 0);
        if (!w16(out, w.rate, o)) return 2; printf("splice: %ld frames removed at frame %ld -> %s\n", k, a, out); return 0; }
    if (op == "noise" && argc > 6) { need16(); long a = std::max(0L, lround(atof(argv[4]) * w.rate)), b = std::min(n, lround(atof(argv[5]) * w.rate)); double rms = 32768.0 * pow(10.0, atof(argv[6]) / 20);
        std::vector<int16_t> o(s); uint32_t st = 0x2545F491u;
        for (long i = a; i < b; i++) for (int c = 0; c < 2; c++) { double u = 0; for (int k = 0; k < 12; k++) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; u += (st >> 8) / 16777216.0; }   // ~Gaussian, unit variance
            o[(size_t)i * 2 + c] = clip16(s[(size_t)i * 2 + c] + (u - 6.0) * rms); }
        if (!w16(out, w.rate, o)) return 2; printf("noise %s dBFS rms in frames %ld-%ld -> %s\n", argv[6], a, b, out); return 0; }
    if (op == "mshift" && argc > 4) { long r = atol(argv[4]); std::vector<int32_t> o((size_t)n * 2, 0);
        for (long i = 0; i < n; i++) { long j = i - r; if (j >= 0 && j < n) { o[i * 2] = w.s[j * 2] >> 8; o[i * 2 + 1] = w.s[j * 2 + 1] >> 8; } }
        if (!wavio::write24(out, w.rate, o)) return 2; printf("master shifted %+ld samples -> %s\n", r, out); return 0; }
    if (op == "to32") {   // 32-bit PCM WAV; the lowest byte gets a deterministic non-zero pattern (content a 24-bit encoder would drop)
        std::vector<uint8_t> o; wavio::hdr(o, w.rate, 2, 32, w.s.size() * 4, 0);
        for (size_t i = 0; i < w.s.size(); i++) { uint32_t v = (uint32_t)w.s[i] | (uint32_t)(1 + (i * 37) % 255); for (int k = 0; k < 4; k++) o.push_back((uint8_t)(v >> (8 * k))); }
        FILE* f = fopen(out, "wb"); if (!f) return 2; bool ok = fwrite(o.data(), 1, o.size(), f) == o.size(); if (fclose(f) || !ok) return 2;
        printf("32-bit master -> %s\n", out); return 0; }
    if (op == "meq" && argc > 6) { std::string ty = argv[4]; bool lin = ty.size() > 3 && ty.substr(ty.size() - 3) == "lin"; if (lin) ty.resize(ty.size() - 3);
        double f0 = atof(argv[5]), dB = atof(argv[6]) / (lin ? 2 : 1), Q = argc > 7 ? atof(argv[7]) : 1.0, A = pow(10.0, dB / 40), w0 = 2 * M_PI * f0 / w.rate, cs = cos(w0), sn = sin(w0);
        double b0, b1, b2, a0, a1, a2, al = ty == "peak" ? sn / (2 * Q) : sn / 2 * sqrt(2.0), sA = 2 * sqrt(A) * al;
        if (ty == "peak") { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
        else if (ty == "low") { b0 = A * ((A + 1) - (A - 1) * cs + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sA); a0 = (A + 1) + (A - 1) * cs + sA; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sA; }
        else if (ty == "high") { b0 = A * ((A + 1) + (A - 1) * cs + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sA); a0 = (A + 1) - (A - 1) * cs + sA; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sA; }
        else { printf("meq type must be low, high or peak (optionally with lin)\n"); return 1; }
        std::vector<double> x[2]; toCh(w, x);
        auto run = [&](std::vector<double>& v) { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; for (double& s : v) { double y = (b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0; x2 = x1; x1 = s; y2 = y1; y1 = y; s = y; } };
        for (int c = 0; c < 2; c++) { run(x[c]); if (lin) { std::reverse(x[c].begin(), x[c].end()); run(x[c]); std::reverse(x[c].begin(), x[c].end()); } }
        long nc; if (!w24(out, w.rate, x, nc)) return 2; printf("EQ %s%s %g Hz %+g dB Q %g, %ld samples clipped -> %s\n", ty.c_str(), lin ? " (zero phase)" : "", f0, atof(argv[6]), Q, nc, out); return 0; }
    if (op == "mlimit" && argc > 4) { std::vector<double> x[2]; toCh(w, x); double pk = 0; for (int c = 0; c < 2; c++) for (double v : x[c]) pk = std::max(pk, std::fabs(v));
        if (pk <= 0) { printf("silent master\n"); return 1; }
        const double ceil = 8388607.0 * pow(10.0, -0.3 / 20), g0 = ceil / pk * pow(10.0, atof(argv[4]) / 20); const long L = lround(0.001 * w.rate); const double rel = 1 - exp(-1.0 / (0.050 * w.rate));
        std::vector<double> req(n), mn(n), g(n); for (long i = 0; i < n; i++) { double a = std::max(std::fabs(x[0][i]), std::fabs(x[1][i])) * g0; req[i] = a > ceil ? ceil / a : 1.0; }
        for (long i = 0; i < n; i++) { double m = 1; for (long k = std::max(0L, i - L); k <= std::min(n - 1, i + L); k++) m = std::min(m, req[k]); mn[i] = m; }   // min over +-L: every average below stays <= req
        std::vector<double> cs(n + 1, 0); for (long i = 0; i < n; i++) cs[i + 1] = cs[i] + mn[i];
        double gp = 1, gmin = 1; long nlim = 0; for (long i = 0; i < n; i++) { long a = std::max(0L, i - L / 2), b = std::min(n, i + L / 2 + 1); double avg = (cs[b] - cs[a]) / (b - a);
            gp = std::min(avg, gp + (1 - gp) * rel); g[i] = gp; gmin = std::min(gmin, gp); if (gp < 0.9999) nlim++; }
        for (int c = 0; c < 2; c++) for (long i = 0; i < n; i++) x[c][i] *= g0 * g[i];
        long nc; if (!w24(out, w.rate, x, nc)) return 2; printf("limiter: gain %+.2f dB, %ld of %ld frames limited, deepest %.2f dB, %ld samples clipped -> %s\n", 20 * log10(g0), nlim, n, 20 * log10(gmin), nc, out); return 0; }
    if (op == "mfdelay" && argc > 4) { double d = atof(argv[4]); std::vector<double> x[2], y[2]; toCh(w, x);
        double f = -d - std::floor(-d); double k[2 * kH]; for (int m = -kH + 1; m <= kH; m++) k[m + kH - 1] = kern(f - m);
        for (int c = 0; c < 2; c++) { y[c].resize(n); for (long i = 0; i < n; i++) y[c][i] = at(x[c], i - d, k); }
        long nc; if (!w24(out, w.rate, y, nc)) return 2; printf("master delayed by %g samples (windowed sinc), %ld samples clipped -> %s\n", d, nc, out); return 0; }
    if (op == "mdrift" && argc > 4) { double e = atof(argv[4]) * 1e-6, mid = n / 2.0; std::vector<double> x[2], y[2]; toCh(w, x);
        const int PH = 1024; std::vector<double> tab((size_t)(PH + 1) * 2 * kH);   // taps for fractions q / PH (error <= 1/2048 sample)
        for (int q = 0; q <= PH; q++) for (int m = -kH + 1; m <= kH; m++) tab[(size_t)q * 2 * kH + m + kH - 1] = kern((double)q / PH - m);
        for (int c = 0; c < 2; c++) { y[c].resize(n); for (long i = 0; i < n; i++) { double p = mid + (i - mid) * (1 + e); long i0 = (long)std::floor(p); int q = (int)lround((p - i0) * PH);
            if (q == PH) { i0++; q = 0; } y[c][i] = at(x[c], (double)i0, &tab[(size_t)q * 2 * kH]); } }
        long nc; if (!w24(out, w.rate, y, nc)) return 2; printf("master speed changed by %g ppm (%.1f master samples between start and end), %ld samples clipped -> %s\n", atof(argv[4]), e * n, nc, out); return 0; }
    if (op == "mgate" && argc > 5) { std::vector<double> x[2]; toCh(w, x); const double thr = 8388608.0 * pow(10.0, atof(argv[4]) / 20); const long B = std::max(1L, lround(0.001 * w.rate)), nb = (n + B - 1) / B;
        const long hb = std::max(0L, lround(atof(argv[5]) / 1.0));   // hold in 1 ms blocks
        std::vector<char> loud(nb, 0); for (long k = 0; k < nb; k++) for (long i = k * B; i < std::min(n, (k + 1) * B) && !loud[k]; i++) if (std::fabs(x[0][i]) >= thr || std::fabs(x[1][i]) >= thr) loud[k] = 1;
        std::vector<long> cs(nb + 1, 0); for (long k = 0; k < nb; k++) cs[k + 1] = cs[k] + loud[k];
        long zeroed = 0; for (long k = 0; k < nb; k++) { long a = std::max(0L, k - hb), b = std::min(nb, k + hb + 1); if (cs[b] - cs[a] == 0) for (long i = k * B; i < std::min(n, (k + 1) * B); i++) { x[0][i] = x[1][i] = 0; zeroed++; } }
        long nc; if (!w24(out, w.rate, x, nc)) return 2; printf("gate %s dBFS, hold %s ms: %ld of %ld frames set to digital silence -> %s\n", argv[4], argv[5], zeroed, n, out); return 0; }
    if (op == "mdenoise" && argc > 5) { std::vector<double> x[2]; toCh(w, x); const double thr = 8388608.0 * pow(10.0, atof(argv[4]) / 20), gmin = pow(10.0, -fabs(atof(argv[5])) / 20);
        const double at = 1 - exp(-1.0 / (0.005 * w.rate)), rl = 1 - exp(-1.0 / (0.100 * w.rate)); double e = 0, gmn = 1; long cut = 0;
        for (long i = 0; i < n; i++) { double p = std::max(x[0][i] * x[0][i], x[1][i] * x[1][i]); e += (p > e ? at : rl) * (p - e); double r = sqrt(e);
            double g = r >= thr ? 1.0 : std::max(gmin, r / thr); if (g < 0.999) cut++; gmn = std::min(gmn, g); x[0][i] *= g; x[1][i] *= g; }
        long nc; if (!w24(out, w.rate, x, nc)) return 2; printf("downward expander below %s dBFS (2:1, max %s dB): %ld of %ld frames reduced, deepest %.1f dB -> %s\n", argv[4], argv[5], cut, n, 20 * log10(gmn), out); return 0; }
    printf("unknown operation %s\n", op.c_str()); return 1; }
