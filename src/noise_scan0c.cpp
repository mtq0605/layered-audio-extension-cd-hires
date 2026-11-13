// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// The LAE Noise Marker scanner (2026-11-04): read-only identification. Writes nothing next to the input. Not encoder code.
// Stationary noise (no comparison with a floor or noise spectrum; no left/right test):
//   grid     the encoder's MDCT (mdct_tb.h, N = 512) of M = r + ((l - r) >> 1) and S = l - r, 24-bit units
//   block    a fixed number of 512 coefficients: 4 columns (of 8 bins) x 16 frames at 44.1/48 kHz, 2 x 32 at 88.2/96, 1 x 64 above
//   S1       evenness: variance / mean^2 of the frame energies of the block, two-sided (too even = a held tone, too uneven = music)
//   S2       Gaussianity: kurtosis of all coefficients of the block, two-sided
//   S3       no outstanding bin: largest upward residual of ln(per-bin variance) after a quadratic fit across the block's bins
//            (the same fit gives the smooth per-bin variances that S1 and S2 normalise by: only the block's own data)
//   thresholds  from 1,000,000 simulated pure-noise blocks (iid Gaussian coefficients = the orthonormal MDCT of white Gaussian
//            noise): one tail probability for all five tails, chosen so that a block fails the three tests together with 0.1 %;
//            "heavy" failure = the same at 0.001 %
//   long     10-s windows (user): at most 2 failed blocks, none heavy, no two failures in a row, no exact-zero block; then the
//            energies of the passing blocks, detrended by a straight line in ln, must be as even as stationary noise (residual
//            variance against trigamma(nu/2), two-sided chi-square at 0.1 %). A slope that is significant and >= 0.05 dB/s marks the
//            window "drifting" (label only). A failed block whose neighbouring column fails at the same time is a run: window fails.
//   edges    overlapping blocks (offsets T/4, T/2, 3T/4) trim the edges of the candidate regions: a failing offset block that
//            reaches outside a region removes its frames from the region (column "trim" = share of region frames removed)
// Narrow lines (user: every line that stays fixed within 10 s; bursts not handled): per 10-s window and bin, coherent
// integration of the STFT values (hop = window, Hann): Q = max over frequency of |sum X_t e^{-i w t}|^2 / sum |X_t|^2. Pure noise
// gives P(Q > x) ~ n e^{-x} (n frames = trial frequencies per bin); the threshold makes the expected number of false lines per file
// 0.01. Only the bin's own values are used. Two resolutions, M and S. Lines are linked across windows into time spans; a strong
// line's coherent leakage into neighbouring bins is removed.
// Output (no labels): one candidate list of everything detected (stationary-noise regions and lines), each with
// plane, start, end, length, frequency range and level, sorted by start time and then frequency. The user chooses.
// usage: lae_noise scan <in.wav> <out.txt> [candidates.laenoise]   (the candidate file has the marking format of noise_marks.h)
//        (2026-11-06: part of lae_noise, which includes this file with NOISE_SCAN_NO_MAIN; built alone it is noise_scan0c, same output)
#define NOISE_MEASURE_NO_MAIN
#include "noise_measure.cpp"
#include "mdct_tb.h"
#include "noise_io.h"
#include "noise_marks.h"   // candidate file in the marking format (cell coordinates, geometry only)
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
#include "lae_math.h"   // the same results on every platform (2026-11-06): no C-library log / exp / sin / cos / pow below
#include <map>
#include <thread>
#include <atomic>
#include <functional>
using mdct::i32;
// body(a, b) for [a, b) of [0, n), split over all cores (2026-11-13); used only where every index is computed independently
static void parFor(long n, const std::function<void(long, long)>& body) {
    unsigned nt = std::thread::hardware_concurrency(); if (nt == 0) nt = 1; if (nt > 64) nt = 64; if ((long)nt > n) nt = (unsigned)max(1L, n);
    vector<std::thread> th; long chunk = (n + nt - 1) / nt;
    for (unsigned k = 0; k < nt; k++) { long a = (long)k * chunk, b = min(n, a + chunk); if (a >= b) break; th.emplace_back(body, a, b); }
    for (auto& x : th) x.join(); }
// natural log and exp from lae_math.h; |z|^2 written out (libstdc++'s norm goes through hypot)
static double lnx(double x) { return lmath::log2(x) * 0.69314718055994530942; }   // ln 2
static double expx(double x) { return lmath::exp2(x * 1.4426950408889634074); }   // log2 e
static double nrm(const complex<double>& z) { return z.real() * z.real() + z.imag() * z.imag(); }

static double trigamma(double x) { double r = 0; while (x < 6) { r += 1 / (x * x); x += 1; }
    double x2 = 1 / (x * x); return r + 1 / x + x2 / 2 + (x2 / x) * (1.0 / 6 - x2 * (1.0 / 30 - x2 / 42)); }

