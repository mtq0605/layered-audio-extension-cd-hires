// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import com.lae.core.U;

/**
 * Port of src/laes_codec.h (namespace laes), decoder side. LAES layer coder, block syntax and decoder (S2 draft, 2026-10-28). One
 * channel of one layer = a sequence of blocks; each block is coded on its own:
 *   block header (fixed-length fields only):
 *     type 2 bits: 0 = constant, 1 = verbatim, 2 = fixed predictor, 3 = LPC
 *     W 6 bits: sample width (two's complement bits needed for every sample of the block, 1..62; fixed only if W <= 58, LPC only if W <= 44)
 *     constant: one W-bit value
 *     verbatim: n W-bit values
 *     fixed:    order 3 bits (0..4); order W-bit warm-up samples; residual
 *     LPC:      order-1 5 bits (1..32); precision-1 4 bits (coefficient bits 1..16); shift 5 bits (0..31);
 *               order coefficients (precision bits, two's complement); order W-bit warm-up samples; residual
 *   residual: partition order 4 bits (0..8, 2^po partitions; block length must be divisible or the last partition takes the rest);
 *             per partition a 6-bit Rice parameter k (0..62); k = 63 = escape (all fields fixed-length): 6-bit raw width r, then r-bit two's complement values;
 *             Rice: zigzag u = 2v (v >= 0) or -2v-1 (v < 0); unary q = u >> k (q zeros then a one), then k low bits.
 *   prediction: fixed (FLAC orders 0..4) or LPC: pred = floor( sum_j c[j] * x[i-1-j] / 2^shift ) (128-bit accumulation), residual = x - pred.
 * Every channel is coded on its own (no inter-channel decorrelation).
 * The 32-bit-build forms lpcPred32 / lpcPredS (sizeof(void*) == 4) are not used on x64 and are not ported; they give the same values.
 */
public final class Codec {
    private Codec() {}

    /**
     * A bit reader with get / getS / fail (the template parameter R of the C++). unary() is the bit-by-bit form of laes::unaryOf; a reader
     * with a faster unary of its own (Play.BitRS) overrides it with the same value and position.
     */
    public abstract static class BitIn {
        public boolean fail = false;
        // Java (2026-11-04): the residual array of decodeBlock, kept by the reader (one block at a time; values [order, n) are written before use)
        private long[] rbuf = null;
        long[] resBuf(int n) { if (rbuf == null || rbuf.length < n) rbuf = new long[n]; return rbuf; }
        public abstract long get(int w);
        // port of laes_codec.h:BitR::getS (the same in laesp::BitRS::getS)
        public long getS(int w) { if (w < 1 || w > 63) { fail = true; return 0; } long v = get(w); if (((v >>> (w - 1)) & 1) != 0) v |= -1L << w; return v; }
        // the quotient of a Rice code: the zeros before the next 1 (consumed too); fails when it exceeds qmax (uint64) or the data ends
        // port of laes_codec.h:unaryOf (the bit-by-bit overload)
        public long unary(long qmax) { long qv = 0; while (!fail && get(1) == 0) { if (U.gt(++qv, qmax)) fail = true; } return qv; }
    }

    /** port of laes_codec.h:BitR (MSB first over a byte array) */
    public static final class BitR extends BitIn {
        final byte[] b; final int bOff; final long bLen; long pos = 0;
        public BitR(byte[] b, int off, int len) { this.b = b; this.bOff = off; this.bLen = len; }
        // port of laes_codec.h:BitR::left
        long left() { return bLen * 8 - pos; }
        // port of laes_codec.h:BitR::get
        public long get(int w) { if (fail || w < 0 || w > 64 || (long) w > left()) { fail = true; return 0; } long v = 0;
            while (w > 0) { final int bp = (int) (pos & 7), avail = 8 - bp, take = Math.min(w, avail);
                v = (take == 64 ? 0 : (v << take)) | (long) (((b[bOff + (int) (pos >>> 3)] & 0xFF) >>> (avail - take)) & ((1 << take) - 1)); pos += take; w -= take; }
            return v; }
    }

