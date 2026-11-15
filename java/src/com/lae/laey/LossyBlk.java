// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.Cv;
import com.lae.core.LMath;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * Port of src/lossy_blk.cpp. Block-wise layer coder (stage 1 of the block-wise encoder, 2026-10-29): the same output as lossy_v2.cpp,
 * byte for byte, with the memory of a few blocks plus small per-track records. The track is scanned block by block; each scan records
 * per-cell information and adds up whole-track summaries (floors, tone / transient events, mean energies, code-length histogram, shares);
 * the summaries decide the plan (tags, lambdas); then every block is coded by the plan and its piece of each stream is appended in time
 * order (one whole-track stream per segment, as before).
 * Blocks are whole floor segments and whole root rows (lcm(seg, 8) frames x LAE_BLOCKUNITS, set by the encoder's memory plan, default 4;
 * LAE_BLOCKUNITS=0: the whole track as one block). Every sum is added in the same order as in lossy_v2.cpp. Large intermediate data (the
 * coefficient plane X, the reconstructed plane of all segments) sit in temporary files next to DUMP; the cell records stay in memory
 * (3 bytes per cell).
 * usage: lossy &lt;raw s32le stereo&gt; &lt;rate&gt;; environment PART, FULLBAND, DEPTHADAPT, CHSEL, K0FRAC, OCTSPLIT, MASTERREF, RULE_D, RULE_F,
 * ZSIGN, PARTEQ, PARTW, DUMP, PLANE, SEGCOUNT, LAE_BLOCKUNITS, PARTTHREADS.
 * Sorts: std::sort / nth_element are applied to plain double / float values only; the value at a position of a sorted
 * multiset does not depend on the algorithm (no NaN can occur, and no -0.0: the values are sums of squares, square roots of them and
 * quotients of them by positive constants), so Arrays.sort gives the same values. The event list uses std::stable_sort by frequency
 * = Collections.sort (stable) with the same comparator.
 */
final class LossyBlk {
    private LossyBlk() {}

    static final int N = MdctTb.N;
    static final int[] TIERS = {0, 2, 4, 8, 8};

    /** port of lossy_blk.cpp:Ev */
    static final class Ev { double f, t0, t1, db; int W;
        Ev(double ff, double a, double b, double d, int w) { f = ff; t0 = a; t1 = b; db = d; W = w; }
        Ev copy() { return new Ev(f, t0, t1, db, W); } }

