// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Adaptive sustained-tone detector (encoder-side analysis, HF region above the base Nyquist).
// No sensitivity setting: thresholds come from the noise statistics of averaged periodograms (Gamma quantile at a fixed
// false-alarm rate per test), evaluated at several time scales (W = 1,2,4,8,16 long-FFT frames), local floor from medians of
// neighbouring bin chunks. A detection needs >= 2 consecutive windows at its scale (+-1 bin drift allowed).
// usage:
//   tone_scan scan <raw s32le stereo> <rate>                 list sustained structures found in a file
//   tone_scan enum <raw s32le stereo|synthetic:white|pink|rising> <rate>   enumerate injected tones (level x duration x frequency)
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <complex>
#include "fft.h"
#include <algorithm>
#include <string>
#include <chrono>
using namespace std;
typedef complex<double> cd;
using fftlib::fft;
struct Ev { double f, t0, t1, db; int W; };
struct Detector {
    double rate; int NF, b0, b1, nfr; double hop;
    vector<float> P;          // [frame][bin-b0], periodogram normalised so white noise sigma^2 -> mean sigma^2
    vector<double> bgFloor;   // per bin (b0..b1): whole-file median-based mean noise power (for injection sizing)
    const vector<float>* refP = nullptr;   // optional: take the local floor from another signal's periodograms (closed-loop error checks)
    void analyse(const vector<double>& x) {
        NF = 1; while (NF < rate * 0.09) NF <<= 1;
        double nyq = rate / 2, base = getenv("FULLBAND") ? 0.0 : (rate >= 88200 ? (fmod(rate, 44100.0) == 0 ? 22050.0 : 24000.0) : nyq);
        b0 = (int)ceil(base / (rate / NF)) + 8; b1 = (int)floor(0.98 * nyq / (rate / NF));
        vector<double> w(NF); double s2 = 0; for (int i = 0; i < NF; i++) { w[i] = 0.5 - 0.5 * cos(2 * M_PI * i / NF); s2 += w[i] * w[i]; }
        nfr = (int)((x.size() - NF) / (NF / 2)) + 1; hop = (NF / 2) / rate;
        int nb = b1 - b0; P.assign((size_t)nfr * nb, 0.f); vector<cd> a(NF);
        for (int f = 0; f < nfr; f++) { long s = (long)f * (NF / 2);
            for (int i = 0; i < NF; i++) a[i] = cd(x[s + i] * w[i], 0); fft(a);
            for (int b = b0; b < b1; b++) P[(size_t)f * nb + b - b0] = (float)(norm(a[b]) / s2); }
    }
    static double meanOverMedian(double k) { double q = 1 - 1 / (9 * k); return 1 / (q * q * q); }
    static double thr(double k, double z) { double q = 1 - 1 / (9 * k) + z * sqrt(1 / (9 * k)); return q * q * q; }
    vector<Ev> detect(long* rawExceed = nullptr) {
        int nb = b1 - b0; const int CH = 32; int nch = nb / CH; vector<Ev> ev;
        const double Z = 6.0;    // ~1e-9 per test
        int Ws[5] = {1, 2, 4, 8, 16};
        for (int wi = 0; wi < 5; wi++) {
            int W = Ws[wi]; int nw = nfr / W; if (nw < 2) continue;
            double k = max(1.0, 0.75 * W), T = thr(k, Z), corr = meanOverMedian(k);
            vector<char> E((size_t)nw * nb, 0); vector<double> avg(nb), chm(nch); vector<float> tmp(CH);
            for (int wdx = 0; wdx < nw; wdx++) {
                for (int b = 0; b < nb; b++) { double s = 0; for (int j = 0; j < W; j++) s += P[(size_t)(wdx * W + j) * nb + b]; avg[b] = s / W; }
                for (int c = 0; c < nch; c++) { for (int j = 0; j < CH; j++) { int b2 = c * CH + j; if (refP) { double s = 0; for (int q = 0; q < W; q++) s += (*refP)[(size_t)(wdx * W + q) * nb + b2]; tmp[j] = (float)(s / W); } else tmp[j] = (float)avg[b2]; } nth_element(tmp.begin(), tmp.begin() + CH / 2, tmp.end()); chm[c] = tmp[CH / 2]; }
                for (int b = 0; b < nch * CH; b++) {
                    int c = b / CH; double m[5]; int n = 0; for (int d = -2; d <= 2; d++) { int cc = c + d; if (cc >= 0 && cc < nch) m[n++] = chm[cc]; }
                    sort(m, m + n); double fl = m[n / 2] * corr;
                    if (fl > 0 && avg[b] > T * fl) { E[(size_t)wdx * nb + b] = 1; if (rawExceed) rawExceed[wi]++; }
                }
            }
            // persistence: >= 2 consecutive windows, +-1 bin drift
            for (int b = 1; b < nb - 1; b++) {
                int wdx = 0;
                while (wdx < nw) {
                    auto hit = [&](int q) { return E[(size_t)q * nb + b] || E[(size_t)q * nb + b - 1] || E[(size_t)q * nb + b + 1]; };
                    if (!E[(size_t)wdx * nb + b]) { wdx++; continue; }
                    int s = wdx; while (wdx < nw && hit(wdx)) wdx++;
                    if (wdx - s >= 2) { double pk = 0; for (int q = s; q < wdx; q++) { double sum = 0; for (int j = 0; j < W; j++) sum += P[(size_t)(q * W + j) * nb + b]; pk = max(pk, sum / W); }
                        ev.push_back({(b + b0) * rate / NF, s * W * hop, wdx * W * hop + (NF / rate), 10 * log10(pk / max(1e-30, 1.0)), W}); }
                }
            }
        }
        // merge across bins/scales: same frequency within 3 bins and overlapping time
        sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.f < b.f; });
        vector<Ev> out; double df = 3 * rate / NF;
        for (auto& e : ev) { bool m = false;
            for (auto& o : out) if (fabs(o.f - e.f) <= df && e.t0 <= o.t1 && o.t0 <= e.t1) { o.t0 = min(o.t0, e.t0); o.t1 = max(o.t1, e.t1); o.W = min(o.W, e.W); m = true; break; }
            if (!m) out.push_back(e); }
        return out;
    }
    void floorProfile() {
        int nb = b1 - b0; bgFloor.assign(nb, 0); vector<float> col(nfr);
        for (int b = 0; b < nb; b++) { for (int f = 0; f < nfr; f++) col[f] = P[(size_t)f * nb + b]; nth_element(col.begin(), col.begin() + nfr / 2, col.end()); bgFloor[b] = col[nfr / 2] * 1.4427; }
        // smooth across 65 bins (median) to remove the effect of existing tones
        vector<double> sm(nb); vector<double> t;
        for (int b = 0; b < nb; b++) { t.clear(); for (int d = -32; d <= 32; d++) { int q = b + d; if (q >= 0 && q < nb) t.push_back(bgFloor[q]); } nth_element(t.begin(), t.begin() + t.size() / 2, t.end()); sm[b] = t[t.size() / 2]; }
        bgFloor = sm;
    }
};
static vector<double> loadOrSynth(const string& src, double rate, double secs) {
    vector<double> x;
    if (src.rfind("synthetic:", 0) == 0) {
        long n = (long)(rate * secs); x.resize(n); uint64_t s = 88172645463325252ull; string t = src.substr(10);
        auto g = [&]() { double u = 0; for (int i = 0; i < 12; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; u += (s >> 11) * (1.0 / 9007199254740992.0); } return u - 6.0; };
        double p1 = 0, p2 = 0;
        if (t == "silence") { for (long i = 0; i < n; i++) x[i] = g() * 0.5; return x; }
        if (t == "electronic") {   // square wave (ultrasonic harmonics) + log sweep 20->0.45*rate + intentional white-noise bursts + low noise floor
            double ph = 0, ph2 = 0;
            for (long i = 0; i < n; i++) { double tt = i / rate; double v = g() * 2.0;
                ph += 2 * M_PI * 3000.0 / rate; v += 200000.0 * (sin(ph) >= 0 ? 1 : -1) * ((long)(tt / 4) % 2 == 0 ? 1 : 0);
                double f = 20000.0 * pow(0.45 * rate / 20000.0, tt / secs); ph2 += 2 * M_PI * f / rate; v += 20000.0 * sin(ph2);
                if (fmod(tt, 5.0) > 4.0) v += g() * 30000.0;
                x[i] = v; }
            return x; }
        for (long i = 0; i < n; i++) { double v = g() * 50;
            if (t == "pink") { p1 = 0.995 * p1 + v * 0.1; v = p1 * 5 + v * 0.3; }
            if (t == "rising") { double d = v - p2; p2 = v; v = d; }
            x[i] = v; }
        return x;
    }
    FILE* f = fopen(src.c_str(), "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    long fr = sz / 8; vector<int32_t> raw(fr * 2); fread(raw.data(), 4, raw.size(), f); fclose(f);
    x.resize(fr); const char* cs = getenv("CHSEL"); char sel = cs ? cs[0] : 'L';
    for (long i = 0; i < fr; i++) { long l = raw[i * 2] >> 8, r = raw[i * 2 + 1] >> 8;
        x[i] = sel == 'R' ? r : sel == 'S' ? (double)(l - r) : sel == 'M' ? (double)(r + ((l - r) >> 1)) : l; }
    return x;
}
#ifndef TONE_NO_MAIN
int main(int argc, char** argv) {
    auto T0 = chrono::steady_clock::now();
    string mode = argv[1], src = argv[2]; double rate = atof(argv[3]);
    vector<double> x = loadOrSynth(src, rate, 40.0);
    Detector D; D.rate = rate;
    if (mode == "scan") {
        D.analyse(x); long ex[5] = {0}; auto ev = D.detect(ex);
        printf("%s: %zu sustained structures (long FFT %d, %.1f Hz bins)\n", src.c_str(), ev.size(), D.NF, rate / D.NF);
        sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return (a.t1 - a.t0) > (b.t1 - b.t0); });
        for (size_t i = 0; i < ev.size() && i < 10; i++) printf("   %8.3f kHz  %6.1f-%6.1f s  scale W=%d\n", ev[i].f / 1000, ev[i].t0, ev[i].t1, ev[i].W);
        printf("runtime %.1f s\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
        return 0;
    }
    // enumeration: background first (false alarms / existing structures), then injected tones
    D.analyse(x); D.floorProfile(); long ex[5] = {0}; auto bg = D.detect(ex);
    bool synthetic = src.rfind("synthetic:", 0) == 0;
    printf("background %s: %zu detections without injection (%s); raw exceedances per scale W1..16: %ld %ld %ld %ld %ld\n", src.c_str(), bg.size(),
           synthetic ? "pure noise -> these are FALSE ALARMS" : "real music -> existing structures", ex[0], ex[1], ex[2], ex[3], ex[4]);
    double levels[] = {-20, -15, -10, -6, -3, 0, 3, 6, 10}; double durs[] = {0.5, 1, 2, 5};
    int nb = D.b1 - D.b0; double fmin = D.b0 * rate / D.NF, fmax = D.b1 * rate / D.NF;
    printf("detection rate by codec-domain SNR (dB re per-coefficient floor of a 512-MDCT) and duration; 12 tones per cell, random frequencies\n");
    printf("  SNR\\dur ");
    for (double d : durs) printf("  %4.1fs", d);
    printf("\n");
    uint32_t rs = 12345; auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs >> 8) / 16777216.0; };
    long newFA = 0;
    for (double L : levels) {
        printf("  %+5.0f  ", L);
        for (double dur : durs) {
            vector<double> y = x; int nt = 12; vector<double> tf(nt), tt(nt);
            for (int i = 0; i < nt; i++) {
                // frequencies spread over the HF region, avoiding existing background structures by +-50 bins
                double f; bool ok;
                do { ok = true; f = fmin + (fmax - fmin) * (0.03 + 0.94 * (i + rnd()) / nt);
                    for (auto& e : bg) if (fabs(e.f - f) < 50 * rate / D.NF) ok = false; } while (!ok && rnd() < 0.999);
                tf[i] = f; tt[i] = 2 + rnd() * (40 - 4 - dur);
                int b = (int)round(f / (rate / D.NF)) - D.b0; b = max(0, min(nb - 1, b));
                double sig = sqrt(D.bgFloor[b]); double A = sig * sqrt(pow(10, L / 10) / 128.0);
                long s0 = (long)(tt[i] * rate), s1 = (long)((tt[i] + dur) * rate);
                for (long n = s0; n < s1 && n < (long)y.size(); n++) y[n] += A * sin(2 * M_PI * f * n / rate + i);
            }
            Detector E; E.rate = rate; E.analyse(y); auto ev = E.detect();
            int hit = 0;
            for (int i = 0; i < nt; i++) for (auto& e : ev) if (fabs(e.f - tf[i]) <= 3 * rate / D.NF && e.t0 <= tt[i] + dur && tt[i] <= e.t1) { hit++; break; }
            // events not explained by injected tones or background structures = new false alarms
            for (auto& e : ev) { bool expl = false;
                for (int i = 0; i < nt; i++) if (fabs(e.f - tf[i]) <= 3 * rate / D.NF) expl = true;
                for (auto& o : bg) if (fabs(e.f - o.f) <= 3 * rate / D.NF) expl = true;
                if (!expl) newFA++; }
            printf("  %3d%%  ", 100 * hit / nt); fflush(stdout);
        }
        printf("\n");
    }
    printf("unexplained detections during enumeration (false alarms): %ld\n", newFA);
    printf("runtime %.1f s\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    return 0;
}
#endif
