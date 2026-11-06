// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Noise system step 0 (2026-11-04): read-only measurement of how noise looks in real masters. Writes nothing next to the input.
// Not encoder code: its numbers only guide the design of the noise identification and never reach a coded file.
// Analysis: STFT of M = (L+R)/2 and S = (L-R)/2, Hann window, hop = window/2, window = 1024 x (rate / 88.2k family base),
// i.e. about 86-94 Hz per bin at every rate. Fixed bands from 20 Hz to Nyquist. Levels are absolute dBFS of the band signal
// (a full-scale sine = 0 dBFS).
// Per band and channel:
//   floor      5th percentile of the frame levels over the file
//   segRange   max - min of the per-5-s floors (how much the floor moves over time)
//   spreadObs  p50 - p5 of the frame levels, spreadTh = the same for stationary Gaussian noise in this band (Gamma quantiles):
//              obs / th near 1 means "at least half the frames are stationary noise", much more means music or a moving floor
//   noiseFrac  share of frames whose level stays below the floor's 1e-6 Gaussian upper bound
//   margin50/90 level - floor of the frames above that bound (how far music rises over the noise; masking headroom)
//   cohN/cohSh L/R magnitude-squared coherence over the noise-like frames, and the same with R shifted by 5 s (surrogate baseline)
// Per file: exact digital zero (samples, whole frames), and persistent narrow lines (bins whose long-term median is >= 10 dB above
// the median of their +-4..12 neighbours in >= 80 % of the frames).
// usage: noise_measure <in.wav> <out.txt>
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "fft.h"
using namespace std;

static bool readWav(const char* path, vector<double>& L, vector<double>& R, double& rate) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    uint8_t hdr[12]; if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return false; }
    int fmt = 0, ch = 0, bits = 0; bool ok = false;
    for (;;) {
        uint8_t c[8]; if (fread(c, 1, 8, f) != 8) break;
        uint32_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) { vector<uint8_t> b(n); if (fread(b.data(), 1, n, f) != n) break;
            fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24; bits = b[14] | b[15] << 8;
            if (fmt == 0xFFFE && n >= 26) fmt = b[24] | b[25] << 8; if (n & 1) fseek(f, 1, SEEK_CUR); continue; }
        if (!memcmp(c, "data", 4)) {
            if (ch != 2 || !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) break;
            int bps = bits / 8; size_t frames = n / (2 * bps); vector<uint8_t> b(frames * 2 * bps);
            if (fread(b.data(), 1, b.size(), f) != b.size()) break;
            L.resize(frames); R.resize(frames);
            for (size_t i = 0; i < frames * 2; i++) { const uint8_t* p = &b[i * bps]; double v;
                if (fmt == 3) { float fl; memcpy(&fl, p, 4); v = fl; }
                else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0;
                else if (bits == 24) v = (int32_t)((uint32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0;
                else v = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0;
                (i & 1 ? R : L)[i / 2] = v; }
            ok = true; break; }
        fseek(f, n + (n & 1), SEEK_CUR);
    }
    fclose(f); return ok;
}
// chi-square quantile (Wilson-Hilferty) for nu degrees of freedom at normal deviate zq, divided by nu (mean 1)
static double chiq(double nu, double zq) { double a = 2.0 / (9.0 * nu); double v = 1.0 - a + zq * sqrt(a); return max(v, 1e-6) * v * v; }
static double pct(vector<double> v, double q) { if (v.empty()) return NAN; size_t k = (size_t)(q * (v.size() - 1)); nth_element(v.begin(), v.begin() + k, v.end()); return v[k]; }

