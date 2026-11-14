// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.Cv;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Port of src/partition.h (namespace part, encoder side): precision-rule selection, dead zone, zero-level option, and the dynamic program
 * that chooses the cut tree and the tiers. The format definitions it builds on are in partition_format.h ({@link PartitionFormat}).
 * The coefficient plane of one channel (frames x bins k0..k1) is divided into cells of CW bins x 1 frame. Cells are grouped into fixed
 * root blocks (ROOTF cells x ROOTT frames); each root is cut recursively, one binary cut at a time, in frequency or time, down to single
 * cells. The encoder minimises the bits plus a penalty lambda per demoted D/C unit (dynamic programming per root).
 * C++ thread_local ruleRef() / zeroSignOpt() (set per layer by lossy_v2 and handed on to the worker threads) are explicit parameters here.
 * Roots are independent and processed in parallel (pure computation); outputs are joined in root order, so the result does not depend
 * on the number of threads.
 */
final class Partition {
    private Partition() {}

    // Quantiser dead zone (encoder only)
    // port of partition.h:deadzone
    static double deadzone() { return 0.30; }
    // port of partition.h:shiftOf (the rule of the layer being coded, C++ ruleRef())
    static int shiftOf(int E, int tier, int rule) { return PartitionFormat.shiftOfR(E, tier, rule); }
    // port of partition.h:bitLength: number of bits of v (0 for 0); v a uint32 value held in a long
    static int bitLength(long v) { return 64 - Long.numberOfLeadingZeros(v & 0xFFFFFFFFL); }

    /** port of partition_format.h:Plane: frames 0..K-1 of an N-bin plane X (row m at m * N), bins k0..k1-1, nc cells */
    static final class Plane { int[] X; long K; int N, k0, k1, nc;
        Plane(int[] x, long k, int n, int kk0, int kk1, int ncc) { X = x; K = k; N = n; k0 = kk0; k1 = kk1; nc = ncc; } }
    // port of partition_format.h:cellBins
    static int cellBins(Plane P, int c, int[] kb) { return PartitionFormat.cellBins(P.k0, P.k1, c, kb); }

    /** port of partition.h:Leaf */
    static final class Leaf { long m0, m1; int c0, c1, tier, E, zl;
        Leaf(long a, long b, int c, int d, int t, int e, int z) { m0 = a; m1 = b; c0 = c; c1 = d; tier = t; E = e; zl = z; } }
    /** port of partition.h:Share */
    static final class Share { double dTot = 0, dKept = 0, cTot = 0, cKept = 0;
        Share copy() { Share s = new Share(); s.dTot = dTot; s.dKept = dKept; s.cTot = cTot; s.cKept = cKept; return s; } }
    /** port of partition.h:Result. Q: reconstructed coefficients of this plane's bins only: Q[m * bw + k - k0] */
    static final class Result { List<Leaf> leaves = new ArrayList<Leaf>(); IntBuf stream = new IntBuf(); int[] Q = new int[0]; int k0 = 0, bw = 0; double bits = 0; Share cnt = new Share(), en = new Share(); double zsum = 0; long zcnt = 0; }
    /** port of partition.h:Costs */
    static final class Costs { double[][] bl = new double[4][33]; double tierBits = 1.6, expBits = 4.0, zlBits = 2.0; }
    /** port of partition.h:MeanE: mean energy of D cells and of C cells over the whole plane (depends only on the plane and the tags) */
    static final class MeanE { double eD = 1, eC = 1; }

    /** a growable int32 vector (std::vector&lt;int32_t&gt; push_back) */
    static final class IntBuf { int[] a = new int[64]; int n = 0;
        void add(int v) { if (n == a.length) a = java.util.Arrays.copyOf(a, a.length * 2); a[n++] = v; }
        void addAll(IntBuf o) { if (n + o.n > a.length) a = java.util.Arrays.copyOf(a, Math.max(a.length * 2, n + o.n)); System.arraycopy(o.a, 0, a, n, o.n); n += o.n; } }

