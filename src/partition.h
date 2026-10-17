// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Partition modes.
// The coefficient plane of one channel (frames x bins k0..k1) is divided into cells of CW bins x 1 frame.
// Cells are grouped into fixed root blocks (ROOTF cells x ROOTT frames); each root is cut recursively, one binary
// cut at a time, in frequency or time, down to single cells. Cut positions are halves (equal) or any cell/frame position
// (unequal, sent as a plain fixed-length integer). Each leaf carries its own tier (A/B/C/D), exponent, zero-level index and
// line-wise coefficients.
// Tiers: a full scan tags every non-A cell B/C/D (most bits needed = D). A mode is defined by the share of the scanned D
// (and C) content that must stay in D (C or higher) blocks (see MODES below).
// The share is measured by cell count, by energy, or both (see lossy_v2.cpp). The encoder minimises the bits plus a
// penalty lambda per demoted D/C unit (dynamic programming per root), and bisects lambda until the share target is met.
// Step relative to the block's own maximum. A leaf is A only if all its cells are A (A is fixed before compression).
// Encoder side of the partition: precision-rule selection, dead zone, zero-level option, and the dynamic program that chooses
// the cut tree and the tiers. The format definitions it builds on are in partition_format.h.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdlib>
#include "partition_format.h"

namespace part {
inline int& ruleRef() { static int r = 0; return r; }   // rule used by the encoder for the layer being coded (set by lossy_v2)
inline int shiftOf(int E, int tier) { return shiftOfR(E, tier, ruleRef()); }
// Quantiser dead zone (encoder only) and reconstruction offset (encoder + decoder)
inline double deadzone() { return 0.30; }   // quantiser dead zone (encoder only)
inline bool& zeroSignOpt() { static bool b = false; return b; }

// ---- encoder search
struct Leaf { long m0, m1; int c0, c1, tier, E, zl; };
struct Share { double dTot = 0, dKept = 0, cTot = 0, cKept = 0; };
struct Result { std::vector<Leaf> leaves; std::vector<int32_t> stream; std::vector<int32_t> Q, Qn; double bits = 0; Share cnt, en; double zsum = 0; long zcnt = 0; };   // Q and Qn: reconstructed coefficient planes
struct Costs { double bl[4][33]; double tierBits = 1.6, expBits = 4.0, zlBits = 2.0; };

// one DP pass for given penalties (bits per demoted D / C unit; unit = one cell (wmode 0) or the mean energy of a D/C cell (wmode 1))
inline Result run(const Plane& P, const std::vector<char>& cellA, const std::vector<int>& cellE, const std::vector<int>& lab,
                  const std::vector<char>& noZL, bool equal, const Costs& cost, double lamD, double lamC, int wmode, double TH = -1) {
    if (TH < 0) TH = deadzone();
    Result R; R.Q.assign((size_t)P.K * P.N, 0); R.Qn.assign((size_t)P.K * P.N, 0);
    // energy normalisation: mean energy of D cells and of C cells over the whole plane
    double eD = 0, eC = 0; long nD = 0, nC = 0;
    for (long m = 0; m < P.K; m++) for (int c = 0; c < P.nc; c++) { size_t cid = (size_t)m * P.nc + c; if (cellA[cid] || lab[cid] < 2) continue; int kb; int nb = cellBins(P, c, kb); double e = 0;
        for (int j = 0; j < nb; j++) { double v = (*P.X)[(size_t)m * P.N + kb + j]; e += v * v; } if (lab[cid] == 3) { eD += e; nD++; } else { eC += e; nC++; } }
    eD = nD ? eD / nD : 1; eC = nC ? eC / nC : 1;
    const int RF = ROOTF, RT = ROOTT, D1 = RF + 1, D2 = RT + 1;
    auto ri = [&](int f0, int f1, int t0, int t1) { return (((size_t)f0 * D1 + f1) * D2 + t0) * D2 + t1; };
    // roots are independent: processed in parallel (pure computation, no global state); outputs are joined in root order
    struct RootOut { std::vector<int32_t> stream; std::vector<Leaf> leaves; double bits = 0; Share cnt, en; double zsum = 0; long zcnt = 0; };
    const long nRT = (P.K + RT - 1) / RT, nRF = (P.nc + RF - 1) / RF, nRoots = nRT * nRF; std::vector<RootOut> outs(nRoots);
    std::atomic<long> nextRoot(0);
    auto worker = [&]() {
    std::vector<double> best(D1 * D1 * D2 * D2), bbits(best.size()); std::vector<int> choice(best.size()), Emax(best.size()), ltier(best.size());
    for (long rix; (rix = nextRoot++) < nRoots;) { const long M0 = (rix / nRF) * RT; const int C0 = (int)(rix % nRF) * RF; RootOut& O = outs[rix];
        const int rf = std::min(RF, P.nc - C0), rt = (int)std::min<long>(RT, P.K - M0);
        int sHi = 0; for (int f = 0; f < rf; f++) for (int t = 0; t < rt; t++) sHi = std::max(sHi, cellE[(size_t)(M0 + t) * P.nc + C0 + f]);
        const int NS = sHi + 1, W = rf + 1, H = rt + 1;
        std::vector<double> pc((size_t)4 * NS * W * H, 0.0), pwD((size_t)W * H, 0.0), pwC((size_t)W * H, 0.0); std::vector<int> pn((size_t)W * H, 0), pa((size_t)W * H, 0);
        auto I = [&](int f, int t) { return (size_t)f * H + t; };
        for (int f = 0; f < rf; f++) for (int t = 0; t < rt; t++) {
            size_t cid = (size_t)(M0 + t) * P.nc + C0 + f; int kb; int nb = cellBins(P, C0 + f, kb); const int32_t* x = &(*P.X)[(size_t)(M0 + t) * P.N + kb];
            bool a = cellA[cid]; int e = cellE[cid]; double en = 0; for (int j = 0; j < nb; j++) en += (double)x[j] * x[j];
            for (int s = 0; s < NS; s++) { double c[4] = {0, 0, 0, 0};   // the quantiser output per step is shared by the three tiers: computed once,
                if (s > e) { for (int tr = 1; tr <= 3; tr++) c[tr] = nb * (cost.bl[tr][0] + (zeroSignOpt() ? 1 : 0)); }   // each tier keeps its own sum in the same order
                else { double st = std::ldexp(1.0, s);
                    for (int j = 0; j < nb; j++) { double q = std::floor(std::fabs((double)x[j]) / st + 0.5 - TH); if (q < 0) q = 0; uint32_t qa = (uint32_t)q; int b = 0; while (qa >> b) b++;
                        for (int tr = 1; tr <= 3; tr++) c[tr] += cost.bl[tr][std::min(b, 32)] + (b >= 2 ? b - 1 : 0) + ((q > 0 || zeroSignOpt()) ? 1 : 0); } }
                for (int tr = 1; tr <= 3; tr++) pc[((size_t)tr * NS + s) * W * H + I(f + 1, t + 1)] = c[tr]; }
            if (!a && lab[cid] == 3) pwD[I(f + 1, t + 1)] = wmode == 2 ? 0.5 * (1.0 + en / eD) : (wmode ? en / eD : 1.0);
            if (!a && lab[cid] == 2) pwC[I(f + 1, t + 1)] = wmode == 2 ? 0.5 * (1.0 + en / eC) : (wmode ? en / eC : 1.0);
            pn[I(f + 1, t + 1)] = a ? 0 : 1; pa[I(f + 1, t + 1)] = a ? 1 : 0; }
        auto prefix = [&](auto* arr) { for (int f = 1; f < W; f++) for (int t = 1; t < H; t++) arr[I(f, t)] += arr[I(f - 1, t)] + arr[I(f, t - 1)] - arr[I(f - 1, t - 1)]; };
        prefix(pn.data()); prefix(pa.data()); prefix(pwD.data()); prefix(pwC.data());
        for (int s = 0; s < NS; s++) for (int tr = 1; tr <= 3; tr++) prefix(&pc[((size_t)tr * NS + s) * W * H]);
        auto box = [&](auto* arr, int f0, int f1, int t0, int t1) { return arr[I(f1, t1)] - arr[I(f0, t1)] - arr[I(f1, t0)] + arr[I(f0, t0)]; };
        for (int w = 1; w <= rf; w++) for (int h = 1; h <= rt; h++) for (int f0 = 0; f0 + w <= rf; f0++) for (int t0 = 0; t0 + h <= rt; t0++) {
            int f1 = f0 + w, t1 = t0 + h; size_t id = ri(f0, f1, t0, t1);
            int E; if (w > 1) E = std::max(Emax[ri(f0, f1 - 1, t0, t1)], Emax[ri(f1 - 1, f1, t0, t1)]); else if (h > 1) E = std::max(Emax[ri(f0, f1, t0, t1 - 1)], Emax[ri(f0, f1, t1 - 1, t1)]);
            else E = cellE[(size_t)(M0 + t0) * P.nc + C0 + f0];
            Emax[id] = E;
            auto cf = cuts(w, equal), ct = cuts(h, equal); bool canSplit = !cf.empty() || !ct.empty();
            double splitFlag = canSplit ? 1.0 : 0.0, dirBit = (!cf.empty() && !ct.empty()) ? 1.0 : 0.0;
            int nonA = box(pn.data(), f0, f1, t0, t1), nA = box(pa.data(), f0, f1, t0, t1);
            double leaf = 1e300, leafBits = 0; int lt = 0;
            if (nonA == 0) { leaf = leafBits = splitFlag + cost.tierBits; lt = 0; }
            else if (nA == 0 || !canSplit) {
                double wD = box(pwD.data(), f0, f1, t0, t1), wC = box(pwC.data(), f0, f1, t0, t1);
                for (int tr = 1; tr <= 3; tr++) { int s = shiftOf(E, tr);
                    double b = splitFlag + cost.tierBits + cost.expBits + cost.zlBits + box(&pc[((size_t)tr * NS + s) * W * H], f0, f1, t0, t1);
                    double pen = (tr < 3 ? lamD * wD : 0) + (tr < 2 ? lamC * wC : 0);
                    if (b + pen < leaf) { leaf = b + pen; leafBits = b; lt = tr; } } }
            double bst = leaf, bb = leafBits; int ch = -1;
            for (size_t i = 0; i < cf.size(); i++) { int p = cf[i]; double sb = splitFlag + dirBit + posBits(cf.size()); double c = sb + best[ri(f0, f0 + p, t0, t1)] + best[ri(f0 + p, f1, t0, t1)]; if (c < bst) { bst = c; ch = p; bb = sb + bbits[ri(f0, f0 + p, t0, t1)] + bbits[ri(f0 + p, f1, t0, t1)]; } }
            for (size_t i = 0; i < ct.size(); i++) { int p = ct[i]; double sb = splitFlag + dirBit + posBits(ct.size()); double c = sb + best[ri(f0, f1, t0, t0 + p)] + best[ri(f0, f1, t0 + p, t1)]; if (c < bst) { bst = c; ch = 1000 + p; bb = sb + bbits[ri(f0, f1, t0, t0 + p)] + bbits[ri(f0, f1, t0 + p, t1)]; } }
            best[id] = bst; bbits[id] = bb; choice[id] = ch; ltier[id] = lt; }
        O.bits += bbits[ri(0, rf, 0, rt)];
        std::function<void(int, int, int, int)> emit = [&](int f0, int f1, int t0, int t1) {
            size_t id = ri(f0, f1, t0, t1); int w = f1 - f0, h = t1 - t0; auto cf = cuts(w, equal), ct = cuts(h, equal);
            bool canSplit = !cf.empty() || !ct.empty(); int ch = choice[id];
            if (canSplit) O.stream.push_back(ch >= 0 ? 1 : 0);
            if (ch >= 0) { int dir = ch >= 1000, p = ch % 1000; if (!cf.empty() && !ct.empty()) O.stream.push_back(dir);
                const auto& cc = dir ? ct : cf; O.stream.push_back((int32_t)(std::find(cc.begin(), cc.end(), p) - cc.begin()));
                if (!dir) { emit(f0, f0 + p, t0, t1); emit(f0 + p, f1, t0, t1); } else { emit(f0, f1, t0, t0 + p); emit(f0, f1, t0 + p, t1); }
                return; }
            Leaf lf{M0 + t0, M0 + t1, C0 + f0, C0 + f1, ltier[id], Emax[id], 0};
            O.stream.push_back(lf.tier);
            if (!lf.tier) { O.leaves.push_back(lf); return; }
            int s = shiftOf(lf.E, lf.tier); double st = std::ldexp(1.0, s); size_t qpos = O.stream.size();
            O.stream.push_back(lf.E); O.stream.push_back(0);   // zero-level index placeholder
            if (zeroSignOpt()) { double zsum = 0; long zn = 0;   // zero level of this leaf: mean |x| / step of its zero coefficients, nearest table entry
                for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int kb; int nb = cellBins(P, c, kb);
                    for (int j = 0; j < nb; j++) { double a = std::fabs((double)(*P.X)[(size_t)m * P.N + kb + j]) / st; if (std::floor(a + 0.5 - TH) <= 0) { zsum += a; zn++; } } }
                int bi = 0; if (zn) { double mu = zsum / zn; for (int i = 1; i < 8; i++) if (std::fabs(ZC[i] - mu) < std::fabs(ZC[bi] - mu)) bi = i; } lf.zl = bi; }
            for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int kb; int nb = cellBins(P, c, kb); size_t cid = (size_t)m * P.nc + c;
                double ce = 0; for (int j = 0; j < nb; j++) { double v = (*P.X)[(size_t)m * P.N + kb + j]; ce += v * v; }
                if (!cellA[cid] && lab[cid] == 3) { O.cnt.dTot++; O.en.dTot += ce; if (lf.tier == 3) { O.cnt.dKept++; O.en.dKept += ce; } }
                if (!cellA[cid] && lab[cid] == 2) { O.cnt.cTot++; O.en.cTot += ce; if (lf.tier >= 2) { O.cnt.cKept++; O.en.cKept += ce; } }
                for (int j = 0; j < nb; j++) { size_t ix = (size_t)m * P.N + kb + j; double v = (*P.X)[ix]; double q = std::floor(std::fabs(v) / st + 0.5 - TH); if (q < 0) q = 0;
                    int32_t qi = (int32_t)q * (v < 0 ? -1 : 1); O.stream.push_back(qi); R.Q[ix] = R.Qn[ix] = (int32_t)std::llround(recon(qi, st));
                    if (!qi && lf.zl) { int zs = v < 0; O.stream.push_back(zs); R.Q[ix] = R.Qn[ix] = (int32_t)std::llround((zs ? -1.0 : 1.0) * ZC[lf.zl] * st); }
                }
            }
            O.stream[qpos + 1] = lf.zl;   // the leaf's zero-level index (0 unless the zs option chose a level)
            O.leaves.push_back(lf); };
        emit(0, rf, 0, rt); } };
    int nth = getenv("PARTTHREADS") ? std::max(1, atoi(getenv("PARTTHREADS"))) : (int)std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool; for (int i = 1; i < nth; i++) pool.emplace_back(worker); worker(); for (auto& th : pool) th.join();
    for (auto& O : outs) { R.stream.insert(R.stream.end(), O.stream.begin(), O.stream.end()); R.leaves.insert(R.leaves.end(), O.leaves.begin(), O.leaves.end()); R.bits += O.bits;
        R.zsum += O.zsum; R.zcnt += O.zcnt; R.cnt.dTot += O.cnt.dTot; R.cnt.dKept += O.cnt.dKept; R.cnt.cTot += O.cnt.cTot; R.cnt.cKept += O.cnt.cKept;
        R.en.dTot += O.en.dTot; R.en.dKept += O.en.dKept; R.en.cTot += O.en.cTot; R.en.cKept += O.en.cKept; }
    return R; }