// One STFT pass with window W over the bands in edges (Hz); tone lines are searched between lineLo and lineHi Hz.
static void runPass(FILE* out, const vector<double>& L, const vector<double>& R, double rate, int W, vector<double> edges, double lineLo, double lineHi) {
    const size_t len = L.size(); int H = W / 2; double binHz = rate / W;
    vector<double> win(W); double sw2 = 0; for (int i = 0; i < W; i++) { win[i] = 0.5 - 0.5 * cos(2 * M_PI * (i + 0.5) / W); sw2 += win[i] * win[i]; }
    // power normalisation: band power of a full-scale sine (amplitude 1) = 0.5 -> 0 dBFS
    const double sc = 2.0 / (W * sw2) / 0.5;   // |X|^2 * sc summed over the band's bins = band power relative to a FS sine
    long nfr = len >= (size_t)W ? (long)((len - W) / H + 1) : 0;
    int nb = (int)edges.size() - 1; int nbin = W / 2 + 1;
    vector<int> b0(nb), b1(nb); for (int b = 0; b < nb; b++) { b0[b] = max(1, (int)ceil(edges[b] / binHz)); b1[b] = min(nbin - 1, (int)floor(edges[b + 1] / binHz) - 1); if (b1[b] < b0[b]) b1[b] = b0[b]; }
    int k0 = max(2, (int)ceil(lineLo / binHz)), k1 = min(nbin - 14, (int)floor(lineHi / binHz));
    // M, S levels per band per frame; per-bin M power in the line range; L and R spectra kept for the shifted surrogate
    vector<double> lvM((size_t)nb * nfr), lvS((size_t)nb * nfr);
    int kl0 = max(1, k0 - 13); int nlb = max(0, k1 - kl0 + 14);   // stored bins kl0 .. k1 + 13
    vector<float> binM((size_t)nlb * nfr);
    vector<complex<double>> crossLR((size_t)nb * nfr); vector<double> pL((size_t)nb * nfr), pR((size_t)nb * nfr);
    long shiftFr = (long)llround(5.0 * rate / H);
    vector<complex<float>> specL((size_t)nfr * nbin), specR((size_t)nfr * nbin);
    vector<complex<double>> a(W), c(W);
    for (long t = 0; t < nfr; t++) {
        size_t o = (size_t)t * H;
        for (int i = 0; i < W; i++) { a[i] = complex<double>(L[o + i] * win[i], 0); c[i] = complex<double>(R[o + i] * win[i], 0); }
        fftlib::fft(a); fftlib::fft(c);
        for (int k = 0; k < nbin; k++) { specL[(size_t)t * nbin + k] = complex<float>(a[k]); specR[(size_t)t * nbin + k] = complex<float>(c[k]); }
        for (int j = 0; j < nlb; j++) { complex<double> m = (a[kl0 + j] + c[kl0 + j]) * 0.5; binM[(size_t)j * nfr + t] = (float)(norm(m) * sc); }
        for (int b = 0; b < nb; b++) { double eM = 0, eS = 0, el = 0, er = 0; complex<double> x(0, 0);
            for (int k = b0[b]; k <= b1[b]; k++) { complex<double> m = (a[k] + c[k]) * 0.5, s = (a[k] - c[k]) * 0.5;
                eM += norm(m); eS += norm(s); el += norm(a[k]); er += norm(c[k]); x += a[k] * conj(c[k]); }
            size_t id = (size_t)b * nfr + t;
            lvM[id] = 10 * log10(max(eM * sc, 1e-30)); lvS[id] = 10 * log10(max(eS * sc, 1e-30)); crossLR[id] = x; pL[id] = el; pR[id] = er; }
    }
    fprintf(out, "--- pass: window %d, bin %.1f Hz, hop %.1f ms, frames %ld ---\n", W, binHz, 1000.0 * H / rate, nfr);
    const double zHi = 4.753;   // 1e-6 one-sided upper tail
    long segFr = (long)llround(5.0 * rate / H);
    for (int chn = 0; chn < 2; chn++) {
        const vector<double>& lv = chn ? lvS : lvM;
        fprintf(out, "[%s]\n%-15s %5s %8s %8s %8s %8s %8s %8s %8s %8s %6s %6s\n", chn ? "S" : "M", "band Hz", "bins", "floor", "f/kHz", "segRng", "sprObs", "sprTh", "noiseF", "marg50", "marg90", "cohN", "cohSh");
        for (int b = 0; b < nb; b++) {
            vector<double> v(lv.begin() + (size_t)b * nfr, lv.begin() + (size_t)(b + 1) * nfr);
            // drop frames that are exact digital zero in this band (level at the 1e-30 clamp)
            vector<double> nz; for (double x : v) if (x > -290) nz.push_back(x);
            int bins = b1[b] - b0[b] + 1; double nu = 2.0 * bins / 1.5;   // Hann: ~1.5 bins per independent complex value
            if (nz.size() < 20) { fprintf(out, "%6.0f-%-8.0f %5d  (digital zero)\n", edges[b], edges[b + 1], bins); continue; }
            double fl = pct(nz, 0.05), p50 = pct(nz, 0.50);
            double sprTh = 10 * log10(chiq(nu, 0.0) / chiq(nu, -1.645)), sprObs = p50 - fl;
            // floor mean level from the 5th percentile: mean = p5 / chiq(nu, z0.05)
            double meanFl = fl - 10 * log10(chiq(nu, -1.645)); double hiBound = meanFl + 10 * log10(chiq(nu, zHi));
            vector<double> segF; for (long s0 = 0; s0 + segFr <= nfr; s0 += segFr) { vector<double> sv; for (long t = s0; t < s0 + segFr; t++) if (v[t] > -290) sv.push_back(v[t]); if (sv.size() > 20) segF.push_back(pct(sv, 0.05)); }
            double segRng = segF.empty() ? NAN : *max_element(segF.begin(), segF.end()) - *min_element(segF.begin(), segF.end());
            long nN = 0; vector<double> mg; complex<double> xs(0, 0), xh(0, 0); double sl = 0, sr = 0, shl = 0, shr = 0;
            for (long t = 0; t < nfr; t++) { size_t id = (size_t)b * nfr + t; if (v[t] <= -290) continue;
                if (v[t] <= hiBound) { nN++;
                    if (chn == 0) { xs += crossLR[id]; sl += pL[id]; sr += pR[id];
                        long t2 = (t + shiftFr) % nfr; complex<double> xx(0, 0); double e2 = 0;
                        for (int k = b0[b]; k <= b1[b]; k++) { xx += complex<double>(specL[(size_t)t * nbin + k]) * conj(complex<double>(specR[(size_t)t2 * nbin + k])); e2 += norm(complex<double>(specR[(size_t)t2 * nbin + k])); }
                        xh += xx; shl += pL[id]; shr += e2; } }
                else mg.push_back(v[t] - meanFl); }
            double cohN = (chn == 0 && sl > 0 && sr > 0) ? norm(xs) / (sl * sr) : NAN, cohSh = (chn == 0 && shl > 0 && shr > 0) ? norm(xh) / (shl * shr) : NAN;
            double bwk = (bins * binHz) / 1000.0;
            fprintf(out, "%6.0f-%-8.0f %5d %8.1f %8.1f %8.1f %8.1f %8.1f %8.3f %8.1f %8.1f %6.3f %6.3f\n", edges[b], edges[b + 1], bins, fl, meanFl - 10 * log10(bwk),
                    segRng, sprObs, sprTh, (double)nN / nz.size(), mg.empty() ? NAN : pct(mg, 0.5), mg.empty() ? NAN : pct(mg, 0.9), cohN, cohSh);
        }
        fprintf(out, "\n");
    }
    // persistent narrow lines on M
    fprintf(out, "[lines M %.0f-%.0f Hz] freq Hz, median level dBFS, excess over neighbours dB, share of frames >= 10 dB above neighbours\n", lineLo, lineHi);
    vector<double> med(nlb); for (int j = 0; j < nlb; j++) { vector<double> v(binM.begin() + (size_t)j * nfr, binM.begin() + (size_t)(j + 1) * nfr); med[j] = 10 * log10(max(pct(v, 0.5), 1e-30)); }
    int nLines = 0;
    for (int j = k0 - kl0; j <= k1 - kl0; j++) {   // neighbours outside the stored range (near 0 Hz) are skipped
        vector<double> nbv; for (int d = 4; d <= 12; d++) { if (j - d >= 0) nbv.push_back(med[j - d]); if (j + d < nlb) nbv.push_back(med[j + d]); }
        double ref = pct(nbv, 0.5); double ex = med[j] - ref; if (ex < 10 || (j > 0 && med[j] < med[j - 1]) || med[j] < med[j + 1]) continue;
        long on = 0; for (long t = 0; t < nfr; t++) { double nref = 0; int cnt = 0; for (int d = 4; d <= 12; d += 4) { if (j - d >= 0) { nref += binM[(size_t)(j - d) * nfr + t]; cnt++; } if (j + d < nlb) { nref += binM[(size_t)(j + d) * nfr + t]; cnt++; } }
            if (binM[(size_t)j * nfr + t] >= 10.0 * nref / cnt) on++; }
        double share = (double)on / max<long>(nfr, 1); if (share < 0.8) continue;
        fprintf(out, "%10.1f %8.1f %6.1f %6.3f\n", (kl0 + j) * binHz, med[j], ex, share); if (++nLines >= 40) { fprintf(out, "(more lines cut)\n"); break; }
    }
    if (!nLines) fprintf(out, "(none)\n");
    fprintf(out, "\n");
}

