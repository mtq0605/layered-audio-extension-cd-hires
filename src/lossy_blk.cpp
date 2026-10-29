// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise layer coder (stage 1 of the block-wise encoder, 2026-10-29): the same output as lossy_v2.cpp, byte for byte, with the memory of a
// few blocks plus small per-track records. The track is scanned block by block; each scan records per-cell information and adds up whole-track
// summaries (floors, tone / transient events, mean energies, code-length histogram, shares); the summaries decide the plan (tags, lambdas);
// then every block is coded by the plan and its piece of each stream is appended in time order (one whole-track stream per segment, as before).
// Blocks are whole floor segments and whole root rows (lcm(seg, 8) frames x LAE_BLOCKUNITS, set by the encoder's memory plan, default 4; LAE_BLOCKUNITS=0: the whole track as
// one block). Every sum is added in the same order as in lossy_v2.cpp. Large intermediate data (the coefficient plane X, the reconstructed plane
// of all segments) sit in temporary files next to DUMP; the cell records stay in memory (3 bytes per cell).
// usage and environment as lossy_v2.cpp: lossy_blk <raw s32le stereo> <rate>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>
#include <complex>
#include <numeric>
#include "mdct_tb.h"
#include "enc_io.h"
#include "fft.h"
#include "partition.h"
using namespace std;
typedef int32_t i32; typedef int64_t i64; static const int N = mdct::N;
struct Ev { double f, t0, t1, db; int W; };
static const int TIERS[5] = {0, 2, 4, 8, 8};

// ---- sample sources: the layer signal (CHSEL of a raw s32 stereo file, as loadOrSynth) and the master reference (MASTERREF, plain 24-bit units)
struct Source { FILE* f = nullptr; long long frames = 0; int mode = 0; std::string path;   // mode: 0 L, 1 R, 2 S, 3 M, 4 master ref L, 5 master ref R
    void open(const char* p, int md) { path = p; mode = md; f = encio::open(p, "rb"); long long n = encio::size(f); if (n < 0) encio::fail(std::string("cannot read the size of ") + p); frames = n / 8; }
    ~Source() { if (f) fclose(f); }
    // values of samples [a, b) as int64; outside [0, frames) zero (only within [0, limit) for the master reference)
    void read(long long a, long long b, std::vector<i64>& out, long long limit = -1) {
        out.assign((size_t)(b - a), 0); const long long lim = limit < 0 ? frames : std::min(limit, frames);
        const long long ra = std::max(a, 0LL), rb = std::min(b, lim); if (ra >= rb) return;
        std::vector<int32_t> raw((size_t)(rb - ra) * 2);
#ifdef _WIN32
        if (_fseeki64(f, ra * 8, SEEK_SET)) encio::fail("seek failed: " + path);
#else
        if (fseeko(f, (off_t)(ra * 8), SEEK_SET)) encio::fail("seek failed: " + path);
#endif
        if (fread(raw.data(), 8, (size_t)(rb - ra), f) != (size_t)(rb - ra)) encio::fail("read failed: " + path);
        for (long long i = ra; i < rb; i++) { const int32_t l0 = raw[(size_t)(i - ra) * 2], r0 = raw[(size_t)(i - ra) * 2 + 1]; i64 v;
            if (mode >= 4) v = mode == 4 ? l0 : r0;
            else { const long l = l0 >> 8, r = r0 >> 8; v = mode == 1 ? r : mode == 2 ? (i64)(l - r) : mode == 3 ? (i64)(r + ((l - r) >> 1)) : (i64)l; }
            out[(size_t)(i - a)] = v; } } };
// ---- a plane of K rows x N int32 in a temporary file (X or the reconstructed plane)
struct PlaneFile { FILE* f = nullptr; std::string path; long long K = 0;
    void create(const std::string& p, long long k) { path = p; K = k; f = encio::open(p.c_str(), "w+b"); }
    ~PlaneFile() { if (f) { fclose(f); remove(path.c_str()); } }
    void seek(long long m) {
#ifdef _WIN32
        if (_fseeki64(f, m * N * 4, SEEK_SET)) encio::fail("seek failed: " + path);
#else
        if (fseeko(f, (off_t)(m * N * 4), SEEK_SET)) encio::fail("seek failed: " + path);
#endif
    }
    void write(long long m0, const int32_t* d, long long rows) { if (rows <= 0) return; seek(m0); if (encio::testFail(path.c_str()) || fwrite(d, 4, (size_t)(rows * N), f) != (size_t)(rows * N)) encio::fail("write failed (disk full?): " + path); }
    void read(long long m0, long long m1, std::vector<int32_t>& out) {   // rows outside [0, K) read as zero
        out.assign((size_t)((m1 - m0) * N), 0); const long long a = std::max(m0, 0LL), b = std::min(m1, K); if (a >= b) return; seek(a);
        if (fread(out.data() + (size_t)((a - m0) * N), 4, (size_t)((b - a) * N), f) != (size_t)((b - a) * N)) encio::fail("read failed: " + path); } };
// ---- MDCT of frames [m0, m1) (frame m covers padded samples [mN, mN + 2N) of [N zeros | signal | zeros]); the same arithmetic as mdct::forward
static void forwardFrames(Source& src, long long L, long long m0, long long m1, std::vector<int32_t>& X, long long limit = -1) {
    X.assign((size_t)((m1 - m0) * N), 0); if (m1 <= m0) return; std::vector<i64> x; src.read(m0 * N - N, m1 * N + N, x, limit);
    for (long long i = 0; i < (long long)x.size(); i++) { const long long g = m0 * N - N + i; if (g < 0 || g >= L) x[(size_t)i] = 0; }
    double z[2 * N], u[N], c[N];
    for (long long m = m0; m < m1; m++) { const i64* p = x.data() + (size_t)((m - m0) * N);   // p[n] = sample (m N + n) - N, zero outside
        for (int n = 0; n < 2 * N; n++) z[n] = (double)(i32)p[n] * mdct::WIN[n];
        for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }
        mdct::dct4(u, c); for (int k = 0; k < N; k++) X[(size_t)((m - m0) * N + k)] = (i32)std::llround(c[k]); } }
