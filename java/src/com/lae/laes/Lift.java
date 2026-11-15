// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.util.ArrayDeque;

/**
 * Port of src/laes_lift.h (namespace laesfast), the portable / scalar form. LAES rate-layer construction L, fast form (2026-10-28).
 * Same results as the reference (laes_math.h fwd / inv), bit for bit:
 *   d[i] = xo[i] - P(xe, i),  P = round(sum_t h0[t] (xe[m(i-t)] + xe[m(i+1+t)]) / 2^30)      h0 = lae::kHalf0 (40 taps, Q31)
 *   s[i] = xe[i] + U(d, i),   U = round(sum_t u[t]  (d[m(i-1-t)] + d[m(i+t)])  / 2^31)      u  = kU0 (60 taps, Q31)
 *   round = add 2^(s-1), arithmetic shift right (ties up); m = symmetric extension, period 2(n-1), m = 0 for n = 1.
 * What is faster, without changing a single value:
 *   - the symmetric extension is computed only near the two ends; in between the samples are indexed directly;
 *   - the two taps of a pair are added first and multiplied once (integer arithmetic: the exact sum does not depend on the order);
 *   - 64-bit accumulation when it provably cannot overflow (2 x max|x| x sum|h| + rounding < 2^63, checked per call from the data),
 *     otherwise 128-bit accumulation as in the reference;
 *   - the samples of one step are split across threads (each output depends only on the input of that step).
 * Java: indices are 64-bit (the C++ "long" of these functions is 32-bit on MSVC; within that range the values are the same).
 */
public final class Lift {
    private Lift() {}

    public static final long[] kU0 = LaesMath.kU0;
    static final long[] kHalf0 = LaesMath.kHalf0;

    /** 128-bit accumulator (same as LaesMath.Acc) */
    // port of laes_lift.h:Acc128
    public static final class Acc128 {
        public long lo, hi;
        // port of laes_lift.h:Acc128::mac
        public void mac(long a, long b) { long l = a * b, h = com.lae.core.U.mulHighSigned(a, b); long n = lo + l; hi += h + (com.lae.core.U.lt(n, lo) ? 1 : 0); lo = n; }
        // port of laes_lift.h:Acc128::shr
        public long shr(int s) { long add = 1L << (s - 1), n = lo + add; long h = hi + (com.lae.core.U.lt(n, lo) ? 1 : 0); return (n >>> s) | (h << (64 - s)); }
    }
    // port of laes_lift.h:mir
    public static long mir(long i, long n) { if (n == 1) return 0; long p = 2 * (n - 1); i %= p; if (i < 0) i += p; return i < n ? i : p - i; }
    // port of laes_lift.h:maxAbs
    public static long maxAbs(long[] x, int len) { long m = 0; for (int k = 0; k < len; k++) { long v = x[k]; long a = v < 0 ? (v == Long.MIN_VALUE ? Long.MAX_VALUE : -v) : v;   /* MIN_VALUE counts as the largest (wide path) */ if (a > m) m = a; } return m; }
    public static long maxAbs(long[] x) { return maxAbs(x, x.length); }
    // port of laes_lift.h:sumAbs
    static long sumAbs(long[] h, int NT) { long s = 0; for (int t = 0; t < NT; t++) s += h[t] < 0 ? -h[t] : h[t]; return s; }
    // one filtered value: sum_t h[t] (x[a(t)] + x[b(t)]) with a(t) = i + oa - t, b(t) = i + ob + t, rounded by 2^sh
    // port of laes_lift.h:tap
    static long tap(int NT, boolean WIDE, long[] h, long[] x, long n, long i, long oa, long ob, int sh) {
        final long a0 = i + oa - (NT - 1), b1 = i + ob + (NT - 1);
        if (a0 >= 0 && b1 < n) {   // interior: direct indexing
            final int pa = (int) (i + oa), pb = (int) (i + ob);
            if (!WIDE) { long acc = 0; for (int t = 0; t < NT; t++) acc += h[t] * (x[pa - t] + x[pb + t]); return (acc + (1L << (sh - 1))) >> sh; }
            Acc128 A = new Acc128(); for (int t = 0; t < NT; t++) { A.mac(h[t], x[pa - t]); A.mac(h[t], x[pb + t]); } return A.shr(sh); }
        if (!WIDE) { long acc = 0; for (int t = 0; t < NT; t++) acc += h[t] * (x[(int) mir(i + oa - t, n)] + x[(int) mir(i + ob + t, n)]); return (acc + (1L << (sh - 1))) >> sh; }
        Acc128 A = new Acc128(); for (int t = 0; t < NT; t++) { A.mac(h[t], x[(int) mir(i + oa - t, n)]); A.mac(h[t], x[(int) mir(i + ob + t, n)]); } return A.shr(sh);
    }
    // 64-bit accumulation is exact when 2 x max|x| x sum|h| + 2^(sh-1) stays below 2^63 (no partial sum can exceed the bound either)
    // port of laes_lift.h:fits64
    static boolean fits64(long[] h, int NT, long mx, int sh) {
        final double b = 2.0 * (double) mx * (double) sumAbs(h, NT) + (double) (1L << (sh - 1)); return b < 9.0e18; }   // 2^63 = 9.22e18