    // port of partition.h:meanEnergies
    static MeanE meanEnergies(Plane P, byte[] cellA, int[] lab) {
        double eD = 0, eC = 0; long nD = 0, nC = 0; int[] kbo = new int[1];
        for (long m = 0; m < P.K; m++) for (int c = 0; c < P.nc; c++) { int cid = (int) (m * P.nc + c); if (cellA[cid] != 0 || lab[cid] < 2) continue; int nb = cellBins(P, c, kbo); int kb = kbo[0]; double e = 0;
            for (int j = 0; j < nb; j++) { double v = P.X[(int) (m * P.N + kb + j)]; e += v * v; } if (lab[cid] == 3) { eD += e; nD++; } else { eC += e; nC++; } }
        MeanE r = new MeanE(); r.eD = nD != 0 ? eD / nD : 1; r.eC = nC != 0 ? eC / nC : 1; return r; }

    /** the per-root output of run() (C++ RootOut) */
    static final class RootOut { IntBuf stream = new IntBuf(); List<Leaf> leaves = new ArrayList<Leaf>(); double bits = 0; Share cnt = new Share(), en = new Share(); double zsum = 0; long zcnt = 0; }

    // port of partition.h:run
    // seed: totals of the blocks before this one (block-wise encoder): bits and shares keep adding in root order, as in one whole-track call
    // full = false (the share search): only bits and shares are computed; no stream, leaves or planes (same tree, same shares)
    // mean: the plane's mean energies when the caller already has them (the share search runs with the same plane and tags many times)
    // rule, zs: the C++ thread_local ruleRef() and zeroSignOpt() of the calling thread; nth: worker threads (C++ PARTTHREADS or the hardware)
    static Result run(final Plane P, final byte[] cellA, final int[] cellE, final int[] lab, final boolean equal, final Costs cost, final double lamD, final double lamC,
                      final int wmode, double TH0, final boolean full, MeanE mean, Result seed, final int rule, final boolean zs, int nth) {
        if (TH0 < 0) TH0 = deadzone(); final double TH = TH0;
        final Result R = new Result(); if (full) { R.k0 = P.k0; R.bw = P.k1 - P.k0; R.Q = new int[(int) (P.K * R.bw)]; }
        final MeanE me = mean != null ? mean : meanEnergies(P, cellA, lab); final double eD = me.eD, eC = me.eC;
        final int RF = PartitionFormat.ROOTF, RT = PartitionFormat.ROOTT, D1 = RF + 1, D2 = RT + 1;
        final long nRT = (P.K + RT - 1) / RT, nRF = (P.nc + RF - 1) / RF, nRoots = nRT * nRF; final RootOut[] outs = new RootOut[(int) nRoots];
        for (int i = 0; i < outs.length; i++) outs[i] = new RootOut();
        final AtomicLong nextRoot = new AtomicLong(0);
        final int maxW = RF > RT ? RF : RT; final int[][] cutTab = new int[maxW + 1][]; for (int w = 0; w <= maxW; w++) cutTab[w] = PartitionFormat.cuts(w, equal);   // the cut positions per width, made once (not per block)
        // code length of one quantised value per tier, by (q > 0 or zero sign) and bit length: the same sum, term for term, as written out before
        final double[][][] term = new double[2][33][4]; for (int z = 0; z < 2; z++) for (int b = 0; b <= 32; b++) for (int tr = 1; tr <= 3; tr++) term[z][b][tr] = cost.bl[tr][b] + (b >= 2 ? b - 1 : 0) + z;
        final Object errMx = new Object(); final Throwable[] err = {null};
        Runnable safe = new Runnable() { @Override public void run() {
            try { new Worker(P, cellA, cellE, lab, cost, lamD, lamC, wmode, TH, full, eD, eC, rule, zs, cutTab, term, R, outs, nextRoot, nRoots, nRF, D1, D2).work(); }
            catch (Throwable t) { synchronized (errMx) { if (err[0] == null) err[0] = t; } nextRoot.set(nRoots); } } };
        // an exception in any worker (out of memory) stops the other workers and is re-thrown here after all have joined; if fewer threads can be
        // started, the started ones take all roots (the result does not depend on the number of threads)
        List<Thread> pool = new ArrayList<Thread>(); try { for (int i = 1; i < nth; i++) { Thread t = new Thread(safe); t.start(); pool.add(t); } } catch (Throwable t) { /* fewer threads */ }
        safe.run(); for (Thread t : pool) RoutesBlk.join(t); if (err[0] != null) throw RoutesBlk.rethrowable(err[0]);
        if (seed != null) { R.bits = seed.bits; R.zsum = seed.zsum; R.zcnt = seed.zcnt; R.cnt = seed.cnt.copy(); R.en = seed.en.copy(); }
        for (RootOut O : outs) { R.stream.addAll(O.stream); R.leaves.addAll(O.leaves); R.bits += O.bits;
            R.zsum += O.zsum; R.zcnt += O.zcnt; R.cnt.dTot += O.cnt.dTot; R.cnt.dKept += O.cnt.dKept; R.cnt.cTot += O.cnt.cTot; R.cnt.cKept += O.cnt.cKept;
            R.en.dTot += O.en.dTot; R.en.dKept += O.en.dKept; R.en.cTot += O.en.cTot; R.en.cKept += O.en.cKept; }
        return R; }