// samples [a, b) of the inverse MDCT of the plane rows (the same overlap-add order as mdct::inverse: frame m - 1, then frame m)
static void inverseRange(PlaneFile& Q, long long K, long long L, long long a, long long b, std::vector<i32>& y) {
    y.assign((size_t)(b - a), 0); if (b <= a) return;
    const long long p0 = a + N, p1 = b + N, mlo = std::max(0LL, p0 / N - 1), mhi = std::min(K - 1, (p1 - 1) / N); if (mlo > mhi) return;
    std::vector<int32_t> rows; Q.read(mlo, mhi + 1, rows); std::vector<double> acc((size_t)((mhi - mlo + 2) * N), 0.0); double c[N], u[N];
    for (long long m = mlo; m <= mhi; m++) { for (int k = 0; k < N; k++) c[k] = rows[(size_t)((m - mlo) * N + k)];
        mdct::dct4(c, u); const long long s = (m - mlo) * N;
        for (int n = 0; n < N / 2; n++) { acc[(size_t)(s + n)] += u[N / 2 + n] * mdct::WIN[n]; acc[(size_t)(s + N / 2 + n)] += -u[N - 1 - n] * mdct::WIN[N / 2 + n];
            acc[(size_t)(s + N + n)] += -u[N / 2 - 1 - n] * mdct::WIN[N + n]; acc[(size_t)(s + 3 * N / 2 + n)] += -u[n] * mdct::WIN[3 * N / 2 + n]; } }
    for (long long i = a; i < b; i++) { const long long p = i + N - mlo * N; if (i >= 0 && i < L && p >= 0 && p < (long long)acc.size()) y[(size_t)(i - a)] = (i32)std::llround(acc[(size_t)p]); } }

// ---- sustained-tone detector (tone_scan.cpp Detector), fed one periodogram row at a time; the same events in the same order
struct SDet {
    double rate = 0; int NF = 0, b0 = 0, b1 = 0, nb = 0, nfr = 0; double hop = 0, s2 = 0; std::vector<double> w;
    void init(double r, long long L, bool fullband) { rate = r; NF = 1; while (NF < rate * 0.09) NF <<= 1;
        double nyq = rate / 2, base = fullband ? 0.0 : (rate >= 88200 ? (fmod(rate, 44100.0) == 0 ? 22050.0 : 24000.0) : nyq);
        b0 = (int)ceil(base / (rate / NF)) + 8; b1 = (int)floor(0.98 * nyq / (rate / NF)); w.assign(NF, 0); s2 = 0;
        for (int i = 0; i < NF; i++) { w[i] = 0.5 - 0.5 * cos(2 * M_PI * i / NF); s2 += w[i] * w[i]; }
        nfr = (int)((size_t)(L - NF) / (size_t)(NF / 2)) + 1; hop = (NF / 2) / rate; nb = b1 - b0; }
    void row(const double* x, std::vector<float>& P) const { P.assign(nb, 0.f); std::vector<std::complex<double>> a(NF);   // x = samples [f hop, f hop + NF)
        for (int i = 0; i < NF; i++) a[i] = std::complex<double>(x[i] * w[i], 0); fftlib::fft(a);
        for (int b = b0; b < b1; b++) P[b - b0] = (float)(std::norm(a[b]) / s2); } };