    // value ranges (normative): W (sample width) 1..62; fixed predictors only for W <= 58, LPC only for W <= 44 (no int64 overflow anywhere);
    // Rice parameter 6 bits 0..62, 63 = escape; escape width 1..63; partition order 0..8 with 2^po <= number of residuals
    public static final int kMaxW = 62, kMaxWFixed = 58, kMaxWLpc = 44;
    // port of laes_codec.h:widthOf
    public static int widthOf(long v) { int w = 1; while (w < 64 && !(v >= -(1L << (w - 1)) && v < (1L << (w - 1)))) w++; return w; }
    // port of laes_codec.h:zz
    public static long zz(long v) { return v >= 0 ? v << 1 : ((-(v + 1)) << 1) | 1; }
    // port of laes_codec.h:unzz
    public static long unzz(long u) { return (u & 1) != 0 ? -(u >>> 1) - 1 : (u >>> 1); }
    // port of laes_codec.h:getResidual  (r[off .. off + n))
    public static void getResidual(BitIn rd, long[] r, int off, long n) { if (rd instanceof Play.BitRS) { getResidualRS((Play.BitRS) rd, r, off, n); return; } int po = (int) rd.get(4); if (n < 1 || po > 8 || (1L << po) > n) { rd.fail = true; return; } long parts = 1L << po, len = n / parts;
        for (long q = 0; q < parts && !rd.fail; q++) { long a = q * len, e = (q == parts - 1) ? n : a + len; int k = (int) rd.get(6);
            if (k == 63) { int rw = (int) rd.get(6); if (rw < 1 || rw > 63) { rd.fail = true; return; } for (long i = a; i < e; i++) r[off + (int) i] = rd.getS(rw); }
            else for (long i = a; i < e && !rd.fail; i++) { long qv; final long qmax = k != 0 ? (-1L >>> k) : -1L;   // u = q * 2^k + low must fit 64 bits
                qv = rd.unary(qmax);
                long u = (qv << k) | (k != 0 ? rd.get(k) : 0); r[off + (int) i] = unzz(u); } } }
    // getResidual for the player's reader (Java, 2026-11-04): the same reads; a Rice value whose zeros, one and k low bits lie inside the
    // next 57 bits is taken from one 64-bit word (its quotient is then at most 56, inside qmax for every k <= 56), the others as above
    static void getResidualRS(Play.BitRS rd, long[] r, int off, long n) { int po = (int) rd.get(4); if (n < 1 || po > 8 || (1L << po) > n) { rd.fail = true; return; } long parts = 1L << po, len = n / parts;
        for (long q = 0; q < parts && !rd.fail; q++) { long a = q * len, e = (q == parts - 1) ? n : a + len; int k = (int) rd.get(6);
            if (k == 63) { int rw = (int) rd.get(6); if (rw < 1 || rw > 63) { rd.fail = true; return; } for (long i = a; i < e; i++) r[off + (int) i] = rd.getS(rw); }
            else { final int ie = off + (int) e; final long qmax = k != 0 ? (-1L >>> k) : -1L;
                for (int i = off + (int) a; i < ie && !rd.fail; i++) {
                    if (rd.left() >= 64) { final long wd = rd.word(); final int lz = Long.numberOfLeadingZeros(wd);
                        if (lz + 1 + k <= 57) { final long lo = k != 0 ? (wd << (lz + 1)) >>> (64 - k) : 0; rd.pos += lz + 1 + k; r[i] = unzz(((long) lz << k) | lo); continue; } }
                    final long qv = rd.unary(qmax); final long u = (qv << k) | (k != 0 ? rd.get(k) : 0); r[i] = unzz(u); } } } }
    // ---- predictors (ranges above keep every intermediate inside int64)
    // port of laes_codec.h:fixedPred  (x[off + i])
    public static long fixedPred(long[] x, int off, int i, int ord) { final int p = off + i; switch (ord) { case 0: return 0; case 1: return x[p - 1]; case 2: return 2 * x[p - 1] - x[p - 2];
            case 3: return 3 * x[p - 1] - 3 * x[p - 2] + x[p - 3]; default: return 4 * x[p - 1] - 6 * x[p - 2] + 4 * x[p - 3] - x[p - 4]; } }
    // port of laes_codec.h:lpcPred
    // (Java 2026-11-04: LaesMath.Acc's mac / shr written out on local variables, no object per value; the same operations)
    static long lpcPred(long[] x, int p, long[] c, int shift) { long lo = 0, hi = 0;
        for (int j = 0; j < c.length; j++) { final long cj = c[j], xv = x[p - 1 - j]; final long l = cj * xv, h = U.mulHighSigned(cj, xv); final long n = lo + l; hi += h + (U.lt(n, lo) ? 1 : 0); lo = n; }
        if (shift == 0) return lo; long blo = lo - (1L << (shift - 1)), bhi = hi; if (U.gt(blo, lo)) bhi--;   // floor: shr rounds, so subtract half first
        final long n = blo + (1L << (shift - 1)); final long h2 = bhi + (U.lt(n, blo) ? 1 : 0); return (n >>> shift) | (h2 << (64 - shift)); }
    // the same value in 64 bits; only when sum|c| x max|x| < 2^62, so no partial sum overflows (then floor = arithmetic shift right)
    // port of laes_codec.h:lpcPred64
    // (Java 2026-11-04: in two parts, even and odd j, added at the end; int64 sums are the same in any order)
    static long lpcPred64(long[] x, int p, long[] c, int o, int shift) { long a = 0, b = 0; int j = 0; final int q = p - 1;
        for (; j + 1 < o; j += 2) { a += c[j] * x[q - j]; b += c[j + 1] * x[q - j - 1]; } if (j < o) a += c[j] * x[q - j]; return (a + b) >> shift; }
    // port of laes_codec.h:lpcFits64
    static boolean lpcFits64(long[] c, long maxAbsX) { double s = 0; for (long v : c) s += Math.abs((double) v); return s * com.lae.core.U.toDouble(maxAbsX) < 4.0e18; }   // 2^62 = 4.61e18
    // port of laes_codec.h:absMax  (uint64 result)
    static long absMax(long[] x, int off, long n) { long m = 0; for (int i = 0; i < n; i++) { final long v = x[off + i]; final long a = v < 0 ? (-(v + 1)) + 1 : v; m = U.max(m, a); } return m; }
    /** port of laes_codec.h:decodeBlock (x[off .. off + n)); returns false on any value outside the normative ranges or a short buffer */
    public static boolean decodeBlock(BitIn rd, long[] x, int off, long n) {
        int type = (int) rd.get(2), W = (int) rd.get(6); if (rd.fail || W < 1 || W > kMaxW) return false;
        if (type == 0) { long v = rd.getS(W); for (int i = 0; i < n; i++) x[off + i] = v; return !rd.fail; }
        if (type == 1) { for (int i = 0; i < n && !rd.fail; i++) x[off + i] = rd.getS(W); return !rd.fail; }
        if (type == 2) { int o = (int) rd.get(3); if (rd.fail || W > kMaxWFixed || o > 4 || o >= n) return false;
            for (int i = 0; i < o; i++) x[off + i] = rd.getS(W); long[] r = rd.resBuf((int) n); getResidual(rd, r, o, n - o); if (rd.fail) return false;
            for (int i = o; i < n; i++) x[off + i] = r[i] + fixedPred(x, off, i, o); return true; }
        int o = (int) rd.get(5) + 1, prec = (int) rd.get(4) + 1, shift = (int) rd.get(5); if (rd.fail || W > kMaxWLpc || o >= n) return false;
        long[] c = new long[o]; for (int j = 0; j < o; j++) c[j] = rd.getS(prec);
        for (int i = 0; i < o; i++) x[off + i] = rd.getS(W); long[] r = rd.resBuf((int) n); getResidual(rd, r, o, n - o); if (rd.fail) return false;
        // 64-bit prediction only while sum|c| x (largest |x| decoded so far) proves it exact; the bound is updated sample by sample
        // (C++ c32: sizeof(void*) == 4 only; false on x64, so the int32 forms are never chosen here)
        double sc = 0; for (long v : c) sc += Math.abs((double) v); long mx = absMax(x, off, o);
        for (int i = o; i < n; i++) { final int p = off + i;
            x[p] = r[i] + ((sc * U.toDouble(mx) < 4.0e18) ? lpcPred64(x, p, c, o, shift) : lpcPred(x, p, c, shift));
            final long a = x[p] < 0 ? (-(x[p] + 1)) + 1 : x[p]; if (U.gt(a, mx)) mx = a; }
        return true;
    }
    /** port of laes_codec.h:decodeLayer; ok[0] receives the result */
    public static long[] decodeLayer(byte[] b, int bOff, int bLen, long n, long B, boolean[] ok) {
        long[] x = new long[(int) n]; BitR rd = new BitR(b, bOff, bLen); ok[0] = true;
        for (long a = 0; a < n && ok[0]; a += B) ok[0] = decodeBlock(rd, x, (int) a, Math.min(B, n - a));
        return x;
    }
}