    /** port of partition.h:run lambda worker (its per-thread tables, allocated once per thread) and lambda emit */
    private static final class Worker {
        final Plane P; final byte[] cellA; final int[] cellE, lab; final Costs cost; final double lamD, lamC, TH, eD, eC; final int wmode, rule; final boolean full, zsNow;
        final int[][] cutTab; final double[][][] term; final Result R; final RootOut[] outs; final AtomicLong nextRoot; final long nRoots, nRF; final int D1, D2;
        final double[] best, bbits; final int[] choice, Emax, ltier; double[] pc = new double[0], pwD = new double[0], pwC = new double[0]; int[] pn = new int[0], pa = new int[0];
        final int[] kbo = new int[1]; final double[] ax = new double[PartitionFormat.CW];
        int H; long M0; int C0; RootOut O;
        Worker(Plane p, byte[] ca, int[] ce, int[] lb, Costs co, double lD, double lC, int wm, double th, boolean fu, double ed, double ec, int ru, boolean zs,
               int[][] ct, double[][][] tm, Result r, RootOut[] os, AtomicLong nr, long nR, long nrf, int d1, int d2) {
            P = p; cellA = ca; cellE = ce; lab = lb; cost = co; lamD = lD; lamC = lC; wmode = wm; TH = th; full = fu; eD = ed; eC = ec; rule = ru; zsNow = zs;
            cutTab = ct; term = tm; R = r; outs = os; nextRoot = nr; nRoots = nR; nRF = nrf; D1 = d1; D2 = d2;
            int sz = D1 * D1 * D2 * D2; best = new double[sz]; bbits = new double[sz]; choice = new int[sz]; Emax = new int[sz]; ltier = new int[sz]; }
        int ri(int f0, int f1, int t0, int t1) { return ((f0 * D1 + f1) * D2 + t0) * D2 + t1; }
        int I(int f, int t) { return f * H + t; }
        double boxD(double[] arr, int off, int f0, int f1, int t0, int t1) { return arr[off + I(f1, t1)] - arr[off + I(f0, t1)] - arr[off + I(f1, t0)] + arr[off + I(f0, t0)]; }
        int boxI(int[] arr, int f0, int f1, int t0, int t1) { return arr[I(f1, t1)] - arr[I(f0, t1)] - arr[I(f1, t0)] + arr[I(f0, t0)]; }
        void prefixD(double[] arr, int off, int W) { for (int f = 1; f < W; f++) for (int t = 1; t < H; t++) arr[off + I(f, t)] += arr[off + I(f - 1, t)] + arr[off + I(f, t - 1)] - arr[off + I(f - 1, t - 1)]; }
        void prefixI(int[] arr, int W) { for (int f = 1; f < W; f++) for (int t = 1; t < H; t++) arr[I(f, t)] += arr[I(f - 1, t)] + arr[I(f, t - 1)] - arr[I(f - 1, t - 1)]; }

