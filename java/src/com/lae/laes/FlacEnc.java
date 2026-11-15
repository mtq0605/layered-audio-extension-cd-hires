// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

import com.lae.core.AlignHash;
import com.lae.core.Cv;
import com.lae.core.LMath;
import com.lae.core.LaeException;
import com.lae.core.U;

/**
 * Port of src/laes_flac_enc.h (namespace laesflac): the FLAC writer for the LAES FLAC form (split from laes_flac.h, 2026-10-29; the code is
 * unchanged). Written for this project from RFC 9639 (no libFLAC code). The reader is Flac (laes_flac.h). Here: the subframe encoder
 * (CONSTANT / VERBATIM / FIXED / LPC, wasted bits, Rice partitions), the frame writer and writeFlac (STREAMINFO, the LAES payload in
 * APPLICATION blocks, the core in audio frames).
 * Java additions (same bytes): the core channels may be int[] (Ch), and writeFlacFrames codes the audio frames into a file piece by piece
 * (the C++ keeps every coded frame in memory until the end), returning what STREAMINFO needs; flacHead writes "fLaC" + STREAMINFO.
 * The choices use lmath::log2 / cospi (LMath); no C library transcendental function.
 */
public final class FlacEnc {
    private FlacEnc() {}

    /** a channel of samples: long[] (V) or int[] (the core in the encoder) */
    public abstract static class Ch { public abstract long n(); public abstract long at(long i); }
    public static final class LongCh extends Ch { final long[] v; public LongCh(long[] x) { v = x; } public long n() { return v.length; } public long at(long i) { return v[(int) i]; } }
    public static final class IntCh extends Ch { final int[] v; public IntCh(int[] x) { v = x; } public long n() { return v.length; } public long at(long i) { return v[(int) i]; } }