struct SDetect {   // detect() of tone_scan.cpp on rows fed in order (optionally with the reference rows of another signal)
    const SDet* D = nullptr; bool useRef = false; int nch = 0; static const int CH = 32;
    struct Scale { int W, nw; double T, corr; int next = 0; std::vector<char> inRun; std::vector<int> s; std::vector<double> pk; std::vector<std::vector<Ev>> ev; };
    std::vector<Scale> sc; long long base = 0; std::vector<std::vector<float>> P, R;   // rows [base, base + P.size())
    void init(const SDet& d, bool ref) { D = &d; useRef = ref; nch = d.nb / CH; const int Ws[5] = {1, 2, 4, 8, 16}; sc.clear();
        for (int wi = 0; wi < 5; wi++) { Scale s; s.W = Ws[wi]; s.nw = d.nfr / s.W; double k = std::max(1.0, 0.75 * s.W); const double Z = 6.0;
            double q = 1 - 1 / (9 * k) + Z * sqrt(1 / (9 * k)); s.T = q * q * q; double q2 = 1 - 1 / (9 * k); s.corr = 1 / (q2 * q2 * q2);
            s.inRun.assign(d.nb, 0); s.s.assign(d.nb, 0); s.pk.assign(d.nb, 0.0); s.ev.assign(d.nb, {}); sc.push_back(s); } }
    void window(Scale& S, int wdx) {
        const int nb = D->nb, W = S.W; std::vector<double> avg(nb), chm(nch); std::vector<float> tmp(CH); std::vector<char> E(nb, 0);
        for (int b = 0; b < nb; b++) { double s = 0; for (int j = 0; j < W; j++) s += P[(size_t)(wdx * W + j - base)][b]; avg[b] = s / W; }
        for (int c = 0; c < nch; c++) { for (int j = 0; j < CH; j++) { int b2 = c * CH + j;
                if (useRef) { double s = 0; for (int q = 0; q < W; q++) s += R[(size_t)(wdx * W + q - base)][b2]; tmp[j] = (float)(s / W); } else tmp[j] = (float)avg[b2]; }
            nth_element(tmp.begin(), tmp.begin() + CH / 2, tmp.end()); chm[c] = tmp[CH / 2]; }
        for (int b = 0; b < nch * CH; b++) { int c = b / CH; double m[5]; int n = 0; for (int d = -2; d <= 2; d++) { int cc = c + d; if (cc >= 0 && cc < nch) m[n++] = chm[cc]; }
            sort(m, m + n); double fl = m[n / 2] * S.corr; if (fl > 0 && avg[b] > S.T * fl) E[b] = 1; }
        for (int b = 1; b < nb - 1; b++) { const bool hit = E[b] || E[b - 1] || E[b + 1];
            if (!S.inRun[b]) { if (E[b]) { S.inRun[b] = 1; S.s[b] = wdx; S.pk[b] = std::max(0.0, avg[b]); } }
            else if (hit) S.pk[b] = std::max(S.pk[b], avg[b]);
            else { close(S, b, wdx); } } }
    void close(Scale& S, int b, int wdx) { S.inRun[b] = 0; if (wdx - S.s[b] >= 2) { const double hop = D->hop;
            S.ev[b].push_back({(b + D->b0) * D->rate / D->NF, S.s[b] * S.W * hop, wdx * S.W * hop + (D->NF / D->rate), 10 * log10(S.pk[b] / std::max(1e-30, 1.0)), S.W}); } }
    void feed(std::vector<float>&& p, std::vector<float>&& r) { P.push_back(std::move(p)); if (useRef) R.push_back(std::move(r));
        const long long have = base + (long long)P.size();
        for (auto& S : sc) while (S.next < S.nw && (long long)(S.next + 1) * S.W <= have) window(S, S.next++);
        long long keep = have; for (auto& S : sc) if (S.next < S.nw) keep = std::min(keep, (long long)S.next * S.W);
        if (keep > base) { const size_t drop = (size_t)(keep - base); P.erase(P.begin(), P.begin() + drop); if (useRef) R.erase(R.begin(), R.begin() + drop); base = keep; } }
    std::vector<Ev> finish() { std::vector<Ev> ev;
        for (auto& S : sc) { if (S.nw < 2) continue; for (int b = 1; b < D->nb - 1; b++) if (S.inRun[b]) close(S, b, S.nw);
            for (int b = 0; b < D->nb; b++) ev.insert(ev.end(), S.ev[b].begin(), S.ev[b].end()); }
        sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.f < b.f; });   // merge as tone_scan.cpp
        vector<Ev> out; double df = 3 * D->rate / D->NF;
        for (auto& e : ev) { bool m = false;
            for (auto& o : out) if (fabs(o.f - e.f) <= df && e.t0 <= o.t1 && o.t0 <= e.t1) { o.t0 = min(o.t0, e.t0); o.t1 = max(o.t1, e.t1); o.W = min(o.W, e.W); m = true; break; }
            if (!m) out.push_back(e); }
        return out; } };