        void work() {
            final int RF = PartitionFormat.ROOTF, RT = PartitionFormat.ROOTT; final int[] X = P.X; final int Nn = P.N;
            for (long rix; (rix = nextRoot.getAndIncrement()) < nRoots;) { M0 = (rix / nRF) * RT; C0 = (int) (rix % nRF) * RF; O = outs[(int) rix];
                final int rf = Math.min(RF, P.nc - C0), rt = (int) Math.min((long) RT, P.K - M0);
                int sHi = 0; for (int f = 0; f < rf; f++) for (int t = 0; t < rt; t++) sHi = Math.max(sHi, cellE[(int) ((M0 + t) * P.nc + C0 + f)]);
                final int NS = sHi + 1, W = rf + 1; H = rt + 1; final int WH = W * H;
                int npc = 4 * NS * WH; if (pc.length < npc) pc = new double[npc]; else java.util.Arrays.fill(pc, 0, npc, 0.0);   // buffers reused per block
                if (pwD.length < WH) { pwD = new double[WH]; pwC = new double[WH]; pn = new int[WH]; pa = new int[WH]; }
                else { java.util.Arrays.fill(pwD, 0, WH, 0.0); java.util.Arrays.fill(pwC, 0, WH, 0.0); java.util.Arrays.fill(pn, 0, WH, 0); java.util.Arrays.fill(pa, 0, WH, 0); }
                for (int f = 0; f < rf; f++) for (int t = 0; t < rt; t++) {
                    int cid = (int) ((M0 + t) * P.nc + C0 + f); int nb = cellBins(P, C0 + f, kbo); int kb = kbo[0]; final int xo = (int) ((M0 + t) * Nn + kb);
                    boolean a = cellA[cid] != 0; int e = cellE[cid]; double en = 0; for (int j = 0; j < nb; j++) en += (double) X[xo + j] * X[xo + j];
                    for (int j = 0; j < nb; j++) ax[j] = Math.abs((double) X[xo + j]);
                    for (int s = 0; s < NS; s++) { double c1 = 0, c2 = 0, c3 = 0;   // the quantiser output per step is shared by the three tiers: computed once,
                        if (s > e) { c1 = nb * (cost.bl[1][0] + (zsNow ? 1 : 0)); c2 = nb * (cost.bl[2][0] + (zsNow ? 1 : 0)); c3 = nb * (cost.bl[3][0] + (zsNow ? 1 : 0)); }   // each tier keeps its own sum in the same order
                        else { final double inv = Cv.ldexp(1.0, -s);   // |x| x 2^-s = |x| / 2^s exactly (power of two)
                            for (int j = 0; j < nb; j++) { double q = Math.floor(ax[j] * inv + 0.5 - TH); if (q < 0) q = 0; final int b = bitLength((long) q);
                                final double[] tm = term[q > 0 || zsNow ? 1 : 0][Math.min(b, 32)];
                                c1 += tm[1]; c2 += tm[2]; c3 += tm[3]; } }
                        int ix = I(f + 1, t + 1); pc[(1 * NS + s) * WH + ix] = c1; pc[(2 * NS + s) * WH + ix] = c2; pc[(3 * NS + s) * WH + ix] = c3; }
                    if (!a && lab[cid] == 3) pwD[I(f + 1, t + 1)] = wmode == 2 ? 0.5 * (1.0 + en / eD) : (wmode != 0 ? en / eD : 1.0);
                    if (!a && lab[cid] == 2) pwC[I(f + 1, t + 1)] = wmode == 2 ? 0.5 * (1.0 + en / eC) : (wmode != 0 ? en / eC : 1.0);
                    pn[I(f + 1, t + 1)] = a ? 0 : 1; pa[I(f + 1, t + 1)] = a ? 1 : 0; }
                prefixI(pn, W); prefixI(pa, W); prefixD(pwD, 0, W); prefixD(pwC, 0, W);
                for (int s = 0; s < NS; s++) for (int tr = 1; tr <= 3; tr++) prefixD(pc, (tr * NS + s) * WH, W);
                for (int w = 1; w <= rf; w++) for (int h = 1; h <= rt; h++) for (int f0 = 0; f0 + w <= rf; f0++) for (int t0 = 0; t0 + h <= rt; t0++) {
                    int f1 = f0 + w, t1 = t0 + h; int id = ri(f0, f1, t0, t1);
                    int E; if (w > 1) E = Math.max(Emax[ri(f0, f1 - 1, t0, t1)], Emax[ri(f1 - 1, f1, t0, t1)]); else if (h > 1) E = Math.max(Emax[ri(f0, f1, t0, t1 - 1)], Emax[ri(f0, f1, t1 - 1, t1)]);
                    else E = cellE[(int) ((M0 + t0) * P.nc + C0 + f0)];
                    Emax[id] = E;
                    final int[] cf = cutTab[w], ct = cutTab[h]; boolean canSplit = cf.length != 0 || ct.length != 0;
                    double splitFlag = canSplit ? 1.0 : 0.0, dirBit = (cf.length != 0 && ct.length != 0) ? 1.0 : 0.0;
                    int nonA = boxI(pn, f0, f1, t0, t1), nA = boxI(pa, f0, f1, t0, t1);
                    double leaf = 1e300, leafBits = 0; int lt = 0;
                    if (nonA == 0) { leaf = leafBits = splitFlag + cost.tierBits; lt = 0; }
                    else if (nA == 0 || !canSplit) {
                        double wD = boxD(pwD, 0, f0, f1, t0, t1), wC = boxD(pwC, 0, f0, f1, t0, t1);
                        for (int tr = 1; tr <= 3; tr++) { int s = shiftOf(E, tr, rule);
                            double b = splitFlag + cost.tierBits + cost.expBits + cost.zlBits + boxD(pc, (tr * NS + s) * WH, f0, f1, t0, t1);
                            double pen = (tr < 3 ? lamD * wD : 0) + (tr < 2 ? lamC * wC : 0);
                            if (b + pen < leaf) { leaf = b + pen; leafBits = b; lt = tr; } } }
                    double bst = leaf, bb = leafBits; int ch = -1;
                    final double sbF = splitFlag + dirBit + PartitionFormat.posBits(cf.length), sbT = splitFlag + dirBit + PartitionFormat.posBits(ct.length);   // the same for every cut of this block
                    for (int i = 0; i < cf.length; i++) { int p = cf[i]; final double sb = sbF; double c = sb + best[ri(f0, f0 + p, t0, t1)] + best[ri(f0 + p, f1, t0, t1)]; if (c < bst) { bst = c; ch = p; bb = sb + bbits[ri(f0, f0 + p, t0, t1)] + bbits[ri(f0 + p, f1, t0, t1)]; } }
                    for (int i = 0; i < ct.length; i++) { int p = ct[i]; final double sb = sbT; double c = sb + best[ri(f0, f1, t0, t0 + p)] + best[ri(f0, f1, t0 + p, t1)]; if (c < bst) { bst = c; ch = 1000 + p; bb = sb + bbits[ri(f0, f1, t0, t0 + p)] + bbits[ri(f0, f1, t0 + p, t1)]; } }
                    best[id] = bst; bbits[id] = bb; choice[id] = ch; ltier[id] = lt; }
                O.bits += bbits[ri(0, rf, 0, rt)];
                emit(0, rf, 0, rt); } }