struct BStat { double V, K, R; };
// x: T frames x B bins (row = frame). Fills the three statistics; returns false for an all-zero block.
static bool blockStats(const double* x, int T, int B, BStat& st, double& energy, double& sumVar, double& sumVar2) {
    vector<double> s(B, 0.0); energy = 0;
    for (int t = 0; t < T; t++) for (int k = 0; k < B; k++) { double v = x[t * B + k]; s[k] += v * v; }
    for (int k = 0; k < B; k++) { energy += s[k]; s[k] /= T; }
    sumVar = 0; sumVar2 = 0; for (int k = 0; k < B; k++) { sumVar += s[k]; sumVar2 += s[k] * s[k]; }
    if (energy <= 0) return false;
    // quadratic fit of ln s_k over k (bins with s_k = 0 get the smallest positive value of the block)
    double smin = 1e300; for (int k = 0; k < B; k++) if (s[k] > 0) smin = min(smin, s[k]);
    vector<double> y(B), fit(B); double kc = (B - 1) / 2.0;
    for (int k = 0; k < B; k++) y[k] = lnx(max(s[k], smin));
    double S0 = B, S1 = 0, S2 = 0, S3 = 0, S4 = 0, Y0 = 0, Y1 = 0, Y2 = 0;
    for (int k = 0; k < B; k++) { double u = k - kc, u2 = u * u; S1 += u; S2 += u2; S3 += u2 * u; S4 += u2 * u2; Y0 += y[k]; Y1 += u * y[k]; Y2 += u2 * y[k]; }
    // solve [S0 S1 S2; S1 S2 S3; S2 S3 S4] [a b c] = [Y0 Y1 Y2] (Cramer)
    auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); };
    double D = det3(S0, S1, S2, S1, S2, S3, S2, S3, S4); double a = 0, b = 0, c = 0;
    if (fabs(D) > 1e-12) { a = det3(Y0, S1, S2, Y1, S2, S3, Y2, S3, S4) / D; b = det3(S0, Y0, S2, S1, Y1, S3, S2, Y2, S4) / D; c = det3(S0, S1, Y0, S1, S2, Y1, S2, S3, Y2) / D; }
    else a = Y0 / B;
    double R = -1e300; for (int k = 0; k < B; k++) { double u = k - kc; fit[k] = a + b * u + c * u * u; R = max(R, y[k] - fit[k]); }
    // S1, S2 on coefficients normalised by the fitted variances
    vector<double> e(T, 0.0); double z2 = 0, z4 = 0;
    for (int t = 0; t < T; t++) for (int k = 0; k < B; k++) { double z = x[t * B + k] / sqrt(expx(fit[k])); double q = z * z; e[t] += q; z2 += q; z4 += q * q; }
    double m = 0, v = 0; for (double q : e) m += q; m /= T; for (double q : e) v += (q - m) * (q - m); v /= (T - 1);
    st.V = (m > 0) ? v / (m * m) * (B / 2.0) : 0; st.K = z2 > 0 ? (double)(T * B) * z4 / (z2 * z2) : 0; st.R = R; return true;
}
// thresholds for one block shape from simulated iid Gaussian blocks
struct Thr { double vLo, vHi, kLo, kHi, rHi; };
struct Calib { Thr pass, heavy; double pPass, pHeavy, rejPass, rejHeavy; };
// Speed (2026-11-13): the 1,000,000 blocks took about 60 s of every scan, whatever the file length. They are simulated on all
// cores now. All blocks share one xorshift stream, block i using its draws [i D, (i + 1) D); the stream is first stepped in order
// to note its state at the start of every chunk of blocks, then each chunk runs from that state with the code of one thread. So
// every block sees the same draws, and every statistic, threshold and output byte is the same as with one thread.
static Calib calibrate(int T, int B) {
    const long NS = 1000000; vector<double> V(NS), K(NS), R(NS); const size_t nx = (size_t)T * B;
    auto step = [](uint64_t& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; };
    const long D = (long)((nx + 1) / 2) * 2;   // draws per block: two (r, th) for every pair of coefficients
    const long CH = 2048, nch = (NS + CH - 1) / CH; vector<uint64_t> st0((size_t)nch);
    { uint64_t s = 0x9E3779B97F4A7C15ull; for (long c = 0; c < nch; c++) { st0[(size_t)c] = s; const long nd = min(CH, NS - c * CH) * D; for (long d = 0; d < nd; d++) step(s); } }
    parFor(nch, [&](long c0, long c1) { vector<double> x(nx);
        for (long c = c0; c < c1; c++) { uint64_t s = st0[(size_t)c]; auto u01 = [&]() { step(s); return ((s >> 11) + 0.5) * (1.0 / 9007199254740992.0); };
            for (long i = c * CH; i < min(NS, (c + 1) * CH); i++) {
                for (size_t j = 0; j < x.size(); j += 2) { double r = sqrt(-2 * lnx(u01())), th = 2 * u01(); x[j] = r * lmath::cospi(th); if (j + 1 < x.size()) x[j + 1] = r * lmath::sinpi(th); }   // th in half-turns
                BStat st; double en, sv, sv2; blockStats(x.data(), T, B, st, en, sv, sv2); V[i] = st.V; K[i] = st.K; R[i] = st.R; } } });
    vector<double> sV = V, sK = K, sR = R; sort(sV.begin(), sV.end()); sort(sK.begin(), sK.end()); sort(sR.begin(), sR.end());
    auto q = [&](const vector<double>& a, double p) { long i = (long)llround(p * (NS - 1)); i = max(0L, min(NS - 1, i)); return a[i]; };
    auto thrOf = [&](double p) { return Thr{q(sV, p), q(sV, 1 - p), q(sK, p), q(sK, 1 - p), q(sR, 1 - p)}; };
    auto rej = [&](const Thr& t) { std::atomic<long> n{0};   // an integer count: the same whatever the split
        parFor(NS, [&](long a, long b) { long m = 0; for (long i = a; i < b; i++) m += (V[i] < t.vLo || V[i] > t.vHi || K[i] < t.kLo || K[i] > t.kHi || R[i] > t.rHi); n += m; });
        return (double)n.load() / NS; };
    auto solve = [&](double target, double& pOut, double& rOut) { double lo = target / 20, hi = target; Thr t{};
        for (int it = 0; it < 30; it++) { double mid = sqrt(lo * hi); t = thrOf(mid); double r = rej(t); if (r > target) hi = mid; else lo = mid; }
        pOut = lo; t = thrOf(lo); rOut = rej(t); return t; };
    Calib c; c.pass = solve(1e-3, c.pPass, c.rejPass); c.heavy = solve(1e-5, c.pHeavy, c.rejHeavy); return c;
}
static bool inside(const BStat& s, const Thr& t) { return s.V >= t.vLo && s.V <= t.vHi && s.K >= t.kLo && s.K <= t.kHi && s.R <= t.rHi; }

