// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.Cv;
import com.lae.core.LMath;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Port of src/routes_blk.cpp. Block-wise routes.cpp (2026-10-29): the same T and F, computed one master-rate block at a time. Every
 * filter (up-conversion stages, the T lowpass, the leak lowpass) is evaluated over the window its block needs, with the same mirrored
 * edges and the same arithmetic per output sample; the cut-off's cross-spectrum is added up segment by segment in the same order. The
 * regenerated dither goes to a temporary file next to T (the random numbers are drawn once, in order).
 * usage: routes &lt;m.raw&gt; &lt;rate&gt; &lt;b16.s16&gt; &lt;b24.s32&gt; &lt;T out&gt; &lt;F out | -&gt; [cut-off]; environment DITHERFLAGS, CDEXT,
 * CUTOFFOUT, LEAKIN, PARTTHREADS
 * std::complex (b[k] * conj(a[k]), norm): MSVC &lt;complex&gt; 14.44: operator* = _Mul: re = a.re b.re - a.im b.im, im = a.re b.im +
 * a.im b.re; conj = (re, -im); norm = re re + im im. Written out below in that order with the conj's negation kept.
 */
final class RoutesBlk {
    private RoutesBlk() {}

    interface ChanFn { void run(int c); }
    interface RangeFn { void run(long i0, long i1); }
    interface WinFn { BlkIo.WinD get(long a, long b); }

    static RuntimeException rethrowable(Throwable t) { if (t instanceof RuntimeException) return (RuntimeException) t; if (t instanceof Error) throw (Error) t; return new RuntimeException(t); }

    // port of routes_blk.cpp:twoWay (as routes.cpp): f(1) on its own thread (unless PARTTHREADS=1), f(0) here; exceptions re-thrown after both
    static void twoWay(final EncEnv env, final ChanFn f) {
        final Throwable[] x = new Throwable[2]; Thread t = null; boolean started = false; final int pt = env.partThreads();
        if (!(pt == 1)) try { t = new Thread(new Runnable() { @Override public void run() { try { f.run(1); } catch (Throwable e) { x[1] = e; } } }); t.start(); started = true; } catch (Throwable e) { started = false; }
        try { f.run(0); } catch (Throwable e) { x[0] = e; }
        if (started) join(t); else { try { f.run(1); } catch (Throwable e) { x[1] = e; } }
        if (x[0] != null) throw rethrowable(x[0]); if (x[1] != null) throw rethrowable(x[1]); }
    static void join(Thread t) { boolean intr = false; for (;;) { try { t.join(); break; } catch (InterruptedException e) { intr = true; } } if (intr) Thread.currentThread().interrupt(); }

    // port of routes_blk.cpp:parFor: hardware threads / 2 (or PARTTHREADS / 2) threads over [0, n) in equal parts; inline when n < 65536
    static void parFor(final EncEnv env, final long n, final RangeFn f) {
        final int pt = env.partThreads(); final int hc = pt >= 0 ? Math.max(1, pt) : env.hwThreads;
        final int th = Math.max(1, hc / 2); if (th <= 1 || n < 65536) { f.run(0L, n); return; }
        List<Thread> pool = new ArrayList<Thread>(); final Throwable[] xs = new Throwable[th]; final long step = (n + th - 1) / th; List<Integer> inl = new ArrayList<Integer>();
        for (int k = 1; k < th; k++) { final long a = k * step, b = Math.min(n, a + step); if (a >= b) continue; final int kk = k;
            try { Thread t = new Thread(new Runnable() { @Override public void run() { try { f.run(a, b); } catch (Throwable e) { xs[kk] = e; } } }); t.start(); pool.add(t); } catch (Throwable e) { inl.add(k); } }
        try { f.run(0L, Math.min(n, step)); } catch (Throwable e) { xs[0] = e; }
        for (int k : inl) { long a = k * step, b = Math.min(n, a + step); try { f.run(a, b); } catch (Throwable e) { xs[k] = e; } }
        for (Thread t : pool) join(t); for (Throwable x : xs) if (x != null) throw rethrowable(x); }