    /** port of lossy_blk.cpp:Source: the layer signal (CHSEL of a raw s32 stereo file) and the master reference (MASTERREF, plain 24-bit units) */
    static final class Source implements java.io.Closeable { RandomAccessFile f; long frames = 0; int mode = 0; String path; byte[] buf = new byte[0];   // mode: 0 L, 1 R, 2 S, 3 M, 4 master ref L, 5 master ref R
        // port of lossy_blk.cpp:Source::open
        void open(String p, int md) { path = p; mode = md; f = EncIo.open(p, "rb"); long n = EncIo.size(f); if (n < 0) throw EncIo.fail("cannot read the size of " + p); frames = n / 8; }
        // port of lossy_blk.cpp:Source::read: values of samples [a, b) as int64; outside [0, frames) zero (only within [0, limit) for the master reference)
        long[] read(long a, long b, long limit) {
            long[] out = new long[(int) (b - a)]; final long lim = limit < 0 ? frames : Math.min(limit, frames);
            final long ra = Math.max(a, 0L), rb = Math.min(b, lim); if (ra >= rb) return out;
            int nb = (int) (rb - ra) * 8; if (buf.length < nb) buf = new byte[nb];
            EncIo.seek(f, ra * 8, path);
            try { f.readFully(buf, 0, nb); } catch (IOException e) { throw EncIo.fail("read failed: " + path); }
            for (long i = ra; i < rb; i++) { int o = (int) (i - ra) * 8; final int l0 = le(buf, o), r0 = le(buf, o + 4); long v;
                if (mode >= 4) v = mode == 4 ? l0 : r0;
                else { final int l = l0 >> 8, r = r0 >> 8; v = mode == 1 ? r : mode == 2 ? (long) (l - r) : mode == 3 ? (long) (r + ((l - r) >> 1)) : (long) l; }
                out[(int) (i - a)] = v; }
            return out; }
        long[] read(long a, long b) { return read(a, b, -1); }
        static int le(byte[] b, int o) { return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF) << 16 | b[o + 3] << 24; }
        @Override public void close() { EncIo.close(f); f = null; } }

    /** port of lossy_blk.cpp:PlaneFile: a plane of K rows x N int32 in a temporary file (X or the reconstructed plane) */
    static final class PlaneFile implements java.io.Closeable { RandomAccessFile f; String path; long K = 0; EncEnv env; byte[] buf = new byte[0];
        // port of lossy_blk.cpp:PlaneFile::create
        void create(EncEnv e, String p, long k) { env = e; path = p; K = k; f = EncIo.open(p, "w+b"); }
        // port of lossy_blk.cpp:PlaneFile::~PlaneFile
        @Override public void close() { if (f != null) { EncIo.close(f); f = null; new File(path).delete(); } }
        // port of lossy_blk.cpp:PlaneFile::write
        void write(long m0, int[] d, long rows) { if (rows <= 0) return; EncIo.seek(f, m0 * N * 4, path); int n = (int) (rows * N);
            if (EncIo.testFail(env, path)) throw EncIo.fail("write failed (disk full?): " + path);
            if (buf.length < n * 4) buf = new byte[n * 4]; EncIo.toBytes(d, 0, n, buf);
            try { f.write(buf, 0, n * 4); } catch (IOException e) { throw EncIo.fail("write failed (disk full?): " + path); } }
        // port of lossy_blk.cpp:PlaneFile::read: rows outside [0, K) read as zero
        int[] read(long m0, long m1) {
            int[] out = new int[(int) ((m1 - m0) * N)]; final long a = Math.max(m0, 0L), b = Math.min(m1, K); if (a >= b) return out; EncIo.seek(f, a * N * 4, path);
            int n = (int) ((b - a) * N); if (buf.length < n * 4) buf = new byte[n * 4];
            try { f.readFully(buf, 0, n * 4); } catch (IOException e) { throw EncIo.fail("read failed: " + path); }
            EncIo.toInts(buf, 0, out, (int) ((a - m0) * N), n); return out; } }

    // port of lossy_blk.cpp:forwardFrames
    // MDCT of frames [m0, m1) (frame m covers padded samples [mN, mN + 2N) of [N zeros | signal | zeros]); the same arithmetic as mdct::forward
    static int[] forwardFrames(Source src, long L, long m0, long m1, long limit) {
        int[] X = new int[(int) ((m1 - m0) * N)]; if (m1 <= m0) return X; long[] x = src.read(m0 * N - N, m1 * N + N, limit);
        for (long i = 0; i < x.length; i++) { final long g = m0 * N - N + i; if (g < 0 || g >= L) x[(int) i] = 0; }
        double[] z = new double[2 * N], u = new double[N], c = new double[N], re = new double[N / 2], im = new double[N / 2]; final double[] WIN = MdctTb.WIN;
        for (long m = m0; m < m1; m++) { final int p = (int) ((m - m0) * N);   // p[n] = sample (m N + n) - N, zero outside
            for (int n = 0; n < 2 * N; n++) z[n] = (double) (int) x[p + n] * WIN[n];
            for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }
            MdctTb.dct4(u, c, re, im); for (int k = 0; k < N; k++) X[(int) ((m - m0) * N + k)] = (int) Cv.llround(c[k]); }
        return X; }

    // port of lossy_blk.cpp:forwardFramesD (2026-11-04): the same transform with the coefficients before rounding (noise= and g: the master's
    // plane and the layer's are subtracted exactly)
    static double[] forwardFramesD(Source src, long L, long m0, long m1) {
        double[] X = new double[(int) ((m1 - m0) * N)]; if (m1 <= m0) return X; long[] x = src.read(m0 * N - N, m1 * N + N);
        for (long i = 0; i < x.length; i++) { final long g = m0 * N - N + i; if (g < 0 || g >= L) x[(int) i] = 0; }
        double[] z = new double[2 * N], u = new double[N], c = new double[N], re = new double[N / 2], im = new double[N / 2]; final double[] WIN = MdctTb.WIN;
        for (long m = m0; m < m1; m++) { final int p = (int) ((m - m0) * N);
            for (int n = 0; n < 2 * N; n++) z[n] = (double) (int) x[p + n] * WIN[n];
            for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }
            MdctTb.dct4(u, c, re, im); System.arraycopy(c, 0, X, p, N); }
        return X; }

    // port of lossy_blk.cpp:main lambda marked: frame m, bin k inside one of the marked ranges [k0, k1) of that frame
    static boolean marked(List<List<int[]>> nzRows, long m, int k) { if (nzRows == null) return false; for (int[] r : nzRows.get((int) m)) if (k >= r[0] && k < r[1]) return true; return false; }

    // port of lossy_blk.cpp:inverseRange
    // samples [a, b) of the inverse MDCT of the plane rows (the same overlap-add order as mdct::inverse: frame m - 1, then frame m)
    static int[] inverseRange(PlaneFile Q, long K, long L, long a, long b) {
        int[] y = new int[(int) (b - a)]; if (b <= a) return y;
        final long p0 = a + N, p1 = b + N, mlo = Math.max(0L, p0 / N - 1), mhi = Math.min(K - 1, (p1 - 1) / N); if (mlo > mhi) return y;
        int[] rows = Q.read(mlo, mhi + 1); double[] acc = new double[(int) ((mhi - mlo + 2) * N)]; double[] c = new double[N], u = new double[N], re = new double[N / 2], im = new double[N / 2]; final double[] WIN = MdctTb.WIN;
        for (long m = mlo; m <= mhi; m++) { for (int k = 0; k < N; k++) c[k] = rows[(int) ((m - mlo) * N + k)];
            MdctTb.dct4(c, u, re, im); final int s = (int) ((m - mlo) * N);
            for (int n = 0; n < N / 2; n++) { acc[s + n] += u[N / 2 + n] * WIN[n]; acc[s + N / 2 + n] += -u[N - 1 - n] * WIN[N / 2 + n];
                acc[s + N + n] += -u[N / 2 - 1 - n] * WIN[N + n]; acc[s + 3 * N / 2 + n] += -u[n] * WIN[3 * N / 2 + n]; } }
        for (long i = a; i < b; i++) { final long p = i + N - mlo * N; if (i >= 0 && i < L && p >= 0 && p < acc.length) y[(int) (i - a)] = (int) Cv.llround(acc[(int) p]); }
        return y; }

    /** port of lossy_blk.cpp:SDet: sustained-tone detector (tone_scan.cpp Detector), fed one periodogram row at a time */
    static final class SDet {
        double rate = 0; int NF = 0, b0 = 0, b1 = 0, nb = 0, nfr = 0; double hop = 0, s2 = 0; double[] w;
        // port of lossy_blk.cpp:SDet::init
        void init(double r, long L, boolean fullband) { rate = r; NF = 1; while (NF < rate * 0.09) NF <<= 1;
            double nyq = rate / 2, base = fullband ? 0.0 : (rate >= 88200 ? (rate % 44100.0 == 0 ? 22050.0 : 24000.0) : nyq);
            b0 = Cv.d2i(Math.ceil(base / (rate / NF))) + 8; b1 = Cv.d2i(Math.floor(0.98 * nyq / (rate / NF))); w = new double[NF]; s2 = 0;
            for (int i = 0; i < NF; i++) { w[i] = 0.5 - 0.5 * LMath.cospi(2.0 * i / NF); s2 += w[i] * w[i]; }
            nfr = (int) U64div(L - NF, NF / 2) + 1; hop = (NF / 2) / rate; nb = b1 - b0; }
        // (int)((size_t)a / (size_t)b) of the C++ (unsigned 64-bit division, then the low 32 bits)
        static long U64div(long a, long b) { return com.lae.core.U.div(a, b); }
        // port of lossy_blk.cpp:SDet::row: x = samples [f hop, f hop + NF)
        float[] row(double[] x) { float[] P = new float[nb]; double[] re = new double[NF], im = new double[NF];
            for (int i = 0; i < NF; i++) { re[i] = x[i] * w[i]; im[i] = 0; } Fft.fft(re, im, NF);
            for (int b = b0; b < b1; b++) P[b - b0] = (float) ((re[b] * re[b] + im[b] * im[b]) / s2); return P; } }   // std::norm (MSVC): re re + im im

    /** port of lossy_blk.cpp:SDetect: detect() of tone_scan.cpp on rows fed in order (optionally with the reference rows of another signal) */
    static final class SDetect {
        SDet D; boolean useRef = false; int nch = 0; static final int CH = 32;
        static final class Scale { int W, nw; double T, corr; int next = 0; byte[] inRun; int[] s; double[] pk; List<List<Ev>> ev; }
        List<Scale> sc = new ArrayList<Scale>(); long base = 0; List<float[]> P = new ArrayList<float[]>(), R = new ArrayList<float[]>();   // rows [base, base + P.size())
        // port of lossy_blk.cpp:SDetect::init
        void init(SDet d, boolean ref) { D = d; useRef = ref; nch = d.nb / CH; final int[] Ws = {1, 2, 4, 8, 16}; sc.clear();
            for (int wi = 0; wi < 5; wi++) { Scale s = new Scale(); s.W = Ws[wi]; s.nw = d.nfr / s.W; double k = Cv.max(1.0, 0.75 * s.W); final double Z = 6.0;
                double q = 1 - 1 / (9 * k) + Z * Math.sqrt(1 / (9 * k)); s.T = q * q * q; double q2 = 1 - 1 / (9 * k); s.corr = 1 / (q2 * q2 * q2);
                s.inRun = new byte[d.nb]; s.s = new int[d.nb]; s.pk = new double[d.nb]; s.ev = new ArrayList<List<Ev>>(); for (int b = 0; b < d.nb; b++) s.ev.add(new ArrayList<Ev>()); sc.add(s); } }
        // port of lossy_blk.cpp:SDetect::window
        void window(Scale S, int wdx) {
            final int nb = D.nb, W = S.W; double[] avg = new double[nb], chm = new double[nch]; float[] tmp = new float[CH]; byte[] E = new byte[nb];
            for (int b = 0; b < nb; b++) { double s = 0; for (int j = 0; j < W; j++) s += P.get((int) (wdx * W + j - base))[b]; avg[b] = s / W; }
            for (int c = 0; c < nch; c++) { for (int j = 0; j < CH; j++) { int b2 = c * CH + j;
                    if (useRef) { double s = 0; for (int q = 0; q < W; q++) s += R.get((int) (wdx * W + q - base))[b2]; tmp[j] = (float) (s / W); } else tmp[j] = (float) avg[b2]; }
                Arrays.sort(tmp); chm[c] = tmp[CH / 2]; }   // nth_element (value only)
            for (int b = 0; b < nch * CH; b++) { int c = b / CH; double[] m = new double[5]; int n = 0; for (int d = -2; d <= 2; d++) { int cc = c + d; if (cc >= 0 && cc < nch) m[n++] = chm[cc]; }
                Arrays.sort(m, 0, n); double fl = m[n / 2] * S.corr; if (fl > 0 && avg[b] > S.T * fl) E[b] = 1; }
            for (int b = 1; b < nb - 1; b++) { final boolean hit = E[b] != 0 || E[b - 1] != 0 || E[b + 1] != 0;
                if (S.inRun[b] == 0) { if (E[b] != 0) { S.inRun[b] = 1; S.s[b] = wdx; S.pk[b] = Cv.max(0.0, avg[b]); } }
                else if (hit) S.pk[b] = Cv.max(S.pk[b], avg[b]);
                else { close(S, b, wdx); } } }
        // port of lossy_blk.cpp:SDetect::close
        void close(Scale S, int b, int wdx) { S.inRun[b] = 0; if (wdx - S.s[b] >= 2) { final double hop = D.hop;
                S.ev.get(b).add(new Ev((b + D.b0) * D.rate / D.NF, S.s[b] * S.W * hop, wdx * S.W * hop + (D.NF / D.rate), 10 * LMath.log10(S.pk[b] / Cv.max(1e-30, 1.0)), S.W)); } }
        // port of lossy_blk.cpp:SDetect::feed
        void feed(float[] p, float[] r) { P.add(p); if (useRef) R.add(r);
            final long have = base + P.size();
            for (Scale S : sc) while (S.next < S.nw && (long) (S.next + 1) * S.W <= have) window(S, S.next++);
            long keep = have; for (Scale S : sc) if (S.next < S.nw) keep = Math.min(keep, (long) S.next * S.W);
            if (keep > base) { final int drop = (int) (keep - base); P.subList(0, drop).clear(); if (useRef) R.subList(0, drop).clear(); base = keep; } }
        // port of lossy_blk.cpp:SDetect::finish
        List<Ev> finish() { List<Ev> ev = new ArrayList<Ev>();
            for (Scale S : sc) { if (S.nw < 2) continue; for (int b = 1; b < D.nb - 1; b++) if (S.inRun[b] != 0) close(S, b, S.nw);
                for (int b = 0; b < D.nb; b++) ev.addAll(S.ev.get(b)); }
            // merge as tone_scan.cpp; stable (2026-11-03): equal frequencies keep the order they were found in (scale, then bin, then time) on every platform
            Collections.sort(ev, new Comparator<Ev>() { @Override public int compare(Ev a, Ev b) { return a.f < b.f ? -1 : (b.f < a.f ? 1 : 0); } });
            List<Ev> out = new ArrayList<Ev>(); double df = 3 * D.rate / D.NF;
            for (Ev e : ev) { boolean m = false;
                for (Ev o : out) if (Math.abs(o.f - e.f) <= df && e.t0 <= o.t1 && o.t0 <= e.t1) { o.t0 = Cv.min(o.t0, e.t0); o.t1 = Cv.max(o.t1, e.t1); o.W = Math.min(o.W, e.W); m = true; break; }
                if (!m) out.add(e.copy()); }
            return out; } }

    /** port of lossy_blk.cpp:main struct Seg */
    static final class Seg { int k0, k1, nc; byte[] cE, lab, fl; double[] lam = {0, 0}; Partition.MeanE me = new Partition.MeanE(); }
    /** port of lossy_blk.cpp:main struct Search (phase 0: C r0, 1: C bisect, 2: D r0, 3: D bisect) */
    static final class Search { double lamD = 0, lamC = 0, lo = 0, hi = 0; int phase = 0, step = 0; boolean done = false; }
    /** port of lossy_blk.cpp:main struct Sum */
    static final class Sum { int b, f0, f1; double se = 0, so = 0, so2 = 0, sn = 0; }

    /** the variables of lossy_blk.cpp:main the lambdas capture */
    private static final class St {
        EncEnv env; double rate; long L, K, B, nblk, nseg, seg; int ng, nsg, wm, mi, rule, nth; boolean eq, zs, fullBand; int[] gS, gW, kb2g; Seg[] segs; PlaneFile Xf, Qf; Partition.Costs cost = new Partition.Costs();
        long[] blk(long j) { long m0 = j * B; return new long[] {m0, Math.min(K, m0 + B)}; }
        // port of lossy_blk.cpp:main lambda meanScan: mean energies of D and C cells per segment, in the order of part::meanEnergies (frame by frame, cell by cell)
        void meanScan() { double[] eD = new double[nsg], eC = new double[nsg]; long[] nD = new long[nsg], nC = new long[nsg];
            for (long j = 0; j < nblk; j++) { long[] mm = blk(j); long m0 = mm[0], m1 = mm[1]; int[] X = Xf.read(m0, m1);
                for (int si = 0; si < nsg; si++) { Seg S = segs[si]; for (long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { int cid = (int) (m * S.nc + c);
                        if ((S.fl[cid] & 1) != 0 || S.lab[cid] < 2) continue; int kb = S.k0 + c * PartitionFormat.CW; int nb = Math.min(S.k1, kb + PartitionFormat.CW) - kb; double e = 0;
                        for (int jj = 0; jj < nb; jj++) { double v = X[(int) ((m - m0) * N + kb + jj)]; e += v * v; } if (S.lab[cid] == 3) { eD[si] += e; nD[si]++; } else { eC[si] += e; nC[si]++; } } } }
            for (int si = 0; si < nsg; si++) { segs[si].me.eD = nD[si] != 0 ? eD[si] / nD[si] : 1; segs[si].me.eC = nC[si] != 0 ? eC[si] / nC[si] : 1; } }
        // port of lossy_blk.cpp:main lambda dpScan
        // one pass of run() over all blocks for the segments in 'which' with their lambdas; full: Q rows to the Q file (all segments together),
        // and the code-length histogram of the leaves when h != null
        void dpScan(int[] which, double[] lD, double[] lC, boolean full, double[][] h, Partition.Result[] tot, long[] nleaves, OutputPair[] dumps) {
            for (int i = 0; i < nsg; i++) { tot[i] = new Partition.Result(); nleaves[i] = 0; }
            int[] kbo = new int[1];
            for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = blk(j); long m0 = mm[0], m1 = mm[1]; int[] X = Xf.read(m0, m1); int[] Qall = null;
                if (full) Qall = new int[X.length];
                for (int wi = 0; wi < which.length; wi++) { final int si = which[wi]; Seg S = segs[si]; Partition.Plane P = new Partition.Plane(X, m1 - m0, N, S.k0, S.k1, S.nc);
                    final int n = (int) ((m1 - m0) * S.nc), o = (int) (m0 * S.nc); byte[] cA = new byte[n]; int[] cE = new int[n], lab = new int[n];   // cellsOf
                    for (int i = 0; i < n; i++) { cA[i] = (byte) (S.fl[o + i] & 1); cE[i] = S.cE[o + i] & 0xFF; lab[i] = S.lab[o + i]; }
                    Partition.Result R = Partition.run(P, cA, cE, lab, eq, cost, lD[wi], lC[wi], wm, -1, full, S.me, tot[si], rule, zs, nth);
                    tot[si].bits = R.bits; tot[si].cnt = R.cnt; tot[si].en = R.en; tot[si].zsum = R.zsum; tot[si].zcnt = R.zcnt; nleaves[si] += R.leaves.size();
                    if (h != null) for (Partition.Leaf lf : R.leaves) { if (lf.tier == 0) continue; int sh = Partition.shiftOf(lf.E, lf.tier, rule);
                        for (long m = lf.m0; m < lf.m1; m++) for (int c = lf.c0; c < lf.c1; c++) { int nb = Partition.cellBins(P, c, kbo); int kb = kbo[0];
                            for (int jj = 0; jj < nb; jj++) { double q = Math.floor(Math.abs((double) X[(int) (m * N + kb + jj)]) / Cv.ldexp(1.0, sh) + 0.5 - Partition.deadzone()); long qa = q > 0 ? Cv.d2ui(q) & 0xFFFFFFFFL : 0; int b = 0; while ((qa >>> b) != 0) b++; h[lf.tier][Math.min(b, 32)]++; } } }
                    if (full) { for (long m = 0; m < m1 - m0; m++) for (int k = 0; k < R.bw; k++) Qall[(int) (m * N + R.k0 + k)] += R.Q[(int) (m * R.bw + k)];
                        if (dumps != null) { OutputPair df = dumps[si]; if (R.stream.n != 0) df.putDump(R.stream.a, R.stream.n);
                            // the segment's plane (its own bins, zeros elsewhere) follows in the plane file of this segment, row by row
                            int[] row = new int[N];
                            for (long m = 0; m < m1 - m0; m++) { Arrays.fill(row, 0); for (int k = 0; k < R.bw; k++) row[R.k0 + k] = R.Q[(int) (m * R.bw + k)];
                                df.putPlane(row); } } } }
                if (full) Qf.write(m0, Qall, m1 - m0); } }
        double[] lamsD() { double[] v = new double[nsg]; for (int i = 0; i < nsg; i++) v[i] = segs[i].lam[0]; return v; }
        double[] lamsC() { double[] v = new double[nsg]; for (int i = 0; i < nsg; i++) v[i] = segs[i].lam[1]; return v; }
    }

    /** the dump and plane files of one segment (C++ FILE* files[si], files[nsg + si]) */
    static final class OutputPair { String dumpPath, planePath; BufferedOutputStream d, p; boolean okD = true, okP = true; long dBytes = 0; byte[] buf = new byte[0];
        void open(String dp, String pp) { dumpPath = dp; planePath = pp;
            try { d = new BufferedOutputStream(new FileOutputStream(dp), 1 << 16); } catch (IOException e) { throw EncIo.fail("cannot open " + dp); }
            try { p = new BufferedOutputStream(new FileOutputStream(pp), 1 << 16); } catch (IOException e) { throw EncIo.fail("cannot open " + pp); } }
        void putDump(int[] v, int n) { if (buf.length < n * 4) buf = new byte[n * 4]; EncIo.toBytes(v, 0, n, buf); try { d.write(buf, 0, n * 4); dBytes += n * 4L; } catch (IOException e) { throw EncIo.fail("write failed (disk full?): dump"); } }
        void putPlane(int[] row) { if (buf.length < row.length * 4) buf = new byte[row.length * 4]; EncIo.toBytes(row, 0, row.length, buf); try { p.write(buf, 0, row.length * 4); } catch (IOException e) { throw EncIo.fail("write failed (disk full?): plane"); } }
        void closeQuietly() { try { if (d != null) d.close(); } catch (IOException e) { /* failure path */ } try { if (p != null) p.close(); } catch (IOException e) { /* failure path */ } d = null; p = null; } }

    // port of lossy_blk.cpp:main
    static int main(final EncEnv env, String[] argv) {
        final long T0 = System.nanoTime(); final double rate = Fmt.atof(argv[2]);
        if (env.get("PART") == null) { env.printf("lossy_v2: PART=<mode> is required\n"); return 1; }
        final boolean fullBand = env.get("FULLBAND") != null, depthAdapt = env.get("DEPTHADAPT") != null;
        final String cs = env.get("CHSEL"); final char sel = cs != null ? cs.charAt(0) : 'L';
        final St st = new St(); st.env = env; st.rate = rate; st.fullBand = fullBand;
        Source src = new Source(), msrc = new Source(), nsrc = new Source(); PlaneFile Xf = new PlaneFile(), Qf = new PlaneFile(), Gf = new PlaneFile(), XTf = new PlaneFile(), MTf = new PlaneFile(), XGf = new PlaneFile(); OutputPair[] files = null; String gNote = "";
        try {
        src.open(argv[1], sel == 'R' ? 1 : sel == 'S' ? 2 : sel == 'M' ? 3 : 0); final long L = src.frames;
        final long K = MdctTb.frames(L); final int GW = 32;
        double baseNyq = rate % 44100.0 == 0 ? 22050.0 : 24000.0; double k0frac = env.get("K0FRAC") != null ? Fmt.atof(env.get("K0FRAC")) : 0.75;
        int k0 = fullBand ? 0 : Cv.d2i(Math.floor(k0frac * baseNyq / (rate / 2 / N)));
        if (!fullBand && env.get("NOISE") != null) k0 -= k0 % NoiseMarks.CW;   // noise=: the layer starts on the marking grid (cells = whole marked columns; at most 7 bins lower)
        List<Integer> gSl = new ArrayList<Integer>(), gWl = new ArrayList<Integer>(); { int k = k0; int first = (GW - k0 % GW) % GW; if (first != 0) { gSl.add(k); gWl.add(first); k += first; } for (; k + GW <= N; k += GW) { gSl.add(k); gWl.add(GW); } }
        final int ng = gSl.size(); final int[] gS = new int[ng], gW = new int[ng]; for (int g = 0; g < ng; g++) { gS[g] = gSl.get(g); gW[g] = gWl.get(g); }
        final int[] kb2g = new int[N]; Arrays.fill(kb2g, -1); for (int g = 0; g < ng; g++) for (int j = 0; j < gW[g]; j++) kb2g[gS[g] + j] = g;
        final long seg = Math.max(1L, Cv.d2i(rate / N)); final long nseg = (K + seg - 1) / seg;
        // blocks: whole floor segments and whole root rows
        long unit = seg; while (unit % 8 != 0) unit += seg; final String bu = env.get("LAE_BLOCKUNITS"); final long units = bu != null ? Fmt.atoll(bu) : 4;
        final long B = units > 0 ? unit * units : K; final long nblk = (K + B - 1) / B;
        final String tmpBase = (env.get("DUMP") != null ? env.get("DUMP") : argv[1]) + ".blk";
        Xf.create(env, tmpBase + ".X", K); Qf.create(env, tmpBase + ".Q", K);
        final boolean masterRef = fullBand && env.get("MASTERREF") != null; long nm = 0;
        if (masterRef) { final String cc = env.get("CHSEL"); msrc.open(env.get("MASTERREF"), (cc != null && cc.charAt(0) == 'R') ? 5 : 4); nm = msrc.frames; }
        // noise= and g (frequency layer only, 2026-11-04). G = the up-converted CD layer's part of a cell
        // = the master's coefficients - the layer's (both before rounding). noise=: the marked coefficients get the master taken as zero
        // (round(-G)), a cell marked in all its bins is A. g: an A cell whose G lies above the 24-bit rounding floor codes -G instead.
        // Neither: nothing here runs and the output is the earlier one, byte for byte.
        final String nzPath = fullBand ? null : env.get("NOISE"); final boolean gMode = !fullBand && env.get("GMODE") != null; final char gLv = gMode ? env.get("GMODE").charAt(0) : 'd';   // g strength: b, c or d (2026-11-05)
        final boolean nzOn = nzPath != null || gMode; List<List<int[]>> nzRows = null;   // per frame: the marked bins [k0, k1)
        if (nzOn) { final String mp = env.get("NOISEM"); if (mp == null) throw EncIo.fail("noise= / g: NOISEM (the padded master) is not set"); nsrc.open(mp, sel == 'S' ? 2 : 3);
            if (nsrc.frames != L) throw EncIo.fail("noise= / g: the padded master and the layer differ in length"); }
        if (nzPath != null) { NoiseMarks.Marks mk = new NoiseMarks.Marks(); String[] why = {""}; if (!NoiseMarks.readMarks(nzPath, mk, why)) throw EncIo.fail("cannot read the marking file " + nzPath + " (" + why[0] + ")");
            final long off = env.get("NOISEPAD") != null ? Fmt.atoll(env.get("NOISEPAD")) / N : 0; nzRows = new ArrayList<List<int[]>>((int) K); for (long m = 0; m < K; m++) nzRows.add(new ArrayList<int[]>());   // frame m of the master = frame m + P / N here
            for (NoiseMarks.Pos p : mk.pos) for (long m = p.m0; m < p.m1; m++) { final long mm = m + off; if (mm >= 0 && mm < K) nzRows.get((int) mm).add(new int[] {p.c0 * NoiseMarks.CW, p.c1 * NoiseMarks.CW}); } }
        // the 24-bit rounding of the decoded output per coefficient of this plane (orthonormal MDCT, so white noise keeps its variance):
        // L and R each 1/12 LSB^2; M = r + (l - r) / 2 -> 1/24; S = l - r -> 1/6
        final double gSig2 = sel == 'S' ? 1.0 / 6.0 : 1.0 / 24.0;
        if (gMode) { Gf.create(env, tmpBase + ".G", K); XTf.create(env, tmpBase + ".XT", K); MTf.create(env, tmpBase + ".MT", K); }   // G and the master's coefficients as float bits; round(-G)
        st.L = L; st.K = K; st.B = B; st.nblk = nblk; st.nseg = nseg; st.seg = seg; st.ng = ng; st.gS = gS; st.gW = gW; st.kb2g = kb2g; st.Xf = Xf; st.Qf = Qf;
        final int pt = env.partThreads(); st.nth = pt >= 0 ? Math.max(1, pt) : env.hwThreads;
        // ---- scan A: transform (to the X file), floors Fl / FlM, the layer signal's periodogram and its tone events
        final double[] Fl = new double[(int) (ng * nseg)]; double[] FlM = masterRef ? new double[(int) (ng * nseg)] : new double[0];
        final SDet RD = new SDet(); RD.init(rate, L, fullBand); SDetect detA = new SDetect(); detA.init(RD, false); long nextRow = 0;
        for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = st.blk(j); long m0 = mm[0], m1 = mm[1]; int[] X;
            if (!nzOn) X = forwardFrames(src, L, m0, m1, -1);
            else { double[] CF = forwardFramesD(src, L, m0, m1), CM = forwardFramesD(nsrc, L, m0, m1); X = new int[CF.length];
                int[] GT = null, XT = null, MT = null; if (gMode) { GT = new int[CF.length]; XT = new int[CF.length]; MT = new int[CF.length]; }
                for (long m = m0; m < m1; m++) for (int k = 0; k < N; k++) { final int i = (int) ((m - m0) * N + k); final double t = CF[i] - CM[i];   // -G
                    X[i] = marked(nzRows, m, k) ? (int) Cv.llround(t) : (int) Cv.llround(CF[i]);
                    if (gMode) { GT[i] = Float.floatToRawIntBits((float) (-t)); MT[i] = Float.floatToRawIntBits((float) CM[i]); XT[i] = (int) Cv.llround(t); } }
                if (gMode) { Gf.write(m0, GT, m1 - m0); XTf.write(m0, XT, m1 - m0); MTf.write(m0, MT, m1 - m0); } }
            Xf.write(m0, X, m1 - m0);
            for (long s = m0 / seg; s < (m1 + seg - 1) / seg && s < nseg; s++) for (int g = 0; g < ng; g++) { double[] r = new double[(int) (Math.min(K, (s + 1) * seg) - s * seg)]; int nr = 0;
                for (long m = s * seg; m < Math.min(K, (s + 1) * seg); m++) { double e = 0; for (int jj = 0; jj < gW[g]; jj++) { double v = X[(int) ((m - m0) * N + gS[g] + jj)]; e += v * v; } r[nr++] = Math.sqrt(e / gW[g]); }
                Arrays.sort(r); double fTime = r[r.length / 10];
                double[] sq = new double[r.length * gW[g]]; int ns = 0; for (long m = s * seg; m < Math.min(K, (s + 1) * seg); m++) for (int jj = 0; jj < gW[g]; jj++) { double v = X[(int) ((m - m0) * N + gS[g] + jj)]; sq[ns++] = v * v; }
                Arrays.sort(sq); double fSpec = Math.sqrt(sq[sq.length / 2] / 0.4549);   // nth_element (value only)
                Fl[(int) (g * nseg + s)] = Cv.max(Cv.min(fTime, fSpec), 0.3); }
            if (masterRef) { int[] XM = forwardFrames(msrc, L, m0, m1, Math.min(L, nm));
                for (long s = m0 / seg; s < (m1 + seg - 1) / seg && s < nseg; s++) for (int g = 0; g < ng; g++) { double[] r = new double[(int) (Math.min(K, (s + 1) * seg) - s * seg)]; int nr = 0;
                    for (long m = s * seg; m < Math.min(K, (s + 1) * seg); m++) { double e = 0; for (int jj = 0; jj < gW[g]; jj++) { double v = XM[(int) ((m - m0) * N + gS[g] + jj)]; e += v * v; } r[nr++] = Math.sqrt(e / gW[g]); }
                    Arrays.sort(r); FlM[(int) (g * nseg + s)] = Cv.max(r[r.length / 10], 0.3); } }
            nextRow = rowsUpTo(src, RD, detA, nextRow, j == nblk - 1 ? L : Math.min(L, m1 * N - N)); }
        if (masterRef) { double mn = 1e30, mxv = 0; for (double v : FlM) { mn = Cv.min(mn, v); mxv = Cv.max(mxv, v); } env.printf("master floor per band (24-bit LSB, coefficient rms): min %.1f max %.1f\n", mn, mxv); }
        final List<Ev> toneEv = detA.finish();
        // ---- segments (frequency) and the per-cell records (3 bytes per cell: exponent, tag, flags 1 = A, 2 = a cell of the structure, of the depth layer, raised by the closed loop or a G cell (a mark only: the partition does not read it, and with zs every leaf gets a zero level), 4 = noise-like)
        final String pm = env.get("PART"); { final String rv = env.get(fullBand ? "RULE_D" : "RULE_F"); st.rule = rv != null ? Fmt.atoi(rv) : 0; st.zs = env.get("ZSIGN") != null; }
        int mi = PartitionFormat.modeIndex(pm); if (mi < 0) { env.printf("unknown mode %s (max, 1, 3, 5, keep, keep20)\n", pm); return 1; }
        final PartitionFormat.Mode MD = PartitionFormat.MODES[mi]; boolean eq = env.get("PARTEQ") != null; int wm = env.get("PARTW") != null ? Fmt.atoi(env.get("PARTW")) : 0;
        if (mi >= PartitionFormat.MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
        st.eq = eq; st.wm = wm; st.mi = mi;
        List<Integer> edges = new ArrayList<Integer>(); edges.add(k0);
        if (env.get("OCTSPLIT") != null) for (double f = 2 * baseNyq; f < rate / 2 - 1; f *= 2) { int b = Cv.lround32(f / (rate / 2 / N)); if (b > edges.get(edges.size() - 1)) edges.add(b); }
        edges.add(N); final int nsg = edges.size() - 1; st.nsg = nsg;
        final Seg[] segs = new Seg[nsg]; for (int si = 0; si < nsg; si++) { Seg S = segs[si] = new Seg(); S.k0 = edges.get(si); S.k1 = edges.get(si + 1); S.nc = PartitionFormat.numCells(S.k0, S.k1);
            S.cE = new byte[(int) (K * S.nc)]; S.lab = new byte[(int) (K * S.nc)]; S.fl = new byte[(int) (K * S.nc)]; }
        st.segs = segs;
        final double C = 4.0;
        // ---- scan B: structure map (tone events + transients), cell classes, cell tags
        for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = st.blk(j); final long m0 = mm[0], m1 = mm[1]; final long a = Math.max(0L, m0 - 9), b = Math.min(K, m1 + 9);
            final int[] X = Xf.read(a, b);
            byte[] structMap = new byte[(int) ((m1 - m0) * ng)];
            for (Ev e : toneEv) { int ka = Cv.d2i((e.f - 2 * rate / RD.NF) / (rate / 2 / N)), kb = Cv.d2i((e.f + 2 * rate / RD.NF) / (rate / 2 / N)) + 1;
                long ma = Math.max(0L, Cv.d2l(e.t0 * rate / N) - 1), mb = Math.min(K - 1, Cv.d2l(e.t1 * rate / N) + 2); ma = Math.max(ma, m0); mb = Math.min(mb, m1 - 1);
                for (int k = ka; k <= kb; k++) { if (k < k0 || k >= N) continue; for (long m = ma; m <= mb; m++) structMap[(int) ((m - m0) * ng + kb2g[k])] = 1; } }
            double[] nbv = new double[16];
            for (int g = 0; g < ng; g++) for (int sb = 0; sb < gW[g]; sb += 4) {
                for (long m = Math.max(8L, m0 - 1); m < Math.min(K - 8, m1 + 1); m++) { int nn = 0; for (long d = -8; d <= 8; d++) if (Math.abs(d) > 1) nbv[nn++] = eo(X, a, gS[g], gW[g], sb, m + d);
                    Arrays.sort(nbv, 0, nn); final double e0 = eo(X, a, gS[g], gW[g], sb, m);   // nth_element (value only)
                    if (e0 > 6 * nbv[nn / 2] && e0 > 4 * 36.0) for (long d = -1; d <= 1; d++) { final long mm2 = m + d; if (mm2 >= m0 && mm2 < m1) structMap[(int) ((mm2 - m0) * ng + g)] = 1; } } }
            byte[] cls = new byte[(int) ((m1 - m0) * ng)];
            for (long m = m0; m < m1; m++) for (int g = 0; g < ng; g++) {
                double mx = 0; for (int jj = 0; jj < gW[g]; jj++) mx = Cv.max(mx, Math.abs((double) X[(int) ((m - a) * N + gS[g] + jj)]));
                int id = (int) ((m - m0) * ng + g); double fl = Fl[(int) (g * nseg + m / seg)];   // E[id] = ceil(log2(mx + 1)) of the C++ is not used further
                if (fl <= 1.5 && mx <= 8.0 && structMap[id] == 0) { cls[id] = 0; continue; }
                cls[id] = 2; }
            for (int si = 0; si < nsg; si++) { Seg S = segs[si];
                for (long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { int cid = (int) (m * S.nc + c); int kb = S.k0 + c * PartitionFormat.CW; int nb = Math.min(S.k1, kb + PartitionFormat.CW) - kb;
                    double mx = 0; for (int jj = 0; jj < nb; jj++) mx = Cv.max(mx, Math.abs((double) X[(int) ((m - a) * N + kb + jj)]));
                    int g = kb2g[kb]; int id = (int) ((m - m0) * ng + g); double fl = Fl[(int) (g * nseg + m / seg)];
                    int g2 = kb2g[kb + nb - 1]; int id2 = (int) ((m - m0) * ng + g2); boolean stt = structMap[id] != 0 || structMap[id2] != 0;
                    if (g2 != g) fl = Cv.min(fl, Fl[(int) (g2 * nseg + m / seg)]);
                    int e = Cv.d2i(Math.ceil(LMath.log2(mx + 1))); S.cE[cid] = (byte) e; int f = (stt || fullBand) ? 2 : 0;
                    if (cls[id] == 0 && cls[id2] == 0) { S.fl[cid] = (byte) (f | 1); S.lab[cid] = 0; continue; }
                    int need; boolean nz = false;
                    if (fullBand && depthAdapt && FlM.length != 0) { need = e - Cv.d2i(Math.floor(LMath.log2(Cv.max(1.0, FlM[(int) (g * nseg + m / seg)])))); nz = need <= 2 && !stt; }
                    else if (mx < 4.5 * fl && !stt) { need = 0; nz = true; }
                    else need = Cv.d2i(Math.ceil(e - LMath.log2(C * fl)));
                    int t = 1; while (t < 3 && TIERS[t] < need) t++;
                    S.lab[cid] = (byte) t; S.fl[cid] = (byte) (f | (nz ? 4 : 0)); } }
            if (nzRows != null) {   // noise=: a cell marked in every bin is deleted (A); the main pass is otherwise unchanged (g codes G in a pass of its own)
                for (int si = 0; si < nsg; si++) { Seg S = segs[si];
                    for (long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { int cid = (int) (m * S.nc + c); int kb = S.k0 + c * PartitionFormat.CW; int nb = Math.min(S.k1, kb + PartitionFormat.CW) - kb;
                        boolean all = true; for (int jj = 0; jj < nb && all; jj++) all = marked(nzRows, m, kb + jj);
                        if (all) { S.fl[cid] = (byte) ((S.fl[cid] & 2) | 1); S.lab[cid] = 0; } } } } }
        st.meanScan();
        // ---- DP scans
        for (int t = 0; t < 4; t++) for (int b = 0; b <= 32; b++) st.cost.bl[t][b] = 1.0 + b;
        final int[] allSeg = new int[nsg]; for (int i = 0; i < nsg; i++) allSeg[i] = i;
        for (Seg S : segs) { S.lam[0] = 1e9; S.lam[1] = 1e9; }
        final Partition.Result[] tot = new Partition.Result[nsg]; final long[] nl = new long[nsg];
        for (int it = 0; it < 3; it++) {
            if (it == 0) { double[][] h = new double[4][33]; for (double[] hr : h) Arrays.fill(hr, 0.5); st.dpScan(allSeg, st.lamsD(), st.lamsC(), true, h, tot, nl, null);
                for (int t = 1; t < 4; t++) { double tt = 0; for (double v : h[t]) tt += v; for (int b = 0; b <= 32; b++) st.cost.bl[t][b] = -LMath.log2(h[t][b] / tt); } }   // refine
            st.dpScan(allSeg, st.lamsD(), st.lamsC(), true, null, tot, nl, null);
            // closed loop, scan E: the coding error's periodogram (reference floor = the layer signal's) -> damaged events
            SDetect detE = new SDetect(); detE.init(RD, true); nextRow = 0;
            for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = st.blk(j); long m1 = mm[1]; long sampleEnd = j == nblk - 1 ? L : Math.min(L, m1 * N - N);
                while (nextRow < RD.nfr && nextRow * (RD.NF / 2) + RD.NF <= sampleEnd) { final long a = nextRow * (RD.NF / 2);   // errRows
                    long[] xv = src.read(a, a + RD.NF); int[] y = inverseRange(Qf, K, L, a, a + RD.NF);
                    double[] er = new double[RD.NF], xs = new double[RD.NF]; for (int i = 0; i < RD.NF; i++) { er[i] = (double) (int) xv[i] - y[i]; xs[i] = (double) xv[i]; }
                    detE.feed(RD.row(er), RD.row(xs)); nextRow++; } }
            List<Ev> evE = detE.finish();
            // scan S: the sums of damaged() and of the 'real' test over each event's span, frame by frame (the same order)
            List<Sum> sums = new ArrayList<Sum>(); final int nbd = RD.nb;
            for (Ev e : evE) { Sum s = new Sum(); s.b = Cv.d2i(Cv.round(e.f / (rate / RD.NF))) - RD.b0; s.f0 = Math.max(0, Cv.d2i(e.t0 / RD.hop)); s.f1 = Math.min(RD.nfr - 1, Cv.d2i(e.t1 / RD.hop)); sums.add(s); }
            for (long f = 0; f < RD.nfr; f++) { boolean any = false; for (Sum s : sums) if (f >= s.f0 && f <= s.f1) { any = true; break; } if (!any) continue;
                env.checkCancel(); final long a = f * (RD.NF / 2); long[] xv = src.read(a, a + RD.NF); int[] y = inverseRange(Qf, K, L, a, a + RD.NF);
                double[] er = new double[RD.NF], xs = new double[RD.NF]; for (int i = 0; i < RD.NF; i++) { er[i] = (double) (int) xv[i] - y[i]; xs[i] = (double) xv[i]; }
                float[] DP = RD.row(er), RP = RD.row(xs);
                for (Sum s : sums) { if (f < s.f0 || f > s.f1) continue;
                    for (int d = -1; d <= 1; d++) { int bb = s.b + d; if (bb < 0 || bb >= nbd) continue; s.se += DP[bb]; s.so += RP[bb]; }
                    s.so2 += RP[s.b]; for (int d = 8; d <= 16; d++) { if (s.b + d < nbd) s.sn += RP[s.b + d]; if (s.b - d >= 0) s.sn += RP[s.b - d]; } } }
            List<Ev> real = new ArrayList<Ev>();
            for (int i = 0; i < evE.size(); i++) { final Sum s = sums.get(i); if (!(s.se > 0.1 * s.so)) continue;   // damaged()
                if (s.so2 > 3.0 * s.sn / 18 && s.so2 / (s.f1 - s.f0 + 1) > 9.0) real.add(evE.get(i)); }
            final long nreal = real.size(); long raised = 0;
            // scan M: marks (real events, damaged transients per cell) and the raised tags
            for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = st.blk(j); final long m0 = mm[0], m1 = mm[1]; final long a = Math.max(0L, m0 - 8), b = Math.min(K, m1 + 8);
                final int[] X = Xf.read(a, b), Q = Qf.read(a, b);
                for (int si = 0; si < nsg; si++) { Seg S = segs[si]; byte[] mark = new byte[(int) ((m1 - m0) * S.nc)];
                    for (Ev e : real) { int ka = Cv.d2i((e.f - 3 * rate / RD.NF) / (rate / 2 / N)), kb = Cv.d2i((e.f + 3 * rate / RD.NF) / (rate / 2 / N)) + 1;
                        long ma = Math.max(0L, Cv.d2l(e.t0 * rate / N) - 2), mb = Math.min(K - 1, Cv.d2l(e.t1 * rate / N) + 3); ma = Math.max(ma, m0); mb = Math.min(mb, m1 - 1);
                        for (int k = Math.max(ka, S.k0); k <= Math.min(kb, S.k1 - 1); k++) { int c = (k - S.k0) / PartitionFormat.CW; for (long m = ma; m <= mb; m++) mark[(int) ((m - m0) * S.nc + c)] = 1; } }
                    double[] nbv = new double[16]; double[] oe = new double[2];
                    for (int c = 0; c < S.nc; c++) { int kb = S.k0 + c * PartitionFormat.CW; int nb = Math.min(S.k1, kb + PartitionFormat.CW) - kb;
                        for (long m = Math.max(8L, m0); m < Math.min(K - 8, m1); m++) { int nn = 0; for (long d = -8; d <= 8; d++) if (Math.abs(d) > 1) { eoen(X, Q, a, kb, nb, m + d, oe); nbv[nn++] = oe[0]; }
                            Arrays.sort(nbv, 0, nn); double medo = nbv[nn / 2]; eoen(X, Q, a, kb, nb, m, oe); double eo = oe[0], en = oe[1];   // nth_element (value only)
                            if (eo > 6 * medo && eo > 16.0 && en > 0.25 * eo) mark[(int) ((m - m0) * S.nc + c)] = 1; } }
                    for (int i = 0; i < mark.length; i++) { int cid = (int) (m0 * S.nc) + i;
                        if (mark[i] != 0 && (S.fl[cid] & 1) == 0 && (S.fl[cid] & 4) == 0 && S.lab[cid] < 3) { S.lab[cid]++; S.fl[cid] |= 2; raised++; } } } }
            st.meanScan();
            Partition.Share cn = new Partition.Share(), en = new Partition.Share(); long nlv = 0; double bits = 0;
            for (int si = 0; si < nsg; si++) { Partition.Result R = tot[si]; cn.dTot += R.cnt.dTot; cn.dKept += R.cnt.dKept; cn.cTot += R.cnt.cTot; cn.cKept += R.cnt.cKept;
                en.dTot += R.en.dTot; en.dKept += R.en.dKept; en.cTot += R.en.cTot; en.cKept += R.en.cKept; nlv += nl[si]; bits += R.bits; }
            env.printf("PART mode %s (share by %s, %s, %zu segment(s)) pass %d: leaves %ld | est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy | lambda D %.3g C %.3g | damaged structures %zu, cells raised %ld\n",
                       MD.name, wm == 2 ? "cells+energy" : (wm != 0 ? "energy" : "cells"), eq ? "equal" : "unequal", (long) nsg, it, nlv, bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot),
                       segs[0].lam[0], segs[0].lam[1], nreal, raised);
            if (raised == 0) break; }
        // ---- share search (runTarget of partition.h for every segment at once: each pass evaluates the next lambda of every segment)
        Search[] sr = new Search[nsg];
        for (int si = 0; si < nsg; si++) { Search s = sr[si] = new Search(); s.lamD = 0; s.lamC = MD.cShare >= 1.0 ? 1e9 : 0; s.phase = MD.cShare >= 1.0 ? 2 : 0; }
        for (;;) { List<Integer> wl = new ArrayList<Integer>();
            for (int si = 0; si < nsg; si++) { if (!sr[si].done) wl.add(si); }
            if (wl.isEmpty()) break;
            int[] which = new int[wl.size()]; double[] lD = new double[wl.size()], lC = new double[wl.size()];
            for (int i = 0; i < which.length; i++) { which[i] = wl.get(i); lD[i] = sr[which[i]].lamD; lC[i] = sr[which[i]].lamC; }
            st.dpScan(which, lD, lC, false, null, tot, nl, null);
            for (int si : which) { Search s = sr[si]; final Partition.Result r = tot[si]; final boolean forD = s.phase >= 2; final double target = forD ? MD.dShare : MD.cShare;
                if (s.phase == 0 || s.phase == 2) { if (share(r, forD, wm) >= target) { if (s.phase == 0) s.phase = 2; else s.done = true; continue; }
                    s.lo = 1e-3; s.hi = 1e4; s.step = 0; s.phase++; double mid = Math.sqrt(s.lo * s.hi); if (forD) s.lamD = mid; else s.lamC = mid; continue; }
                final double mid = forD ? s.lamD : s.lamC; if (share(r, forD, wm) >= target) s.hi = mid; else s.lo = mid;
                if (++s.step < 7) { double nm2 = Math.sqrt(s.lo * s.hi); if (forD) s.lamD = nm2; else s.lamC = nm2; continue; }
                if (forD) s.lamD = s.hi; else s.lamC = s.hi; if (s.phase == 1) s.phase = 2; else s.done = true; } }
        for (int si = 0; si < nsg; si++) { segs[si].lam[0] = sr[si].lamD; segs[si].lam[1] = sr[si].lamC; }
        // ---- final pass: every segment's stream and plane, block by block, appended in time order. Stream files first .. first + nsg - 1
        // (no suffix when the layer has one stream only); g adds a second pass of nsg streams after the first.
        final int nOut = gMode ? 2 * nsg : nsg;
        for (int pass = 0; pass < (gMode ? 2 : 1); pass++) { final int first = pass * nsg;
        if (pass == 1) {   // g (2026-11-04): the same layer coded a second time, G only. Its plane holds
            // round(-G) in the cells the first pass made A whose G lies above the 24-bit floor, zero elsewhere (A). A cell needing at most 4 bits is
            // C, a larger one D; neither is demoted (lambda 1e9), so every G cell is coded with step 1 (C: block exponent <= 4, D: always). Its
            // streams follow the first pass's, segment by segment with the same bins; the decoder adds a plane's streams (SPEC 9.5).
            // g exists for the up-converted CD layer's noise and touches nothing else (2026-11-05): -G only where the master is taken as zero, i.e.
            // marked in every bin (deleted) or silent itself (master at or below the 24-bit floor); an A cell whose master is not silent is left as it is
            final Seg[] cur = st.segs; final Seg[] gsegs = new Seg[nsg]; long gC = 0, gD = 0, gKept = 0;
            for (int si = 0; si < nsg; si++) { Seg T = gsegs[si] = new Seg(); final Seg S = cur[si]; T.k0 = S.k0; T.k1 = S.k1; T.nc = S.nc; T.lam[0] = T.lam[1] = 1e9;
                T.cE = new byte[(int) (K * T.nc)]; T.lab = new byte[(int) (K * T.nc)]; T.fl = new byte[(int) (K * T.nc)]; Arrays.fill(T.fl, (byte) 1); }
            XGf.create(env, tmpBase + ".XG", K);
            for (long j = 0; j < nblk; j++) { env.checkCancel(); long[] mm = st.blk(j); final long m0 = mm[0], m1 = mm[1]; final int[] GT = Gf.read(m0, m1), XT = XTf.read(m0, m1), MT = MTf.read(m0, m1);
                final int[] XG = new int[XT.length];
                for (int si = 0; si < nsg; si++) { final Seg S = cur[si]; final Seg T = gsegs[si];
                    for (long m = m0; m < m1; m++) for (int c = 0; c < S.nc; c++) { int cid = (int) (m * S.nc + c); if ((S.fl[cid] & 1) == 0) continue;
                        int kb = S.k0 + c * PartitionFormat.CW; int nb = Math.min(S.k1, kb + PartitionFormat.CW) - kb; double ge = 0, me = 0, mx = 0; boolean all = nzRows != null;
                        for (int jj = 0; jj < nb; jj++) { final int i = (int) ((m - m0) * N + kb + jj); final double g = Float.intBitsToFloat(GT[i]), mc = Float.intBitsToFloat(MT[i]);
                            ge += g * g; me += mc * mc; mx = Cv.max(mx, Math.abs((double) XT[i])); if (all && !marked(nzRows, m, kb + jj)) all = false; }
                        if (!(ge > gSig2 * nb)) continue;   // G at or below the 24-bit floor: stays A
                        if (!all && me > gSig2 * nb) { gKept++; continue; }   // not deleted and the master is not silent here: left as it is
                        final int e = Cv.d2i(Math.ceil(LMath.log2(mx + 1))); T.cE[cid] = (byte) e; T.lab[cid] = (byte) (gLv == 'b' ? 1 : gLv == 'c' ? 2 : (e <= 4 ? 2 : 3)); T.fl[cid] = 2;   // flag 2 marks a G cell; the partition does not read it. With zs these cells get a zero level like every leaf, by design: where a coefficient that g is responsible for becomes zero, the up-converted CD layer's noise would stay there, and zs reverses it (g itself makes no zero level)
                        if (e <= 4) gC++; else gD++;
                        for (int jj = 0; jj < nb; jj++) { final int i = (int) ((m - m0) * N + kb + jj); XG[i] = XT[i]; } } }
                XGf.write(m0, XG, m1 - m0); }
            st.segs = gsegs; { RandomAccessFile tf = Xf.f; Xf.f = XGf.f; XGf.f = tf; String tp = Xf.path; Xf.path = XGf.path; XGf.path = tp; } st.meanScan();
            gNote = Fmt.c("g: second pass of this layer: %ld G cells coded as C, %ld as D; %ld A cells left as they are (master not silent there)\n", gC, gD, gKept); }
        final Seg[] fsegs = st.segs;
        files = new OutputPair[nsg];
        for (int si = 0; si < nsg; si++) { String suf = nOut > 1 ? "." + (first + si) : "";
            String dumpPath = (env.get("DUMP") != null ? env.get("DUMP") : (tmpBase + ".dump")) + suf, planePath = (env.get("PLANE") != null ? env.get("PLANE") : (tmpBase + ".plane")) + suf;
            files[si] = new OutputPair(); files[si].open(dumpPath, planePath);
            int[] hdr = {0x50415254, (int) K, N, fsegs[si].k0, fsegs[si].k1, Cv.d2i(rate), (int) L, PartitionFormat.CW + 256 * st.rule, mi, eq ? 1 : 0, 0};
            files[si].putDump(hdr, 11); }
        st.dpScan(allSeg, st.lamsD(), st.lamsC(), true, null, tot, nl, files);
        for (int si = 0; si < nsg; si++) {   // the token count goes into header word 10; a failed write removes the files and stops
            OutputPair op = files[si]; long bytes = op.dBytes; int ntok = (int) ((bytes - 44) / 4); boolean ok = true, okp = true;
            try { op.d.close(); } catch (IOException e) { ok = false; } op.d = null;
            if (ok) { RandomAccessFile f = null; try { f = new RandomAccessFile(op.dumpPath, "rw"); f.seek(40); byte[] b4 = new byte[4]; com.lae.core.Le.put32(b4, 0, ntok); f.write(b4); } catch (IOException e) { ok = false; } finally { if (f != null) try { f.close(); } catch (IOException e) { ok = false; } } }
            ok = ok && !EncIo.testFail(env, op.dumpPath);
            okp = !EncIo.testFail(env, op.planePath); try { op.p.close(); } catch (IOException e) { okp = false; } op.p = null;
            if (!ok || !okp) { new File(op.dumpPath).delete(); new File(op.planePath).delete(); throw EncIo.fail("write failed (disk full?): " + (ok ? op.planePath : op.dumpPath)); } }
        files = null;
        if (pass == 0) { Partition.Share cn = new Partition.Share(), en = new Partition.Share(); double bits = 0; for (int si = 0; si < nsg; si++) { Partition.Result R = tot[si]; cn.dTot += R.cnt.dTot; cn.dKept += R.cnt.dKept; cn.cTot += R.cnt.cTot; cn.cKept += R.cnt.cKept;
              en.dTot += R.en.dTot; en.dKept += R.en.dKept; en.cTot += R.en.cTot; en.cKept += R.en.cKept; bits += R.bits; }
          env.printf("PART FINAL mode %s (share by %s): est %.1f kb/s | kept D: %.1f%% of cells, %.1f%% of energy | kept C: %.1f%% of cells, %.1f%% of energy\n", MD.name, wm == 2 ? "cells+energy" : (wm != 0 ? "energy" : "cells"),
                     bits / (L / rate) / 1000, pc(cn.dKept, cn.dTot), pc(en.dKept, en.dTot), pc(cn.cKept, cn.cTot), pc(en.cKept, en.cTot)); }
        if (pass == 1) env.printf("%s", gNote); }
        if (env.get("SEGCOUNT") != null) EncIo.writeText(env, env.get("SEGCOUNT"), nOut + "\n");
        env.printf("runtime %.1f s\n", (System.nanoTime() - T0) / 1e9);
        return 0; }
        finally { if (files != null) for (OutputPair op : files) if (op != null) op.closeQuietly(); Xf.close(); Qf.close(); Gf.close(); XTf.close(); MTf.close(); XGf.close(); src.close(); msrc.close(); nsrc.close(); } }

    // port of lossy_blk.cpp:main lambda pc
    static double pc(double a, double b) { return b > 0 ? 100.0 * a / b : 100.0; }
    // port of lossy_blk.cpp:main lambda share
    static double share(Partition.Result r, boolean forD, int wm) { return forD ? Partition.shareD(r, wm) : Partition.shareC(r, wm); }
    // port of lossy_blk.cpp:main lambda eo (scan B): energy of the 4-bin sub-band sb of group g in frame m (X window starts at frame a)
    static double eo(int[] X, long a, int gs, int gw, int sb, long m) { double so = 0; for (int jj = sb; jj < Math.min(sb + 4, gw); jj++) { double wv = X[(int) ((m - a) * N + gs + jj)]; so += wv * wv; } return so; }
    // port of lossy_blk.cpp:main lambda eoen (scan M): cell energy so and coding-error energy se of frame m
    static void eoen(int[] X, int[] Q, long a, int kb, int nb, long m, double[] r) { double so = 0, se = 0; for (int jj = 0; jj < nb; jj++) { int ix = (int) ((m - a) * N + kb + jj); double wv = X[ix], v = (double) X[ix] - Q[ix]; so += wv * wv; se += v * v; } r[0] = so; r[1] = se; }
    // port of lossy_blk.cpp:main lambda rowsUpTo with xsig: feeds every periodogram row whose samples end before sampleEnd; returns the next row
    static long rowsUpTo(Source src, SDet RD, SDetect det, long nextRow, long sampleEnd) {
        while (nextRow < RD.nfr && nextRow * (RD.NF / 2) + RD.NF <= sampleEnd) { long[] v = src.read(nextRow * (RD.NF / 2), nextRow * (RD.NF / 2) + RD.NF); double[] xs = new double[v.length]; for (int i = 0; i < v.length; i++) xs[i] = (double) v[i];
            det.feed(RD.row(xs), new float[0]); nextRow++; }
        return nextRow; }
}