    // ---- subframe encoder
    static final int kMaxLpcOrder = 12, kMaxPrec = 15, kMaxShift = 15;
    // cost of one Rice partition with the 5-bit method (parameter 0..30, 31 = escape)
    // L5 (2026-11-03): from the zz values, with the same result as the former loops. The cost was the double sum 5 + sum((u >> k) + 1 + k) in order;
    // every term and partial sum is an integer, below 2^53 when sum(u) <= 2^52 (n <= 2^16, k <= 30), so it equals the integer sum then (otherwise
    // the double loop as before). rw = the largest widthOf of the non-zero values = bitLen(OR of the zz values) (laes::bitLen).
    // port of laes_flac_enc.h:riceParamU  (u[a .. a + n); cost[0] receives the cost)
    public static int riceParamU(long[] u, int a, int n, double[] cost) {
        long sum = 0, orv = 0; for (int i = 0; i < n; i++) { sum += u[a + i]; orv |= u[a + i]; }
        final boolean small = CodecEnc.bitLen(orv) + CodecEnc.bitLen(n) <= 63 && U.le(sum, 1L << 52) && n <= 65536;
        double mean = 0; if (small) mean = U.toDouble(sum); else for (int i = 0; i < n; i++) mean += U.toDouble(u[a + i]);
        mean = n != 0 ? mean / n : 0;
        final int k0 = mean > 1 ? Cv.d2i(Math.floor(LMath.log2(mean))) : 0; int best = 0; cost[0] = 1e300;
        for (int k = Math.max(0, k0 - 2); k <= Math.min(30, k0 + 2); k++) { double c;
            if (small) { long t = 0; for (int i = 0; i < n; i++) t += u[a + i] >>> k; c = U.toDouble(5 + t + (long) n * (long) (1 + k)); }
            else { c = 5; for (int i = 0; i < n; i++) c += U.toDouble(u[a + i] >>> k) + 1 + k; }
            if (c < cost[0]) { cost[0] = c; best = k; } }
        final int rw = CodecEnc.bitLen(orv);
        if (rw >= 1 && rw <= 31) { final double ce = 10 + (double) rw * n; if (ce < cost[0]) { cost[0] = ce; best = 31; } }   // no 0-bit escape: some older decoders mishandle it (an all-zero partition takes k = 0)
        return best; }
    // port of laes_flac_enc.h:zzBuf  (ub[0 .. n) = zz(r[off .. off + n)))
    static long[] zzBuf(long[] r, int off, int n, long[] ub) { for (int i = 0; i < n; i++) ub[i] = Codec.zz(r[off + i]); return ub; }
    // partition orders 0..8 with the block length divisible by 2^po and a non-empty first partition (block >> po > order)
    // port of laes_flac_enc.h:residualCost  (r[off ..): blk - order residuals)
    static double residualCost(long[] r, int off, int blk, int order, int[] po, long[] ub) { double best = 1e300; po[0] = 0; final long[] u = zzBuf(r, off, blk - order, ub); final double[] cc = new double[1];
        for (int p = 0; p <= 8; p++) { if (blk % (1 << p) != 0 || (blk >> p) <= order) break; final int plen = blk >> p; double c = 2 + 4; int a = 0;
            for (int q = 0; q < (1 << p); q++) { final int len = q != 0 ? plen : plen - order; riceParamU(u, a, len, cc); c += cc[0]; a += len; }
            if (c < best) { best = c; po[0] = p; } } return best; }
    // port of laes_flac_enc.h:putResidual  (r[off ..))
    static void putResidual(CodecEnc.BitW w, long[] r, int off, int blk, int order, int po, long[] ub) { w.put(1, 2); w.put(po, 4); final int plen = blk >> po; int a = 0;   // method 01: 5-bit parameters
        final long[] u = zzBuf(r, off, blk - order, ub); final double[] c = new double[1];
        for (int q = 0; q < (1 << po); q++) { final int len = q != 0 ? plen : plen - order; final int k = riceParamU(u, a, len, c); w.put(k, 5);
            if (k == 31) { int rw = 0; for (int i = a; i < a + len; i++) if (r[off + i] != 0) rw = Math.max(rw, Codec.widthOf(r[off + i])); w.put(rw, 5); if (rw != 0) for (int i = a; i < a + len; i++) w.putS(r[off + i], rw); }
            else for (int i = a; i < a + len; i++) { final long qv = u[i] >>> k; w.zeros(qv); w.put(1, 1); if (k != 0) w.put(u[i] & ((1L << k) - 1), k); }
            a += len; } }
    // |s| < 2^50: int64, arithmetic shift = floor
    // port of laes_flac_enc.h:lpcPred
    static long lpcPred(long[] x, int i, long[] c, int shift) { long s = 0; for (int j = 0; j < c.length; j++) s += c[j] * x[i - 1 - j]; return s >> shift; }
    private static final int[] PRECS = {12, 15};
    // port of laes_flac_enc.h:encodeSubframe  (x0[0 .. n))
    public static void encodeSubframe(CodecEnc.BitW w, long[] x0, int n, int bps0) {
        boolean same = true; for (int i = 1; i < n && same; i++) same = x0[i] == x0[0];
        if (same) { w.put(0, 1); w.put(0, 6); w.put(0, 1); w.putS(x0[0], bps0); return; }
        // wasted bits: low bits that are zero in every sample of the block are left out (subframe header flag + unary count)
        long orv = 0; for (int i = 0; i < n; i++) orv |= x0[i]; int wb = 0; while (wb < bps0 - 1 && ((orv >> wb) & 1) == 0) wb++;
        long[] x = x0; if (wb != 0) { x = new long[n]; for (int i = 0; i < n; i++) x[i] = x0[i] >> wb; }
        final int bps = bps0 - wb;
        final long[] res = new long[n], ub = new long[n]; final int[] po = new int[1]; double bestBits = 8 + (double) bps * n; int bestType = 1, bestOrd = 0, bestPo = 0, bestShift = 0, bestPrec = 0; long[] bestC = new long[0];
        for (int o = 0; o <= 4 && o < n; o++) { boolean ok = true; for (int i = o; i < n && ok; i++) { res[i] = x[i] - Codec.fixedPred(x, 0, i, o); ok = Flac.fits32(res[i]); } if (!ok) continue;
            final double c = 8 + (double) bps * o + residualCost(res, o, n, o, po, ub); if (c < bestBits) { bestBits = c; bestType = 2; bestOrd = o; bestPo = po[0]; } }
        final int MAXO = (int) Math.min(kMaxLpcOrder, (long) n - 1);
        if (MAXO >= 1) { final double[] xw = new double[n], R = new double[MAXO + 1]; for (int i = 0; i < n; i++) { final double t = n > 1 ? (double) i / (n - 1) : 0; double wv = 1;
                if (t < 0.25) wv = 0.5 * (1 - LMath.cospi(t / 0.25)); else if (t > 0.75) wv = 0.5 * (1 - LMath.cospi((1 - t) / 0.25)); xw[i] = (double) x[i] * wv; }
            for (int l = 0; l <= MAXO; l++) { double s = 0; for (int i = l; i < n; i++) s += xw[i] * xw[i - l]; R[l] = s; }
            if (R[0] > 0) { final double[][] A = new double[MAXO + 1][]; double[] a = new double[MAXO + 1], tmp; double err = R[0] * (1 + 1e-10);
                for (int m = 1; m <= MAXO; m++) { double acc = R[m]; for (int j = 1; j < m; j++) acc -= a[j] * R[m - j]; final double k = acc / err; tmp = a.clone(); a[m] = k; for (int j = 1; j < m; j++) a[j] = tmp[j] - k * tmp[m - j];
                    err *= (1 - k * k); A[m] = a.clone(); if (err <= 0) break; }
                for (int o = 1; o <= MAXO; o++) { if (A[o] == null) continue;
                    for (int prec : PRECS) { double mx = 0; for (int j = 1; j <= o; j++) mx = Cv.max(mx, Math.abs(A[o][j])); if (mx <= 0) continue;
                        final int lg = Cv.d2i(Math.ceil(LMath.log2(mx + 1e-300))); final int shift = Math.max(0, Math.min(kMaxShift, prec - 1 - lg));
                        final long[] c = new long[o]; final long lim = (1L << (prec - 1)) - 1; for (int j = 0; j < o; j++) c[j] = Math.max(-lim - 1, Math.min(lim, Cv.d2l(Math.floor(A[o][j + 1] * (double) (1L << shift) + 0.5))));
                        boolean ok = true; for (int i = o; i < n && ok; i++) { res[i] = x[i] - lpcPred(x, i, c, shift); ok = Flac.fits32(res[i]); } if (!ok) continue;
                        final double cst = 8 + 4 + 5 + (double) prec * o + (double) bps * o + residualCost(res, o, n, o, po, ub);
                        if (cst < bestBits) { bestBits = cst; bestType = 3; bestOrd = o; bestPo = po[0]; bestShift = shift; bestPrec = prec; bestC = c; } } } } }
        if (bestType == 1) { head(w, 1, wb); for (int i = 0; i < n; i++) w.putS(x[i], bps); return; }
        if (bestType == 2) { head(w, 8 + bestOrd, wb); for (int i = 0; i < bestOrd; i++) w.putS(x[i], bps);
            for (int i = bestOrd; i < n; i++) res[i] = x[i] - Codec.fixedPred(x, 0, i, bestOrd); putResidual(w, res, bestOrd, n, bestOrd, bestPo, ub); return; }
        head(w, 32 + bestOrd - 1, wb); for (int i = 0; i < bestOrd; i++) w.putS(x[i], bps);
        w.put(bestPrec - 1, 4); w.put(bestShift, 5); for (int j = 0; j < bestOrd; j++) w.putS(bestC[j], bestPrec);
        for (int i = bestOrd; i < n; i++) res[i] = x[i] - lpcPred(x, i, bestC, bestShift); putResidual(w, res, bestOrd, n, bestOrd, bestPo, ub); }
    // port of laes_flac_enc.h:encodeSubframe::head (the lambda)
    private static void head(CodecEnc.BitW w, int type, int wb) { w.put(0, 1); w.put(type, 6); if (wb != 0) { w.put(1, 1); for (int i = 1; i < wb; i++) w.put(0, 1); w.put(1, 1); } else w.put(0, 1); }