// ---- block-wise input (2026-11-04: memory like the encoder): WavStream in noise_io.h
// MDCT frames [fr0, fr1) of plane 0 = M = r + ((l - r) >> 1) or 1 = S = l - r, bit-identical to mdct::forward on the whole plane
static void planeFrames(WavStream& ws, int plane, long fr0, long fr1, vector<i32>& Xc) {
    const int N = mdct::N; long long s0 = (long long)fr0 * N - N, ns = (long long)(fr1 - fr0) * N + N; vector<double> L, R; ws.read(s0, ns, L, R);
    vector<i32> p((size_t)ns); for (long long i = 0; i < ns; i++) { long long g = s0 + i; if (g < 0 || g >= ws.frames) { p[(size_t)i] = 0; continue; }
        i32 l = (i32)llround(L[(size_t)i] * 8388608.0), r = (i32)llround(R[(size_t)i] * 8388608.0); p[(size_t)i] = plane ? l - r : r + ((l - r) >> 1); }
    Xc.assign((size_t)(fr1 - fr0) * N, 0); double z[2 * mdct::N], u[mdct::N], c[mdct::N];
    for (long m = fr0; m < fr1; m++) { const i32* q = p.data() + (size_t)(m - fr0) * N;   // q[n] = plane sample (m N + n - N)
        for (int n = 0; n < 2 * N; n++) z[n] = (double)q[n] * mdct::WIN[n];
        for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }
        mdct::dct4(u, c); for (int k = 0; k < N; k++) Xc[(size_t)(m - fr0) * N + k] = (i32)std::llround(c[k]); } }

// one detected candidate (no labels, everything listed, sorted by position and length)
struct Cand { double t0, t1, f0, f1, lv; string kind, plane; long m0 = 0, m1 = 0; int c0 = 0, c1 = 0; double fLine = 0; };   // m0..m1, c0..c1: encoder cell coordinates (noise_marks.h)

