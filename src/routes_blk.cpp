// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise routes.cpp (2026-10-29): the same T and F, computed one master-rate block at a time. Every filter (up-conversion stages,
// the T lowpass, the leak lowpass) is evaluated over the window its block needs, with the same mirrored edges and the same arithmetic
// per output sample; the cut-off's cross-spectrum is added up segment by segment in the same order. The regenerated dither goes to a
// temporary file next to T (the random numbers are drawn once, in order).
// usage and environment as routes.cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <thread>
#include <exception>
#include <complex>
#include <functional>
#include <memory>
#include <string>
#include "fft.h"
#include <algorithm>
#include "halfband.h"
#include "blk_io.h"
using namespace std; using namespace lae;
typedef blkio::Win<double> WinD;
static const int32_t* tab(int s, int& half) { switch (s) { case 0: half = kHalfLen0; return kHalf0; case 1: half = kHalfLen1; return kHalf1; case 2: half = kHalfLen2; return kHalf2; default: half = kHalfLen3; return kHalf3; } }
// as routes.cpp
template <class F> static void twoWay(F f) {
    std::exception_ptr x1; std::thread t; bool started = false; const char* pt = getenv("PARTTHREADS");
    if (!(pt && atoi(pt) == 1)) try { t = std::thread([&]() { try { f(1); } catch (...) { x1 = std::current_exception(); } }); started = true; } catch (...) {}
    std::exception_ptr x0; try { f(0); } catch (...) { x0 = std::current_exception(); }
    if (started) t.join(); else { try { f(1); } catch (...) { x1 = std::current_exception(); } }
    if (x0) std::rethrow_exception(x0); if (x1) std::rethrow_exception(x1); }
template <class F> static void parFor(long n, F f) {
    const char* pt = getenv("PARTTHREADS"); unsigned hc = pt ? (unsigned)std::max(1, atoi(pt)) : std::thread::hardware_concurrency();
    int th = (int)std::max(1u, hc / 2); if (th <= 1 || n < 65536) { f(0L, n); return; }
    std::vector<std::thread> pool; std::vector<std::exception_ptr> xs(th); long step = (n + th - 1) / th; std::vector<int> inline_;
    for (int k = 1; k < th; k++) { long a = k * step, b = std::min(n, a + step); if (a >= b) continue;
        try { pool.emplace_back([&f, &xs, k, a, b]() { try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }); } catch (...) { inline_.push_back(k); } }
    try { f(0L, std::min(n, step)); } catch (...) { xs[0] = std::current_exception(); }
    for (int k : inline_) { long a = k * step, b = std::min(n, a + step); try { f(a, b); } catch (...) { xs[k] = std::current_exception(); } }
    for (auto& t : pool) t.join(); for (auto& x : xs) if (x) std::rethrow_exception(x); }
// level k of the up-conversion (level 0 = the base-rate signal from p0, level k has Lb << k samples) over [a, b); up2() of routes.cpp per stage
static WinD upEval(int k, long a, long b, long Lb, const std::function<WinD(long, long)>& p0) {
    if (k == 0) return p0(a, b);
    const int s = k - 1; int half; const int32_t* h = tab(s, half); const long n = Lb << s;
    long lo, hi; blkio::cover((a - half) / 2 - 2, (b - 1 + half) / 2 + 1, n, lo, hi);
    const WinD x = upEval(k - 1, lo, hi + 1, Lb, p0);
    WinD y; y.base = a; y.d.resize((size_t)(b - a));
    parFor(b - a, [&](long i0, long i1) { for (long i = a + i0; i < a + i1; i++) {
        if (i % 2 == 0) { y.d[(size_t)(i - a)] = x.at(i / 2); continue; }
        double acc = 0; for (int j = 1, t = 0; j <= half; j += 2, t++) { long p = (i - j) / 2, q = (i + j) / 2; double hv = 2.0 * h[t] / 2147483648.0;
            long pp = p < 0 ? -p : p, qq = q < n ? q : 2 * (n - 1) - q; pp = min(max(pp, 0L), n - 1); qq = min(max(qq, 0L), n - 1);
            if (i - j < 0) pp = min(max((j - i) / 2, 0L), n - 1); acc += hv * x.at(pp) + hv * x.at(qq); }
        y.d[(size_t)(i - a)] = acc; } });
    return y; }
