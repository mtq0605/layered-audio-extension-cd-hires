// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.util.Arrays;

import com.lae.core.Cv;
import com.lae.core.LMath;
import com.lae.core.LaeException;
import com.lae.core.U;

/**
 * Port of src/laes_codec_enc.h (namespace laes), the encoder side of the LAES layer coder (split from laes_codec.h, 2026-10-29; the code is
 * unchanged). The block syntax and the decoder are in Codec (laes_codec.h). Here: the bit writer, the residual cost and Rice parameter
 * search, the block plan (predictor, order, precision, shift, partition), the block writer and encodeLayer (block parameters chosen in
 * parallel threads, written in order).
 * The coder's choices use lmath::log2 / cospi (com.lae.core.LMath): identical on every platform (2026-11-03). No C library
 * transcendental function is used here.
 * Java: the C++ thread_local scratch vectors of residualCost / putResidual are a scratch array passed by the caller (planBlock /
 * writeBlock allocate one per block); the values are the same.
 */
public final class CodecEnc {
    private CodecEnc() {}

    /** port of laes_codec_enc.h:BitW (MSB first); b[0 .. len) are the whole bytes written so far, the bits of a byte not yet full stay in acc */
    public static final class BitW {
        public byte[] b = new byte[256]; public int len = 0; long acc = 0; int n = 0; public long bits = 0;
        // (std::vector::push_back)
        void push(int v) { if (len == b.length) b = Arrays.copyOf(b, b.length * 2); b[len++] = (byte) v; }
        /** the whole bytes as an array of their own */
        public byte[] bytes() { return Arrays.copyOf(b, len); }
        // the w low bits of v, most significant first; w <= 64
        // port of laes_codec_enc.h:BitW::put
        public void put(long v, int w) {
            if (w <= 0) return; bits += w;
            while (w > 0) { final int take = Math.min(w, 8 - n); acc = (acc << take) | ((v >>> (w - take)) & ((1L << take) - 1)); n += take; w -= take;
                if (n == 8) { push((int) acc); acc = 0; n = 0; } } }
        // port of laes_codec_enc.h:BitW::zeros  (q: uint64)
        public void zeros(long q) { while (q != 0) { final int t = (int) U.min(q, 32); put(0, t); q -= t; } }
        // port of laes_codec_enc.h:BitW::putS  (w <= 63)
        public void putS(long v, int w) { put(v & ((1L << w) - 1), w); }
        // port of laes_codec_enc.h:BitW::flush
        public void flush() { while (n != 0) put(0, 1); }
    }