static void stationaryPass(FILE* out, WavStream& ws, int plane, double rate, const char* name, double dbOff, const vector<double>& edges, const Calib& cal, int C, int T, vector<Cand>& cands) {
    const int N = mdct::N, B = 8 * C; long K = mdct::frames((long)ws.frames); double binHz = rate / (2.0 * N);
    long nbk = (K - 2) / T; int ncol = (N / 8) / C;   // blocks start at frame 1: frames touching the zero padding are left out
    double blkSec = T * (double)N / rate; int WL = (int)llround(10.0 / blkSec);
    const double fsPow = 0.5 * 70368744177664.0;
    vector<char> ok((size_t)ncol * nbk), heavy((size_t)ncol * nbk), zero((size_t)ncol * nbk); vector<double> en((size_t)ncol * nbk), sv((size_t)ncol * nbk), sv2((size_t)ncol * nbk);
    vector<long> p1(ncol), p2(ncol), p3(ncol), pAll(ncol), nz(ncol);
    long nfc = nbk * (long)T; vector<vector<char>> offFail(4, vector<char>((size_t)ncol * nbk, 1));   // offset blocks (q = 1..3): failed (or zero)
    const long G = 64; vector<i32> Xc;   // blocks per chunk: the plane is transformed chunk by chunk
    // (2026-11-13) the columns of a chunk on all cores: a column writes only its own entries (its counters, its block ids), from
    // the chunk's coefficients, read only; each thread has its own block buffer x
    for (long b0 = 0; b0 < nbk; b0 += G) { long b1 = min(nbk, b0 + G); long fr0 = 1 + b0 * T, fr1 = min(K, 1 + b1 * T + T); planeFrames(ws, plane, fr0, fr1, Xc);
        parFor(ncol, [&](long colA, long colB) { vector<double> x((size_t)T * B);
        for (int col = (int)colA; col < (int)colB; col++) {
            for (long b = b0; b < b1; b++) {
                for (int t = 0; t < T; t++) for (int k = 0; k < B; k++) x[(size_t)t * B + k] = Xc[(size_t)(1 + b * T + t - fr0) * N + col * B + k];
                BStat st; size_t id = (size_t)col * nbk + b; double e, s1, s2;
                if (!blockStats(x.data(), T, B, st, e, s1, s2)) { zero[id] = 1; continue; }
                en[id] = e; sv[id] = s1; sv2[id] = s2; nz[col]++;
                p1[col] += st.V >= cal.pass.vLo && st.V <= cal.pass.vHi; p2[col] += st.K >= cal.pass.kLo && st.K <= cal.pass.kHi; p3[col] += st.R <= cal.pass.rHi;
                ok[id] = inside(st, cal.pass); heavy[id] = !inside(st, cal.heavy); pAll[col] += ok[id]; }
            for (int q = 1; q <= 3; q++) { long off = (long)q * T / 4;
                for (long b = b0; b < b1; b++) { long s0 = off + b * T; if (s0 + T > nfc) continue;
                    for (int t = 0; t < T; t++) for (int k = 0; k < B; k++) x[(size_t)t * B + k] = Xc[(size_t)(1 + s0 + t - fr0) * N + col * B + k];
                    BStat st; double e, s1, s2; offFail[q][(size_t)col * nbk + b] = !(blockStats(x.data(), T, B, st, e, s1, s2) && inside(st, cal.pass)); } } } }); }
    // long windows
    vector<long> cLong(ncol), cTol(ncol), cDriftWin(ncol), cWin(ncol), cFrBefore(ncol), cFrAfter(ncol); vector<vector<double>> lvL(ncol), slopes(ncol);
    double zW = 3.29;
    for (int col = 0; col < ncol; col++) { vector<char> lng(nbk, 0), tol(nbk, 0);
        for (long w0 = 0; w0 + WL <= nbk; w0++) {
            int F = 0; bool bad = false; long prevFail = -10;
            for (long b = w0; b < w0 + WL && !bad; b++) { size_t id = (size_t)col * nbk + b;
                if (zero[id] || heavy[id]) { bad = true; break; }
                if (!ok[id]) { F++; if (b == prevFail + 1 || F > 2) bad = true; prevFail = b;
                    // a run across columns: the neighbouring column fails at the same time (music fails in runs, chance failures are isolated)
                    for (int nc : {col - 1, col + 1}) if (nc >= 0 && nc < ncol) { size_t nid = (size_t)nc * nbk + b; if (!zero[nid] && !ok[nid]) bad = true; } } }
            if (bad) continue;
            // detrended evenness of the passing blocks' energies
            vector<double> tt, yy; double svS = 0, sv2S = 0;
            for (long b = w0; b < w0 + WL; b++) { size_t id = (size_t)col * nbk + b; if (!ok[id]) continue; tt.push_back((double)(b - w0)); yy.push_back(lnx(en[id])); svS += sv[id]; sv2S += sv2[id]; }
            int n = (int)tt.size(); if (n < 8) continue;
            double nu = T * svS * svS / sv2S / n;   // effective dof of one block from the window's per-bin variances (mean over blocks)
            double mt = 0, my = 0; for (int i = 0; i < n; i++) { mt += tt[i]; my += yy[i]; } mt /= n; my /= n;
            double stt = 0, sty = 0; for (int i = 0; i < n; i++) { stt += (tt[i] - mt) * (tt[i] - mt); sty += (tt[i] - mt) * (yy[i] - my); }
            double sl = sty / stt, rs = 0; for (int i = 0; i < n; i++) { double r = yy[i] - (my + sl * (tt[i] - mt)); rs += r * r; }
            double d = n - 2, ratio = rs / d / trigamma(nu / 2);
            if (ratio < chiq(d, -zW) || ratio > chiq(d, zW)) continue;
            double slDb = sl * 10 / 2.3025850929940456840 / blkSec;   // dB per second
            cWin[col]++; double se = sqrt(trigamma(nu / 2) / stt); bool drift = fabs(sl) > zW * se && fabs(slDb) >= 0.05; if (drift) cDriftWin[col]++;   // label only: significant and >= 0.05 dB/s
            slopes[col].push_back(slDb);
            for (long b = w0; b < w0 + WL; b++) { size_t id = (size_t)col * nbk + b; lng[b] = 1; if (!ok[id]) tol[b] = 1; } }
        for (long b = 0; b < nbk; b++) { size_t id = (size_t)col * nbk + b; if (lng[b]) { cLong[col]++; cTol[col] += tol[b];
                lvL[col].push_back(10 * lmath::log10(max(en[id] / ((double)T * N) / fsPow, 1e-30)) - 10 * lmath::log10(B * binHz / 1000.0) + dbOff); } }
        // edge trimming with overlapping blocks (offsets T/4, T/2, 3T/4): a failing offset block that reaches outside the candidate
        // region takes its frames out of the region's edge, so the edge is exact to a quarter block; failures inside are left alone
        vector<char> mask(nfc, 0); for (long b = 0; b < nbk; b++) if (lng[b]) for (int t = 0; t < T; t++) mask[b * T + t] = 1;
        vector<char> trimmed = mask; long before = 0; for (char c : mask) before += c;
        if (before) for (int q = 1; q <= 3; q++) { long off = (long)q * T / 4;
            for (long s0 = off; s0 + T <= nfc; s0 += T) {
                bool anyIn = false, anyOut = false; for (long f = s0; f < s0 + T; f++) (mask[f] ? anyIn : anyOut) = true;
                if (!(anyIn && anyOut)) continue;
                if (!offFail[q][(size_t)col * nbk + (s0 - off) / T]) continue;
                for (long f = s0; f < s0 + T; f++) trimmed[f] = 0; } }
        long after = 0; for (char c : trimmed) after += c; cFrBefore[col] = before; cFrAfter[col] = after;
        // candidate regions of this column: runs of frames left after trimming (frame f of the plane = MDCT frame 1 + f)
        for (long a = 0; a < nfc; ) { if (!trimmed[a]) { a++; continue; } long e2 = a; while (e2 < nfc && trimmed[e2]) e2++;
            double es = 0; long nb = 0; for (long b = a / T; b <= (e2 - 1) / T; b++) { size_t id = (size_t)col * nbk + b; if (ok[id]) { es += en[id]; nb++; } }
            double lv = nb ? 10 * lmath::log10(max(es / nb / ((double)T * N) / fsPow, 1e-30)) - 10 * lmath::log10(B * binHz / 1000.0) + dbOff : NAN;
            { Cand cd{(1 + a) * (double)N / rate, (1 + e2) * (double)N / rate, col * B * binHz, (col + 1) * B * binHz, lv, "noise", plane ? "S" : "M"}; cd.m0 = 1 + a; cd.m1 = 1 + e2; cd.c0 = col * C; cd.c1 = (col + 1) * C; cands.push_back(cd); } a = e2; } }
    fprintf(out, "[%s] block %d columns x %d frames = %.0f Hz x %.1f ms (%d coefficients), 10-s window = %d blocks, blocks per column %ld\n",
            name, C, T, B * binHz, 1000.0 * blkSec, B * T, WL, nbk);
    fprintf(out, "%-15s %4s %6s %6s %6s %6s %6s %6s %6s %8s %8s %6s\n", "band Hz", "cols", "S1", "S2", "S3", "all", "long", "tolBlk", "drift", "lvLong50", "slope50", "trim");
    for (size_t e = 0; e + 1 < edges.size(); e++) { long n = 0, a1 = 0, a2 = 0, a3 = 0, aa = 0, al = 0, at = 0, dw = 0, ww = 0, fb = 0, fa = 0; vector<double> lv, sl; int cols = 0;
        for (int col = 0; col < ncol; col++) { double fc = (col * B + B / 2.0) * binHz; if (fc < edges[e] || fc >= edges[e + 1]) continue;
            cols++; n += nz[col]; a1 += p1[col]; a2 += p2[col]; a3 += p3[col]; aa += pAll[col]; al += cLong[col]; at += cTol[col]; dw += cDriftWin[col]; ww += cWin[col]; fb += cFrBefore[col]; fa += cFrAfter[col];
            lv.insert(lv.end(), lvL[col].begin(), lvL[col].end()); sl.insert(sl.end(), slopes[col].begin(), slopes[col].end()); }
        if (!cols) continue; if (!n) { fprintf(out, "%6.0f-%-8.0f %4d  (exact zero)\n", edges[e], edges[e + 1], cols); continue; }
        fprintf(out, "%6.0f-%-8.0f %4d %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %8.1f %8.3f %6.3f\n", edges[e], edges[e + 1], cols, (double)a1 / n, (double)a2 / n, (double)a3 / n, (double)aa / n,
                (double)al / n, al ? (double)at / al : 0.0, ww ? (double)dw / ww : 0.0, lv.empty() ? NAN : pct(lv, 0.5), sl.empty() ? NAN : pct(sl, 0.5), fb ? (double)(fb - fa) / fb : 0.0); }
    fprintf(out, "\n");
}

