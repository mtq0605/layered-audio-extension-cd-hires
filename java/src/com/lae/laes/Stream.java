// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

import com.lae.core.LaeException;
import com.lae.core.U;

/**
 * Port of src/laes_stream.h (namespace laess), the scalar path. LAES, block-wise processing (2026-10-30): the same values as
 * laes_file.h / laes_lift.h / laes_codec.h, computed piece by piece so that memory does not grow with the length. Signals live in files
 * of a work folder; a piece is a whole multiple of the format's coding blocks (4096 x 2^k samples) and every filter (lifting P: 40 taps,
 * U: 60 taps, reference: 256 taps) reads a window around it, with the same symmetric extension at the ends (laes::mir over the whole
 * length). Every output value is computed with the same integer operations as the whole-signal forms: 64-bit sums only where they
 * provably cannot overflow (then they equal the 128-bit sums), 128-bit otherwise.
 * Not ported: the AVX2 forms and the int32-product forms (wtapN, wtapS, the n32 / s48 branches of refPiece); the C++ uses them only when
 * int32Path() (32-bit builds, or AVX2 at run time) and documents them as giving the same values as the scalar 64 / 128-bit sums ported here.
 */
public final class Stream {
    private Stream() {}

    // port of laes_stream.h:fail  (laess::Fail is a std::runtime_error with the message)
    static RuntimeException fail(String m) { throw new LaeException(m); }

    // ---- signal sources: values [a, b) of a signal of n samples (int64)
    /** port of laes_stream.h:Sig */
    public abstract static class Sig { public long n = 0; public abstract void get(long a, long b, long[] out, int outOff); }
    /** port of laes_stream.h:MemSig */
    public static final class MemSig extends Sig { final long[] v; public MemSig(long[] x) { v = x; n = x.length; }
        // port of laes_stream.h:MemSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) fail("internal error: signal range"); System.arraycopy(v, (int) a, out, outOff, (int) (b - a)); } }