    // ---- writer
    // the frame number, coded as in UTF-8 (up to 36 bits)
    // port of laes_flac_enc.h:putUtf8
    public static void putUtf8(CodecEnc.BitW w, long v) {
        if (U.lt(v, 0x80)) { w.put(v, 8); return; } final int nb = U.lt(v, 0x800) ? 2 : U.lt(v, 0x10000) ? 3 : U.lt(v, 0x200000) ? 4 : U.lt(v, 0x4000000) ? 5 : U.lt(v, 0x80000000L) ? 6 : 7;
        w.put(((0xFF00 >> nb) & 0xFF) | (nb == 7 ? 0 : (v >>> (6 * (nb - 1)))), 8); for (int i = nb - 2; i >= 0; i--) w.put(0x80 | ((v >>> (6 * i)) & 0x3F), 8); }
    // port of laes_flac_enc.h:encodeFrame
    public static byte[] encodeFrame(Ch[] ch, long at, int n, long frameNo, int rate, int bps) {
        CodecEnc.BitW w = new CodecEnc.BitW(); w.put(0xFFF8, 16);   // sync 0b111111111111100, blocking strategy 0 (fixed block size)
        final int bsCode = n == 4096 ? 12 : (n <= 256 ? 6 : 7), ssCode = bps == 16 ? 4 : bps == 24 ? 6 : 7;
        final int srCode = rate == 88200 ? 1 : rate == 176400 ? 2 : rate == 192000 ? 3 : rate == 8000 ? 4 : rate == 16000 ? 5 : rate == 22050 ? 6 : rate == 24000 ? 7 : rate == 32000 ? 8
                         : rate == 44100 ? 9 : rate == 48000 ? 10 : rate == 96000 ? 11 : 0;   // 0 = as in STREAMINFO
        w.put(bsCode, 4); w.put(srCode, 4); w.put(1, 4); w.put(ssCode, 3); w.put(0, 1); putUtf8(w, frameNo); if (bsCode == 6) w.put(n - 1, 8); else if (bsCode == 7) w.put(n - 1, 16);
        w.put(Flac.crc8(w.b, 0, w.len), 8);
        final long[] x = new long[n];
        for (int c = 0; c < 2; c++) { for (int i = 0; i < n; i++) x[i] = ch[c].at(at + i); encodeSubframe(w, x, n, bps); }
        w.flush(); final int cr = Flac.crc16(w.b, 0, w.len); w.push(cr >> 8); w.push(cr); return w.bytes(); }
    // port of laes_flac_enc.h:putBlockHeader
    static void putBlockHeader(byte[] o, int at, boolean last, int type, int len) { o[at] = (byte) ((last ? 0x80 : 0) | type); o[at + 1] = (byte) (len >> 16); o[at + 2] = (byte) (len >> 8); o[at + 3] = (byte) len; }