// ---- narrow lines
struct LDet { double f, lv, Q, cv; int win; };
static vector<LDet> lineScan(WavStream& ws, double rate, int W, bool sPlane, double alphaTest) {
    vector<LDet> res; double binHz = rate / W; long nfr = (long)(ws.frames / W); long n = (long)llround(10.0 * rate / W); if (n < 8) return res;
    vector<double> L, R;   // one 10-s window at a time
    long nwin = nfr / n; if (nwin < 1) return res; int nb = W / 2 + 1;
    vector<double> win(W); double sw2 = 0; for (int i = 0; i < W; i++) { win[i] = 0.5 - 0.5 * lmath::cospi(2 * (i + 0.5) / W); sw2 += win[i] * win[i]; }
    const double sc = 2.0 / (W * sw2) / 0.5;
    int P2 = 1; while (P2 < 4 * n) P2 <<= 1;
    double thr = lnx((double)n / alphaTest);
    vector<complex<float>> spec((size_t)n * nb);
    // (2026-11-13) the frames' FFTs, then the bins' coherent integrations, on all cores: each frame / bin is computed alone, by the
    // same code, into its own entries; every thread has its own FFT buffer (fftlib's plan cache is thread_local)
    for (long w = 0; w < nwin; w++) { ws.read((long long)w * n * W, (long long)n * W, L, R);
        parFor(n, [&](long tA, long tB) { vector<complex<double>> a(W);
            for (long t = tA; t < tB; t++) { size_t o = (size_t)t * W;
                for (int i = 0; i < W; i++) { double v = sPlane ? 0.5 * (L[o + i] - R[o + i]) : 0.5 * (L[o + i] + R[o + i]); a[i] = complex<double>(v * win[i], 0); }
                fftlib::fft(a); for (int k = 0; k < nb; k++) spec[(size_t)t * nb + k] = complex<float>(a[k]); } });
        vector<double> Qk(nb, 0.0), dk(nb, 0.0), pk(nb, 0.0), cvk(nb, 0.0);
        parFor(nb - 2, [&](long kA, long kB) { vector<complex<double>> seq(P2);
        for (int k = 1 + (int)kA; k < 1 + (int)kB; k++) { double S = 0; for (long t = 0; t < n; t++) { seq[t] = complex<double>(spec[(size_t)t * nb + k]); S += nrm(seq[t]); }
            if (S <= 0) continue; for (long t = n; t < P2; t++) seq[t] = 0; fftlib::fft(seq);
            int jb = 0; double mx = 0; for (int j = 0; j < P2; j++) { double v = nrm(seq[j]); if (v > mx) { mx = v; jb = j; } }
            Qk[k] = mx / S; double dl = (double)jb / P2; if (dl >= 0.5) dl -= 1; dk[k] = dl;   // X_t ~ e^{+i 2pi dl t}: the phase step per frame is the fractional bin offset
            pk[k] = mx / ((double)n * n) * sc;
            double m = S / n, v = 0; for (long t = 0; t < n; t++) { double q = nrm(complex<double>(spec[(size_t)t * nb + k])); v += (q - m) * (q - m); } cvk[k] = sqrt(v / (n - 1)) / m; } });
        for (int k = 1; k < nb - 1; ) { if (Qk[k] < thr) { k++; continue; } int e = k; while (e + 1 < nb - 1 && Qk[e + 1] >= thr) e++;
            int best = k; for (int q = k; q <= e; q++) if (pk[q] > pk[best]) best = q;
            k = e + 1; double fl = (best + dk[best]) * binHz; if (fl < 20.0) continue;   // below 20 Hz: DC offset leaking into bin 1
            // present through the whole window (user: lines that stay fixed for 10 s): in each quarter the line must be coherent at the
            // same frequency (|Y_q|^2 / S_q >= ln 1000, i.e. 0.1 % per quarter for noise), and the quarters' coherent amplitudes within 6 dB
            { const int NQ = 4; long qn = n / NQ; double ymin = 1e300, ymax = 0; bool okq = true;
              for (int qq = 0; qq < NQ && okq; qq++) { complex<double> Y(0, 0); double Sq = 0;
                  for (long t = qq * qn; t < (qq + 1) * qn; t++) { complex<double> xv(spec[(size_t)t * nb + best]); double ph = -2 * dk[best] * t, cr = lmath::cospi(ph), ci = lmath::sinpi(ph);   // ph in half-turns
                      Y += complex<double>(xv.real() * cr - xv.imag() * ci, xv.real() * ci + xv.imag() * cr); Sq += nrm(xv); }
                  if (Sq <= 0 || nrm(Y) / Sq < 6.9077552789821370521) okq = false; double ay = sqrt(nrm(Y)); ymin = min(ymin, ay); ymax = max(ymax, ay); }   // 6.9077... = ln 1000
              if (!okq || ymax > 2 * ymin) continue; }
            double plv = 0; for (int q = max(1, best - 2); q <= min(nb - 2, best + 2); q++) plv += pk[q];   // main lobe sum
            res.push_back({fl, 10 * lmath::log10(max(plv / 1.5, 1e-30)), Qk[best], cvk[best], (int)w}); } }
    return res;
}
struct Line { double f, lv, Qmin, cv, binHz; int w0, w1, nwin; string plane; };