    // ---- residual coding
    // bit length of u (0 for 0). widthOf(v) == max(1, bitLen(zz(v))) for every int64 v, so the width of a partition is max(1, bitLen(OR of its zz values))
    // port of laes_codec_enc.h:bitLen
    public static int bitLen(long u) { return 64 - Long.numberOfLeadingZeros(u); }
    // port of laes_codec_enc.h:riceCostU  (u[a .. a + n))
    static double riceCostU(long[] u, int a, int n, int k) { double b = 6; for (int i = 0; i < n; i++) b += U.toDouble(u[a + i] >>> k) + 1 + k; return b; }
    // the Rice parameter of one partition, from its zz values: start from the mean, then test neighbours exactly; escape compared too.
    // L5 (2026-11-03): the same choice and cost as before, computed faster. The mean is the double sum of the values in order; while every
    // partial sum is an integer below 2^53 that sum is exact and equals the integer sum, so the integer sum is used then (otherwise the double
    // loop as before). The five costs 6 + sum(u >> k) + n (1 + k) are exact integers; sum(u >> k) <= sum(u) >> klo, so when the integer sum is at
    // most 2^52 the former check "s[0] > 2^52" can never fire and the branch-free loop gives the same sums (otherwise the former loop runs).
    // port of laes_codec_enc.h:bestKu  (u[a .. a + n); cost[0] receives the cost)
    public static int bestKu(long[] u, int a, int n, double[] cost) {
        long sum = 0, orv = 0; for (int i = 0; i < n; i++) { sum += u[a + i]; orv |= u[a + i]; }
        final int rw = Math.max(1, bitLen(orv)); final long lim = 1L << 52;
        final boolean small = rw + bitLen(n) <= 63 && U.le(sum, lim);   // no wrap-around in sum, and sum <= 2^52 < 2^53
        double mean = 0; if (small) mean = U.toDouble(sum); else for (int i = 0; i < n; i++) mean += U.toDouble(u[a + i]);
        mean = n != 0 ? mean / n : 0;
        final int k0 = mean > 1 ? Cv.d2i(Math.floor(LMath.log2(mean))) : 0; int best = 0; cost[0] = 1e300;
        final int klo = Math.max(0, k0 - 2), khi = Math.min(62, k0 + 2);
        final long[] s = new long[5]; boolean exact = true;
        if (small) { final int nk = khi - klo + 1; for (int k = 0; k < nk; k++) { final int sh = klo + k; long t = 0; for (int i = 0; i < n; i++) t += u[a + i] >>> sh; s[k] = t; } }
        else for (int i = 0; i < n && exact; i++) { for (int k = klo; k <= khi; k++) { s[k - klo] += u[a + i] >>> k; } if (U.gt(s[0], lim)) exact = false; }
        for (int k = klo; k <= khi; k++) { double c;
            if (exact) c = U.toDouble(6 + s[k - klo] + (long) n * (long) (1 + k)); else c = riceCostU(u, a, n, k);
            if (c < cost[0]) { cost[0] = c; best = k; } }
        final double ce = 12 + (double) rw * n; if (rw <= 63 && ce < cost[0]) { cost[0] = ce; best = 63; } return best; }
    // port of laes_codec_enc.h:residualCost  (r[off .. off + n); po[0] receives the partition order; ub: scratch of at least n)
    public static double residualCost(long[] r, int off, int n, int[] po, long[] ub) { double best = 1e300; po[0] = 0;
        for (int i = 0; i < n; i++) ub[i] = Codec.zz(r[off + i]); final double[] cc = new double[1];
        for (int p = 0; p <= 8; p++) { final long parts = 1L << p; if (parts > n) break; final long len = n / parts; double c = 4;
            for (long q = 0; q < parts; q++) { final long a = q * len, e = (q == parts - 1) ? n : a + len; bestKu(ub, (int) a, (int) (e - a), cc); c += cc[0]; }
            if (c < best) { best = c; po[0] = p; } } return best; }
    // port of laes_codec_enc.h:putResidual  (r[off .. off + n))
    public static void putResidual(BitW w, long[] r, int off, int n, int po, long[] ub) { w.put(po, 4); final long parts = 1L << po, len = n / parts;
        for (int i = 0; i < n; i++) ub[i] = Codec.zz(r[off + i]); final double[] c = new double[1];
        for (long q = 0; q < parts; q++) { final int a = (int) (q * len), e = (int) ((q == parts - 1) ? n : a + len); final int k = bestKu(ub, a, e - a, c); w.put(k, 6);
            if (k == 63) { int rw = 1; for (int i = a; i < e; i++) rw = Math.max(rw, Codec.widthOf(r[off + i])); w.put(rw, 6); for (int i = a; i < e; i++) w.putS(r[off + i], rw); }
            else for (int i = a; i < e; i++) { final long u = Codec.zz(r[off + i]), qv = u >>> k; w.zeros(qv); w.put(1, 1); if (k != 0) w.put(u & ((1L << k) - 1), k); } } }
    // port of laes_codec_enc.h:lpcResidual  (x[off .. off + n), res[0 .. n))
    static void lpcResidual(long[] x, int off, int n, long[] c, int shift, long[] res, long maxAbsX) {
        final int o = c.length;
        if (Codec.lpcFits64(c, maxAbsX)) { for (int i = o; i < n; i++) res[i] = x[off + i] - Codec.lpcPred64(x, off + i, c, o, shift); }
        else for (int i = o; i < n; i++) res[i] = x[off + i] - Codec.lpcPred(x, off + i, c, shift); }