#ifndef NOISE_MEASURE_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: noise_measure <in.wav> <out.txt>\n"); return 2; }
    vector<double> L, R; double rate = 0;
    if (!readWav(argv[1], L, R, rate)) { fprintf(stderr, "cannot read %s (needs stereo PCM 16/24/32 or float32)\n", argv[1]); return 1; }
    const size_t len = L.size();
    int fam = (fmod(rate, 44100.0) == 0) ? 44100 : 48000; int mult = (int)llround(rate / (2.0 * fam));
    int W = 1024 * max(mult, 1);
    FILE* out = fopen(argv[2], "w"); if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    size_t zeroSamples = 0; for (size_t i = 0; i < len; i++) if (L[i] == 0 && R[i] == 0) zeroSamples++;
    long zeroBlocks = 0; for (size_t o = 0; o + W <= len; o += W) { bool z = true; for (int i = 0; i < W && z; i++) z = L[o + i] == 0 && R[o + i] == 0; zeroBlocks += z; }
    fprintf(out, "file %s\nrate %.0f  length %.2f s\n", argv[1], rate, len / rate);
    fprintf(out, "exact digital zero: %.3f %% of samples, %ld whole blocks of %d (%.2f s)\n\n", 100.0 * zeroSamples / max<size_t>(len, 1), zeroBlocks, W, zeroBlocks * (double)W / rate);
    // low pass: 8x window (~11 Hz bins) for 20-1600 Hz and low tone lines (hum)
    runPass(out, L, R, rate, 8 * W, {20, 50, 100, 200, 400, 800, 1600}, 30, 2000);
    vector<double> edges = {1600, 3150, 6300, 10000, 14000, 18000, 20000, 22050, 24000, 30000, 40000, 50000,
                            60000, 80000, 100000, 120000, 140000, 160000, 176400};
    double binHz = rate / W;
    while (edges.size() > 1 && edges.back() > rate / 2) edges.pop_back();
    if (edges.back() < rate / 2 - binHz) edges.push_back(rate / 2);
    runPass(out, L, R, rate, W, edges, 2000, rate / 2);
    fclose(out); return 0;
}
#endif
