// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Encoder-side split of the master into the depth target T and the frequency target F.
//   v  = 256 x CD16 - regenerated dither                      (what every decoder can rebuild from the CD alone)
//   T  = lowpass_cutoff(base24 - v)                          (depth layer target, base rate: 16 -> 24 bit below the CD cut-off)
//   F  = master - up(v + T)                                  (frequency layer target, master rate: supplement band + octaves)
// Decoders: CD + F -> 16-bit route (lacks only T); CD + T^ + F^ -> 24-bit route; the three parts add back to the master exactly
// in the time domain, so no band splicing between layers. T and F are encoder-side targets only (float lowpass is fine).
// usage: routes <master s32 stereo @rate> <rate> <cd16 s16 stereo> <base24 s32 stereo> <T out s32> <F out s32> [cutoff Hz | 0 = detect]
// env DITHERFLAGS = per-block dither flags written by base_layer. Prints the detected cut-off.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <thread>
#include <exception>
#include <complex>
#include "fft.h"
#include <algorithm>
#include "halfband.h"
#include "enc_io.h"
using namespace std; using namespace lae;
static const int32_t* tab(int s, int& half) { switch (s) { case 0: half = kHalfLen0; return kHalf0; case 1: half = kHalfLen1; return kHalf1; case 2: half = kHalfLen2; return kHalf2; default: half = kHalfLen3; return kHalf3; } }
// runs f(0) here and f(1) in a second thread (the two channels); an exception in either is re-thrown here after both have finished
template <class F> static void twoWay(F f) {
    std::exception_ptr x1; std::thread t; bool started = false; const char* pt = getenv("PARTTHREADS");   // PARTTHREADS=1: no second thread
    if (!(pt && atoi(pt) == 1)) try { t = std::thread([&]() { try { f(1); } catch (...) { x1 = std::current_exception(); } }); started = true; } catch (...) {}
    std::exception_ptr x0; try { f(0); } catch (...) { x0 = std::current_exception(); }
    if (started) t.join(); else { try { f(1); } catch (...) { x1 = std::current_exception(); } }
    if (x0) std::rethrow_exception(x0); if (x1) std::rethrow_exception(x1); }
// splits the sample range [0, n) across threads; every output sample depends only on the input, so the result is the same as one loop.
// Threads: half of PARTTHREADS (or of the logical processors), since the two channels run at once. A chunk whose thread cannot be started
// runs here; an exception in any chunk is re-thrown here after all have finished.
template <class F> static void parFor(long n, F f) {
    const char* pt = getenv("PARTTHREADS"); unsigned hc = pt ? (unsigned)std::max(1, atoi(pt)) : std::thread::hardware_concurrency();
    int th = (int)std::max(1u, hc / 2); if (th <= 1 || n < 65536) { f(0L, n); return; }
    std::vector<std::thread> pool; std::vector<std::exception_ptr> xs(th); long step = (n + th - 1) / th; std::vector<int> inline_;
    for (int k = 1; k < th; k++) { long a = k * step, b = std::min(n, a + step); if (a >= b) continue;
        try { pool.emplace_back([&f, &xs, k, a, b]() { try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }); } catch (...) { inline_.push_back(k); } }
    try { f(0L, std::min(n, step)); } catch (...) { xs[0] = std::current_exception(); }
    for (int k : inline_) { long a = k * step, b = std::min(n, a + step); try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }
    for (auto& t : pool) t.join(); for (auto& x : xs) if (x) std::rethrow_exception(x); }
static vector<double> up2(const vector<double>& x, int s) {   // same tables as the decoder's up-conversion, evaluated in double (encoder-side target only)
    int half; const int32_t* h = tab(s, half); long n = (long)x.size(); vector<double> y(2 * n);
    parFor(2 * n, [&](long i0, long i1) { for (long i = i0; i < i1; i++) {
        if (i % 2 == 0) { y[i] = x[i / 2]; continue; }
        double a = 0; for (int j = 1, t = 0; j <= half; j += 2, t++) { long p = (i - j) / 2, q = (i + j) / 2; double hv = 2.0 * h[t] / 2147483648.0;
            long pp = p < 0 ? -p : p, qq = q < n ? q : 2 * (n - 1) - q; pp = min(max(pp, 0L), n - 1); qq = min(max(qq, 0L), n - 1);
            if (i - j < 0) pp = min(max((j - i) / 2, 0L), n - 1); a += hv * x[pp] + hv * x[qq]; }
        y[i] = a; } });
    return y; }
