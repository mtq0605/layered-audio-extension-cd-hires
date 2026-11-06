// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

/**
 * C / C++ conversions and rounding functions with the exact results of the reference build (MSVC x64, SSE2, /fp:strict), where Java
 * differs. Use these instead of Java casts and Math.round / Math.max / Math.min on doubles.
 */
public final class Cv {
    private Cv() {}

    private static final double TWO63 = 9223372036854775808.0, TWO64 = 18446744073709551616.0, TWO31 = 2147483648.0, TWO32 = 4294967296.0;

    /** (int64_t)d: truncation; NaN or out of range gives 0x8000000000000000 (x64 cvttsd2si), where Java would saturate */
    public static long d2l(double d) {
        if (d != d || d >= TWO63 || d < -TWO63) return Long.MIN_VALUE;
        return (long) d;
    }

    /** (int)d: truncation; NaN or out of range gives 0x80000000 (x64 cvttsd2si, 32-bit form) */
    public static int d2i(double d) {
        if (d != d || d >= TWO31 || d < -TWO31) return Integer.MIN_VALUE;
        return (int) d;
    }

    /** (int)f and (int64_t)f for float */
    public static int f2i(float f) { return d2i(f); }
    public static long f2l(float f) { return d2l(f); }

    /**
     * (uint64_t)d for 0 <= d < 2^64. Outside that range the C++ result depends on the compiler version; the port must not reach it, so
     * it throws (a test that reaches it shows a place to look at).
     */
    public static long d2ul(double d) {
        if (d >= 0 && d < TWO63) return (long) d;
        if (d >= TWO63 && d < TWO64) return ((long) (d - TWO63)) ^ Long.MIN_VALUE;
        throw new LaeException("port: (uint64_t) of " + d + " is outside 0..2^64");
    }

    /** (uint32_t)d for 0 <= d < 2^32 (held in an int); outside: throws, as d2ul */
    public static int d2ui(double d) {
        if (d >= 0 && d < TWO32) return (int) (long) d;
        throw new LaeException("port: (uint32_t) of " + d + " is outside 0..2^32");
    }

    /** std::trunc */
    public static double trunc(double x) { return x < 0 ? Math.ceil(x) : Math.floor(x); }

    /** std::round: half away from zero (Java's Math.round rounds half up) */
    public static double round(double x) {
        if (x != x || Double.isInfinite(x)) return x;
        double t = trunc(x);
        double f = x - t;                              // exact
        if (f >= 0.5) return t + 1.0;
        if (f <= -0.5) return t - 1.0;
        return t;
    }

    /** std::llround / lround (64-bit long on the reference platform's llround; lround is 32-bit on MSVC: use lround32) */
    public static long llround(double x) { return d2l(round(x)); }
    public static int lround32(double x) { return d2i(round(x)); }

    /** std::max / std::min for double and float: (a < b) ? b : a  and  (b < a) ? b : a  (NaN and -0.0 as C++) */
    public static double max(double a, double b) { return (a < b) ? b : a; }
    public static double min(double a, double b) { return (b < a) ? b : a; }
    public static float max(float a, float b) { return (a < b) ? b : a; }
    public static float min(float a, float b) { return (b < a) ? b : a; }

    /** std::frexp: x = mant * 2^exp, mant in [0.5, 1); for 0, NaN, infinity: mant = x, exp = 0 */
    public static double frexpMant(double x) {
        if (x == 0 || x != x || Double.isInfinite(x)) return x;
        return Math.scalb(x, -frexpExp(x));
    }

    public static int frexpExp(double x) {
        if (x == 0 || x != x || Double.isInfinite(x)) return 0;
        int e = Math.getExponent(x);
        if (e == Double.MIN_EXPONENT - 1) return Math.getExponent(x * 18014398509481984.0) - 54 + 1;   // subnormal: scale by 2^54
        return e + 1;
    }

    /** std::ldexp (exact, or correctly rounded into the subnormal range) */
    public static double ldexp(double x, int e) { return Math.scalb(x, e); }

    /** std::copysign, std::signbit */
    public static boolean signbit(double x) { return (Double.doubleToRawLongBits(x) < 0); }
}
