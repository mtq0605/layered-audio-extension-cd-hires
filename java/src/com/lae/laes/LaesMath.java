// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import com.lae.core.Cv;
import com.lae.core.Halfband;
import com.lae.core.U;

/**
 * Port of src/laes_math.h (namespace laes). LAES normative arithmetic (S1, 2026-10-28). Everything here decides decoded sample values
 * and must give identical results on every platform:
 *   - 128-bit accumulator; rounding = add 2^(s-1), arithmetic shift right by s (ties towards +infinity, as rq31 of the LAEY SPEC);
 *   - whole-sample symmetric extension (period 2(n-1); n = 1 -> index 0);
 *   - rate-layer lifting L: d = xo - P(xe), s = xe + U(d)  (P = LAEY half-band table 0, U = kU0, both Q31);
 *   - reference taps for the time position T (Q32 fraction of a base-rate sample): 256-tap Kaiser (beta 14) windowed sinc computed
 *     with IEEE-754 binary64 +, -, *, / and sqrt only, in the fixed order written below (no library sin/exp, no fused multiply-add),
 *     rounded to Q31, then the tap nearest the centre corrects the sum to exactly 2^31.
 * V (std::vector&lt;int64_t&gt;) is long[] here.
 */
public final class LaesMath {
    private LaesMath() {}

    /** signed 64 x 64 -> 128-bit product: lo = low word (as uint64 bits), hi = high word */
    // port of laes_math.h:mul128 (the portable form; the x64 intrinsic gives the same two's-complement result)
    static long mul128Lo(long a, long b) { return a * b; }
    static long mul128Hi(long a, long b) { return U.mulHighSigned(a, b); }

    /** port of laes_math.h:Acc (lo is a uint64 held in a long) */
    public static final class Acc {
        public long lo, hi;
        // port of laes_math.h:Acc::mac
        public void mac(long a, long b) {
            long l = a * b, h = U.mulHighSigned(a, b);
            long n = lo + l; hi += h + (U.lt(n, lo) ? 1 : 0); lo = n;
        }
        // port of laes_math.h:Acc::shr  (s in 1..63 at every call site: callers guard s > 0; 64 - s is then 1..63)
        public long shr(int s) {
            long add = 1L << (s - 1), n = lo + add; long h = hi + (U.lt(n, lo) ? 1 : 0);
            return (n >>> s) | (h << (64 - s));
        }
    }

    // port of laes_math.h:mir
    public static long mir(long i, long n) { if (n == 1) return 0; long p = 2 * (n - 1); i %= p; if (i < 0) i += p; return i < n ? i : p - i; }

    public static final long[] kU0 = {683270116, -226970762, 135242270, -95599562, 73323768, -58947533, 48830814, -41276037, 35385490, -30640348, 26720250, -23416981, 20589580, -18139377, 15995343, -14105100, 12429177, -10937236, 9605506, -8415006, 7350282, -6398504, 5548802, -4791782, 4119170, -3523546, 2998153, -2536758, 2133556, -1783104, 1480274, -1220234, 998431, -810587, 652702, -521052, 412202, -322998, 250575, -192354, 146076, -109516, 81159, -59401, 42896, -30529, 21386, -14723, 9943, -6573, 4241, -2663, 1619, -948, 531, -281, 138, -61, 23, -6};
    /** lae::kHalf0 as long (the same 40 values) */
    public static final long[] kHalf0;
    static { kHalf0 = new long[Halfband.kHalf0.length]; for (int t = 0; t < kHalf0.length; t++) kHalf0[t] = Halfband.kHalf0[t]; }

    // port of laes_math.h:P
    public static long P(long[] xe, long i) { Acc a = new Acc(); long n = xe.length; for (int t = 0; t < 40; t++) { a.mac(kHalf0[t], xe[(int) mir(i - t, n)]); a.mac(kHalf0[t], xe[(int) mir(i + 1 + t, n)]); } return a.shr(30); }
    // port of laes_math.h:U
    public static long U(long[] d, long i) { long n = d.length; if (n == 0) return 0; Acc a = new Acc(); for (int t = 0; t < 60; t++) { a.mac(kU0[t], d[(int) mir(i - 1 - t, n)]); a.mac(kU0[t], d[(int) mir(i + t, n)]); } return a.shr(31); }
    /** port of laes_math.h:fwd (normative form); returns {s, d} */
    public static long[][] fwd(long[] x) {
        int n = x.length, ne = (n + 1) / 2, no = n / 2; long[] xe = new long[ne], d = new long[no], s = new long[ne];
        for (int i = 0; i < ne; i++) xe[i] = x[2 * i];
        for (int i = 0; i < no; i++) d[i] = x[2 * i + 1] - P(xe, i);
        for (int i = 0; i < ne; i++) s[i] = xe[i] + U(d, i);
        return new long[][] {s, d};
    }
    // port of laes_math.h:inv (normative form)
    public static long[] inv(long[] s, long[] d, int n) {
        int ne = s.length, no = d.length; long[] xe = new long[ne], x = new long[n];
        for (int i = 0; i < ne; i++) xe[i] = s[i] - U(d, i);
        for (int i = 0; i < ne; i++) x[2 * i] = xe[i];
        for (int i = 0; i < no; i++) x[2 * i + 1] = d[i] + P(xe, i);
        return x;
    }