using fftlib::fft;
int main(int argc, char** argv) {
    double rate = atof(argv[2]);
    auto rd32 = [](const char* p, long& n) { vector<int32_t> v = encio::readAll<int32_t>(p); n = encio::count(v.size()) / 2; v.resize((size_t)n * 2); return v; };
    long L, Lb, Lb2; vector<int32_t> mr = rd32(argv[1], L), b24 = rd32(argv[4], Lb2);
    vector<int16_t> b16 = encio::readAll<int16_t>(argv[3]); Lb = encio::count(b16.size()) / 2; b16.resize((size_t)Lb * 2);
    double baseRate = fmod(rate, 44100.0) == 0 ? 44100.0 : 48000.0; int stages = (int)round(log2(rate / baseRate));
    // dither regeneration (same seeds and draw order as base_layer / decoder)
    vector<char> df; long nbk = 0; if (getenv("DITHERFLAGS")) { df = encio::readAll<char>(getenv("DITHERFLAGS")); nbk = encio::count(df.size()) / 2; }
    vector<double> v[2]; for (int c = 0; c < 2; c++) v[c].assign(Lb, 0.0);
    { uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u};
      for (long i = 0; i < Lb; i++) for (int c = 0; c < 2; c++) { uint32_t& s = st[c]; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
          double d = (!df.empty() && df[c * nbk + i / 4096]) ? (u1 + u2 - 4096) / 16.0 : 0.0; v[c][i] = 256.0 * b16[i * 2 + c] - d; } }
    // cut-off: first frequency >= 15 kHz where the CD path's transfer (cross-spectrum, robust to the CD's own noise) drops below -0.1 dB
    double fc = argc > 7 ? atof(argv[7]) : 0;
    // the CD path at the master rate: needed only to find the cut-off (not when it is given, as in the second split after leak compensation)
    vector<double> uv[2]; if (fc <= 0) { auto upc = [&](int c) { uv[c] = v[c]; for (int s = 0; s < stages; s++) uv[c] = up2(uv[c], s); uv[c].resize(L, 0.0); }; twoWay(upc); }
    if (fc <= 0) { const int M = 16384; vector<double> sxy(M / 2, 0), sxx(M / 2, 0), syy(M / 2, 0), sxyi(M / 2, 0);
        for (long st0 = 0; st0 + M <= L; st0 += M) for (int c = 0; c < 2; c++) { vector<complex<double>> a(M), b(M);
            for (int i = 0; i < M; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / M); a[i] = (mr[(st0 + i) * 2 + c] >> 8) * w; b[i] = uv[c][st0 + i] * w; }
            fft(a); fft(b); for (int k = 0; k < M / 2; k++) { sxy[k] += real(b[k] * conj(a[k])); sxyi[k] += imag(b[k] * conj(a[k])); sxx[k] += norm(a[k]); syy[k] += norm(b[k]); } }
        // scan down from the base Nyquist: the cut-off is just above the highest frequency where the CD still tracks the master
        // (transfer >= -0.1 dB); bins where the master has almost no energy are skipped (no evidence there)
        int k15 = (int)(15000.0 / (rate / M)), kN = (int)(baseRate / 2 / (rate / M)) - 9; double mxx = 0; for (int k = k15; k <= kN; k++) mxx = max(mxx, sxx[k]);
        fc = 15000;
        const bool indep = getenv("CDEXT") != nullptr;   // independent CD: its level / EQ may differ, so judge by coherence (gain-independent), not by the transfer
        for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += sxy[k + d]; xx += sxx[k + d]; yy += syy[k + d]; }
            if (indep) { if (xx < 1e-9 * mxx * 17) continue; if (h > 0 && h * h >= 0.98 * xx * yy) { fc = (k + 1) * rate / M; break; } continue; }
            if (xx < 1e-9 * mxx * 17) continue; if (h / xx >= 0.9886) { fc = (k + 1) * rate / M; break; } }
        if (indep) {   // diagnostic only (printed, never used): the same scan with the per-bin magnitude of the cross-spectrum, which a time offset does not cancel
            double fm = 15000; for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += sqrt(sxy[k + d] * sxy[k + d] + sxyi[k + d] * sxyi[k + d]); xx += sxx[k + d]; yy += syy[k + d]; }
                if (xx < 1e-9 * mxx * 17) continue; if (h * h >= 0.98 * xx * yy) { fm = (k + 1) * rate / M; break; } }
            printf("CD cut-off by magnitude coherence (diagnostic only): %.0f Hz\n", fm);
            if (fabs(fm - fc) > 500 || fc <= 15000) printf("WARNING: the cut-off of the independent CD looks wrong (%.0f Hz, magnitude coherence %.0f Hz): CD and master may be offset in time\n", fc, fm); } }
    for (auto& u : uv) vector<double>().swap(u);   // not needed any more (memory)
    printf("CD cut-off: %.0f Hz\n", fc);
    if (getenv("CUTOFFOUT")) { char t[64]; snprintf(t, sizeof t, "%.0f\n", fc); encio::writeText(getenv("CUTOFFOUT"), t); }
    // T = lowpass_fc(base24 - v), windowed-sinc (Kaiser beta 9, 511 taps, zero phase) at the base rate
    const int NT = 255; vector<double> h(2 * NT + 1); double bes0 = 0; { auto I0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }; bes0 = I0(9.0);
        for (int i = -NT; i <= NT; i++) { double r = (double)i / NT; double w = I0(9.0 * sqrt(max(0.0, 1 - r * r))) / bes0; double x = 2 * fc / baseRate; h[i + NT] = (i == 0 ? x : sin(M_PI * x * i) / (M_PI * i)) * w; } }
    bool full = fc >= baseRate / 2 - 1;   // CD complete to Nyquist: no lowpass needed
    vector<int32_t> leak; if (getenv("LEAKIN")) { leak = encio::readAll<int32_t>(getenv("LEAKIN")); if (leak.empty()) encio::fail("leak compensation: empty leak file"); }   // required when asked for: a missing leak must not silently skip the compensation
    vector<int32_t> Tout(Lb * 2), Fout(L * 2);
    auto chanTF = [&](int c) { vector<double> r(Lb), T(Lb);   // one channel; the two channels are independent and run in parallel
        for (long i = 0; i < Lb; i++) r[i] = b24[i * 2 + c] - v[c][i];   // base24 file: plain 24-bit units (may exceed full scale)
        if (!leak.empty()) {   // leak compensation: the depth layer cancels the frequency layer's leak into the CD band
            // only the part BELOW the frequency layer's start (cut-off - 1 kHz, minus a 150 Hz margin: fc - 1150 Hz): above it the frequency layer codes real
            // content, which must not be subtracted a second time
            const int NL = 1023; vector<double> lk(Lb, 0.0), hl(2 * NL + 1); double xl = 2 * (fc - 1150.0) / baseRate; double b0 = 0;   // 2047 taps: ~120 Hz transition, cut 150 Hz below the start
            { auto I0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }; b0 = I0(9.0);
              for (int k = -NL; k <= NL; k++) { double rr = (double)k / NL; double w = I0(9.0 * sqrt(max(0.0, 1 - rr * rr))) / b0; hl[k + NL] = (k == 0 ? xl : sin(M_PI * xl * k) / (M_PI * k)) * w; } }
            for (long i = 0; i < Lb && i < (long)leak.size() / 2; i++) lk[i] = leak[i * 2 + c] / 16.0;
            parFor(Lb, [&](long i0, long i1) { for (long i = i0; i < i1; i++) { double a = 0; for (int k = -NL; k <= NL; k++) { long j = i + k; j = j < 0 ? -j : (j >= Lb ? 2 * (Lb - 1) - j : j); a += hl[k + NL] * lk[j]; } r[i] -= a; } }); }
        parFor(Lb, [&](long i0, long i1) { for (long i = i0; i < i1; i++) { if (full) { T[i] = llround(r[i]); continue; } double a = 0; for (int k = -NT; k <= NT; k++) { long j = i + k; j = j < 0 ? -j : (j >= Lb ? 2 * (Lb - 1) - j : j); a += h[k + NT] * r[j]; } T[i] = llround(a); } });
        for (long i = 0; i < Lb; i++) Tout[i * 2 + c] = (int32_t)max(-8388608.0, min(8388607.0, T[i])) * 256;
        vector<double> u = v[c]; for (long i = 0; i < Lb; i++) u[i] += T[i]; for (int s = 0; s < stages; s++) u = up2(u, s); u.resize(L, 0.0);
        for (long i = 0; i < L; i++) { long long d = (long long)(mr[i * 2 + c] >> 8) - llround(u[i]); d = max(-8388608LL, min(8388607LL, d)); Fout[i * 2 + c] = (int32_t)(d * 256); } };
    twoWay(chanTF);   // same arithmetic per channel as a sequential loop: output byte-identical
    encio::writeVec(argv[5], Tout);
    encio::writeVec(argv[6], Fout);
    return 0; }