// share of D (or C) kept, in the chosen measure
// wm 0 = cell count, 1 = energy, 2 = both (the smaller of the two must meet the target: preservation mode)
inline double shareD(const Result& r, int wm) { auto f = [](const Share& s) { return s.dTot > 0 ? s.dKept / s.dTot : 1.0; }; return wm == 2 ? std::min(f(r.cnt), f(r.en)) : f(wm ? r.en : r.cnt); }
inline double shareC(const Result& r, int wm) { auto f = [](const Share& s) { return s.cTot > 0 ? s.cKept / s.cTot : 1.0; }; return wm == 2 ? std::min(f(r.cnt), f(r.en)) : f(wm ? r.en : r.cnt); }

// smallest penalties meeting the mode's targets: bisection on lambda (log scale, 7 steps), C first with lambda D = 0, then D.
// Raising lambda D only moves blocks up to tier D, which keeps every C cell in them at C or higher, so one round suffices.
inline Result runTarget(const Plane& P, const std::vector<char>& cellA, const std::vector<int>& cellE, const std::vector<int>& lab,
                        const std::vector<char>& noZL, bool equal, const Costs& cost, const Mode& md, int wm, double* lamOut = nullptr) {
    const double HARD = 1e9;
    double lamD = 0, lamC = md.cShare >= 1.0 ? HARD : 0;
    auto solve = [&](bool forD) {
        double target = forD ? md.dShare : md.cShare; if (!forD && md.cShare >= 1.0) return;
        Result r0 = run(P, cellA, cellE, lab, noZL, equal, cost, lamD, lamC, wm);
        if ((forD ? shareD(r0, wm) : shareC(r0, wm)) >= target) return;
        double lo = 1e-3, hi = 1e4;
        for (int it = 0; it < 7; it++) { double mid = std::sqrt(lo * hi); (forD ? lamD : lamC) = mid;
            Result r = run(P, cellA, cellE, lab, noZL, equal, cost, lamD, lamC, wm);
            if ((forD ? shareD(r, wm) : shareC(r, wm)) >= target) hi = mid; else lo = mid; }
        (forD ? lamD : lamC) = hi; };
    solve(false); solve(true);
    if (lamOut) { lamOut[0] = lamD; lamOut[1] = lamC; }
    return run(P, cellA, cellE, lab, noZL, equal, cost, lamD, lamC, wm); }
}   // namespace part