    // ---- encoder for one block: the choice (expensive, independent per block) and the writing
    /** port of laes_codec_enc.h:BlockPlan */
    public static final class BlockPlan { public int W = 1; public boolean same = false; public int type = 1, ord = 0, po = 0, shift = 0, prec = 0; public long[] c = new long[0]; }
    private static final int[] ORDERS = {1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
    private static final int[] PRECS = {12, 15};
    // throws if a sample needs more than kMaxW bits (LAES layers stay below about 2^41)
    // port of laes_codec_enc.h:planBlock  (x[off .. off + n))
    public static BlockPlan planBlock(long[] x, int off, int n) {
        BlockPlan P = new BlockPlan(); long orv = 0; boolean same = true; for (int i = 0; i < n; i++) { orv |= Codec.zz(x[off + i]); if (x[off + i] != x[off]) same = false; } final int W = Math.max(1, bitLen(orv));   // = max widthOf
        if (W > Codec.kMaxW) throw new LaeException("laes: sample wider than 62 bits");
        P.W = W; P.same = same; if (same) { P.type = 0; return P; }
        final long[] res = new long[n], ub = new long[n]; final int[] po = new int[1]; double bestBits = 2 + 6 + (double) W * n; final long mx = Codec.absMax(x, off, n);
        // fixed orders
        for (int o = 0; W <= Codec.kMaxWFixed && o <= 4 && o < n; o++) { for (int i = o; i < n; i++) res[i] = x[off + i] - Codec.fixedPred(x, off, i, o);
            final double c = 2 + 6 + 3 + (double) W * o + residualCost(res, o, n - o, po, ub);
            if (c < bestBits) { bestBits = c; P.type = 2; P.ord = o; P.po = po[0]; } }
        // LPC: Tukey(0.5) window, autocorrelation, Levinson; orders 1..32 (fewer than n)
        final int MAXO = W <= Codec.kMaxWLpc ? (int) Math.min(32, (long) n - 1) : 0;
        if (MAXO >= 1) { final double[] xw = new double[n], R = new double[MAXO + 1]; for (int i = 0; i < n; i++) { final double t = (double) i / (n - 1); double wv = 1;
                if (t < 0.25) wv = 0.5 * (1 - LMath.cospi(t / 0.25)); else if (t > 0.75) wv = 0.5 * (1 - LMath.cospi((1 - t) / 0.25)); xw[i] = (double) x[off + i] * wv; }
            for (int l = 0; l <= MAXO; l++) { double s = 0; for (int i = l; i < n; i++) s += xw[i] * xw[i - l]; R[l] = s; }
            if (R[0] > 0) { final double[][] A = new double[MAXO + 1][]; double[] a = new double[MAXO + 1], tmp; double err = R[0] * (1 + 1e-10);
                for (int m = 1; m <= MAXO; m++) { double acc = R[m]; for (int j = 1; j < m; j++) acc -= a[j] * R[m - j]; final double k = acc / err; tmp = a.clone(); a[m] = k; for (int j = 1; j < m; j++) a[j] = tmp[j] - k * tmp[m - j];
                    err *= (1 - k * k); A[m] = a.clone(); if (err <= 0) break; }
                for (int o : ORDERS) { if (o > MAXO || A[o] == null) continue;
                    for (int prec : PRECS) { double mxc = 0; for (int j = 1; j <= o; j++) mxc = Cv.max(mxc, Math.abs(A[o][j])); if (mxc <= 0) continue;
                        final int lg = Cv.d2i(Math.ceil(LMath.log2(mxc + 1e-300))); int shift = prec - 1 - lg; shift = Math.max(0, Math.min(31, shift));
                        final long[] c = new long[o]; final long lim = (1L << (prec - 1)) - 1; for (int j = 0; j < o; j++) { final double q = Math.floor(A[o][j + 1] * (double) (1L << shift) + 0.5); c[j] = Math.max(-lim - 1, Math.min(lim, Cv.d2l(q))); }
                        lpcResidual(x, off, n, c, shift, res, mx);
                        final double cst = 2 + 6 + 5 + 4 + 5 + (double) prec * o + (double) W * o + residualCost(res, o, n - o, po, ub);
                        if (cst < bestBits) { bestBits = cst; P.type = 3; P.ord = o; P.po = po[0]; P.shift = shift; P.prec = prec; P.c = c; } } } } }
        return P; }
    // port of laes_codec_enc.h:writeBlock  (x[off .. off + n))
    public static void writeBlock(BitW w, long[] x, int off, int n, BlockPlan P) {
        if (P.same) { w.put(0, 2); w.put(P.W, 6); w.putS(x[off], P.W); return; }
        w.put(P.type, 2); w.put(P.W, 6);
        if (P.type == 1) { for (int i = 0; i < n; i++) w.putS(x[off + i], P.W); return; }
        final long[] res = new long[n], ub = new long[n];
        if (P.type == 2) { w.put(P.ord, 3); for (int i = 0; i < P.ord; i++) w.putS(x[off + i], P.W); for (int i = P.ord; i < n; i++) res[i] = x[off + i] - Codec.fixedPred(x, off, i, P.ord); putResidual(w, res, P.ord, n - P.ord, P.po, ub); return; }
        w.put(P.ord - 1, 5); w.put(P.prec - 1, 4); w.put(P.shift, 5); for (int j = 0; j < P.ord; j++) w.putS(P.c[j], P.prec);
        for (int i = 0; i < P.ord; i++) w.putS(x[off + i], P.W); lpcResidual(x, off, n, P.c, P.shift, res, Codec.absMax(x, off, n)); putResidual(w, res, P.ord, n - P.ord, P.po, ub); }
    // port of laes_codec_enc.h:encodeBlock
    public static void encodeBlock(BitW w, long[] x, int off, int n) { writeBlock(w, x, off, n, planBlock(x, off, n)); }

    // ---- a whole layer (one channel): blocks of length B. The blocks' parameters are chosen in parallel threads (each block on its own), then written in order.
    // port of laes_codec_enc.h:encodeLayer  (bits[0] receives the number of bits before the final flush)
    public static byte[] encodeLayer(final long[] x, final long B, long[] bits, int threads) {
        final long N = x.length; final int nb = (int) ((N + B - 1) / B); final BlockPlan[] plans = new BlockPlan[Math.max(0, nb)];
        Par.forBlocks(nb, threads, new Par.Block() { public void run(int k) { final long s = (long) k * B; plans[k] = planBlock(x, (int) s, (int) Math.min(B, N - s)); } }, null);
        BitW w = new BitW(); for (int k = 0; k < nb; k++) { final long s = (long) k * B; writeBlock(w, x, (int) s, (int) Math.min(B, N - s), plans[k]); }
        bits[0] = w.bits; w.flush(); return w.bytes(); }
}