int main(int argc, char** argv) {
    auto T0 = chrono::steady_clock::now(); const double rate = atof(argv[2]);
    if (!getenv("PART")) { printf("lossy_v2: PART=<mode> is required\n"); return 1; }
    const bool fullBand = getenv("FULLBAND") != nullptr, depthAdapt = getenv("DEPTHADAPT") != nullptr;
    const char* cs = getenv("CHSEL"); const char sel = cs ? cs[0] : 'L';
    Source src; src.open(argv[1], sel == 'R' ? 1 : sel == 'S' ? 2 : sel == 'M' ? 3 : 0); const long long L = src.frames;
    const long long K = mdct::frames((long)L); const int GW = 32;
    double baseNyq = fmod(rate, 44100.0) == 0 ? 22050.0 : 24000.0; double k0frac = getenv("K0FRAC") ? atof(getenv("K0FRAC")) : 0.75;
    int k0 = fullBand ? 0 : (int)floor(k0frac * baseNyq / (rate / 2 / N));
    vector<int> gS, gW; { int k = k0; int first = (GW - k0 % GW) % GW; if (first) { gS.push_back(k); gW.push_back(first); k += first; } for (; k + GW <= N; k += GW) { gS.push_back(k); gW.push_back(GW); } }
    int ng = (int)gS.size(); vector<int> kb2g(N, -1); for (int g = 0; g < ng; g++) for (int j = 0; j < gW[g]; j++) kb2g[gS[g] + j] = g;
    const long seg = max(1L, (long)(rate / N)); const long long nseg = (K + seg - 1) / seg;
    // blocks: whole floor segments and whole root rows
    long long unit = seg; while (unit % 8) unit += seg; const char* bu = getenv("LAE_BLOCKUNITS"); const long long units = bu ? atoll(bu) : 4;
    const long long B = units > 0 ? unit * units : K; const long long nblk = (K + B - 1) / B;
    auto blk = [&](long long j, long long& m0, long long& m1) { m0 = j * B; m1 = std::min(K, m0 + B); };
    const std::string tmpBase = std::string(getenv("DUMP") ? getenv("DUMP") : argv[1]) + ".blk";
    PlaneFile Xf; Xf.create(tmpBase + ".X", K); PlaneFile Qf; Qf.create(tmpBase + ".Q", K);
    const bool masterRef = fullBand && getenv("MASTERREF"); Source msrc; long long nm = 0;
    if (masterRef) { const char* cc = getenv("CHSEL"); msrc.open(getenv("MASTERREF"), (cc && cc[0] == 'R') ? 5 : 4); nm = msrc.frames; }
    // ---- scan A: transform (to the X file), floors Fl / FlM, the layer signal's periodogram and its tone events
    vector<double> Fl((size_t)ng * nseg), FlM; if (masterRef) FlM.assign((size_t)ng * nseg, 0.0);
    SDet RD; RD.init(rate, L, fullBand); SDetect detA; detA.init(RD, false); long long nextRow = 0;
    auto rowsUpTo = [&](long long sampleEnd, SDetect& det, auto&& sig) {   // feeds every periodogram row whose samples end before sampleEnd
        while (nextRow < RD.nfr && nextRow * (RD.NF / 2) + RD.NF <= sampleEnd) { std::vector<double> xs; sig(nextRow * (RD.NF / 2), xs); std::vector<float> p, r; RD.row(xs.data(), p); det.feed(std::move(p), std::move(r)); nextRow++; } };
    auto xsig = [&](long long a, std::vector<double>& xs) { std::vector<i64> v; src.read(a, a + RD.NF, v); xs.assign(v.begin(), v.end()); };
    for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); std::vector<int32_t> X; forwardFrames(src, L, m0, m1, X); Xf.write(m0, X.data(), m1 - m0);
        for (long long s = m0 / seg; s < (m1 + seg - 1) / seg && s < nseg; s++) for (int g = 0; g < ng; g++) { vector<double> r;
            for (long long m = s * seg; m < min(K, (s + 1) * seg); m++) { double e = 0; for (int jj = 0; jj < gW[g]; jj++) { double v = X[(size_t)((m - m0) * N + gS[g] + jj)]; e += v * v; } r.push_back(sqrt(e / gW[g])); }
            sort(r.begin(), r.end()); double fTime = r[r.size() / 10];
            vector<double> sq; for (long long m = s * seg; m < min(K, (s + 1) * seg); m++) for (int jj = 0; jj < gW[g]; jj++) { double v = X[(size_t)((m - m0) * N + gS[g] + jj)]; sq.push_back(v * v); }
            nth_element(sq.begin(), sq.begin() + sq.size() / 2, sq.end()); double fSpec = sqrt(sq[sq.size() / 2] / 0.4549);
            Fl[(size_t)g * nseg + s] = max(min(fTime, fSpec), 0.3); }
        if (masterRef) { std::vector<int32_t> XM; forwardFrames(msrc, L, m0, m1, XM, std::min(L, nm));
            for (long long s = m0 / seg; s < (m1 + seg - 1) / seg && s < nseg; s++) for (int g = 0; g < ng; g++) { vector<double> r;
                for (long long m = s * seg; m < min(K, (s + 1) * seg); m++) { double e = 0; for (int jj = 0; jj < gW[g]; jj++) { double v = XM[(size_t)((m - m0) * N + gS[g] + jj)]; e += v * v; } r.push_back(sqrt(e / gW[g])); }
                sort(r.begin(), r.end()); FlM[(size_t)g * nseg + s] = max(r[r.size() / 10], 0.3); } }
        rowsUpTo(j == nblk - 1 ? L : std::min(L, m1 * N - N), detA, xsig); }
    if (masterRef) { double mn = 1e30, mxv = 0; for (double v : FlM) { mn = min(mn, v); mxv = max(mxv, v); } printf("master floor per band (24-bit LSB, coefficient rms): min %.1f max %.1f\n", mn, mxv); }
    const std::vector<Ev> toneEv = detA.finish();
    // ---- segments (frequency) and the per-cell records (3 bytes per cell: exponent, tag, flags 1 = A, 2 = no zero level, 4 = noise-like)
    const char* pm = getenv("PART"); { const char* rv = getenv(fullBand ? "RULE_D" : "RULE_F"); part::ruleRef() = rv ? atoi(rv) : 0; part::zeroSignOpt() = getenv("ZSIGN") != nullptr; }
    int mi = part::modeIndex(pm); if (mi < 0) { printf("unknown mode %s (max, 1, 3, 5, keep, keep20)\n", pm); return 1; }
    const part::Mode& MD = part::MODES[mi]; bool eq = getenv("PARTEQ") != nullptr; int wm = getenv("PARTW") ? atoi(getenv("PARTW")) : 0;
    if (mi >= part::MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
    vector<int> edges{k0};
    if (getenv("OCTSPLIT")) for (double f = 2 * baseNyq; f < rate / 2 - 1; f *= 2) { int b = (int)lround(f / (rate / 2 / N)); if (b > edges.back()) edges.push_back(b); }
    edges.push_back(N); const int nsg = (int)edges.size() - 1;
    struct Seg { int k0, k1, nc; vector<uint8_t> cE, lab, fl; double lam[2] = {0, 0}; part::MeanE me; part::Result tot; long leaves = 0; };
    vector<Seg> segs(nsg); for (int si = 0; si < nsg; si++) { Seg& S = segs[si]; S.k0 = edges[si]; S.k1 = edges[si + 1]; S.nc = part::numCells(S.k0, S.k1);
        S.cE.assign((size_t)(K * S.nc), 0); S.lab.assign((size_t)(K * S.nc), 0); S.fl.assign((size_t)(K * S.nc), 0); }
    const double C = 4.0;
    // mean energies of D and C cells per segment, in the order of part::meanEnergies (frame by frame, cell by cell)
    auto meanScan = [&]() { vector<double> eD(nsg, 0), eC(nsg, 0); vector<long> nD(nsg, 0), nC(nsg, 0);
        for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); std::vector<int32_t> X; Xf.read(m0, m1, X);
            for (int si = 0; si < nsg; si++) { Seg& S = segs[si]; for (long long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { size_t cid = (size_t)(m * S.nc + c);
                    if ((S.fl[cid] & 1) || S.lab[cid] < 2) continue; int kb = S.k0 + c * part::CW; int nb = std::min(S.k1, kb + part::CW) - kb; double e = 0;
                    for (int jj = 0; jj < nb; jj++) { double v = X[(size_t)((m - m0) * N + kb + jj)]; e += v * v; } if (S.lab[cid] == 3) { eD[si] += e; nD[si]++; } else { eC[si] += e; nC[si]++; } } } }
        for (int si = 0; si < nsg; si++) { segs[si].me.eD = nD[si] ? eD[si] / nD[si] : 1; segs[si].me.eC = nC[si] ? eC[si] / nC[si] : 1; } };
    // ---- scan B: structure map (tone events + transients), cell classes, cell tags
    for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); const long long a = std::max(0LL, m0 - 9), b = std::min(K, m1 + 9);
        std::vector<int32_t> X; Xf.read(a, b, X); auto Xat = [&](long long m, int k) { return X[(size_t)((m - a) * N + k)]; };
        vector<char> structMap((size_t)((m1 - m0) * ng), 0);
        for (auto& e : toneEv) { int ka = (int)((e.f - 2 * rate / RD.NF) / (rate / 2 / N)), kb = (int)((e.f + 2 * rate / RD.NF) / (rate / 2 / N)) + 1;
            long long ma = max(0LL, (long long)(e.t0 * rate / N) - 1), mb = min(K - 1, (long long)(e.t1 * rate / N) + 2); ma = std::max(ma, m0); mb = std::min(mb, m1 - 1);
            for (int k = ka; k <= kb; k++) { if (k < k0 || k >= N) continue; for (long long m = ma; m <= mb; m++) structMap[(size_t)((m - m0) * ng + kb2g[k])] = 1; } }
        for (int g = 0; g < ng; g++) for (int sb = 0; sb < gW[g]; sb += 4) {
            auto eo = [&](long long m) { double so = 0; for (int jj = sb; jj < min(sb + 4, gW[g]); jj++) { double wv = Xat(m, gS[g] + jj); so += wv * wv; } return so; };
            vector<double> nbv; nbv.reserve(16);
            for (long long m = std::max(8LL, m0 - 1); m < std::min(K - 8, m1 + 1); m++) { nbv.clear(); for (long d = -8; d <= 8; d++) if (labs(d) > 1) nbv.push_back(eo(m + d));
                nth_element(nbv.begin(), nbv.begin() + nbv.size() / 2, nbv.end()); const double e0 = eo(m);
                if (e0 > 6 * nbv[nbv.size() / 2] && e0 > 4 * 36.0) for (long d = -1; d <= 1; d++) { const long long mm = m + d; if (mm >= m0 && mm < m1) structMap[(size_t)((mm - m0) * ng + g)] = 1; } } }
        vector<int> E((size_t)((m1 - m0) * ng)); vector<char> cls((size_t)((m1 - m0) * ng));
        for (long long m = m0; m < m1; m++) for (int g = 0; g < ng; g++) {
            double mx = 0; for (int jj = 0; jj < gW[g]; jj++) mx = max(mx, fabs((double)Xat(m, gS[g] + jj)));
            size_t id = (size_t)((m - m0) * ng + g); double fl = Fl[(size_t)g * nseg + m / seg]; E[id] = (int)ceil(log2(mx + 1));
            if (fl <= 1.5 && mx <= 8.0 && !structMap[id]) { cls[id] = 0; continue; }
            cls[id] = 2; }
        for (int si = 0; si < nsg; si++) { Seg& S = segs[si];
            for (long long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { size_t cid = (size_t)(m * S.nc + c); int kb = S.k0 + c * part::CW; int nb = std::min(S.k1, kb + part::CW) - kb;
                double mx = 0; for (int jj = 0; jj < nb; jj++) mx = max(mx, fabs((double)Xat(m, kb + jj)));
                int g = kb2g[kb]; size_t id = (size_t)((m - m0) * ng + g); double fl = Fl[(size_t)g * nseg + m / seg];
                int g2 = kb2g[kb + nb - 1]; size_t id2 = (size_t)((m - m0) * ng + g2); bool st = structMap[id] || structMap[id2];
                if (g2 != g) fl = min(fl, Fl[(size_t)g2 * nseg + m / seg]);
                int e = (int)ceil(log2(mx + 1)); S.cE[cid] = (uint8_t)e; uint8_t f = (st || fullBand) ? 2 : 0;
                if (cls[id] == 0 && cls[id2] == 0) { S.fl[cid] = (uint8_t)(f | 1); S.lab[cid] = 0; continue; }
                int need; bool nz = false;
                if (fullBand && depthAdapt && !FlM.empty()) { need = e - (int)floor(log2(max(1.0, FlM[(size_t)g * nseg + m / seg]))); nz = need <= 2 && !st; }
                else if (mx < 4.5 * fl && !st) { need = 0; nz = true; }
                else need = (int)ceil(e - log2(C * fl));
                int t = 1; while (t < 3 && TIERS[t] < need) t++;
                S.lab[cid] = (uint8_t)t; S.fl[cid] = (uint8_t)(f | (nz ? 4 : 0)); } } }
    meanScan();
    // ---- DP scans
    part::Costs cost; for (int t = 0; t < 4; t++) for (int b = 0; b <= 32; b++) cost.bl[t][b] = 1.0 + b;
    auto cellsOf = [&](Seg& S, long long m0, long long m1, vector<char>& cA, vector<int>& cE, vector<int>& lab, vector<char>& noF) {
        const size_t n = (size_t)((m1 - m0) * S.nc), o = (size_t)(m0 * S.nc); cA.resize(n); cE.resize(n); lab.resize(n); noF.resize(n);
        for (size_t i = 0; i < n; i++) { cA[i] = S.fl[o + i] & 1; cE[i] = S.cE[o + i]; lab[i] = S.lab[o + i]; noF[i] = (S.fl[o + i] & 2) ? 1 : 0; } };
    // one pass of run() over all blocks for the segments in 'which' with their lambdas; full: Q rows to the Q file (all segments together),
    // and the code-length histogram of the leaves when h != null
    auto dpScan = [&](const vector<int>& which, const vector<double>& lD, const vector<double>& lC, bool full, vector<vector<double>>* h, vector<part::Result>& tot, vector<long>& nleaves,
                      std::vector<FILE*>* dumps) {
        tot.assign(nsg, part::Result()); nleaves.assign(nsg, 0);
        for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); std::vector<int32_t> X; Xf.read(m0, m1, X); std::vector<int32_t> Qall;
            if (full) Qall.assign(X.size(), 0);
            for (size_t wi = 0; wi < which.size(); wi++) { const int si = which[wi]; Seg& S = segs[si]; part::Plane P{&X, (long)(m1 - m0), N, S.k0, S.k1, S.nc};
                vector<char> cA, noF; vector<int> cE, lab; cellsOf(S, m0, m1, cA, cE, lab, noF);
                part::Result R = part::run(P, cA, cE, lab, noF, eq, cost, lD[wi], lC[wi], wm, -1, full, &S.me, &tot[si]);
                tot[si].bits = R.bits; tot[si].cnt = R.cnt; tot[si].en = R.en; tot[si].zsum = R.zsum; tot[si].zcnt = R.zcnt; nleaves[si] += (long)R.leaves.size();
                if (h) for (auto& lf : R.leaves) { if (!lf.tier) continue; int sh = part::shiftOf(lf.E, lf.tier);
                    for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int kb; int nb = part::cellBins(P, c, kb);
                        for (int jj = 0; jj < nb; jj++) { double q = floor(fabs((double)X[(size_t)m * N + kb + jj]) / ldexp(1.0, sh) + 0.5 - part::deadzone()); uint32_t qa = q > 0 ? (uint32_t)q : 0; int b = 0; while (qa >> b) b++; (*h)[lf.tier][min(b, 32)]++; } } }
                if (full) { for (long long m = 0; m < m1 - m0; m++) for (int k = 0; k < R.bw; k++) Qall[(size_t)(m * N + R.k0 + k)] += R.Q[(size_t)(m * R.bw + k)];
                    if (dumps) { FILE* df = (*dumps)[si]; if (!R.stream.empty() && fwrite(R.stream.data(), 4, R.stream.size(), df) != R.stream.size()) encio::fail("write failed (disk full?): dump");
                        // the segment's plane (its own bins, zeros elsewhere) follows in the plane file of this segment, row by row
                        FILE* pf = (*dumps)[nsg + si]; std::vector<int32_t> row(N, 0);
                        for (long long m = 0; m < m1 - m0; m++) { std::fill(row.begin(), row.end(), 0); for (int k = 0; k < R.bw; k++) row[(size_t)(R.k0 + k)] = R.Q[(size_t)(m * R.bw + k)];
                            if (fwrite(row.data(), 4, N, pf) != (size_t)N) encio::fail("write failed (disk full?): plane"); } } } }
            if (full) Qf.write(m0, Qall.data(), m1 - m0); } };
    vector<int> allSeg(nsg); std::iota(allSeg.begin(), allSeg.end(), 0);
    for (auto& S : segs) { S.lam[0] = 1e9; S.lam[1] = 1e9; }
    auto lamsD = [&]() { vector<double> v; for (auto& S : segs) v.push_back(S.lam[0]); return v; };
    auto lamsC = [&]() { vector<double> v; for (auto& S : segs) v.push_back(S.lam[1]); return v; };
    vector<part::Result> tot; vector<long> nl;
    auto refine = [&](vector<vector<double>>& h) { for (int t = 1; t < 4; t++) { double tt = 0; for (double v : h[t]) tt += v; for (int b = 0; b <= 32; b++) cost.bl[t][b] = -log2(h[t][b] / tt); } };
    for (int it = 0; it < 3; it++) {
        if (it == 0) { vector<vector<double>> h(4, vector<double>(33, 0.5)); dpScan(allSeg, lamsD(), lamsC(), true, &h, tot, nl, nullptr); refine(h); }
        dpScan(allSeg, lamsD(), lamsC(), true, nullptr, tot, nl, nullptr);
        // closed loop, scan E: the coding error's periodogram (reference floor = the layer signal's) -> damaged events
        SDetect detE; detE.init(RD, true); nextRow = 0;
        auto errRows = [&](long long sampleEnd) {
            while (nextRow < RD.nfr && nextRow * (RD.NF / 2) + RD.NF <= sampleEnd) { const long long a = nextRow * (RD.NF / 2);
                std::vector<i64> xv; src.read(a, a + RD.NF, xv); std::vector<i32> y; inverseRange(Qf, K, L, a, a + RD.NF, y);
                std::vector<double> er(RD.NF), xs(RD.NF); for (int i = 0; i < RD.NF; i++) { er[i] = (double)(i32)xv[i] - y[i]; xs[i] = (double)xv[i]; }
                std::vector<float> p, r; RD.row(er.data(), p); RD.row(xs.data(), r); detE.feed(std::move(p), std::move(r)); nextRow++; } };
        for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); errRows(j == nblk - 1 ? L : std::min(L, m1 * N - N)); }
        std::vector<Ev> evE = detE.finish();
        // scan S: the sums of damaged() and of the 'real' test over each event's span, frame by frame (the same order)
        struct Sum { int b, f0, f1; double se = 0, so = 0, so2 = 0, sn = 0; };
        std::vector<Sum> sums; const int nbd = RD.nb;
        for (auto& e : evE) { Sum s; s.b = (int)round(e.f / (rate / RD.NF)) - RD.b0; s.f0 = max(0, (int)(e.t0 / RD.hop)); s.f1 = min(RD.nfr - 1, (int)(e.t1 / RD.hop)); sums.push_back(s); }
        for (long long f = 0; f < RD.nfr; f++) { bool any = false; for (auto& s : sums) if (f >= s.f0 && f <= s.f1) { any = true; break; } if (!any) continue;
            const long long a = f * (RD.NF / 2); std::vector<i64> xv; src.read(a, a + RD.NF, xv); std::vector<i32> y; inverseRange(Qf, K, L, a, a + RD.NF, y);
            std::vector<double> er(RD.NF), xs(RD.NF); for (int i = 0; i < RD.NF; i++) { er[i] = (double)(i32)xv[i] - y[i]; xs[i] = (double)xv[i]; }
            std::vector<float> DP, RP; RD.row(er.data(), DP); RD.row(xs.data(), RP);
            for (auto& s : sums) { if (f < s.f0 || f > s.f1) continue;
                for (int d = -1; d <= 1; d++) { int bb = s.b + d; if (bb < 0 || bb >= nbd) continue; s.se += DP[bb]; s.so += RP[bb]; }
                s.so2 += RP[s.b]; for (int d = 8; d <= 16; d++) { if (s.b + d < nbd) s.sn += RP[s.b + d]; if (s.b - d >= 0) s.sn += RP[s.b - d]; } } }
        std::vector<Ev> real;
        for (size_t i = 0; i < evE.size(); i++) { const Sum& s = sums[i]; if (!(s.se > 0.1 * s.so)) continue;   // damaged()
            if (s.so2 > 3.0 * s.sn / 18 && s.so2 / (s.f1 - s.f0 + 1) > 9.0) real.push_back(evE[i]); }
        const size_t nreal = real.size(); long raised = 0;
        // scan M: marks (real events, damaged transients per cell) and the raised tags
        for (long long j = 0; j < nblk; j++) { long long m0, m1; blk(j, m0, m1); const long long a = std::max(0LL, m0 - 8), b = std::min(K, m1 + 8);
            std::vector<int32_t> X, Q; Xf.read(a, b, X); Qf.read(a, b, Q);
            for (int si = 0; si < nsg; si++) { Seg& S = segs[si]; vector<char> mark((size_t)((m1 - m0) * S.nc), 0);
                for (auto& e : real) { int ka = (int)((e.f - 3 * rate / RD.NF) / (rate / 2 / N)), kb = (int)((e.f + 3 * rate / RD.NF) / (rate / 2 / N)) + 1;
                    long long ma = max(0LL, (long long)(e.t0 * rate / N) - 2), mb = min(K - 1, (long long)(e.t1 * rate / N) + 3); ma = std::max(ma, m0); mb = std::min(mb, m1 - 1);
                    for (int k = max(ka, S.k0); k <= min(kb, S.k1 - 1); k++) { int c = (k - S.k0) / part::CW; for (long long m = ma; m <= mb; m++) mark[(size_t)((m - m0) * S.nc + c)] = 1; } }
                for (int c = 0; c < S.nc; c++) { int kb = S.k0 + c * part::CW; int nb = std::min(S.k1, kb + part::CW) - kb;
                    auto eoen = [&](long long m, double& so, double& se) { so = 0; se = 0; for (int jj = 0; jj < nb; jj++) { size_t ix = (size_t)((m - a) * N + kb + jj); double wv = X[ix], v = (double)X[ix] - Q[ix]; so += wv * wv; se += v * v; } };
                    vector<double> nbv; nbv.reserve(16);
                    for (long long m = std::max(8LL, m0); m < std::min(K - 8, m1); m++) { nbv.clear(); for (long d = -8; d <= 8; d++) if (labs(d) > 1) { double so, se; eoen(m + d, so, se); nbv.push_back(so); }
                        nth_element(nbv.begin(), nbv.begin() + nbv.size() / 2, nbv.end()); double medo = nbv[nbv.size() / 2]; double eo, en; eoen(m, eo, en);
                        if (eo > 6 * medo && eo > 16.0 && en > 0.25 * eo) mark[(size_t)((m - m0) * S.nc + c)] = 1; } }
                for (size_t i = 0; i < mark.size(); i++) { size_t cid = (size_t)(m0 * S.nc) + i;
                    if (mark[i] && !(S.fl[cid] & 1) && !(S.fl[cid] & 4) && S.lab[cid] < 3) { S.lab[cid]++; S.fl[cid] |= 2; raised++; } } } }
        meanScan();
        part::Share cn, en; long nlv = 0; double bits = 0;
        for (int si = 0; si < nsg; si++) { auto& R = tot[si]; cn.dTot += R.cnt.dTot; cn.dKept += R.cnt.dKept; cn.cTot += R.cnt.cTot; cn.cKept += R.cnt.cKept;
            en.dTot += R.en.dTot; en.dKept += R.en.dKept; en.cTot += R.en.cTot; en.cKept += R.en.cKept; nlv += nl[si]; bits += R.bits; }
        auto pc = [](double a, double b) { return b > 0 ? 100.0 * a / b : 100.0; };
        printf("PART mode %s (share by %s, %s, %zu segment(s)) pass %d: leaves %ld | est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy | lambda D %.3g C %.3g | damaged structures %zu, cells raised %ld\n",
               MD.name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), eq ? "equal" : "unequal", (size_t)nsg, it, nlv, bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot),
               segs[0].lam[0], segs[0].lam[1], nreal, raised);
        if (!raised) break; }
    // ---- share search (runTarget of partition.h for every segment at once: each pass evaluates the next lambda of every segment)
    struct Search { double lamD = 0, lamC = 0, lo = 0, hi = 0; int phase = 0, step = 0; bool done = false; };   // phase 0: C r0, 1: C bisect, 2: D r0, 3: D bisect
    vector<Search> sr(nsg);
    for (int si = 0; si < nsg; si++) { Search& s = sr[si]; s.lamD = 0; s.lamC = MD.cShare >= 1.0 ? 1e9 : 0; s.phase = MD.cShare >= 1.0 ? 2 : 0; }
    auto share = [&](const part::Result& r, bool forD) { return forD ? part::shareD(r, wm) : part::shareC(r, wm); };
    for (;;) { vector<int> which; vector<double> lD, lC;
        for (int si = 0; si < nsg; si++) { Search& s = sr[si]; if (s.done) continue; which.push_back(si); lD.push_back(s.lamD); lC.push_back(s.lamC); }
        if (which.empty()) break;
        dpScan(which, lD, lC, false, nullptr, tot, nl, nullptr);
        for (int si : which) { Search& s = sr[si]; const part::Result& r = tot[si]; const bool forD = s.phase >= 2; const double target = forD ? MD.dShare : MD.cShare;
            if (s.phase == 0 || s.phase == 2) { if (share(r, forD) >= target) { if (s.phase == 0) s.phase = 2; else s.done = true; continue; }
                s.lo = 1e-3; s.hi = 1e4; s.step = 0; s.phase++; double mid = sqrt(s.lo * s.hi); (forD ? s.lamD : s.lamC) = mid; continue; }
            const double mid = forD ? s.lamD : s.lamC; if (share(r, forD) >= target) s.hi = mid; else s.lo = mid;
            if (++s.step < 7) { double nm2 = sqrt(s.lo * s.hi); (forD ? s.lamD : s.lamC) = nm2; continue; }
            (forD ? s.lamD : s.lamC) = s.hi; if (s.phase == 1) s.phase = 2; else s.done = true; } }
    for (int si = 0; si < nsg; si++) { segs[si].lam[0] = sr[si].lamD; segs[si].lam[1] = sr[si].lamC; }
    // ---- final pass: every segment's stream and plane, block by block, appended in time order
    vector<FILE*> files(2 * nsg, nullptr); vector<std::string> dumpPath(nsg), planePath(nsg); vector<long> hdrAt(nsg, 0);
    for (int si = 0; si < nsg; si++) { std::string suf = nsg > 1 ? "." + to_string(si) : "";
        dumpPath[si] = std::string(getenv("DUMP") ? getenv("DUMP") : (tmpBase + ".dump")) + suf; planePath[si] = std::string(getenv("PLANE") ? getenv("PLANE") : (tmpBase + ".plane")) + suf;
        files[si] = encio::open(dumpPath[si].c_str(), "wb"); files[nsg + si] = encio::open(planePath[si].c_str(), "wb");
        int32_t hdr[11] = {0x50415254, (int32_t)K, N, segs[si].k0, segs[si].k1, (int32_t)rate, (int32_t)L, part::CW + 256 * part::ruleRef(), mi, eq ? 1 : 0, 0};
        if (fwrite(hdr, 4, 11, files[si]) != 11) encio::fail("write failed (disk full?): " + dumpPath[si]); }
    dpScan(allSeg, lamsD(), lamsC(), true, nullptr, tot, nl, &files);
    for (int si = 0; si < nsg; si++) {   // the token count goes into header word 10; a failed write removes the files and stops
        long long bytes = 0;
#ifdef _WIN32
        bytes = _ftelli64(files[si]);
#else
        bytes = (long long)ftello(files[si]);
#endif
        int32_t ntok = (int32_t)((bytes - 44) / 4);
#ifdef _WIN32
        bool ok = _fseeki64(files[si], 40, SEEK_SET) == 0;
#else
        bool ok = fseeko(files[si], 40, SEEK_SET) == 0;
#endif
        ok = ok && fwrite(&ntok, 4, 1, files[si]) == 1 && !encio::testFail(dumpPath[si].c_str()); ok = (fclose(files[si]) == 0) && ok;
        bool okp = !encio::testFail(planePath[si].c_str()); okp = (fclose(files[nsg + si]) == 0) && okp;
        if (!ok || !okp) { remove(dumpPath[si].c_str()); remove(planePath[si].c_str()); encio::fail("write failed (disk full?): " + (ok ? planePath[si] : dumpPath[si])); } }
    { part::Share cn, en; double bits = 0; for (int si = 0; si < nsg; si++) { auto& R = tot[si]; cn.dTot += R.cnt.dTot; cn.dKept += R.cnt.dKept; cn.cTot += R.cnt.cTot; cn.cKept += R.cnt.cKept;
          en.dTot += R.en.dTot; en.dKept += R.en.dKept; en.cTot += R.en.cTot; en.cKept += R.en.cKept; bits += R.bits; }
      auto pc = [](double a, double b) { return b > 0 ? 100.0 * a / b : 100.0; };
      printf("PART FINAL mode %s (share by %s): est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy\n", MD.name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"),
             bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot)); }
    if (getenv("SEGCOUNT")) encio::writeText(getenv("SEGCOUNT"), to_string(nsg) + "\n");
    printf("runtime %.1f s\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    return 0; }