    // ---- reference taps. T = whole part (int64) + frac (uint32, units of 2^-32 base-rate sample). The reference value at index i is the
    // band-limited core at time i + T: ref[i] = sum_{q=-127..128} h[q] * core[i + whole + q], h[q] ~ sinc(f - q) * kaiser(f - q), f = frac / 2^32.
    // All operations: binary64 + - * / sqrt, written out in a fixed order. Returns 256 taps (index q + 127) in Q31 with sum exactly 2^31.
    // port of laes_math.h:kPi
    public static double kPi() { return 3.141592653589793115997963468544185161590576171875; }   // binary64 nearest to pi
    // port of laes_math.h:sinSeries  (sin x for |x| <= pi/2: Taylor to x^31, Horner, fixed order)
    public static double sinSeries(double x) { double x2 = x * x, s = 1.0; for (int k = 15; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k) * (2.0 * k + 1.0)); return x * s; }
    // port of laes_math.h:I0Series  (modified Bessel I0, power series, 80 terms, fixed order)
    public static double I0Series(double x) { double y = x * x / 4.0, term = 1.0, s = 1.0; for (int k = 1; k <= 80; k++) { term = term * y / ((double) k * (double) k); s = s + term; } return s; }
    /** port of laes_math.h:refTaps; frac is a uint32 held in an int */
    public static long[] refTaps(int frac) {   // int64: the centre tap reaches 2^31 when frac = 0
        final int H = 128; final double beta = 14.0, f = (double) (frac & 0xFFFFFFFFL) / 4294967296.0;
        final double g = f <= 0.5 ? f : 1.0 - f;                 // sin(pi f) = sin(pi (1 - f)), both exact in binary64
        final double sf = sinSeries(kPi() * g), i0b = I0Series(beta);
        long[] h = new long[2 * H]; long sum = 0; int centre = 0; double best = 1e300;
        for (int q = -H + 1; q <= H; q++) { final double u = f - (double) q;      // exact
            double sinc; if (u == 0.0) sinc = 1.0; else { final double sgn = (q % 2 == 0) ? 1.0 : -1.0; sinc = sgn * sf / (kPi() * u); }   // sin(pi(f-q)) = (-1)^q sin(pi f)
            final double r = u / (double) H; double w = 0.0; if (r > -1.0 && r < 1.0) w = I0Series(beta * Math.sqrt(1.0 - r * r)) / i0b;
            final double v = sinc * w * 2147483648.0; final long t = Cv.d2l(Math.floor(v + 0.5));
            h[q + H - 1] = t; sum += t;
            final double au = Math.abs(u);   // C++ "u < 0 ? -u : u" (MSVC makes it fabs); only compared below
            if (au < best) { best = au; centre = q + H - 1; } }
        h[centre] += (1L << 31) - sum;     // DC gain exactly 1
        return h;
    }
    // reference on the master's grid: ref[i] = round( sum_q h[q] core[i + whole + q] / 2^(31 - up) ), up = extra grid bits (may be negative);
    // samples outside the core are taken by symmetric extension (same rule as the lifting)
    // port of laes_math.h:reference (normative form)
    public static long[] reference(long[] core, long whole, int frac, int up) {
        long[] h = refTaps(frac); long n = core.length; long[] r = new long[core.length];
        for (int i = 0; i < n; i++) { Acc a = new Acc(); for (int q = -127; q <= 128; q++) a.mac(h[q + 127], core[(int) mir(i + whole + q, n)]);
            final int s = 31 - up; r[i] = s > 0 ? a.shr(s) : a.lo << (-s); }
        return r;
    }
}