using fftlib::fft;
int main(int argc, char** argv) {
    double rate = atof(argv[2]);
    blkio::Reader<int32_t, 2> mr(argv[1]), b24(argv[4]); blkio::Reader<int16_t, 2> b16(argv[3]);
    const long L = mr.frames, Lb = b16.frames;
    double baseRate = fmod(rate, 44100.0) == 0 ? 44100.0 : 48000.0; int stages = (int)round(log2(rate / baseRate));
    // dither regeneration (same seeds and draw order as base_layer / decoder), stored as the integer before the division by 16
    vector<char> df; long nbk = 0; if (getenv("DITHERFLAGS")) { df = encio::readAll<char>(getenv("DITHERFLAGS")); nbk = encio::count(df.size()) / 2; }
    const std::string dvPath = std::string(argv[5]) + ".dv";
    { blkio::Writer dv(dvPath.c_str()); uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u}; vector<int16_t> buf; const long BS = 1L << 18;
      for (long a = 0; a < Lb; a += BS) { const long b = min(Lb, a + BS); buf.resize((size_t)(b - a) * 2);
          for (long i = a; i < b; i++) for (int c = 0; c < 2; c++) { uint32_t& s = st[c]; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
              buf[(size_t)(i - a) * 2 + c] = (int16_t)((!df.empty() && df[c * nbk + i / 4096]) ? (u1 + u2 - 4096) : 0); }
          dv.put(buf); }
      dv.finish(); }
    struct DvGone { std::string p; ~DvGone() { remove(p.c_str()); } } dvGone{dvPath};
    blkio::Reader<int16_t, 2> dvr(dvPath.c_str());
    auto vWin = [&](long a, long b, int c) { WinD w; w.base = a; w.d.resize((size_t)(b - a)); vector<int16_t> x, d; b16.read(a, b, x); dvr.read(a, b, d);
        for (long i = a; i < b; i++) w.d[(size_t)(i - a)] = 256.0 * x[(size_t)(i - a) * 2 + c] - d[(size_t)(i - a) * 2 + c] / 16.0; return w; };
    const long Lu = Lb << stages;   // the up-converted signal; resized to L (zeros beyond)
    const int M = 16384; const long BM = 64L * M;   // master samples per block (whole cross-spectrum segments, a multiple of 2^stages)
    double fc = argc > 7 ? atof(argv[7]) : 0;
    if (fc <= 0) { vector<double> sxy(M / 2, 0), sxx(M / 2, 0), syy(M / 2, 0), sxyi(M / 2, 0);
        for (long A = 0; A < L; A += BM) { const long B = min(L, A + BM); if (A + M > L) break;
            WinD uv[2]; auto upc = [&](int c) { uv[c].base = A; uv[c].d.assign((size_t)(B - A), 0.0); const long e = min(B, Lu); if (e <= A) return;
                WinD w = upEval(stages, A, e, Lb, [&](long a, long b) { return vWin(a, b, c); }); std::copy(w.d.begin(), w.d.end(), uv[c].d.begin()); };
            twoWay(upc); vector<int32_t> m; mr.read(A, B, m);
            for (long st0 = A; st0 < B && st0 + M <= L; st0 += M) for (int c = 0; c < 2; c++) { vector<complex<double>> a(M), b(M);
                for (int i = 0; i < M; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / M); a[i] = (m[(size_t)(st0 - A + i) * 2 + c] >> 8) * w; b[i] = uv[c].at(st0 + i) * w; }
                fft(a); fft(b); for (int k = 0; k < M / 2; k++) { sxy[k] += real(b[k] * conj(a[k])); sxyi[k] += imag(b[k] * conj(a[k])); sxx[k] += norm(a[k]); syy[k] += norm(b[k]); } } }
        int k15 = (int)(15000.0 / (rate / M)), kN = (int)(baseRate / 2 / (rate / M)) - 9; double mxx = 0; for (int k = k15; k <= kN; k++) mxx = max(mxx, sxx[k]);
        fc = 15000;
        const bool indep = getenv("CDEXT") != nullptr;
        for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += sxy[k + d]; xx += sxx[k + d]; yy += syy[k + d]; }
            if (indep) { if (xx < 1e-9 * mxx * 17) continue; if (h > 0 && h * h >= 0.98 * xx * yy) { fc = (k + 1) * rate / M; break; } continue; }
            if (xx < 1e-9 * mxx * 17) continue; if (h / xx >= 0.9886) { fc = (k + 1) * rate / M; break; } }
        if (indep) {
            double fm = 15000; for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += sqrt(sxy[k + d] * sxy[k + d] + sxyi[k + d] * sxyi[k + d]); xx += sxx[k + d]; yy += syy[k + d]; }
                if (xx < 1e-9 * mxx * 17) continue; if (h * h >= 0.98 * xx * yy) { fm = (k + 1) * rate / M; break; } }
            printf("CD cut-off by magnitude coherence (diagnostic only): %.0f Hz\n", fm);
            if (fabs(fm - fc) > 500 || fc <= 15000) printf("WARNING: the cut-off of the independent CD looks wrong (%.0f Hz, magnitude coherence %.0f Hz): CD and master may be offset in time\n", fc, fm); } }
    printf("CD cut-off: %.0f Hz\n", fc);
    if (getenv("CUTOFFOUT")) { char t[64]; snprintf(t, sizeof t, "%.0f\n", fc); encio::writeText(getenv("CUTOFFOUT"), t); }
    const int NT = 255; vector<double> h(2 * NT + 1); double bes0 = 0; { auto I0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }; bes0 = I0(9.0);
        for (int i = -NT; i <= NT; i++) { double r = (double)i / NT; double w = I0(9.0 * sqrt(max(0.0, 1 - r * r))) / bes0; double x = 2 * fc / baseRate; h[i + NT] = (i == 0 ? x : sin(M_PI * x * i) / (M_PI * i)) * w; } }
    bool full = fc >= baseRate / 2 - 1;
    std::unique_ptr<blkio::Reader<int32_t, 2>> leak; if (getenv("LEAKIN")) { leak.reset(new blkio::Reader<int32_t, 2>(getenv("LEAKIN"))); if (leak->frames == 0) encio::fail("leak compensation: empty leak file"); }
    const int NL = 1023; vector<double> hl(2 * NL + 1); double xl = 2 * (fc - 1150.0) / baseRate;
    { auto I0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }; double b0 = I0(9.0);
      for (int k = -NL; k <= NL; k++) { double rr = (double)k / NL; double w = I0(9.0 * sqrt(max(0.0, 1 - rr * rr))) / b0; hl[k + NL] = (k == 0 ? xl : sin(M_PI * xl * k) / (M_PI * k)) * w; } }
    auto mir = [&](long j) { return j < 0 ? -j : (j >= Lb ? 2 * (Lb - 1) - j : j); };
    // r = base24 - v, minus the leak's part below the frequency layer's start (as routes.cpp)
    auto rWin = [&](long a, long b, int c) { WinD w = vWin(a, b, c); vector<int32_t> x; b24.read(a, b, x);
        for (long i = a; i < b; i++) w.d[(size_t)(i - a)] = x[(size_t)(i - a) * 2 + c] - w.d[(size_t)(i - a)];
        if (leak) { long lo, hi; blkio::cover(a - NL, b - 1 + NL, Lb, lo, hi); WinD lk; lk.base = lo; lk.d.assign((size_t)(hi + 1 - lo), 0.0);
            const long e = min(hi + 1, leak->frames); if (e > lo) { vector<int32_t> l; leak->read(lo, e, l); for (long i = lo; i < e; i++) lk.d[(size_t)(i - lo)] = l[(size_t)(i - lo) * 2 + c] / 16.0; }
            parFor(b - a, [&](long i0, long i1) { for (long i = a + i0; i < a + i1; i++) { double acc = 0; for (int k = -NL; k <= NL; k++) acc += hl[k + NL] * lk.at(mir(i + k)); w.d[(size_t)(i - a)] -= acc; } }); }
        return w; };
    auto tWin = [&](long a, long b, int c) { WinD T; T.base = a; T.d.resize((size_t)(b - a));
        if (full) { WinD r = rWin(a, b, c); for (long i = a; i < b; i++) T.d[(size_t)(i - a)] = llround(r.d[(size_t)(i - a)]); return T; }
        long lo, hi; blkio::cover(a - NT, b - 1 + NT, Lb, lo, hi); const WinD r = rWin(lo, hi + 1, c);
        parFor(b - a, [&](long i0, long i1) { for (long i = a + i0; i < a + i1; i++) { double acc = 0; for (int k = -NT; k <= NT; k++) acc += h[k + NT] * r.at(mir(i + k)); T.d[(size_t)(i - a)] = llround(acc); } });
        return T; };
    blkio::Writer Tf(argv[5]), Ff(argv[6]); vector<int32_t> Tout, Fout, m;
    for (long A = 0; A < L; A += BM) { const long B = min(L, A + BM); const long ta = A >> stages, tb = B == L ? Lb : (B >> stages);
        Tout.assign((size_t)(max(0L, tb - ta)) * 2, 0); Fout.assign((size_t)(B - A) * 2, 0); mr.read(A, B, m);
        auto chanTF = [&](int c) { WinD Tw; bool haveT = false;   // the T window the up-conversion asked for (it covers this block's T samples)
            auto p0 = [&](long a, long b) { WinD T = tWin(a, b, c); WinD u = vWin(a, b, c); for (long i = a; i < b; i++) u.d[(size_t)(i - a)] += T.d[(size_t)(i - a)]; Tw = std::move(T); haveT = true; return u; };
            const long e = min(B, Lu); WinD u; if (e > A) u = upEval(stages, A, e, Lb, p0);
            if (tb > ta) { if (!haveT || Tw.base > ta || Tw.base + (long)Tw.d.size() < tb) Tw = tWin(ta, tb, c);
                for (long i = ta; i < tb; i++) Tout[(size_t)(i - ta) * 2 + c] = (int32_t)max(-8388608.0, min(8388607.0, Tw.at(i))) * 256; }
            for (long i = A; i < B; i++) { const double ui = i < e ? u.at(i) : 0.0; long long d = (long long)(m[(size_t)(i - A) * 2 + c] >> 8) - llround(ui); d = max(-8388608LL, min(8388607LL, d)); Fout[(size_t)(i - A) * 2 + c] = (int32_t)(d * 256); } };
        twoWay(chanTF);
        Tf.put(Tout); Ff.put(Fout); }
    Tf.finish(); Ff.finish();
    return 0; }