        // port of partition.h:run lambda emit
        void emit(int f0, int f1, int t0, int t1) {
            int id = ri(f0, f1, t0, t1); int w = f1 - f0, h = t1 - t0; final int[] cf = cutTab[w], ct = cutTab[h];
            boolean canSplit = cf.length != 0 || ct.length != 0; int ch = choice[id];
            if (canSplit && full) O.stream.add(ch >= 0 ? 1 : 0);
            if (ch >= 0) { int dir = ch >= 1000 ? 1 : 0, p = ch % 1000; if (cf.length != 0 && ct.length != 0 && full) O.stream.add(dir);
                final int[] cc = dir != 0 ? ct : cf; if (full) { int idx = 0; while (idx < cc.length && cc[idx] != p) idx++; O.stream.add(idx); }
                if (dir == 0) { emit(f0, f0 + p, t0, t1); emit(f0 + p, f1, t0, t1); } else { emit(f0, f1, t0, t0 + p); emit(f0, f1, t0 + p, t1); }
                return; }
            Leaf lf = new Leaf(M0 + t0, M0 + t1, C0 + f0, C0 + f1, ltier[id], Emax[id], 0);
            final int[] X = P.X;
            if (!full) {   // the share search: the shares of this leaf only (same order and arithmetic as below)
                if (lf.tier == 0) return;
                for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int nb = cellBins(P, c, kbo); int kb = kbo[0]; int cid = (int) (m * P.nc + c);
                    double ce = 0; for (int j = 0; j < nb; j++) { double v = X[(int) (m * P.N + kb + j)]; ce += v * v; }
                    if (cellA[cid] == 0 && lab[cid] == 3) { O.cnt.dTot++; O.en.dTot += ce; if (lf.tier == 3) { O.cnt.dKept++; O.en.dKept += ce; } }
                    if (cellA[cid] == 0 && lab[cid] == 2) { O.cnt.cTot++; O.en.cTot += ce; if (lf.tier >= 2) { O.cnt.cKept++; O.en.cKept += ce; } } }
                return; }
            O.stream.add(lf.tier);
            if (lf.tier == 0) { O.leaves.add(lf); return; }
            int s = shiftOf(lf.E, lf.tier, rule); double st = Cv.ldexp(1.0, s); int qpos = O.stream.n;
            O.stream.add(lf.E); O.stream.add(0);   // zero-level index placeholder
            if (zsNow) { double zsum = 0; long zn = 0;   // zero level of this leaf: mean |x| / step of its zero coefficients, nearest table entry
                for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int nb = cellBins(P, c, kbo); int kb = kbo[0];
                    for (int j = 0; j < nb; j++) { double a = Math.abs((double) X[(int) (m * P.N + kb + j)]) / st; if (Math.floor(a + 0.5 - TH) <= 0) { zsum += a; zn++; } } }
                int bi = 0; if (zn != 0) { double mu = zsum / zn; for (int i = 1; i < 8; i++) if (Math.abs(PartitionFormat.ZC[i] - mu) < Math.abs(PartitionFormat.ZC[bi] - mu)) bi = i; } lf.zl = bi; }
            for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int nb = cellBins(P, c, kbo); int kb = kbo[0]; int cid = (int) (m * P.nc + c);
                double ce = 0; for (int j = 0; j < nb; j++) { double v = X[(int) (m * P.N + kb + j)]; ce += v * v; }
                if (cellA[cid] == 0 && lab[cid] == 3) { O.cnt.dTot++; O.en.dTot += ce; if (lf.tier == 3) { O.cnt.dKept++; O.en.dKept += ce; } }
                if (cellA[cid] == 0 && lab[cid] == 2) { O.cnt.cTot++; O.en.cTot += ce; if (lf.tier >= 2) { O.cnt.cKept++; O.en.cKept += ce; } }
                for (int j = 0; j < nb; j++) { int ix = (int) (m * P.N + kb + j), qx = (int) (m * R.bw + (kb + j - P.k0)); double v = X[ix]; double q = Math.floor(Math.abs(v) / st + 0.5 - TH); if (q < 0) q = 0;
                    int qi = Cv.d2i(q) * (v < 0 ? -1 : 1); O.stream.add(qi); R.Q[qx] = (int) Cv.llround(PartitionFormat.recon(qi, st));
                    if (qi == 0 && lf.zl != 0) { int zsg = v < 0 ? 1 : 0; O.stream.add(zsg); R.Q[qx] = (int) Cv.llround((zsg != 0 ? -1.0 : 1.0) * PartitionFormat.ZC[lf.zl] * st); }
                }
            }
            O.stream.a[qpos + 1] = lf.zl;   // the leaf's zero-level index (0 unless the zs option chose a level)
            O.leaves.add(lf); }
    }

    // port of partition.h:shareD: share of D kept, in the chosen measure (wm 0 = cell count, 1 = energy, 2 = both: the smaller must meet the target)
    static double shareD(Result r, int wm) { return wm == 2 ? Cv.min(fD(r.cnt), fD(r.en)) : fD(wm != 0 ? r.en : r.cnt); }
    private static double fD(Share s) { return s.dTot > 0 ? s.dKept / s.dTot : 1.0; }
    // port of partition.h:shareC
    static double shareC(Result r, int wm) { return wm == 2 ? Cv.min(fC(r.cnt), fC(r.en)) : fC(wm != 0 ? r.en : r.cnt); }
    private static double fC(Share s) { return s.cTot > 0 ? s.cKept / s.cTot : 1.0; }
}