    // port of routes_blk.cpp:upEval
    // level k of the up-conversion (level 0 = the base-rate signal from p0, level k has Lb << k samples) over [a, b); up2() of routes.cpp per stage
    static BlkIo.WinD upEval(final EncEnv env, int k, final long a, long b, long Lb, WinFn p0) {
        if (k == 0) return p0.get(a, b);
        final int s = k - 1; int[] hf = new int[1]; final int[] h = BaseBlk.tab(s, hf); final int half = hf[0]; final long n = Lb << s;
        long[] lh = BlkIo.cover((a - half) / 2 - 2, (b - 1 + half) / 2 + 1, n);
        final BlkIo.WinD x = upEval(env, k - 1, lh[0], lh[1] + 1, Lb, p0);
        final BlkIo.WinD y = new BlkIo.WinD(); y.base = a; y.d = new double[(int) (b - a)];
        parFor(env, b - a, new RangeFn() { @Override public void run(long i0, long i1) { for (long i = a + i0; i < a + i1; i++) {
            if (i % 2 == 0) { y.d[(int) (i - a)] = x.at(i / 2); continue; }
            double acc = 0; for (int j = 1, t = 0; j <= half; j += 2, t++) { long p = (i - j) / 2, q = (i + j) / 2; double hv = 2.0 * h[t] / 2147483648.0;
                long pp = p < 0 ? -p : p, qq = q < n ? q : 2 * (n - 1) - q; pp = Math.min(Math.max(pp, 0L), n - 1); qq = Math.min(Math.max(qq, 0L), n - 1);
                if (i - j < 0) pp = Math.min(Math.max((j - i) / 2, 0L), n - 1); acc += hv * x.at(pp) + hv * x.at(qq); }
            y.d[(int) (i - a)] = acc; } } });
        return y; }

