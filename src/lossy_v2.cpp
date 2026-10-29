// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Lossy extension layer: classification and partition coding of one layer (depth layer with FULLBAND, otherwise frequency layer).
// Cells (8 bins x 1 frame) are classified from the bits they need above a floor estimate:
//   A  : digital silence and noise removed before encoding -> nothing coded
//   B/C/D : precision classes; every other content, including noise left in the recording, is programme material and starts at B.
// The cells are then coded by the time-frequency partition of partition.h (mode share targets, dynamic program per root), with an
// optional per-leaf zero level (ZSIGN). Detected tones and transients force structure.
// usage: lossy_v2 <raw s32le stereo | synthetic:white|silence|electronic> <rate>
// Transform: textbook MDCT (mdct_tb.h).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>
#include "mdct_tb.h"
#include "enc_io.h"
using namespace std;
typedef int32_t i32; typedef int64_t i64; static const int N = mdct::N;
using mdct::forward; using mdct::inverse;
#define TONE_NO_MAIN
#include "tone_scan.cpp"
#include <map>
#include <cstring>
#include "partition.h"

static const int TIERS[5] = {0, 2, 4, 8, 8};    // tier 0 = no payload (A0 only); depth capped at 8 (24-bit total)

int main(int argc, char** argv) {
    auto T0 = chrono::steady_clock::now();
    string src = argv[1]; double rate = atof(argv[2]);
    const bool fullBand = getenv("FULLBAND") != nullptr, depthAdapt = getenv("DEPTHADAPT") != nullptr;   // read once (the loops below test them per cell)
    vector<double> xd = loadOrSynth(src, rate, 40.0); long L = (long)xd.size();
    vector<i32> x(L); for (long i = 0; i < L; i++) x[i] = (i32)llround(xd[i]);
    // ---- transform, floors
    vector<i32> X = forward(x); long K = (long)X.size() / N;
    const int GW = 32;
    // Extension layer of the difference model: the input is d = master - CD-path reconstruction; coded from k0 = K0FRAC x base Nyquist upward
    // (default 0.75). The decoder adds the decoded extension to the CD-path reconstruction in the time domain.
    double baseNyq = fmod(rate, 44100.0) == 0 ? 22050.0 : 24000.0;
    double k0frac = getenv("K0FRAC") ? atof(getenv("K0FRAC")) : 0.75;
    int k0 = getenv("FULLBAND") ? 0 : (int)floor(k0frac * baseNyq / (rate / 2 / N));
    vector<int> gS, gW; { int k = k0; int first = (GW - k0 % GW) % GW; if (first) { gS.push_back(k); gW.push_back(first); k += first; } for (; k + GW <= N; k += GW) { gS.push_back(k); gW.push_back(GW); } }
    int ng = (int)gS.size(); vector<int> kb2g(N, -1); for (int g = 0; g < ng; g++) for (int j = 0; j < gW[g]; j++) kb2g[gS[g] + j] = g;
    long seg = max(1L, (long)(rate / N)), nseg = (K + seg - 1) / seg;
    vector<double> Fl((size_t)ng * nseg);
    for (int g = 0; g < ng; g++) for (long s = 0; s < nseg; s++) { vector<double> r;
        for (long m = s * seg; m < min(K, (s + 1) * seg); m++) { double e = 0; for (int j = 0; j < gW[g]; j++) { double v = X[(size_t)m * N + gS[g] + j]; e += v * v; } r.push_back(sqrt(e / gW[g])); }
        sort(r.begin(), r.end()); double fTime = r[r.size() / 10];
        // spectral floor: median of the squared coefficients over all bins and frames of the group-segment (a sustained tone occupies
        // a few bins and does not move the median; the 10th percentile over time alone takes a steady tone for floor)
        vector<double> sq; for (long m = s * seg; m < min(K, (s + 1) * seg); m++) for (int j = 0; j < gW[g]; j++) { double v = X[(size_t)m * N + gS[g] + j]; sq.push_back(v * v); }
        nth_element(sq.begin(), sq.begin() + sq.size() / 2, sq.end()); double fSpec = sqrt(sq[sq.size() / 2] / 0.4549);   // median of chi2(1) = 0.4549 sigma^2
        Fl[(size_t)g * nseg + s] = max(min(fTime, fSpec), 0.3); }
    const double C = 4.0, TH = part::deadzone();
    vector<double> FlM;   // master's own floor (depth layer only), same grouping
    vector<double> XMg;   // master coefficients at the base rate (depth layer only)
    if (getenv("FULLBAND") && getenv("MASTERREF")) {
        vector<int32_t> rm = encio::readAll<int32_t>(getenv("MASTERREF")); long nm = encio::count(rm.size()) / 2; rm.resize((size_t)nm * 2);
        const char* cs = getenv("CHSEL"); int cc = (cs && cs[0] == 'R') ? 1 : 0; vector<i32> xm(L); for (long i = 0; i < L && i < nm; i++) xm[i] = rm[i * 2 + cc];   // base24 file: plain 24-bit units
        vector<i32> XM = forward(xm); XMg.assign(XM.begin(), XM.end()); FlM.assign((size_t)ng * nseg, 0.0);
        for (int g = 0; g < ng; g++) for (long s = 0; s < nseg; s++) { vector<double> r;
            for (long m = s * seg; m < min(K, (s + 1) * seg); m++) { double e = 0; for (int j = 0; j < gW[g]; j++) { double v = XM[(size_t)m * N + gS[g] + j]; e += v * v; } r.push_back(sqrt(e / gW[g])); }
            sort(r.begin(), r.end()); FlM[(size_t)g * nseg + s] = max(r[r.size() / 10], 0.3); }
        double mn = 1e30, mxv = 0; for (double v : FlM) { mn = min(mn, v); mxv = max(mxv, v); }
        printf("master floor per band (24-bit LSB, coefficient rms): min %.1f max %.1f\n", mn, mxv);
    }
    if (getenv("TIMING")) printf("  [t %.1f s] analysis + transform done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    vector<char> structMap((size_t)K * ng, 0);
    // one periodogram of the layer signal: the tones that force structure, and the reference floor of the closed loop's error checks
    Detector RefD; RefD.rate = rate; RefD.analyse(xd); vector<double>().swap(xd);   // xd is not needed any more (memory)
    if (getenv("TIMING")) printf("  [t %.1f s] periodogram done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    { const Detector& M = RefD; auto ev = RefD.detect();
      for (auto& e : ev) { int ka = (int)((e.f - 2 * rate / M.NF) / (rate / 2 / N)), kb = (int)((e.f + 2 * rate / M.NF) / (rate / 2 / N)) + 1;
          long ma = max(0L, (long)(e.t0 * rate / N) - 1), mb = min(K - 1, (long)(e.t1 * rate / N) + 2);
          for (int k = ka; k <= kb; k++) { if (k < k0 || k >= N) continue; for (long m = ma; m <= mb; m++) structMap[(size_t)m * ng + kb2g[k]] = 1; } }
      for (int g = 0; g < ng; g++) for (int sb = 0; sb < gW[g]; sb += 4) { vector<double> eo(K);
          for (long m = 0; m < K; m++) { double so = 0; for (int j = sb; j < min(sb + 4, gW[g]); j++) { double w = X[(size_t)m * N + gS[g] + j]; so += w * w; } eo[m] = so; }
          vector<double> nbv; nbv.reserve(16); for (long m = 8; m < K - 8; m++) { nbv.clear(); for (long d = -8; d <= 8; d++) if (labs(d) > 1) nbv.push_back(eo[m + d]);
              nth_element(nbv.begin(), nbv.begin() + nbv.size() / 2, nbv.end()); if (eo[m] > 6 * nbv[nbv.size() / 2] && eo[m] > 4 * 36.0) for (long d = -1; d <= 1; d++) structMap[(size_t)(m + d) * ng + g] = 1; } } }
    if (getenv("TIMING")) printf("  [t %.1f s] tones + transients done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    // per group-frame state
    vector<int> E((size_t)K * ng), tier((size_t)K * ng); vector<char> cls((size_t)K * ng);   // cls: 0 = A0, 2 = coded (value 1 is not used)
    for (long m = 0; m < K; m++) for (int g = 0; g < ng; g++) {
        double mx = 0; for (int j = 0; j < gW[g]; j++) mx = max(mx, fabs((double)X[(size_t)m * N + gS[g] + j]));
        size_t id = (size_t)m * ng + g; double fl = Fl[(size_t)g * nseg + m / seg];
        E[id] = (int)ceil(log2(mx + 1));
        if (fl <= 1.5 && mx <= 8.0 && !structMap[id]) { cls[id] = 0; tier[id] = 0; continue; }
        if (fullBand && depthAdapt && !FlM.empty()) {
            // ABCD depth: refine only down to the master's own floor in this band (CD requantisation noise ~74 LSB24 rms per coefficient)
            double fmv = FlM[(size_t)g * nseg + m / seg];
            if (fmv >= 2 * 74.0 && !structMap[id]) { cls[id] = 2; tier[id] = 1; continue; }   // A is only silence or noise removed before encoding; this 24-bit detail is B
            int stepLog = (int)floor(log2(max(1.0, fmv)));
            int need2 = E[id] - stepLog; int t2 = 1; while (t2 < 3 && TIERS[t2] < need2) t2++;
            cls[id] = 2; tier[id] = t2; continue;
        }
        if (mx < 4.5 * fl && !structMap[id]) { cls[id] = 2; tier[id] = 1; continue; }   // noise left in the recording is programme material: coded from B (no level-only class)
        cls[id] = 2; int need = (int)ceil(E[id] - log2(C * fl)); int t = 1; while (t < 3 && TIERS[t] < need) t++; tier[id] = t;
    }
    if (getenv("TIMING")) printf("  [t %.1f s] classes done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
    auto damaged = [&](const Detector& D, const vector<Ev>& ev) { vector<Ev> out; int nb = D.b1 - D.b0;
        for (auto& e : ev) { int b = (int)round(e.f / (rate / D.NF)) - D.b0; int f0 = max(0, (int)(e.t0 / D.hop)), f1 = min(D.nfr - 1, (int)(e.t1 / D.hop)); double se = 0, so = 0;
            for (int f = f0; f <= f1; f++) for (int d = -1; d <= 1; d++) { int bb = b + d; if (bb < 0 || bb >= nb) continue; se += D.P[(size_t)f * nb + bb]; so += RefD.P[(size_t)f * nb + bb]; }
            if (se > 0.1 * so) out.push_back(e); }
        return out; };
    if (getenv("PART")) {   // ---- partition modes (partition.h)
        { const char* rv = getenv(getenv("FULLBAND") ? "RULE_D" : "RULE_F"); part::ruleRef() = rv ? atoi(rv) : 0; part::zeroSignOpt() = getenv("ZSIGN") != nullptr; }   // precision rule of this layer
        int mi = part::modeIndex(getenv("PART")); if (mi < 0) { printf("unknown mode %s (max, 1, 3, 5, keep, keep20)\n", getenv("PART")); return 1; }
        const part::Mode& MD = part::MODES[mi]; bool eq = getenv("PARTEQ") != nullptr; int wm = getenv("PARTW") ? atoi(getenv("PARTW")) : 0;   // share by 0 = cell count, 1 = energy
        if (mi >= part::MODE_KEEP) wm = 2;   // preservation (keep, keep20): the share must hold by cell count AND by energy
        if (mi == 0) wm = 1;   // maximum compression = smallest file: always measured by energy
        // frequency segments: one independent stream per octave (OCTSPLIT) so a decoder can stop at any octave; else one segment
        vector<int> edges{k0};
        if (getenv("OCTSPLIT")) for (double f = 2 * baseNyq; f < rate / 2 - 1; f *= 2) { int b = (int)lround(f / (rate / 2 / N)); if (b > edges.back()) edges.push_back(b); }
        edges.push_back(N);
        struct Seg { part::Plane P; vector<char> cA, noF, nz; vector<int> cE, lab; part::Result R; double lam[2] = {0, 0}; };   // nz: noise-like cell (never raised by the closed loop)
        vector<Seg> segs(edges.size() - 1);
        // full scan: every non-A cell gets a depth tag from the bits it needs above its floor (most bits = D, then C, the rest B).
        // A is fixed before this point (silence + noise removed before encoding) and never changes.
        for (size_t si = 0; si < segs.size(); si++) { Seg& S = segs[si]; S.P = part::Plane{&X, K, N, edges[si], edges[si + 1], part::numCells(edges[si], edges[si + 1])};
            size_t nc = (size_t)K * S.P.nc; S.cA.assign(nc, 0); S.noF.assign(nc, 0); S.nz.assign(nc, 0); S.cE.assign(nc, 0); S.lab.assign(nc, 0);
            for (long m = 0; m < K; m++) for (int c = 0; c < S.P.nc; c++) { size_t cid = (size_t)m * S.P.nc + c; int kb; int nb = part::cellBins(S.P, c, kb);
                double mx = 0; for (int j = 0; j < nb; j++) mx = max(mx, fabs((double)X[(size_t)m * N + kb + j]));
                int g = kb2g[kb]; size_t id = (size_t)m * ng + g; double fl = Fl[(size_t)g * nseg + m / seg];
                // a cell may straddle two groups (k0 not a multiple of the cell width): it is A only if every group it touches is A,
                // carries a structure if any of them does, and uses the lower floor
                int g2 = kb2g[kb + nb - 1]; size_t id2 = (size_t)m * ng + g2; bool st = structMap[id] || structMap[id2];
                if (g2 != g) fl = min(fl, Fl[(size_t)g2 * nseg + m / seg]);
                int e = (int)ceil(log2(mx + 1)); S.cE[cid] = e; S.noF[cid] = st || fullBand;
                if (cls[id] == 0 && cls[id2] == 0) { S.cA[cid] = 1; continue; }
                int need;
                if (fullBand && depthAdapt && !FlM.empty()) { need = e - (int)floor(log2(max(1.0, FlM[(size_t)g * nseg + m / seg]))); S.nz[cid] = need <= 2 && !st; }   // depth: bits above the master's own floor
                else if (mx < 4.5 * fl && !st) { need = 0; S.nz[cid] = 1; }                                                             // noise-like: B
                else need = (int)ceil(e - log2(C * fl));                                                                                             // bits above the local floor
                int t = 1; while (t < 3 && TIERS[t] < need) t++;
                S.lab[cid] = t; } }
        if (getenv("TIMING")) printf("  [t %.1f s] cell scan done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
        part::Costs cost; for (int t = 0; t < 4; t++) for (int b = 0; b <= 32; b++) cost.bl[t][b] = 1.0 + b;   // first pass: generic code lengths
        vector<i32> Qall;
        // bisect: search the penalties for the mode's share targets; otherwise reuse the penalties found before (one DP pass)
        // Qall (all segments' planes together) is needed only by the closed loop; the final bisection does not build it (memory)
        auto solveAll = [&](bool bisect) { if (bisect) vector<i32>().swap(Qall); else Qall.assign((size_t)K * N, 0); for (auto& S : segs) {
            S.R = part::Result();   // the previous result is not needed by the search: free it first (memory)
            S.R = bisect ? part::runTarget(S.P, S.cA, S.cE, S.lab, S.noF, eq, cost, MD, wm, S.lam) : part::run(S.P, S.cA, S.cE, S.lab, S.noF, eq, cost, S.lam[0], S.lam[1], wm);
            if (!bisect) for (long m = 0; m < K; m++) for (int k = 0; k < S.R.bw; k++) Qall[(size_t)m * N + S.R.k0 + k] += S.R.Q[(size_t)m * S.R.bw + k]; } };   // each segment holds its own bins
        // closed loop on the tags themselves: every cell kept at its own tier (penalties effectively infinite), so a tag is raised
        // only where the content is damaged even at its own tier; the mode's demotion is applied afterwards (final bisection)
        for (auto& S : segs) { S.lam[0] = 1e9; S.lam[1] = 1e9; }
        auto refine = [&]() {   // code-length model measured from the chosen tiers (same binarisation as the coder)
            vector<vector<double>> h(4, vector<double>(33, 0.5));
            for (auto& S : segs) for (auto& lf : S.R.leaves) { if (!lf.tier) continue; int sh = part::shiftOf(lf.E, lf.tier);
                for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int kb; int nb = part::cellBins(S.P, c, kb);
                    for (int j = 0; j < nb; j++) { double q = floor(fabs((double)X[(size_t)m * N + kb + j]) / ldexp(1.0, sh) + 0.5 - TH); uint32_t qa = q > 0 ? (uint32_t)q : 0; int b = 0; while (qa >> b) b++; h[lf.tier][min(b, 32)]++; } } }
            for (int t = 1; t < 4; t++) { double tot = 0; for (double v : h[t]) tot += v; for (int b = 0; b <= 32; b++) cost.bl[t][b] = -log2(h[t][b] / tot); } };
        for (int it = 0; it < 3; it++) {
            auto TT = [&](const char* w) { if (getenv("TIMING")) printf("  [t %.1f s] %s\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count(), w); };
            TT("before DP"); if (it == 0) { solveAll(false); refine(); } solveAll(false); TT("after DP");   // closed loop with a first-guess penalty; the share targets are met afterwards
            // closed loop: a tag is raised only for real structure that is damaged at its own tier.
            //  - sustained tones: detected in the error of the decoded signal, kept only if the tone stands out of the master's floor;
            //  - transients: per cell, onset in the ORIGINAL (6 x its local median over time), damaged if the quantisation error
            //    exceeds a quarter of the original energy;
            //  - noise-like cells (no structure, peak < 4.5 x floor) are never raised.
            size_t nreal = 0; long raised = 0;
            {   vector<i32> y = inverse(Qall, L); vector<double> err(L); for (long i = 0; i < L; i++) err[i] = (double)x[i] - y[i]; vector<i32>().swap(y);
                Detector D; D.rate = rate; D.analyse(err); D.refP = &RefD.P; auto ev = damaged(D, D.detect()); int nbd = D.b1 - D.b0;
                vector<Ev> real;
                for (auto& e : ev) { int b = (int)round(e.f / (rate / D.NF)) - D.b0; int f0 = max(0, (int)(e.t0 / D.hop)), f1 = min(D.nfr - 1, (int)(e.t1 / D.hop)); double so = 0, sn = 0;
                    for (int f = f0; f <= f1; f++) { so += RefD.P[(size_t)f * nbd + b]; for (int d = 8; d <= 16; d++) { if (b + d < nbd) sn += RefD.P[(size_t)f * nbd + b + d]; if (b - d >= 0) sn += RefD.P[(size_t)f * nbd + b - d]; } }
                    if (so > 3.0 * sn / 18 && so / (f1 - f0 + 1) > 9.0) real.push_back(e); }
                nreal = real.size();
                for (auto& S : segs) {
                    vector<char> mark((size_t)K * S.P.nc, 0);
                    for (auto& e : real) { int ka = (int)((e.f - 3 * rate / D.NF) / (rate / 2 / N)), kb = (int)((e.f + 3 * rate / D.NF) / (rate / 2 / N)) + 1;
                        long ma = max(0L, (long)(e.t0 * rate / N) - 2), mb = min(K - 1, (long)(e.t1 * rate / N) + 3);
                        for (int k = max(ka, S.P.k0); k <= min(kb, S.P.k1 - 1); k++) { int c = (k - S.P.k0) / part::CW; for (long m = ma; m <= mb; m++) mark[(size_t)m * S.P.nc + c] = 1; } }
                    for (int c = 0; c < S.P.nc; c++) { int kb; int nb = part::cellBins(S.P, c, kb); vector<double> eo(K), en(K);
                        for (long m = 0; m < K; m++) { double so = 0, se = 0; for (int j = 0; j < nb; j++) { size_t ix = (size_t)m * N + kb + j; double w = X[ix], v = (double)X[ix] - S.R.Q[(size_t)m * S.R.bw + kb + j - S.R.k0]; so += w * w; se += v * v; } eo[m] = so; en[m] = se; }
                        vector<double> nbv; nbv.reserve(16); for (long m = 8; m < K - 8; m++) { nbv.clear(); for (long d = -8; d <= 8; d++) if (labs(d) > 1) nbv.push_back(eo[m + d]);
                            nth_element(nbv.begin(), nbv.begin() + nbv.size() / 2, nbv.end()); double medo = nbv[nbv.size() / 2];
                            if (eo[m] > 6 * medo && eo[m] > 16.0 && en[m] > 0.25 * eo[m]) mark[(size_t)m * S.P.nc + c] = 1; } }
                    for (size_t cid = 0; cid < mark.size(); cid++) if (mark[cid] && !S.cA[cid] && !S.nz[cid] && S.lab[cid] < 3) { S.lab[cid]++; S.noF[cid] = 1; raised++; } } }
            part::Share cn, en; long nl = 0; double bits = 0;
            for (auto& S : segs) { cn.dTot += S.R.cnt.dTot; cn.dKept += S.R.cnt.dKept; cn.cTot += S.R.cnt.cTot; cn.cKept += S.R.cnt.cKept;
                en.dTot += S.R.en.dTot; en.dKept += S.R.en.dKept; en.cTot += S.R.en.cTot; en.cKept += S.R.en.cKept; nl += (long)S.R.leaves.size(); bits += S.R.bits; }
            auto pc = [](double a, double b) { return b > 0 ? 100.0 * a / b : 100.0; };
            printf("PART mode %s (share by %s, %s, %zu segment(s)) pass %d: leaves %ld | est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy | lambda D %.3g C %.3g | damaged structures %zu, cells raised %ld\n",
                   MD.name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), eq ? "equal" : "unequal", segs.size(), it, nl, bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot),
                   segs[0].lam[0], segs[0].lam[1], nreal, raised);
            if (!raised) break; }
        if (getenv("TIMING")) printf("  [t %.1f s] closed loop done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
        solveAll(true);   // final: bisect the penalties so the kept D / C shares meet the mode's targets with the final tags
        { part::Share cn, en; double bits = 0; for (auto& S : segs) { cn.dTot += S.R.cnt.dTot; cn.dKept += S.R.cnt.dKept; cn.cTot += S.R.cnt.cTot; cn.cKept += S.R.cnt.cKept;
              en.dTot += S.R.en.dTot; en.dKept += S.R.en.dKept; en.cTot += S.R.en.cTot; en.cKept += S.R.en.cKept; bits += S.R.bits; }
          auto pc = [](double a, double b) { return b > 0 ? 100.0 * a / b : 100.0; };
          printf("PART FINAL mode %s (share by %s): est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy\n", MD.name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"),
                 bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot));
          if (getenv("TIMING")) printf("  [t %.1f s] final bisection done\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
          }
        for (size_t si = 0; si < segs.size(); si++) { Seg& S = segs[si]; string suf = segs.size() > 1 ? "." + to_string(si) : "";
            if (getenv("DUMP")) {
                int32_t hdr[11] = {0x50415254, (int32_t)K, N, S.P.k0, S.P.k1, (int32_t)rate, (int32_t)L, part::CW + 256 * part::ruleRef(), mi, eq ? 1 : 0, (int32_t)S.R.stream.size()};
                vector<int32_t> buf(hdr, hdr + 11); buf.insert(buf.end(), S.R.stream.begin(), S.R.stream.end()); encio::writeVec((string(getenv("DUMP")) + suf).c_str(), buf); }
            if (getenv("PLANE")) encio::writeBandPlane((string(getenv("PLANE")) + suf).c_str(), S.R.Q.data(), K, N, S.R.k0, S.R.bw); }   // the full K x N plane, as before
        if (getenv("SEGCOUNT")) encio::writeText(getenv("SEGCOUNT"), to_string(segs.size()) + "\n");
        printf("runtime %.1f s\n", chrono::duration<double>(chrono::steady_clock::now() - T0).count());
        return 0; }
    printf("lossy_v2: PART=<mode> is required\n"); return 1;
}