    /** a body of parallelFor: values [a, b) */
    public interface Range { void run(long a, long b); }

    // ---- worker threads kept between calls (2026-11-02): parallelFor used to start and join new threads on every call, which cost more
    // than the work in the many small calls of piecewise decoding. The pool starts a thread when no idle one is there (at most the logical
    // cores - 1) and a thread ends after 2 s without work, so none outlives a run by long. The split of the work and every output value are
    // the same as before; only the threads that run the chunks differ.
    // port of laes_lift.h:Pool
    static final class Pool {
        private static final Pool P = new Pool();
        // port of laes_lift.h:Pool::get
        static Pool get() { return P; }   // never destroyed: idle threads end by themselves
        private final ArrayDeque<Runnable> q = new ArrayDeque<Runnable>(); private int idle = 0, alive = 0; private final int maxThreads;
        // port of laes_lift.h:Pool::Pool
        private Pool() { final int h = Runtime.getRuntime().availableProcessors(); maxThreads = h > 1 ? h - 1 : 1; }
        // port of laes_lift.h:Pool::push
        void push(Runnable f) {
            synchronized (this) { q.addLast(f);
                if (idle == 0 && alive < maxThreads) { alive++;
                    try { Thread t = new Thread(new Runnable() { public void run() { loop(); } }, "laes-pool"); t.setDaemon(true); t.start(); }
                    catch (Throwable x) { alive--; } }
                notify(); }
        }
        // port of laes_lift.h:Pool::runOne
        boolean runOne() { Runnable f; synchronized (this) { f = q.pollFirst(); } if (f == null) return false; f.run(); return true; }
        // port of laes_lift.h:Pool::loop
        private void loop() {
            for (;;) { Runnable f;
                synchronized (this) {
                    final long deadline = System.nanoTime() + 2000000000L;
                    while (q.isEmpty()) { final long w = (deadline - System.nanoTime()) / 1000000L;
                        if (w <= 0) { alive--; return; }
                        idle++; try { wait(w); } catch (InterruptedException e) { idle--; alive--; return; } idle--; }
                    f = q.pollFirst(); }
                f.run(); }
        }
    }

    static RuntimeException rethrow(Throwable x) {
        if (x instanceof RuntimeException) throw (RuntimeException) x;
        if (x instanceof Error) throw (Error) x;
        throw new RuntimeException(x);
    }

    // splits [0, n) across threads (the calling thread takes the first chunk and then helps with queued chunks); an exception is re-thrown
    // after all chunks have finished
    // port of laes_lift.h:parallelFor
    // Java (2026-11-04, speed; every body computes each index from its inputs alone, so the values never depend on the split): [0, n) is cut
    // into up to 4 x threads chunks of at least kMinChunk indices, which the calling thread and threads - 1 pool workers take in turn (a
    // faster core takes more chunks: phones mix fast and slow cores); a range of fewer than 2 x kMinChunk indices runs here. The exception
    // re-thrown is that of the lowest chunk that threw, after all chunks have finished.
    static final int kMinChunk = 2048;
    public static void parallelFor(long n, int threads, final Range f) {
        if (threads <= 1 || n < 2 * kMinChunk) { f.run(0, n); return; }
        final int nc = (int) Math.min((long) threads * 4, n / kMinChunk); final long step = (n + nc - 1) / nc; final Throwable[] xs = new Throwable[nc];
        final java.util.concurrent.atomic.AtomicInteger next = new java.util.concurrent.atomic.AtomicInteger(0);
        final Runnable work = new Runnable() { public void run() { for (;;) { final int c = next.getAndIncrement(); if (c >= nc) return; final long a = c * step, b = Math.min(n, a + step);
            if (a < b) { try { f.run(a, b); } catch (Throwable x) { xs[c] = x; } } } } };
        final Object dm = new Object(); final int[] pending = {0}; final int helpers = Math.min(threads, nc) - 1;
        for (int k = 0; k < helpers; k++) { synchronized (dm) { pending[0]++; }
            Pool.get().push(new Runnable() { public void run() { try { work.run(); } finally { synchronized (dm) { if (--pending[0] == 0) dm.notifyAll(); } } } }); }
        work.run(); boolean intr = false;   // an interruption is set again after the wait, not inside it (wait(1) threw again at once: a busy loop)
        for (;;) { synchronized (dm) { if (pending[0] == 0) break; }
            if (!Pool.get().runOne()) { synchronized (dm) { if (pending[0] != 0) { try { dm.wait(1); } catch (InterruptedException e) { intr = true; } } } } }
        if (intr) Thread.currentThread().interrupt();
        synchronized (dm) { for (Throwable x : xs) if (x != null) throw rethrow(x); }
    }
    // port of laes_lift.h:defaultThreads
    public static int defaultThreads() { int h = Runtime.getRuntime().availableProcessors(); return h > 0 ? h : 1; }