    // port of routes_blk.cpp:main lambda I0 (both copies are the same function)
    static double I0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }

    // port of routes_blk.cpp:main
    static int main(final EncEnv env, String[] argv) {
        final double rate = Fmt.atof(argv[2]);
        final BlkIo.ReaderI mr = new BlkIo.ReaderI(argv[1], 2), b24 = new BlkIo.ReaderI(argv[4], 2); final BlkIo.ReaderS b16 = new BlkIo.ReaderS(argv[3]);
        BlkIo.ReaderS dvr0 = null; BlkIo.ReaderI leak0 = null; BlkIo.Writer Tf = null, Ff = null; final String dvPath = argv[5] + ".dv";
        try {
            final long L = mr.frames, Lb = b16.frames;
            final double baseRate = rate % 44100.0 == 0 ? 44100.0 : 48000.0; final int stages = (int) Cv.round(LMath.log2(rate / baseRate));
            // dither regeneration (same seeds and draw order as base_layer / decoder), stored as the integer before the division by 16
            byte[] df = new byte[0]; long nbk = 0; if (env.get("DITHERFLAGS") != null) { df = EncIo.readAll(env.get("DITHERFLAGS")); nbk = EncIo.count(df.length) / 2; }
            { BlkIo.Writer dv = new BlkIo.Writer(env, dvPath); try { int[] st = {0x12345678, 0x12345678 + 0x9E3779B9}; final long BS = 1L << 18;
                  for (long a = 0; a < Lb; a += BS) { final long b = Math.min(Lb, a + BS); short[] buf = new short[(int) (b - a) * 2];
                      for (long i = a; i < b; i++) for (int c = 0; c < 2; c++) { int s = st[c]; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u1 = (s >>> 20) & 0xFFF; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u2 = (s >>> 20) & 0xFFF; st[c] = s;
                          buf[(int) (i - a) * 2 + c] = (short) ((df.length != 0 && df[(int) (c * nbk + i / 4096)] != 0) ? (u1 + u2 - 4096) : 0); }
                      dv.put(buf); }
                  dv.finish(); dv = null; } finally { if (dv != null) dv.close(); } }
            final BlkIo.ReaderS dvr = dvr0 = new BlkIo.ReaderS(dvPath);
            final long Lu = Lb << stages;   // the up-converted signal; resized to L (zeros beyond)
            final int M = 16384; final long BM = 64L * M;   // master samples per block (whole cross-spectrum segments, a multiple of 2^stages)
            double fc = argv.length > 7 ? Fmt.atof(argv[7]) : 0;
            if (fc <= 0) { final double[] sxy = new double[M / 2], sxx = new double[M / 2], syy = new double[M / 2], sxyi = new double[M / 2], hannM = new double[M];
                for (int i = 0; i < M; i++) hannM[i] = 0.5 - 0.5 * LMath.cospi(2.0 * i / M);   // the Hann window, once
                double[] are = new double[M], aim = new double[M], bre = new double[M], bim = new double[M];
                for (long A = 0; A < L; A += BM) { env.checkCancel(); final long B = Math.min(L, A + BM); if (A + M > L) break;
                    final BlkIo.WinD[] uv = new BlkIo.WinD[2]; final long AA = A;
                    twoWay(env, new ChanFn() { @Override public void run(final int c) { uv[c] = new BlkIo.WinD(); uv[c].base = AA; uv[c].d = new double[(int) (B - AA)]; final long e = Math.min(B, Lu); if (e <= AA) return;
                        BlkIo.WinD w = upEval(env, stages, AA, e, Lb, new WinFn() { @Override public BlkIo.WinD get(long a, long b) { return vWin(b16, dvr, a, b, c); } }); System.arraycopy(w.d, 0, uv[c].d, 0, w.d.length); } });
                    int[] m = mr.read(A, B);
                    for (long st0 = A; st0 < B && st0 + M <= L; st0 += M) for (int c = 0; c < 2; c++) {
                        for (int i = 0; i < M; i++) { final double w = hannM[i]; are[i] = (m[(int) (st0 - A + i) * 2 + c] >> 8) * w; aim[i] = 0; bre[i] = uv[c].at(st0 + i) * w; bim[i] = 0; }
                        Fft.fft(are, aim, M); Fft.fft(bre, bim, M);
                        for (int k = 0; k < M / 2; k++) { final double ar = are[k], ai = -aim[k], br = bre[k], bi = bim[k];   // conj(a[k]) = (ar, -aim)
                            sxy[k] += br * ar - bi * ai; sxyi[k] += br * ai + bi * ar; sxx[k] += are[k] * are[k] + aim[k] * aim[k]; syy[k] += br * br + bi * bi; } } }
                int k15 = Cv.d2i(15000.0 / (rate / M)), kN = Cv.d2i(baseRate / 2 / (rate / M)) - 9; double mxx = 0; for (int k = k15; k <= kN; k++) mxx = Cv.max(mxx, sxx[k]);
                fc = 15000;
                final boolean indep = env.get("CDEXT") != null;
                for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += sxy[k + d]; xx += sxx[k + d]; yy += syy[k + d]; }
                    if (indep) { if (xx < 1e-9 * mxx * 17) continue; if (h > 0 && h * h >= 0.98 * xx * yy) { fc = (k + 1) * rate / M; break; } continue; }
                    if (xx < 1e-9 * mxx * 17) continue; if (h / xx >= 0.9886) { fc = (k + 1) * rate / M; break; } }
                if (indep) {
                    double fm = 15000; for (int k = kN; k >= k15; k--) { double h = 0, xx = 0, yy = 0; for (int d = -8; d <= 8; d++) { h += Math.sqrt(sxy[k + d] * sxy[k + d] + sxyi[k + d] * sxyi[k + d]); xx += sxx[k + d]; yy += syy[k + d]; }
                        if (xx < 1e-9 * mxx * 17) continue; if (h * h >= 0.98 * xx * yy) { fm = (k + 1) * rate / M; break; } }
                    env.printf("CD cut-off by magnitude coherence (diagnostic only): %.0f Hz\n", fm);
                    if (Math.abs(fm - fc) > 500 || fc <= 15000) env.printf("WARNING: the cut-off of the independent CD looks wrong (%.0f Hz, magnitude coherence %.0f Hz): CD and master may be offset in time\n", fc, fm); } }
            env.printf("CD cut-off: %.0f Hz\n", fc);
            if (env.get("CUTOFFOUT") != null) { String t = Fmt.c("%.0f\n", fc); EncIo.writeText(env, env.get("CUTOFFOUT"), t); }
            final int NT = 255; final double[] h = new double[2 * NT + 1]; double bes0 = 0; { bes0 = I0(9.0);
                for (int i = -NT; i <= NT; i++) { double r = (double) i / NT; double w = I0(9.0 * Math.sqrt(Cv.max(0.0, 1 - r * r))) / bes0; double x = 2 * fc / baseRate; h[i + NT] = (i == 0 ? x : LMath.sinpi(x * i) / (Math.PI * i)) * w; } }
            final boolean full = fc >= baseRate / 2 - 1;
            if (env.get("LEAKIN") != null) { leak0 = new BlkIo.ReaderI(env.get("LEAKIN"), 2); if (leak0.frames == 0) throw EncIo.fail("leak compensation: empty leak file"); }
            final BlkIo.ReaderI leak = leak0;
            final int NL = 1023; final double[] hl = new double[2 * NL + 1]; double xl = 2 * (fc - 1150.0) / baseRate;
            { double b0 = I0(9.0);
              for (int k = -NL; k <= NL; k++) { double rr = (double) k / NL; double w = I0(9.0 * Math.sqrt(Cv.max(0.0, 1 - rr * rr))) / b0; hl[k + NL] = (k == 0 ? xl : LMath.sinpi(xl * k) / (Math.PI * k)) * w; } }
            final Ctx X = new Ctx(); X.env = env; X.b16 = b16; X.dvr = dvr; X.b24 = b24; X.leak = leak; X.Lb = Lb; X.NL = NL; X.NT = NT; X.hl = hl; X.h = h; X.full = full;
            // T and F are stored as 24-bit values. A value outside is limited below as before, and counted (2026-11-14): the decoder would
            // then not give back the master, so the split stops after the last block. It cannot happen with the CD layer derived from the
            // master; it happens with an independent CD that differs from the master by more than full scale (inverted polarity, loud passages).
            final long[] overT = {0, 0}, overF = {0, 0};   // per channel (the two channels run in two threads)
            Tf = new BlkIo.Writer(env, argv[5]); if (!argv[6].equals("-")) Ff = new BlkIo.Writer(env, argv[6]);   // "-" (as routes_blk.cpp): the frequency layer signal is not written (the second split after leak compensation needs only T; F is still computed and its 24-bit overflow counted)
            for (long A = 0; A < L; A += BM) { env.checkCancel(); final long B = Math.min(L, A + BM); final long ta = A >> stages, tb = B == L ? Lb : (B >> stages);
                final int[] Tout = new int[(int) (Math.max(0L, tb - ta)) * 2], Fout = new int[(int) (B - A) * 2]; final int[] m = mr.read(A, B); final long AA = A;
                twoWay(env, new ChanFn() { @Override public void run(final int c) {   // chanTF
                    final BlkIo.WinD[] Tw = {null}; final boolean[] haveT = {false};   // the T window the up-conversion asked for (it covers this block's T samples)
                    WinFn p0 = new WinFn() { @Override public BlkIo.WinD get(long a, long b) { BlkIo.WinD T = X.tWin(a, b, c); BlkIo.WinD u = vWin(b16, dvr, a, b, c); for (long i = a; i < b; i++) u.d[(int) (i - a)] += T.d[(int) (i - a)]; Tw[0] = T; haveT[0] = true; return u; } };
                    final long e = Math.min(B, Lu); BlkIo.WinD u = new BlkIo.WinD(); if (e > AA) u = upEval(env, stages, AA, e, Lb, p0);
                    if (tb > ta) { if (!haveT[0] || Tw[0].base > ta || Tw[0].base + Tw[0].d.length < tb) Tw[0] = X.tWin(ta, tb, c);
                        for (long i = ta; i < tb; i++) { final double tv = Tw[0].at(i); if (tv < -8388608.0 || tv > 8388607.0) overT[c]++; Tout[(int) (i - ta) * 2 + c] = Cv.d2i(Cv.max(-8388608.0, Cv.min(8388607.0, tv))) * 256; } }
                    for (long i = AA; i < B; i++) { final double ui = i < e ? u.at(i) : 0.0; long d = (long) (m[(int) (i - AA) * 2 + c] >> 8) - Cv.llround(ui); if (d < -8388608L || d > 8388607L) overF[c]++; d = Math.max(-8388608L, Math.min(8388607L, d)); Fout[(int) (i - AA) * 2 + c] = (int) (d * 256); } } });
                Tf.put(Tout); if (Ff != null) Ff.put(Fout); }
            if (overT[0] + overT[1] + overF[0] + overF[1] > 0)   // stops before finish(): the writers remove their unfinished files (finally: close)
                throw EncIo.fail(Fmt.c("the difference between the master and the CD / core layer does not fit into 24 bits at %lld sample(s) of the depth layer signal and %lld sample(s) of the frequency layer signal, so the file would not decode to the master. This happens when an independent CD differs from the master by more than full scale, for example with inverted polarity and passages above -6 dBFS. Correct the polarity or the level of the CD and encode again",
                                       overT[0] + overT[1], overF[0] + overF[1]));
            Tf.finish(); Tf = null; if (Ff != null) { Ff.finish(); Ff = null; }
            return 0; }
        finally { if (Tf != null) Tf.close(); if (Ff != null) Ff.close(); mr.close(); b24.close(); b16.close(); if (dvr0 != null) dvr0.close(); if (leak0 != null) leak0.close(); new File(dvPath).delete(); } }

    // port of routes_blk.cpp:main lambda vWin
    static BlkIo.WinD vWin(BlkIo.ReaderS b16, BlkIo.ReaderS dvr, long a, long b, int c) { BlkIo.WinD w = new BlkIo.WinD(); w.base = a; w.d = new double[(int) (b - a)]; short[] x = b16.read(a, b), d = dvr.read(a, b);
        for (long i = a; i < b; i++) w.d[(int) (i - a)] = 256.0 * x[(int) (i - a) * 2 + c] - d[(int) (i - a) * 2 + c] / 16.0; return w; }

    /** the state the C++ lambdas rWin / tWin capture */
    static final class Ctx { EncEnv env; BlkIo.ReaderS b16, dvr; BlkIo.ReaderI b24, leak; long Lb; int NL, NT; double[] hl, h; boolean full;
        // port of routes_blk.cpp:main lambda mir
        long mir(long j) { return j < 0 ? -j : (j >= Lb ? 2 * (Lb - 1) - j : j); }
        // port of routes_blk.cpp:main lambda rWin: r = base24 - v, minus the leak's part below the frequency layer's start (as routes.cpp)
        BlkIo.WinD rWin(final long a, long b, int c) { final BlkIo.WinD w = vWin(b16, dvr, a, b, c); int[] x = b24.read(a, b);
            for (long i = a; i < b; i++) w.d[(int) (i - a)] = x[(int) (i - a) * 2 + c] - w.d[(int) (i - a)];
            if (leak != null) { long[] lh = BlkIo.cover(a - NL, b - 1 + NL, Lb); final long lo = lh[0], hi = lh[1]; final BlkIo.WinD lk = new BlkIo.WinD(); lk.base = lo; lk.d = new double[(int) (hi + 1 - lo)];
                final long e = Math.min(hi + 1, leak.frames); if (e > lo) { int[] l = leak.read(lo, e); for (long i = lo; i < e; i++) lk.d[(int) (i - lo)] = l[(int) (i - lo) * 2 + c] / 16.0; }
                parFor(env, b - a, new RangeFn() { @Override public void run(long i0, long i1) { for (long i = a + i0; i < a + i1; i++) { double acc = 0; for (int k = -NL; k <= NL; k++) acc += hl[k + NL] * lk.at(mir(i + k)); w.d[(int) (i - a)] -= acc; } } }); }
            return w; }
        // port of routes_blk.cpp:main lambda tWin
        BlkIo.WinD tWin(final long a, long b, int c) { final BlkIo.WinD T = new BlkIo.WinD(); T.base = a; T.d = new double[(int) (b - a)];
            if (full) { BlkIo.WinD r = rWin(a, b, c); for (long i = a; i < b; i++) T.d[(int) (i - a)] = Cv.llround(r.d[(int) (i - a)]); return T; }
            long[] lh = BlkIo.cover(a - NT, b - 1 + NT, Lb); final BlkIo.WinD r = rWin(lh[0], lh[1] + 1, c);
            parFor(env, b - a, new RangeFn() { @Override public void run(long i0, long i1) { for (long i = a + i0; i < a + i1; i++) { double acc = 0; for (int k = -NT; k <= NT; k++) acc += h[k + NT] * r.at(mir(i + k)); T.d[(int) (i - a)] = Cv.llround(acc); } } });
            return T; } }
}
