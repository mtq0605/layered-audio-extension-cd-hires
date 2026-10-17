// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Compare a decoded WAV with the reference (same rate and length): time-domain error, error per band (Welch FFT, Hann 16384),
// samples with error above -60 dBFS, samples at full scale in the decoded file.   usage: cmp_wav <reference.wav> <decoded.wav>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <complex>
#include "fft.h"
#include <algorithm>
#include "wavio.h"
#include "halfband.h"
using namespace std;
using fftlib::fft;
int main(int argc, char** argv) {
    wavio::Wav a, b; if (!wavio::read(argv[1], a) || !wavio::read(argv[2], b)) { printf("cannot read\n"); return 1; }
    if (a.ch != 2 || b.ch != 2) { printf("need stereo\n"); return 1; }
    if (a.rate != b.rate) {   // decoded at a lower octave: bring the reference down with the codec's own half-band tables (highest stage first)
        int base = fmod((double)a.rate, 44100.0) == 0 ? 44100 : 48000, stages = 0, d = 0; while ((base << stages) < a.rate) stages++; while ((a.rate >> d) > b.rate) d++;
        if ((a.rate >> d) != b.rate || d > stages) { printf("rate mismatch %d/%d\n", a.rate, b.rate); return 1; }
        long n0 = (long)a.s.size() / 2; std::vector<double> ch[2]; for (int c = 0; c < 2; c++) { ch[c].resize(n0); for (long i = 0; i < n0; i++) ch[c][i] = a.s[i * 2 + c] / 256.0; }
        for (int k = 0; k < d; k++) { int s = stages - 1 - k, half; const int32_t* h = s == 0 ? lae::kHalf0 : s == 1 ? lae::kHalf1 : s == 2 ? lae::kHalf2 : lae::kHalf3; half = s == 0 ? lae::kHalfLen0 : s == 1 ? lae::kHalfLen1 : s == 2 ? lae::kHalfLen2 : lae::kHalfLen3;
            for (int c = 0; c < 2; c++) { const std::vector<double>& x = ch[c]; long n = (long)x.size(); std::vector<double> y(n / 2);
                auto at = [&](long i) { if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; return x[std::min(std::max(i, 0L), n - 1)]; };
                for (long i = 0; i < n / 2; i++) { long cc = 2 * i; double acc = 0.5 * x[cc]; for (int j = 1, t = 0; j <= half; j += 2, t++) acc += h[t] / 2147483648.0 * (at(cc - j) + at(cc + j)); y[i] = acc; }
                ch[c].swap(y); } }
        long n1 = (long)ch[0].size(); a.s.assign(n1 * 2, 0); for (long i = 0; i < n1; i++) for (int c = 0; c < 2; c++) a.s[i * 2 + c] = (int32_t)llround(ch[c][i]) * 256;
        a.rate = b.rate; printf("reference brought down to %d Hz with the codec's half-band tables\n", a.rate); }
    long n = (long)min(a.s.size(), b.s.size()) / 2; const double FS = 8388608.0;
    const double fam = fmod(a.rate, 44100.0) == 0 ? 44100.0 : 48000.0;   // octave edges of the rate family
    double edges[] = {0, 20000, fam / 2, fam, 2 * fam, 4 * fam, 1e9}; const int NB = 6;
    for (int c = 0; c < 2; c++) {
        double se = 0, sx = 0, mx = 0; long big = 0, full = 0;
        for (long i = 0; i < n; i++) { double x = a.s[i * 2 + c] / 256, y = b.s[i * 2 + c] / 256, e = x - y; se += e * e; sx += x * x; mx = max(mx, fabs(e)); if (fabs(e) > FS / 1000) big++; if (fabs(y) >= FS - 1) full++; }
        const int M = 16384; vector<double> pe(NB, 0), px(NB, 0); long frames = 0;
        for (long s0 = 0; s0 + M <= n; s0 += M / 2, frames++) { vector<complex<double>> E(M), X(M);
            for (int i = 0; i < M; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / M), x = a.s[(s0 + i) * 2 + c] / 256, y = b.s[(s0 + i) * 2 + c] / 256; E[i] = (x - y) * w; X[i] = x * w; }
            fft(E); fft(X); for (int k = 1; k < M / 2; k++) { double f = (double)k * a.rate / M; int bi = 0; while (bi < NB - 1 && f >= edges[bi + 1]) bi++; pe[bi] += norm(E[k]); px[bi] += norm(X[k]); } }
        double norm0 = frames * (double)M * M * 0.375 * FS * FS / 2;   // Hann power, one-sided -> dBFS of the band's mean square
        printf("ch %c: error RMS %.1f dBFS (signal %.1f dBFS), peak %.0f LSB24 | |err| > -60 dBFS: %ld | decoded samples at full scale: %ld\n bands (signal / error dBFS):",
               c ? 'R' : 'L', 20 * log10(sqrt(se / n) / FS + 1e-30), 20 * log10(sqrt(sx / n) / FS + 1e-30), mx, big, full);
        for (int bi = 0; bi < NB; bi++) { if (edges[bi] >= a.rate / 2) break; printf(" [%.1f-%.1fk %.0f / %.0f]", edges[bi] / 1000, min(edges[bi + 1], a.rate / 2.0) / 1000, 10 * log10(px[bi] / norm0 + 1e-30), 10 * log10(pe[bi] / norm0 + 1e-30)); }
        printf("\n");
        if (getenv("LEVELS")) {   // decoded band level minus master band level (dB): shows spectral holes / excess energy
            vector<double> pdx(NB, 0), pxx(NB, 0);
            for (long s0 = 0; s0 + M <= n; s0 += M / 2) { vector<complex<double>> Y(M), X(M);
                for (int i = 0; i < M; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / M); X[i] = a.s[(s0 + i) * 2 + c] / 256 * w; Y[i] = b.s[(s0 + i) * 2 + c] / 256 * w; }
                fft(X); fft(Y); for (int k = 1; k < M / 2; k++) { double f = (double)k * a.rate / M; int bi = 0; while (bi < NB - 1 && f >= edges[bi + 1]) bi++; pdx[bi] += norm(Y[k]); pxx[bi] += norm(X[k]); } }
            printf(" level diff (decoded - master, dB):"); for (int bi = 0; bi < NB; bi++) { if (edges[bi] >= a.rate / 2) break; printf(" [%.1f-%.1fk %+.2f]", edges[bi] / 1000, min(edges[bi + 1], a.rate / 2.0) / 1000, 10 * log10((pdx[bi] + 1e-30) / (pxx[bi] + 1e-30))); } printf("\n"); }
        if (getenv("SPEC")) {   // diagnostic: error spectrum in 1 kHz bins up to SPEC kHz (same Welch frames)
            int kmax = atoi(getenv("SPEC")); vector<double> pb(kmax, 0), xb(kmax, 0);
            for (long s0 = 0; s0 + M <= n; s0 += M / 2) { vector<complex<double>> E(M), X(M);
                for (int i = 0; i < M; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / M), x = a.s[(s0 + i) * 2 + c] / 256, y = b.s[(s0 + i) * 2 + c] / 256; E[i] = (x - y) * w; X[i] = x * w; }
                fft(E); fft(X); for (int k = 1; k < M / 2; k++) { int bi = (int)((double)k * a.rate / M / 1000); if (bi < kmax) { pb[bi] += norm(E[k]); xb[bi] += norm(X[k]); } } }
            printf(" spectrum (kHz: signal/error dB):"); for (int bi = 0; bi < kmax; bi++) printf(" %d:%.0f/%.0f", bi, 10 * log10(xb[bi] / norm0 + 1e-30), 10 * log10(pb[bi] / norm0 + 1e-30)); printf("\n"); }
        {   // waveform match beyond the level: overall SNR, 10 ms blocks (worst error, worst and median local SNR where the signal is above -60 dBFS)
            long B = a.rate / 100; double wErr = 0, wSnr = 1e300; vector<double> snrs;
            for (long s0 = 0; s0 + B <= n; s0 += B) { double q = 0, p = 0; for (long i = s0; i < s0 + B; i++) { double x = a.s[i * 2 + c] / 256, e = x - b.s[i * 2 + c] / 256; q += e * e; p += x * x; }
                wErr = max(wErr, q / B); if (p / B > FS * FS * 1e-6) { double r = p / max(q, 1e-30); wSnr = min(wSnr, r); snrs.push_back(r); } }
            sort(snrs.begin(), snrs.end()); double med = snrs.empty() ? 0 : snrs[snrs.size() / 2];
            printf(" match: SNR %.1f dB | peak error %.1f dBFS | 10 ms blocks: worst error %.1f dBFS, local SNR worst %.1f dB / median %.1f dB\n",
                   10 * log10(sx / max(se, 1e-30)), 20 * log10(mx / FS + 1e-30), 10 * log10(wErr / FS / FS + 1e-30), snrs.empty() ? 0 : 10 * log10(wSnr), snrs.empty() ? 0 : 10 * log10(med)); }
        if (getenv("PERSEC")) { printf(" per second error dBFS:"); for (long s0 = 0; s0 < n; s0 += a.rate) { double q = 0; long m = 0; for (long i = s0; i < min(n, s0 + (long)a.rate); i++) { double e = (a.s[i * 2 + c] - b.s[i * 2 + c]) / 256.0; q += e * e; m++; } printf(" %.0f", 10 * log10(q / max(1L, m) / FS / FS + 1e-30)); } printf("\n"); } }
    return 0; }