    // d[i] = xo[i] - P(xe, i)
    // port of laes_lift.h:predictStep
    static void predictStep(final long[] xe, final long[] xo, final long[] d, long no, final int sign, int threads) {
        final long n = xe.length; final boolean w = !fits64(kHalf0, 40, maxAbs(xe), 30);
        parallelFor(no, threads, new Range() { public void run(long a, long b) { for (long i = a; i < b; i++) {
            long p = tap(40, w, kHalf0, xe, n, i, 0, 1, 30);
            d[(int) i] = sign > 0 ? xo[(int) i] - p : xo[(int) i] + p; } } });
    }
    // s[i] = xe[i] + U(d, i)
    // port of laes_lift.h:updateStep
    static void updateStep(final long[] d, final long[] xe, final long[] s, long ne, final int sign, int threads) {
        final long n = d.length; if (n == 0) { for (int i = 0; i < ne; i++) s[i] = xe[i]; return; }
        final boolean w = !fits64(kU0, 60, maxAbs(d), 31);
        parallelFor(ne, threads, new Range() { public void run(long a, long b) { for (long i = a; i < b; i++) {
            long u = tap(60, w, kU0, d, n, i, -1, 0, 31);
            s[(int) i] = sign > 0 ? xe[(int) i] + u : xe[(int) i] - u; } } });
    }
    /** one stage 2R -> R: s (low level, ceil(n/2)) and d (detail, floor(n/2)); returns {s, d} */
    // port of laes_lift.h:fwd
    public static long[][] fwd(long[] x, int threads) {
        final int n = x.length, ne = (n + 1) / 2, no = n / 2; long[] xe = new long[ne], xo = new long[no];
        for (int i = 0; i < ne; i++) xe[i] = x[2 * i]; for (int i = 0; i < no; i++) xo[i] = x[2 * i + 1];
        long[] d = new long[no], s = new long[ne]; predictStep(xe, xo, d, no, +1, threads); updateStep(d, xe, s, ne, +1, threads);
        return new long[][] {s, d};
    }
    public static long[][] fwd(long[] x) { return fwd(x, defaultThreads()); }
    // port of laes_lift.h:inv
    public static long[] inv(long[] s, long[] d, int n, int threads) {
        final int ne = s.length, no = d.length; long[] xe = new long[ne], xo = new long[no], x = new long[n];
        updateStep(d, s, xe, ne, -1, threads); predictStep(xe, d, xo, no, -1, threads);
        for (int i = 0; i < ne; i++) x[2 * i] = xe[i]; for (int i = 0; i < no; i++) x[2 * i + 1] = xo[i]; return x;
    }
    public static long[] inv(long[] s, long[] d, int n) { return inv(s, d, n, defaultThreads()); }
    // the core reference on the master's grid (laes::reference in src/laes_math.h), with the taps h = laes::refTaps(frac) given by the caller:
    // ref[i] = round(sum_q h[q] core[m(i + whole + q)] / 2^(31 - up)); same values, direct indexing inside, 64-bit when it cannot overflow
    // port of laes_lift.h:reference
    public static long[] reference(final long[] core, final long whole, final long[] h, int up, int threads) {
        final long n = core.length; final long[] r = new long[core.length]; final int s = 31 - up; long shv = 0; for (long v : h) shv += v < 0 ? -v : v;
        final boolean w = !((double) maxAbs(core) * (double) shv + (s > 0 ? (double) (1L << (s - 1)) : 0.0) < 9.0e18);
        parallelFor(n, threads, new Range() { public void run(long a, long b) { for (long i = a; i < b; i++) { final long j0 = i + whole - 127, j1 = i + whole + 128;
            if (!w) { long acc = 0;
                if (j0 >= 0 && j1 < n) { final int c = (int) j0; for (int q = 0; q < 256; q++) acc += h[q] * core[c + q]; }
                else for (int q = -127; q <= 128; q++) acc += h[q + 127] * core[(int) mir(i + whole + q, n)];
                r[(int) i] = s > 0 ? (acc + (1L << (s - 1))) >> s : acc << (-s); }
            else { Acc128 A = new Acc128();
                if (j0 >= 0 && j1 < n) { final int c = (int) j0; for (int q = 0; q < 256; q++) A.mac(h[q], core[c + q]); }
                else for (int q = -127; q <= 128; q++) A.mac(h[q + 127], core[(int) mir(i + whole + q, n)]);
                r[(int) i] = s > 0 ? A.shr(s) : A.lo << (-s); } } } });
        return r;
    }
}
