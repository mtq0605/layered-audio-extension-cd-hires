// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

/**
 * Unsigned 64-bit and 32-bit arithmetic on Java's signed types, without the java.lang methods Android added after API 24
 * (Long.compareUnsigned, divideUnsigned, remainderUnsigned, Integer.toUnsignedLong). A C++ uint64_t is held in a long with the same
 * bits; a uint32_t in an int.
 */
public final class U {
    private U() {}

    /** uint64 a < b */
    public static boolean lt(long a, long b) { return (a + Long.MIN_VALUE) < (b + Long.MIN_VALUE); }
    public static boolean le(long a, long b) { return (a + Long.MIN_VALUE) <= (b + Long.MIN_VALUE); }
    public static boolean gt(long a, long b) { return (a + Long.MIN_VALUE) > (b + Long.MIN_VALUE); }
    public static boolean ge(long a, long b) { return (a + Long.MIN_VALUE) >= (b + Long.MIN_VALUE); }
    public static long min(long a, long b) { return lt(b, a) ? b : a; }
    public static long max(long a, long b) { return lt(a, b) ? b : a; }

    /** uint64 a / d (d != 0) */
    public static long div(long a, long d) {
        if (d == 0) throw new ArithmeticException("division by zero");
        if (d < 0) return lt(a, d) ? 0 : 1;            // d >= 2^63: the quotient is 0 or 1
        if (a >= 0) return a / d;
        long q = ((a >>> 1) / d) << 1;                 // a >= 2^63: halve, divide, double, then correct by at most one
        long r = a - q * d;
        if (!lt(r, d)) q++;
        return q;
    }

    /** uint64 a % d (d != 0) */
    public static long rem(long a, long d) { return a - div(a, d) * d; }

    /** uint32 held in an int, as a non-negative long */
    public static long u32(int x) { return x & 0xFFFFFFFFL; }

    /** uint32 a < b */
    public static boolean lt32(int a, int b) { return (a + Integer.MIN_VALUE) < (b + Integer.MIN_VALUE); }

    /** uint32 a / d, a % d */
    public static int div32(int a, int d) { return (int) (u32(a) / u32(d)); }
    public static int rem32(int a, int d) { return (int) (u32(a) % u32(d)); }

    /** (double) of a uint64, correctly rounded (as the C++ conversion) */
    public static double toDouble(long x) {
        if (x >= 0) return (double) x;
        return (double) ((x >>> 1) | (x & 1)) * 2.0;   // halve keeping a sticky bit: the rounding of the 64-bit value is unchanged
    }

    /** (float) of a uint64, correctly rounded */
    public static float toFloat(long x) {
        if (x >= 0) return (float) x;
        return (float) ((x >>> 1) | (x & 1)) * 2.0f;
    }

    /** high 64 bits of the unsigned 128-bit product a * b */
    public static long mulHighUnsigned(long a, long b) {
        long a0 = a & 0xFFFFFFFFL, a1 = a >>> 32, b0 = b & 0xFFFFFFFFL, b1 = b >>> 32;
        long p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        long mid = (p00 >>> 32) + (p01 & 0xFFFFFFFFL) + (p10 & 0xFFFFFFFFL);
        return p11 + (p01 >>> 32) + (p10 >>> 32) + (mid >>> 32);
    }

    /** high 64 bits of the signed 128-bit product a * b (the portable form of laes_math.h mul128) */
    public static long mulHighSigned(long a, long b) {
        long h = mulHighUnsigned(a, b);
        if (a < 0) h -= b;
        if (b < 0) h -= a;
        return h;
    }

    /** number of leading zero bits (C++ __builtin_clzll / _lzcnt_u64; 64 for 0) */
    public static int clz64(long x) { return Long.numberOfLeadingZeros(x); }
    public static int clz32(int x) { return Integer.numberOfLeadingZeros(x); }
}