// argv[1] = input, argv[2] = report, argv[3] = candidate file (lae_noise passes its arguments after "scan")
static int noiseScanMain(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: lae_noise scan <in.wav> <out.txt> [candidates.laenoise]\n"); return 2; }
    WavStream ws; if (!ws.open(argv[1])) { fprintf(stderr, "cannot read %s (needs stereo PCM 16/24/32 or float32 WAV)\n", argv[1]); return 1; }
    const double rate = ws.rate; const long long len = ws.frames;
    FILE* out = fopen(argv[2], "wb");   /* LF on every platform */ if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fprintf(out, "file %s\nrate %.0f  length %.2f s\n\n", argv[1], rate, len / rate);
    int fam = (fmod(rate, 44100.0) == 0) ? 44100 : 48000; int mult = max(1, (int)llround(rate / fam));
    int C = mult >= 4 ? 1 : 4 / mult, T = 512 / (8 * C);
    Calib cal = calibrate(T, 8 * C);
    fprintf(out, "calibration (1,000,000 simulated noise blocks, %d x %d): tail p %.2e -> block failure %.4f %%; heavy tail p %.2e -> %.5f %%\n", T, 8 * C, cal.pPass, 100 * cal.rejPass, cal.pHeavy, 100 * cal.rejHeavy);
    fprintf(out, "  pass: S1 %.3f..%.3f  S2 %.3f..%.3f  S3 <= %.3f | heavy: S1 %.3f..%.3f  S2 %.3f..%.3f  S3 <= %.3f\n\n", cal.pass.vLo, cal.pass.vHi, cal.pass.kLo, cal.pass.kHi, cal.pass.rHi,
            cal.heavy.vLo, cal.heavy.vHi, cal.heavy.kLo, cal.heavy.kHi, cal.heavy.rHi);
    vector<double> edges = {0, 1600, 3150, 6300, 10000, 14000, 18000, 20000, 22050, 24000, 30000, 40000, 50000, 60000, 80000, 100000, 120000, 140000, 160000, 176400};
    while (edges.size() > 1 && edges.back() > rate / 2) edges.pop_back(); if (edges.back() < rate / 2) edges.push_back(rate / 2 + 1);
    vector<Cand> cands;
    stationaryPass(out, ws, 0, rate, "M", 0.0, edges, cal, C, T, cands);
    stationaryPass(out, ws, 1, rate, "S (level -6.02 dB, comparable with M)", -6.02, edges, cal, C, T, cands);
    // lines: two resolutions x two planes; false-line budget 0.01 per file split over all tests
    int W1 = 1024 * max(1, (int)llround(rate / (2.0 * fam))), W2 = 8 * W1;
    double nTests = 0; for (int W : {W1, W2}) { long n = (long)llround(10.0 * rate / W); long nwin = (long)(len / W) / max(1L, n); nTests += 2.0 * (W / 2 - 1) * max(1L, nwin); }
    double alphaTest = 0.01 / nTests;
    vector<Line> lines;
    for (int pl = 0; pl < 2; pl++) for (int W : {W2, W1}) {
        vector<LDet> d = lineScan(ws, rate, W, pl == 1, alphaTest); double binHz = rate / W; long n = (long)llround(10.0 * rate / W);
        double ftol = 3.0 * binHz / n;   // frequency precision of a 10-s coherent estimate
        sort(d.begin(), d.end(), [](const LDet& p, const LDet& q) { return p.win != q.win ? p.win < q.win : p.f < q.f; });   // link in time order
        vector<Line> v;   // link detections across windows
        for (const LDet& x : d) { bool done = false;
            for (Line& ln : v) if (fabs(ln.f - x.f) <= ftol + 0.004 * x.f && x.win == ln.w1 + 1) { ln.f = (ln.f * ln.nwin + x.f) / (ln.nwin + 1); ln.lv = max(ln.lv, x.lv); ln.Qmin = min(ln.Qmin, x.Q); ln.cv = max(ln.cv, x.cv); ln.w1 = x.win; ln.nwin++; done = true; break; }
            if (!done) v.push_back({x.f, x.lv, x.Q, x.cv, binHz, x.win, x.win, 1, pl ? "S" : "M"}); }
        for (Line& ln : v) { bool dup = false;   // the same line from the other resolution: keep the finer one
            for (Line& o : lines) if (o.plane == ln.plane && fabs(o.f - ln.f) <= max(o.binHz, ln.binHz) && o.w0 <= ln.w1 + 1 && ln.w0 <= o.w1 + 1) { dup = true; int a0 = min(o.w0, ln.w0), a1 = max(o.w1, ln.w1); double lvM = max(o.lv, ln.lv), qm = min(o.Qmin, ln.Qmin);
                if (ln.binHz < o.binHz) { o.f = ln.f; o.binHz = ln.binHz; o.cv = ln.cv; } o.w0 = a0; o.w1 = a1; o.nwin = a1 - a0 + 1; o.lv = lvM; o.Qmin = qm; break; }
            if (!dup) lines.push_back(ln); } }
    sort(lines.begin(), lines.end(), [](const Line& p, const Line& q) { return p.f < q.f; });
    // leakage: with hop = window, a strong line leaks coherently into neighbouring bins and shows there with the same phase step,
    // i.e. at a frequency that differs by whole bins. Drop a line when a line >= 10 dB stronger lies within 4 of its bins in the
    // same plane and time span and has the same fractional bin position (frequency modulo the bin width).
    { vector<char> leak(lines.size(), 0);
      for (size_t i = 0; i < lines.size(); i++) for (size_t j = 0; j < lines.size(); j++) { if (i == j) continue; const Line& a = lines[i]; const Line& b = lines[j];
          if (b.plane != a.plane || b.lv < a.lv + 10 || fabs(b.f - a.f) > 4 * a.binHz || b.w0 > a.w1 || a.w0 > b.w1) continue;
          double fa = a.f / a.binHz, fb = b.f / a.binHz, df = (fa - floor(fa)) - (fb - floor(fb)); df -= floor(df + 0.5);
          if (fabs(df) <= 0.02) { leak[i] = 1; break; } }
      vector<Line> kept; for (size_t i = 0; i < lines.size(); i++) if (!leak[i]) kept.push_back(lines[i]); lines.swap(kept); }
    long n1 = (long)llround(10.0 * rate / W1); double winSec = (double)n1 * W1 / rate;   // one line window (10 s)
    fprintf(out, "[lines] %zu found (false-line budget 0.01 per file; test threshold Q >= ln(n / %.2e)). freq Hz | res Hz | level dBFS | plane | windows (10 s) | Qmin | amp CV\n",
            lines.size(), alphaTest);
    for (const Line& ln : lines) { fprintf(out, "%10.2f %6.1f %8.1f   %s   %3d-%-3d (%2d) %8.1f %5.2f\n", ln.f, ln.binHz, ln.lv, ln.plane.c_str(), ln.w0, ln.w1, ln.nwin, ln.Qmin, ln.cv);
        // a line takes its cell column; also the neighbouring column when it lies within 2 bins of a column edge (the MDCT spreads a
        // steady tone over about +-2 bins); frames: whole frames inside its windows
        Cand cd{ln.w0 * winSec, (ln.w1 + 1) * winSec, 0, 0, ln.lv, "line", ln.plane}; cd.fLine = ln.f;
        const double colW = nmk::colHz(1, rate), fb = ln.f / (colW / nmk::CW); int c = (int)(ln.f / colW); double inCol = fb - nmk::CW * c;
        cd.c0 = (inCol < 2 && c > 0) ? c - 1 : c; cd.c1 = (inCol > nmk::CW - 2 && c + 1 < nmk::NCOL) ? c + 2 : c + 1;
        nmk::snapTime(cd.t0, cd.t1, rate, mdct::frames((long)len), cd.m0, cd.m1);
        if (cd.m1 > cd.m0) { cd.t0 = nmk::frameT0(cd.m0, rate); cd.t1 = nmk::frameT0(cd.m1, rate); cd.f0 = nmk::colHz(cd.c0, rate); cd.f1 = nmk::colHz(cd.c1, rate); cands.push_back(cd); } }
    // stationary regions: a deletion removes the region from both channels, so a region counts only where M and S both pass
    // the intersection must still be 10 s long. Then neighbouring columns whose spans differ by at most one
    // block are one region over their common span (conservative: nothing outside either column's own region is included).
    { const double blk = T * (double)mdct::N / rate; vector<Cand> nM, nS, rest;
      for (const Cand& c : cands) (c.kind != "noise" ? rest : c.plane == "M" ? nM : nS).push_back(c);
      vector<Cand> both;
      for (const Cand& a : nM) for (const Cand& b : nS) { if (a.c0 != b.c0) continue; long m0 = max(a.m0, b.m0), m1 = min(a.m1, b.m1);
          double t0 = (1.0 * m0 - 1) * mdct::N / rate, t1 = (1.0 * m1 - 1) * mdct::N / rate;
          if (t1 - t0 >= 10.0) { Cand cd{t0, t1, a.f0, a.f1, 10 * lmath::log10((lmath::exp10(a.lv / 10) + lmath::exp10(b.lv / 10)) / 2), "noise", "M+S"}; cd.m0 = m0; cd.m1 = m1; cd.c0 = a.c0; cd.c1 = a.c1; both.push_back(cd); } }
      sort(both.begin(), both.end(), [](const Cand& p, const Cand& q) { return p.t0 != q.t0 ? p.t0 < q.t0 : p.f0 < q.f0; });
      vector<char> used(both.size(), 0); vector<Cand> mg;
      for (size_t i = 0; i < both.size(); i++) { if (used[i]) continue; Cand m = both[i]; used[i] = 1; double bwSum = m.f1 - m.f0, pw = lmath::exp10(m.lv / 10) * bwSum;
          for (bool grew = true; grew; ) { grew = false;
              for (size_t j = 0; j < both.size(); j++) { if (used[j]) continue; const Cand& c = both[j];
                  if (c.c0 == m.c1 && fabs(c.t0 - m.t0) <= blk && fabs(c.t1 - m.t1) <= blk && min(m.t1, c.t1) - max(m.t0, c.t0) >= 10.0) {
                      m.t0 = max(m.t0, c.t0); m.t1 = min(m.t1, c.t1); m.m0 = max(m.m0, c.m0); m.m1 = min(m.m1, c.m1); m.f1 = c.f1; m.c1 = c.c1;
                      double bw = c.f1 - c.f0; pw += lmath::exp10(c.lv / 10) * bw; bwSum += bw; used[j] = 1; grew = true; } } }
          m.lv = 10 * lmath::log10(pw / bwSum); mg.push_back(m); }
      cands = rest; cands.insert(cands.end(), mg.begin(), mg.end()); }
    // every candidate is a rectangle of cells now; the same rectangle found twice (a line in M and in S) is one candidate
    for (Cand& c : cands) { c.t0 = nmk::frameT0(c.m0, rate); c.t1 = nmk::frameT0(c.m1, rate); c.f0 = nmk::colHz(c.c0, rate); c.f1 = nmk::colHz(c.c1, rate); }
    sort(cands.begin(), cands.end(), [](const Cand& p, const Cand& q) { return p.m0 != q.m0 ? p.m0 < q.m0 : p.c0 != q.c0 ? p.c0 < q.c0 : p.m1 != q.m1 ? p.m1 > q.m1 : p.c1 > q.c1; });
    { vector<Cand> u; for (const Cand& c : cands) { if (!u.empty() && u.back().m0 == c.m0 && u.back().m1 == c.m1 && u.back().c0 == c.c0 && u.back().c1 == c.c1) { u.back().lv = max(u.back().lv, c.lv); if (c.fLine && !u.back().fLine) u.back().fLine = c.fLine; continue; } u.push_back(c); } cands.swap(u); }
    fprintf(out, "\n[candidates] %zu, rectangles of encoder cells, sorted by start, then frequency, then length. no | start s | end s | length s | Hz from - to | level | narrow at Hz\n", cands.size());
    for (size_t i = 0; i < cands.size(); i++) { const Cand& c = cands[i];
        char at[24] = ""; if (c.fLine) snprintf(at, sizeof at, "%.2f", c.fLine);
        fprintf(out, "%4zu %8.2f %8.2f %7.2f   %10.1f %10.1f %8.1f   %s\n", i + 1, c.t0, c.t1, c.t1 - c.t0, c.f0, c.f1, c.lv, at); }
    fclose(out);
    // candidate file (same format as a marking file; the user keeps the ones to silence)
    if (argc > 3) { nmk::Marks mk; mk.sha = nmk::masterSha(ws); mk.frames = len; mk.rate = rate;
        for (const Cand& c : cands) { mk.pos.push_back({c.m0, c.m1, c.c0, c.c1}); char nt[64]; if (c.fLine) snprintf(nt, sizeof nt, "level %.1f, narrow at %.2f Hz", c.lv, c.fLine); else snprintf(nt, sizeof nt, "level %.1f", c.lv); mk.note.push_back(nt); }
        if (!nmk::writeMarks(argv[3], mk)) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; } }
    return 0;
}
#ifndef NOISE_SCAN_NO_MAIN
int main(int argc, char** argv) { lae_u8::init(argc, argv); return noiseScanMain(argc, argv); }   // UTF-8 arguments and file names on Windows (win_utf8.h)
#endif