    // a file written in order (a failed write throws; the caller's work folder is removed on failure)
    /** port of laes_stream.h:Out */
    public static final class Out { FileOutputStream f; final String path; long n = 0;
        public Out(String p) { path = p; try { f = new FileOutputStream(p); } catch (IOException e) { throw fail("cannot open " + p); } }
        // port of laes_stream.h:Out::put
        public void put(byte[] d, int off, int k) { if (k == 0) return; try { f.write(d, off, k); } catch (IOException e) { throw fail("write failed (disk full?): " + path); } n += k; }
        // port of laes_stream.h:Out::close
        public void close() { if (f == null) return; FileOutputStream g = f; f = null; try { g.close(); } catch (IOException e) { throw fail("write failed (disk full?): " + path); } } }
    // a file read by byte ranges
    /** port of laes_stream.h:In */
    public static final class In { RandomAccessFile f; final String path; public final long n;
        public In(String p) { path = p; try { f = new RandomAccessFile(p, "r"); } catch (IOException e) { throw fail("cannot open " + p); }
            try { n = f.length(); } catch (IOException e) { throw fail("cannot read the size of " + p); } }
        // port of laes_stream.h:In::read
        public synchronized void read(long at, byte[] d, int off, int k) { if (k == 0) return;
            if (at < 0 || at + k > n) fail("read failed: " + path);
            try { f.seek(at); f.readFully(d, off, k); } catch (IOException e) { throw fail("read failed: " + path); } }
        public void close() { try { f.close(); } catch (IOException e) { throw fail("cannot close " + path); } } }
    /** port of laes_stream.h:FileSig (int64 values, little-endian) */
    public static final class FileSig extends Sig { final In in; byte[] buf = new byte[0];
        public FileSig(String p) { in = new In(p); if (in.n % 8 != 0) fail("bad signal file " + p); n = in.n / 8; }
        // port of laes_stream.h:FileSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) fail("internal error: signal range"); final int k = (int) (b - a); if (buf.length < k * 8) buf = new byte[k * 8];
            in.read(a * 8, buf, 0, k * 8); ByteBuffer.wrap(buf, 0, k * 8).order(ByteOrder.LITTLE_ENDIAN).asLongBuffer().get(out, outOff, k); }
        public void close() { in.close(); } }
    // one channel of an interleaved int32 stereo file (a master), shifted right by w
    /** port of laes_stream.h:RawSig */
    public static final class RawSig extends Sig { final In in; final int c, w; byte[] buf = new byte[0];
        public RawSig(String p, int ch, int sh) { in = new In(p); c = ch; w = sh; if (in.n % 8 != 0) fail("bad master file " + p); n = in.n / 8; }
        // port of laes_stream.h:RawSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) fail("internal error: signal range"); final int k = (int) (b - a); if (buf.length < k * 8) buf = new byte[k * 8];
            in.read(a * 8, buf, 0, k * 8); for (int i = 0; i < k; i++) out[outOff + i] = (long) com.lae.core.Le.i32(buf, (i * 2 + c) * 4) >> w; } }
    // int64 values written in order to a file
    /** port of laes_stream.h:SigOut */
    public static final class SigOut { final Out o; byte[] buf = new byte[0];
        public SigOut(String p) { o = new Out(p); }
        // port of laes_stream.h:SigOut::put
        public void put(long[] d, int off, int k) { if (buf.length < k * 8) buf = new byte[k * 8]; ByteBuffer.wrap(buf, 0, k * 8).order(ByteOrder.LITTLE_ENDIAN).asLongBuffer().put(d, off, k); o.put(buf, 0, k * 8); }
        public void close() { o.close(); } }
    // values at the indices g0..g1 (inclusive) of s, each index mapped by the symmetric extension laes::mir over s.n
    // port of laes_stream.h:window
    public static long[] window(Sig s, long g0, long g1) {
        long[] w = new long[(int) Math.max(0, g1 - g0 + 1)]; if (g1 < g0) return w;
        if (g0 >= 0 && g1 < s.n) { s.get(g0, g1 + 1, w, 0); return w; }
        final long[] lh = mirSpan(g0, g1, s.n); final long lo = lh[0], hi = lh[1];
        long[] r = new long[(int) (hi - lo + 1)]; s.get(lo, hi + 1, r, 0); mirCopy(r, lo, g0, g1, s.n, w);
        return w; }
    // Java (2026-11-04), the loops of the C++ written without a division per index (mir(g, n) = g for 0 <= g < n):
    // {lo, hi} = the smallest and largest mir(g, n) for g in [g0, g1] ({n, -1} when the range is empty)
    static long[] mirSpan(long g0, long g1, long n) {
        if (g1 < g0) return new long[] {n, -1};
        if (g0 >= 0 && g1 < n) return new long[] {g0, g1};
        long lo = n, hi = -1; for (long g = g0; g <= g1; g++) { final long m = LaesMath.mir(g, n); lo = Math.min(lo, m); hi = Math.max(hi, m); } return new long[] {lo, hi}; }
    // dst[g - g0] = src[mir(g, n) - lo] for g in [g0, g1]
    static void mirCopy(long[] src, long lo, long g0, long g1, long n, long[] dst) {
        final long ia = Math.max(g0, 0), ib = Math.min(g1, n - 1);
        if (ia > ib) { for (long g = g0; g <= g1; g++) dst[(int) (g - g0)] = src[(int) (LaesMath.mir(g, n) - lo)]; return; }
        for (long g = g0; g < ia; g++) dst[(int) (g - g0)] = src[(int) (LaesMath.mir(g, n) - lo)];
        System.arraycopy(src, (int) (ia - lo), dst, (int) (ia - g0), (int) (ib - ia + 1));
        for (long g = ib + 1; g <= g1; g++) dst[(int) (g - g0)] = src[(int) (LaesMath.mir(g, n) - lo)]; }
    // the even / odd samples of x as signals of their own (xe: (n + 1) / 2, xo: n / 2)
    /** port of laes_stream.h:HalfSig */
    public static final class HalfSig extends Sig { final Sig x; final int odd; public HalfSig(Sig s, int o) { x = s; odd = o; n = o != 0 ? s.n / 2 : (s.n + 1) / 2; }
        // port of laes_stream.h:HalfSig::get
        public void get(long a, long b, long[] out, int outOff) { if (b <= a) return; final long xa = 2 * a + odd, xb = 2 * (b - 1) + odd + 1; long[] t = new long[(int) (xb - xa)]; x.get(xa, xb, t, 0);
            for (long i = a; i < b; i++) out[outOff + (int) (i - a)] = t[(int) (2 * (i - a))]; } }
    // ---- filter taps on a window (already extended): sum_t h[t] (w[ia - t] + w[ib + t]), rounded by 2^sh (add 2^(sh-1), arithmetic shift)
    // port of laes_stream.h:maxAbsOf
    public static long maxAbsOf(long[] x) { long m = 0; for (long v : x) { final long a = v < 0 ? (v == Long.MIN_VALUE ? Long.MAX_VALUE : -v) : v; if (a > m) m = a; } return m; }
    // port of laes_stream.h:fits
    static boolean fits(long[] h, int NT, long mx, int sh) { return Lift.fits64(h, NT, mx, sh); }
    // port of laes_stream.h:wtap
    static long wtap(int NT, boolean WIDE, long[] h, long[] w, long ia, long ib, int sh) {
        final int pa = (int) ia, pb = (int) ib;
        if (!WIDE) { long acc = 0; for (int t = 0; t < NT; t++) acc += h[t] * (w[pa - t] + w[pb + t]); return (acc + (1L << (sh - 1))) >> sh; }
        Lift.Acc128 A = new Lift.Acc128(); for (int t = 0; t < NT; t++) { A.mac(h[t], w[pa - t]); A.mac(h[t], w[pb + t]); } return A.shr(sh); }

    // ---- Java speed forms (2026-11-04), each giving exactly the value of wtap above:
    //   mode 0: the 64-bit sum (wtap, !WIDE; only when fits() proves it exact);
    //   mode 1: the split sum of the C++ wtapS (every window value below 2^45 in magnitude: each pair sum p (below 2^46) is split into
    //           p >> 16 and p & 0xFFFF, the two sums h (p >> 16) and h (p & 0xFFFF) are exact in int64 (|h| < 2^31, 60 taps), and they are
    //           combined in 128 bits (comb16)); equal to wtap(WIDE) and wtap(!WIDE) alike (laes_stream.h);
    //   mode 2: the 128-bit sum of wtap(WIDE) with the accumulator in local variables (no object per value).
    // tapMode: the mode for a window whose largest magnitude is mx
    // (mx = maxAbsOf(win), which skips the value -2^63: the choice of mode 0 is that of wtap's WIDE; the split form is taken only when no
    // window value is -2^63 or beyond 2^45 in magnitude)
    // the register-blocked forms of the 64-bit sums (wtapRange mode 0, refPiece): faster on ART (phones), slower on HotSpot (fewer registers on
    // x64); the same values (the same integer terms summed in another order: exact, as the sums are proven inside int64). System property
    // lae.blockedTaps = 1 / 0 forces them on / off (tests run both).
    static final boolean BLOCKED = blockedDefault();
    static boolean blockedDefault() { final String p = System.getProperty("lae.blockedTaps"); if (p != null) return p.equals("1"); return "Dalvik".equals(System.getProperty("java.vm.name")); }
    static int tapMode(long[] h, int NT, long[] win, int len, int sh) { return fits(h, NT, maxAbsOf(win, len), sh) ? 0 : maxAbsAll(win, len) < (1L << 45) ? 1 : 2; }
    // the largest magnitude of x[0, len), with -2^63 counted as 2^63 - 1 (Java only: for the choice of the split forms)
    static long maxAbsAll(long[] x, int len) { long m = 0; for (int i = 0; i < len; i++) { final long v = x[i]; final long a = v < 0 ? (v == Long.MIN_VALUE ? Long.MAX_VALUE : -v) : v; if (a > m) m = a; } return m; }
    // maxAbsOf over x[0, len)
    static long maxAbsOf(long[] x, int len) { long m = 0; for (int i = 0; i < len; i++) { final long v = x[i]; final long a = v < 0 ? (v == Long.MIN_VALUE ? Long.MAX_VALUE : -v) : v; if (a > m) m = a; } return m; }
    /** Java (2026-11-04): reusable arrays of one caller (a player node), so that a piece does not allocate its temporaries anew; not shared between threads */
    public static final class Work { private final long[][] a = new long[10][];
        long[] get(int slot, int len) { long[] x = a[slot]; if (x == null || x.length < len) { x = new long[Math.max(len, x == null ? 0 : x.length + x.length / 4)]; a[slot] = x; } return x; } }
    // an array of at least len values: from ws (contents arbitrary), or a new one of exactly len values when ws is null
    static long[] alloc(Work ws, int slot, int len) { return ws == null ? new long[len] : ws.get(slot, len); }
    // window(s, g0, g1) written to w[0, g1 - g0 + 1) (w at least that long); returns w
    static long[] windowInto(Sig s, long g0, long g1, long[] w) {
        if (g1 < g0) return w;
        if (g0 >= 0 && g1 < s.n) { s.get(g0, g1 + 1, w, 0); return w; }
        final long[] lh = mirSpan(g0, g1, s.n); final long lo = lh[0], hi = lh[1];
        long[] r = new long[(int) (hi - lo + 1)]; s.get(lo, hi + 1, r, 0); mirCopy(r, lo, g0, g1, s.n, w);
        return w; }
    // round(2^-sh (ah 2^16 + al)) with the sum taken exactly in 128 bits, as Acc128.shr rounds (port of laes_stream.h:comb16)
    static long comb16(long ah, long al, int sh) { final long lo0 = ah << 16; long hi = ah >> 48; final long lo = lo0 + al; hi += (al >> 63) + (U.lt(lo, lo0) ? 1 : 0);
        final long add = 1L << (sh - 1), n = lo + add; final long h2 = hi + (U.lt(n, lo) ? 1 : 0); return (n >>> sh) | (h2 << (64 - sh)); }
    // out[o + k] = wtap(NT, ., h, w, ia0 + k, ia0 + k + 1, sh) for k in [0, cnt)
    static void wtapRange(int mode, int NT, long[] h, long[] w, int ia0, long[] out, int o, int cnt, int sh) {
        final long rnd = 1L << (sh - 1);
        // modes 0 and 1: the sums in four parts (taps t = 0, 1, 2, 3 mod 4; NT is 40 or 60), added at the end: integer sums proven inside
        // int64 do not depend on the order, and four independent chains run faster than one (multiply-add latency)
        if (mode == 0 && BLOCKED) { int k = 0;   // four outputs at a time: the window values slide through registers (each read once per tap)
            for (; k + 3 < cnt; k += 4) { final int L = ia0 + k, R = L + 1;
                long l0 = w[L + 1], l1 = w[L + 2], l2 = w[L + 3], l3, r0, r1 = w[R], r2 = w[R + 1], r3 = w[R + 2], s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                for (int t = 0; t < NT; t++) { l3 = l2; l2 = l1; l1 = l0; l0 = w[L - t]; r0 = r1; r1 = r2; r2 = r3; r3 = w[R + t + 3]; final long ht = h[t];
                    s0 += ht * (l0 + r0); s1 += ht * (l1 + r1); s2 += ht * (l2 + r2); s3 += ht * (l3 + r3); }
                out[o + k] = (s0 + rnd) >> sh; out[o + k + 1] = (s1 + rnd) >> sh; out[o + k + 2] = (s2 + rnd) >> sh; out[o + k + 3] = (s3 + rnd) >> sh; }
            for (; k < cnt; k++) { final int pa = ia0 + k, pb = pa + 1; long acc = 0; for (int t = 0; t < NT; t++) acc += h[t] * (w[pa - t] + w[pb + t]); out[o + k] = (acc + rnd) >> sh; }
            return; }
        if (mode == 0) { for (int k = 0; k < cnt; k++) { final int pa = ia0 + k, pb = pa + 1; long a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                for (int t = 0; t < NT; t += 4) { a0 += h[t] * (w[pa - t] + w[pb + t]); a1 += h[t + 1] * (w[pa - t - 1] + w[pb + t + 1]);
                    a2 += h[t + 2] * (w[pa - t - 2] + w[pb + t + 2]); a3 += h[t + 3] * (w[pa - t - 3] + w[pb + t + 3]); }
                out[o + k] = (((a0 + a1) + (a2 + a3)) + rnd) >> sh; } return; }
        if (mode == 1) { for (int k = 0; k < cnt; k++) { final int pa = ia0 + k, pb = pa + 1; long ah0 = 0, al0 = 0, ah1 = 0, al1 = 0;
                for (int t = 0; t < NT; t += 2) { final long p0 = w[pa - t] + w[pb + t], h0 = h[t], p1 = w[pa - t - 1] + w[pb + t + 1], h1 = h[t + 1];
                    ah0 += h0 * (p0 >> 16); al0 += h0 * (p0 & 0xFFFF); ah1 += h1 * (p1 >> 16); al1 += h1 * (p1 & 0xFFFF); }
                out[o + k] = comb16(ah0 + ah1, al0 + al1, sh); } return; }
        for (int k = 0; k < cnt; k++) { final int pa = ia0 + k, pb = pa + 1; long lo = 0, hi = 0;
            for (int t = 0; t < NT; t++) { final long ht = h[t];
                long x = w[pa - t]; long l = ht * x, hh = U.mulHighSigned(ht, x); long n = lo + l; hi += hh + (U.lt(n, lo) ? 1 : 0); lo = n;
                x = w[pb + t]; l = ht * x; hh = U.mulHighSigned(ht, x); n = lo + l; hi += hh + (U.lt(n, lo) ? 1 : 0); lo = n; }
            final long n = lo + rnd; final long h2 = hi + (U.lt(n, lo) ? 1 : 0); out[o + k] = (n >>> sh) | (h2 << (64 - sh)); } }

    // ---- one lifting stage, a piece at a time
    // forward (laesfast::fwd): x (n samples) -> s[a, b) (ne = (n + 1) / 2) and d[a, min(b, no)) (no = n / 2); returns {s, d}
    // port of laes_stream.h:fwdPiece
    public static long[][] fwdPiece(Sig x, final long a, final long b, int threads) {
        final long n = x.n, no = n / 2; HalfSig xe = new HalfSig(x, 0), xo = new HalfSig(x, 1); final long[] s = new long[(int) (b - a)]; final long db = Math.min(b, no); final long[] d = new long[(int) Math.max(0, db - a)];
        if (no == 0) { xe.get(a, b, s, 0); return new long[][] {s, d}; }
        // the detail values the update needs: d at mir(g, no) for g in [a - 60, b + 58], and the piece's own d[a, db)
        long lo, hi; { final long[] lh = mirSpan(a - 60, b + 58, no); lo = lh[0]; hi = lh[1]; }
        if (a < db) { lo = Math.min(lo, a); hi = Math.max(hi, db - 1); }
        // d[lo..hi] = xo - P(xe), P reading xe[mir(i - 39 .. i + 40, ne)]
        final long[] xeW = window(xe, lo - 39, hi + 40); final long[] xoW = new long[(int) (hi - lo + 1)]; xo.get(lo, hi + 1, xoW, 0); final long[] dv = new long[(int) (hi - lo + 1)];
        { final int mode = tapMode(Lift.kHalf0, 40, xeW, xeW.length, 30);   // ia = (lo + k) - (lo - 39) = 39 + k
          Lift.parallelFor(hi - lo + 1, threads, new Lift.Range() { public void run(long p, long q) { final int k0 = (int) p, cnt = (int) (q - p);
              wtapRange(mode, 40, Lift.kHalf0, xeW, 39 + k0, dv, k0, cnt, 30); for (int k = k0; k < k0 + cnt; k++) dv[k] = xoW[k] - dv[k]; } }); }
        for (long i = a; i < db; i++) d[(int) (i - a)] = dv[(int) (i - lo)];
        // s = xe + U(d), U reading d[mir(i - 60 .. i + 59, no)]
        final long[] dW = new long[(int) (b + 58 - (a - 60) + 1)]; mirCopy(dv, lo, a - 60, b + 58, no, dW);
        final long[] xeA = new long[(int) (b - a)]; xe.get(a, b, xeA, 0);
        final int mode = tapMode(Lift.kU0, 60, dW, dW.length, 31);   // ia = (a + k) - 1 - (a - 60) = 59 + k
        Lift.parallelFor(b - a, threads, new Lift.Range() { public void run(long p, long q) { final int k0 = (int) p, cnt = (int) (q - p);
            wtapRange(mode, 60, Lift.kU0, dW, 59 + k0, s, k0, cnt, 31); for (int k = k0; k < k0 + cnt; k++) s[k] = xeA[k] + s[k]; } });
        return new long[][] {s, d};
    }
    // inverse (laesfast::inv): s (ne) and d (no), n = ne + no -> x[2a, min(2b, n)) for a piece [a, b) of the s indices
    // port of laes_stream.h:invPiece
    public static long[] invPiece(Sig s, Sig d, long n, final long a, long b, int threads) { return invPieceW(s, d, n, a, b, threads, null); }
    // Java (2026-11-04): with a workspace ws (null: new arrays of the exact lengths, as invPiece): the result is x[0, min(2b, n) - 2a) of an
    // array of ws that may be longer and that the next call with ws overwrites; the values are those of invPiece
    static long[] invPieceW(Sig s, Sig d, long n, final long a, long b, int threads, Work ws) {
        final long ne = s.n, no = d.n; final long ob = Math.min(b, no), x0 = 2 * a, x1 = Math.min(2 * b, n); final long[] x = alloc(ws, 0, (int) Math.max(0, x1 - x0));
        if (no == 0) { long[] t = new long[(int) (b - a)]; s.get(a, b, t, 0); for (long i = a; i < b; i++) if (2 * i < n) x[(int) (2 * i - x0)] = t[(int) (i - a)]; return x; }
        // xe at the indices the odd samples need: mir(g, ne) for g in [a - 39, ob - 1 + 40], and the piece's own xe[a, b)
        long lo, hi; { final long[] lh = mirSpan(a - 39, ob + 39, ne); lo = lh[0]; hi = lh[1]; }
        lo = Math.min(lo, a); hi = Math.max(hi, b - 1); final int nv = (int) (hi - lo + 1);
        // xe[lo..hi] = s - U(d), U reading d[mir(i - 60 .. i + 59, no)]
        final int nW = (int) (hi + 59 - (lo - 60) + 1); final long[] dW = windowInto(d, lo - 60, hi + 59, alloc(ws, 1, nW));
        final long[] sW = alloc(ws, 2, nv); s.get(lo, hi + 1, sW, 0); final long[] xev = alloc(ws, 3, nv);
        { final int mode = tapMode(Lift.kU0, 60, dW, nW, 31);   // ia = (lo + k) - 1 - (lo - 60) = 59 + k
          Lift.parallelFor(nv, threads, new Lift.Range() { public void run(long p, long q) { final int k0 = (int) p, cnt = (int) (q - p);
              wtapRange(mode, 60, Lift.kU0, dW, 59 + k0, xev, k0, cnt, 31); for (int k = k0; k < k0 + cnt; k++) xev[k] = sW[k] - xev[k]; } }); }
        { final int xa = (int) (2 * a - x0), va = (int) (a - lo), cnt = (int) (b - a); for (int k = 0; k < cnt; k++) x[xa + 2 * k] = xev[va + k]; }
        if (a >= ob) return x;
        // xo = d + P(xe), P reading xe[mir(i - 39 .. i + 40, ne)]
        final int nE = (int) (ob + 39 - (a - 39) + 1); final long[] xeW = alloc(ws, 4, nE); mirCopy(xev, lo, a - 39, ob + 39, ne, xeW);
        final int no_ = (int) (ob - a); final long[] dA = alloc(ws, 5, no_); d.get(a, ob, dA, 0);
        final int mode = tapMode(Lift.kHalf0, 40, xeW, nE, 30); final long[] pvA = alloc(ws, 6, no_);   // ia = (a + k) - (a - 39) = 39 + k
        Lift.parallelFor(no_, threads, new Lift.Range() { public void run(long p, long q) { final int k0 = (int) p, cnt = (int) (q - p);
            wtapRange(mode, 40, Lift.kHalf0, xeW, 39 + k0, pvA, k0, cnt, 30); final int xo = (int) (2 * a + 1 - x0);
            for (int k = k0; k < k0 + cnt; k++) x[xo + 2 * k] = dA[k] + pvA[k]; } });
        return x;
    }
    // the reference (laes::refLen) for the output indices [a, b): r[i] = round(sum_q h[q] src[mir(i + whole + q, n)] / 2^(31 - up))
    // port of laes_stream.h:refPiece
    public static long[] refPiece(Sig src, final long[] h, long whole, int up, long a, long b, int threads) { return refPieceW(src, h, whole, up, a, b, threads, null); }
    // Java (2026-11-04): with a workspace (as invPieceW): r[0, b - a) of a possibly longer array of ws
    static long[] refPieceW(Sig src, final long[] h, long whole, int up, long a, long b, int threads, Work ws) {
        final long[] r = alloc(ws, 7, (int) Math.max(0, b - a)); if (b <= a) return r; final long g0 = a + whole - 127; final int nw = (int) (b - 1 + whole + 128 - g0 + 1);
        final long[] w = windowInto(src, g0, b - 1 + whole + 128, alloc(ws, 8, nw));
        // a whole-sample offset (frac = 0): the taps are 2^31 at one place and 0 elsewhere, so r[i] = round(src[i + whole] * 2^up), computed
        // directly (the same value as the sum below: 2^31 * v is a multiple of 2^(31 - up) when up >= 0)
        { int c = -1, nz = 0; for (int t = 0; t < 256; t++) if (h[t] != 0) { nz++; c = t; }
          if (nz == 1 && h[c] == (1L << 31)) { for (int k = 0; k < b - a; k++) { final long v = w[k + c];
                  r[k] = up >= 0 ? v << up : (v + (1L << (-up - 1))) >> (-up); }
              return r; } }
        final int s = 31 - up; long shv = 0; for (long v : h) shv += v < 0 ? -v : v;
        final long mxw = maxAbsOf(w, nw); final boolean wide = !((double) mxw * (double) shv + (s > 0 ? (double) (1L << (s - 1)) : 0.0) < 9.0e18);
        // (C++ n32: int32Path() only; the same values as the 64-bit sum below)
        // C++ s48 (Java 2026-11-04: taken when the sum is wide): every window value below 2^47 (Java: and none -2^63), every tap below 2^31:
        // each value c split into c >> 16 and c & 0xFFFF, the two sums exact in int64, combined in 128 bits; equal to the 128-bit sum
        long mh = 0; for (long v : h) mh = Math.max(mh, v < 0 ? -v : v);
        final long mxa = wide ? maxAbsAll(w, nw) : 0;
        final boolean s48 = wide && mxa < (1L << 47) && mh < (1L << 31) && (double) shv * (double) ((mxa >> 16) + 1) < 9.0e18;
        Lift.parallelFor(b - a, threads, new Lift.Range() { public void run(long p, long q) { final int k0 = (int) p, k1 = (int) q;   // w[k + q + 127] = src at (a + k) + whole + q
            if (!wide && BLOCKED) { final long rnd = s > 0 ? 1L << (s - 1) : 0; int k = k0;   // four outputs at a time (as wtapRange)
                for (; k + 3 < k1; k += 4) { long x1 = w[k], x2 = w[k + 1], x3 = w[k + 2], x0, s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                    for (int t = 0; t < 256; t++) { x0 = x1; x1 = x2; x2 = x3; x3 = w[k + t + 3]; final long ht = h[t]; s0 += ht * x0; s1 += ht * x1; s2 += ht * x2; s3 += ht * x3; }
                    if (s > 0) { r[k] = (s0 + rnd) >> s; r[k + 1] = (s1 + rnd) >> s; r[k + 2] = (s2 + rnd) >> s; r[k + 3] = (s3 + rnd) >> s; }
                    else { r[k] = s0 << (-s); r[k + 1] = s1 << (-s); r[k + 2] = s2 << (-s); r[k + 3] = s3 << (-s); } }
                for (; k < k1; k++) { long acc = 0; for (int t = 0; t < 256; t++) acc += h[t] * w[k + t]; r[k] = s > 0 ? (acc + rnd) >> s : acc << (-s); } }
            else
            if (!wide) { final long rnd = s > 0 ? 1L << (s - 1) : 0;
                for (int k = k0; k < k1; k++) { long a0 = 0, a1 = 0, a2 = 0, a3 = 0;   // the exact sum in four parts (64-bit, proven not to overflow)
                    for (int t = 0; t < 256; t += 4) { a0 += h[t] * w[k + t]; a1 += h[t + 1] * w[k + t + 1]; a2 += h[t + 2] * w[k + t + 2]; a3 += h[t + 3] * w[k + t + 3]; }
                    final long acc = (a0 + a1) + (a2 + a3); r[k] = s > 0 ? (acc + rnd) >> s : acc << (-s); } }
            else if (s48) { for (int k = k0; k < k1; k++) { long ah = 0, al = 0;
                    for (int t = 0; t < 256; t++) { final long c = w[k + t], ht = h[t]; ah += ht * (c >> 16); al += ht * (c & 0xFFFF); }
                    final long lo0 = ah << 16; long hi = ah >> 48; final long lo = lo0 + al; hi += (al >> 63) + (U.lt(lo, lo0) ? 1 : 0);
                    if (s > 0) { final long n = lo + (1L << (s - 1)); final long h2 = hi + (U.lt(n, lo) ? 1 : 0); r[k] = (n >>> s) | (h2 << (64 - s)); } else r[k] = lo << (-s); } }
            else { for (int k = k0; k < k1; k++) { long lo = 0, hi = 0;   // LaesMath.Acc in local variables
                    for (int t = 0; t < 256; t++) { final long ht = h[t], x = w[k + t]; final long l = ht * x, hh = U.mulHighSigned(ht, x); final long n = lo + l; hi += hh + (U.lt(n, lo) ? 1 : 0); lo = n; }
                    if (s > 0) { final long n = lo + (1L << (s - 1)); final long h2 = hi + (U.lt(n, lo) ? 1 : 0); r[k] = (n >>> s) | (h2 << (64 - s)); } else r[k] = lo << (-s); } } } });
        return r;
    }
    // ---- the bit reader of laes_codec.h over a byte range of a file, read through a window
    /** port of laes_stream.h:BitRF */
    public static final class BitRF extends Codec.BitIn { In in = null; long off = 0, len = 0, pos = 0; byte[] w = new byte[0]; int wsize = 0; long wb = -1;
        // port of laes_stream.h:BitRF::left
        long left() { return len * 8 - pos; }
        // port of laes_stream.h:BitRF::byteAt
        int byteAt(long k) { if (k < wb || k >= wb + wsize) { wb = k; final long e = Math.min(len, wb + (1L << 22)); wsize = (int) (e - wb); if (w.length < wsize) w = new byte[wsize]; in.read(off + wb, w, 0, wsize); }
            return w[(int) (k - wb)] & 0xFF; }
        // port of laes_stream.h:BitRF::get
        public long get(int n) { if (fail || n < 0 || n > 64 || (long) n > left()) { fail = true; return 0; } long v = 0;
            while (n > 0) { final int bp = (int) (pos & 7), avail = 8 - bp, take = Math.min(n, avail);
                v = (take == 64 ? 0 : (v << take)) | (long) ((byteAt(pos >>> 3) >>> (avail - take)) & ((1 << take) - 1)); pos += take; n -= take; }
            return v; } }
    // a layer (n values, blocks of B) decoded from a byte range of a file into a signal file
    // port of laes_stream.h:decodeLayerToFile
    public static boolean decodeLayerToFile(In in, long off, long len, long n, long B, String out) {
        BitRF rd = new BitRF(); rd.in = in; rd.off = off; rd.len = len; SigOut o = new SigOut(out); long[] x = new long[(int) Math.min(B, Math.max(n, 0))];
        try { for (long a = 0; a < n; a += B) { final int k = (int) Math.min(B, n - a); if (!Codec.decodeBlock(rd, x, 0, k)) return false; o.put(x, 0, k); } }
        finally { o.close(); }
        return true; }
    // ---- block-wise decoder (laes::decodeLaes, piece by piece): the payload is a file; every decoded master is written as two channel files
    // of its samples >> w (top level) and its lower levels (for the masters that refer to it). piece = base-level samples per piece (a multiple
    // of 4096; level k uses piece << k).
    /** port of laes_stream.h:DecMaster */
    public static final class DecMaster { public LaesFile.MasterInfo I = new LaesFile.MasterInfo(); public String[] top = new String[2]; public String[][] lev = new String[2][]; public boolean shaOk = false; }
    // port of laes_stream.h:levelOf
    public static FileSig levelOf(DecMaster m, int c, int k) { return new FileSig(k >= m.I.stages ? m.top[c] : m.lev[c][k]); }
    // SHA-256 of a master given as two channel files of samples >> w (as laes::hashPcm of the samples); returns false when a sample lies
    // outside the master's word length (bits), which fails the master whatever the hash says
    // port of laes_stream.h:hashFiles
    public static boolean hashFiles(String c0, String c1, int w, int bits, byte[] out) {
        FileSig a = new FileSig(c0), b = new FileSig(c1); LaesFile.Sha256 s = new LaesFile.Sha256(); final long P = 1 << 20; long[] x = new long[(int) P], y = new long[(int) P]; byte[] buf = new byte[(int) P * 8]; boolean inw = true;
        try { for (long i = 0; i < a.n; i += P) { final long e = Math.min(a.n, i + P); final int k = (int) (e - i); a.get(i, e, x, 0); b.get(i, e, y, 0); int f = 0;
                for (int q = 0; q < k; q++) { inw = inw && LaesFile.inWord(x[q] << w, bits) && LaesFile.inWord(y[q] << w, bits); int v = (int) (x[q] << w); buf[f++] = (byte) v; buf[f++] = (byte) (v >>> 8); buf[f++] = (byte) (v >>> 16); buf[f++] = (byte) (v >>> 24);
                    v = (int) (y[q] << w); buf[f++] = (byte) v; buf[f++] = (byte) (v >>> 8); buf[f++] = (byte) (v >>> 16); buf[f++] = (byte) (v >>> 24); }
                s.update(buf, 0, f); } }
        finally { a.close(); b.close(); }
        s.finish(out); return inw; }
    // the levels below the top of one channel: lev[k] for k = stages - 1 .. 0, each from the level above by the forward stage
    // port of laes_stream.h:levelsDown
    public static String[] levelsDown(String top, int stages, String stem, long piece, int threads) {
        String[] lev = new String[stages];
        for (int k = stages; k > 0; k--) { FileSig x = new FileSig(k == stages ? top : lev[k]); final long ne = (x.n + 1) / 2; lev[k - 1] = stem + "_L" + (k - 1);
            SigOut o = new SigOut(lev[k - 1]); final long P = piece << (k - 1);
            try { for (long a = 0; a < ne; a += P) { long[][] sd = fwdPiece(x, a, Math.min(ne, a + P), threads); o.put(sd[0], 0, sd[0].length); } }
            finally { o.close(); x.close(); } }
        return lev; }
    static void removeFile(String p) { new java.io.File(p).delete(); }   // C++ remove(): the result is not checked there either
    // port of laes_stream.h:decodeLaesS
    public static String decodeLaesS(String bodyPath, LaesFile.Pcm core, int baseRate, String wd, List<DecMaster> out, long piece, int threads) { return decodeLaesS(bodyPath, core, baseRate, wd, out, piece, threads, null); }
    public static String decodeLaesS(String bodyPath, LaesFile.Pcm core, int baseRate, String wd, List<DecMaster> out, long piece, int threads, LaesFile.Head head) {
        out.clear(); if (head != null) { head.def = 0; head.coreFlags = 0; } final In body = new In(bodyPath);
        try { return decodeLaesSImpl(body, core, baseRate, wd, out, piece, threads, head); } finally { body.close(); } }
    private static String decodeLaesSImpl(final In body, LaesFile.Pcm core, int baseRate, String wd, List<DecMaster> out, long piece, int threads, LaesFile.Head head) {
        if (body.n < 64 + 4) return "LAES block too short";
        { java.util.zip.CRC32 crc = new java.util.zip.CRC32(); final long last = body.n - 4; byte[] b = new byte[1 << 22];   // CRC-32 IEEE 802.3, reflected: the bitwise loop of the C++
          for (long a = 0; a < last; a += 1 << 22) { final int k = (int) Math.min(1 << 22, last - a); body.read(a, b, 0, k); crc.update(b, 0, k); }
          byte[] t = new byte[4]; body.read(last, t, 0, 4); final long stored = com.lae.core.Le.u32(t, 0); if (crc.getValue() != stored) return "LAES block damaged (CRC-32)"; }
        final long end = body.n - 4; final long[] pos = {0}; final boolean[] trunc = {false}; final byte[] t8 = new byte[8];
        class Rd { long u(int k) { if (pos[0] + k > end) { trunc[0] = true; return 0; } body.read(pos[0], t8, 0, k); pos[0] += k; long v = 0; for (int i = 0; i < k; i++) v |= (long) (t8[i] & 0xFF) << (8 * i); return v; } }
        Rd R = new Rd();
        long magic = R.u(4), ver = R.u(2), hs = R.u(2), base = R.u(4), cbits = R.u(1), ch = R.u(1), nm = R.u(1), cmode = R.u(1), nc = R.u(8); byte[] csha = new byte[32]; if (pos[0] + 32 <= end) { body.read(pos[0], csha, 0, 32); pos[0] += 32; } else trunc[0] = true;
        final byte[] hb = new byte[8]; if (pos[0] + 8 <= end) { body.read(pos[0], hb, 0, 8); pos[0] += 8; } else trunc[0] = true;
        if (trunc[0] || magic != 0x3245414CL) return "not an LAES block"; if ((ver != 1 && ver != 2) || hs != 64) return "unsupported LAES version";
        final LaesFile.Head hd = new LaesFile.Head(); if ((base != 44100 && base != 48000) || (int) base != baseRate || (int) cbits != core.bits || ch != 2 || cmode > 1 || !LaesFile.headerCountOk(ver, nm) || !LaesFile.headFrom(ver, hb, 0, nm, hd)) return "invalid LAES header";
        if (head != null) { head.def = hd.def; head.coreFlags = hd.coreFlags; }
        if (nc != core.frames()) return "core master length differs from the LAES header";
        { byte[] h = new byte[32]; LaesFile.hashAnyCore(core, h); if (!LaesFile.eq32(h, 0, csha, 0)) return "core master differs from the one this LAES block was made for (SHA-256)"; }
        for (long mi = 0; mi < nm; mi++) { DecMaster D = new DecMaster(); LaesFile.MasterInfo I = D.I;
            I.bits = (int) R.u(1); I.stages = (int) R.u(1); I.w = (int) R.u(1); I.storage = (int) R.u(1); I.refTier = (int) R.u(1);
            final long nl = R.u(1), fl = R.u(1), mp = R.u(1); I.rate = (int) R.u(4); R.u(4); final long F64 = R.u(8);
            I.whole = R.u(8); I.frac = (int) R.u(4); R.u(4); byte[] sha = new byte[32]; if (pos[0] + 32 <= end) { body.read(pos[0], sha, 0, 32); pos[0] += 32; } else trunc[0] = true;
            if (trunc[0]) return "LAES block truncated";
            if (ver == 2) { if (pos[0] + nl > end) return "LAES block truncated"; final byte[] nb = new byte[(int) nl]; if (nl != 0) body.read(pos[0], nb, 0, (int) nl); pos[0] += nl;
                if ((fl & ~(long) LaesFile.NO_DERIVE) != 0) return "invalid master record"; if (!LaesFile.validMasterName(nb, 0, nb.length)) return "invalid master name";
                I.name = LaesFile.nameOf(nb, 0, nb.length); I.flags = (int) fl; I.menu = (int) mp; }
            if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int) mi) || (I.storage == 2 && I.refTier < 1)
                || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 == 0 || U.gt((F64 + (1L << I.stages) - 1) >>> I.stages, nc + (1L << 24))
                || I.whole <= -(1L << 40) || I.whole >= (1L << 40) || !LaesFile.tWithinTrack(I.whole, I.frac, nc)) return "invalid master record";
            if (F64 < 0) return "master too long for this decoder"; I.frames = F64;   // F64 > INT64_MAX: as laes_play.h / laes_stream.h since 635019a
            System.arraycopy(sha, 0, I.sha, 0, 32); final int j = I.storage == 2 ? Math.min(I.stages, out.get(I.refTier - 1).I.stages) : 0; final long nj = LaesFile.levLen(I.frames, I.stages - j);
            final String stem = wd + "/d" + mi;
            for (int c = 0; c < 2; c++) { final String sc = stem + "_c" + c;
                final long eb = R.u(4); if (trunc[0] || pos[0] + eb > end) return "LAES block truncated";
                final String xj = sc + "_x" + j;
                if (!decodeLayerToFile(body, pos[0], eb, nj, 4096L << j, I.storage != 1 ? sc + "_e" : xj)) return "damaged base layer"; pos[0] += eb;
                if (I.storage != 1) {   // level j = layer + the reference of the tier (laes::refLen)
                    Sig src; int up; FileSig srcF = null; if (I.refTier == 0) { src = new MemSig(core.c[c]); up = (I.bits - I.w) - core.bits; }
                    else { srcF = levelOf(out.get(I.refTier - 1), c, j); src = srcF; final LaesFile.MasterInfo T = out.get(I.refTier - 1).I; up = (I.bits - I.w) - (T.bits - T.w); }
                    final long wj = I.whole * (1L << j) + (j != 0 ? ((I.frac & 0xFFFFFFFFL) >>> (32 - j)) : 0); final int fj = (int) ((I.frac & 0xFFFFFFFFL) << j);
                    final long[] h = LaesMath.refTaps(fj); { FileSig e = new FileSig(sc + "_e"); SigOut o = new SigOut(xj); final long P = piece << j; long[] ev = new long[(int) Math.min(P, nj)];
                        try { for (long a = 0; a < nj; a += P) { final long b = Math.min(nj, a + P); final int k = (int) (b - a); e.get(a, b, ev, 0); long[] rv = refPiece(src, h, wj, up, a, b, threads);
                                for (int i = 0; i < k; i++) ev[i] += rv[i]; o.put(ev, 0, k); } }
                        finally { o.close(); e.close(); if (srcF != null) srcF.close(); } }
                    removeFile(sc + "_e"); }
                String cur = xj;
                for (int k = j + 1; k <= I.stages; k++) { final long nh = LaesFile.levLen(I.frames, I.stages - k); final long db = R.u(4); if (trunc[0] || pos[0] + db > end) return "LAES block truncated";
                    final String dk = sc + "_d" + k; if (!decodeLayerToFile(body, pos[0], db, nh / 2, 4096L << (k - 1), dk)) return "damaged octave layer"; pos[0] += db;
                    final String nx = sc + "_x" + k;
                    { FileSig s = new FileSig(cur), d = new FileSig(dk);
                      try { if (s.n + d.n != nh) return "damaged octave layer"; SigOut o = new SigOut(nx); final long P = piece << (k - 1);
                          try { for (long a = 0; a < s.n; a += P) { long[] x = invPiece(s, d, nh, a, Math.min(s.n, a + P), threads); o.put(x, 0, x.length); } } finally { o.close(); } }
                      finally { s.close(); d.close(); } }
                    removeFile(dk); removeFile(cur); cur = nx; }
                D.top[c] = cur; }
            byte[] h = new byte[32]; final boolean inw = hashFiles(D.top[0], D.top[1], I.w, I.bits, h); D.shaOk = inw && LaesFile.eq32(h, 0, I.sha, 0);
            for (int c = 0; c < 2; c++) D.lev[c] = levelsDown(D.top[c], I.stages, stem + "_c" + c + "_lv", piece, threads);
            out.add(D); }
        { List<LaesFile.MasterInfo> mi = new ArrayList<LaesFile.MasterInfo>(); for (DecMaster D : out) mi.add(D.I); if (!LaesFile.menuOk(mi)) return "invalid menu positions"; }
        if (pos[0] != end) return "unexpected bytes after the last master";
        return ""; }
}