    // MD5 of the audio as STREAMINFO defines it: interleaved, each sample little-endian signed in ceil(bits/8) bytes
    // port of laes_flac.h:audioMd5 (over Ch)
    public static byte[] audioMd5(Ch[] ch, long n, int bits) { AlignHash.Md5 m = new AlignHash.Md5(); final int by = (bits + 7) / 8; byte[] b = new byte[1 << 16]; int fill = 0;
        for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) { final long v = ch[c].at(i); for (int k = 0; k < by; k++) b[fill++] = (byte) (v >>> (8 * k)); if (fill > (1 << 16) - 8) { m.update(b, 0, fill); fill = 0; } }
        m.update(b, 0, fill); return Flac.hexBytes(m.hex(), 16); }

    /** what STREAMINFO needs from the frames */
    public static final class Frames { public long fmin = 0xFFFFFF, fmax = 0, count = 0, bytes = 0; }
    // the checks of writeFlac (throws on input it cannot write exactly)
    // port of laes_flac_enc.h:writeFlac (the checks)
    static void checkInput(Ch[] ch, int rate, int bps) {
        final long n = ch[0].n();
        if (bps != 16 && bps != 24 && bps != 32) throw new LaeException("laesflac: the core master must be 16, 24 or 32 bit");
        if (rate < 1 || rate >= (1 << 20) || ch[1].n() != n || n >= (1L << 36)) throw new LaeException("laesflac: bad rate or length");
        if (n == 0) throw new LaeException("laesflac: no samples (an LAES core master has at least one frame)");   // as laes_flac_enc.h: the reader refuses a total of 0
        { final long lim = 1L << (bps - 1); for (int c = 0; c < 2; c++) for (long i = 0; i < n; i++) { final long v = ch[c].at(i); if (v < -lim || v >= lim) throw new LaeException("laesflac: a sample does not fit the bit depth"); } } }
    /**
     * the audio frames of writeFlac, coded in parallel threads a group at a time and written in order to o (the same bytes as the C++
     * frames); returns the frame size range for STREAMINFO
     */
    // port of laes_flac_enc.h:writeFlac (the frames)
    public static Frames writeFlacFrames(final Ch[] ch, final int rate, final int bps, Stream.Out o, int threads, AtomicBoolean cancel) {
        checkInput(ch, rate, bps);
        final long n = ch[0].n(); final long B = 4096; final Frames F = new Frames();
        final long nf = (n + B - 1) / B; final int group = 256;
        for (long g0 = 0; g0 < nf; g0 += group) { final long base = g0; final int cnt = (int) Math.min(group, nf - g0); final byte[][] fr = new byte[cnt][];
            // the frames are independent: coded in parallel threads (L5, 2026-11-03), then taken in order
            Par.forBlocks(cnt, threads, new Par.Block() { public void run(int q) { final long k = base + q; fr[q] = encodeFrame(ch, k * B, (int) Math.min(B, n - k * B), k, rate, bps); } }, cancel);
            for (byte[] f : fr) { o.put(f, 0, f.length); F.fmin = Math.min(F.fmin, f.length); F.fmax = Math.max(F.fmax, f.length); F.bytes += f.length; F.count++; } }
        if (F.count == 0) F.fmin = 0;
        return F; }
    /** "fLaC" + the STREAMINFO block (4 + 4 + 34 bytes) as writeFlac writes it; napp = number of APPLICATION blocks that follow */
    // port of laes_flac_enc.h:writeFlac (the head)
    public static byte[] flacHead(Ch[] ch, int rate, int bps, Frames F, long napp) {
        final long n = ch[0].n(); final long B = 4096; byte[] o = new byte[42]; o[0] = 'f'; o[1] = 'L'; o[2] = 'a'; o[3] = 'C';
        putBlockHeader(o, 4, napp == 0, 0, 34); final long bs = n >= B ? B : Math.max(16, n);
        CodecEnc.BitW si = new CodecEnc.BitW(); si.put(bs, 16); si.put(bs, 16); si.put(F.fmin, 24); si.put(F.fmax, 24); si.put(rate, 20); si.put(1, 3); si.put(bps - 1, 5); si.put(n, 36);
        final byte[] md5 = audioMd5(ch, n, bps); System.arraycopy(si.b, 0, o, 8, si.len); System.arraycopy(md5, 0, o, 8 + si.len, 16);
        return o; }
    // core: two channels of n samples at bps bits (16 / 24 / 32), rate 44100 or 48000; app: the LAES payload (may be empty); in memory
    // port of laes_flac_enc.h:writeFlac
    public static byte[] writeFlac(final Ch[] ch, final int rate, final int bps, byte[] app, int threads) {
        checkInput(ch, rate, bps);
        final long n = ch[0].n(); final long B = 4096; final int nf = (int) ((n + B - 1) / B); final byte[][] fr = new byte[nf][]; final Frames F = new Frames();
        Par.forBlocks(nf, threads, new Par.Block() { public void run(int k) { fr[k] = encodeFrame(ch, (long) k * B, (int) Math.min(B, n - (long) k * B), k, rate, bps); } }, null);
        for (byte[] f : fr) { F.fmin = Math.min(F.fmin, f.length); F.fmax = Math.max(F.fmax, f.length); }
        if (nf == 0) F.fmin = 0;
        final int piece = (1 << 24) - 1 - 4; final int napp = app.length == 0 ? 0 : (app.length + piece - 1) / piece;
        java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); final byte[] h = flacHead(ch, rate, bps, F, napp); o.write(h, 0, h.length);
        for (int i = 0; i < napp; i++) { final int a = i * piece, len = Math.min(piece, app.length - a); byte[] bh = new byte[4]; putBlockHeader(bh, 0, i + 1 == napp, 2, len + 4); o.write(bh, 0, 4);
            o.write('L'); o.write('A'); o.write('E'); o.write('2'); o.write(app, a, len); }
        for (byte[] f : fr) o.write(f, 0, f.length); return o.toByteArray(); }
    static byte[] grow(byte[] b, int need) { return need <= b.length ? b : Arrays.copyOf(b, Math.max(need, b.length * 2)); }
}
