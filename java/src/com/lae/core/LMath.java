// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

/**
 * Port of src/lae_math.h (namespace lmath): the encoder's elementary functions, identical on every platform. Only IEEE-754 binary64
 * +, -, *, / and the exact operations sqrt, floor, frexp, ldexp, in the fixed order of the C++ (do not reorder).
 */
public final class LMath {
    private LMath() {}

    // port of lae_math.h:pi (the binary64 nearest to pi)
    public static double pi() { return 3.141592653589793115997963468544185161590576171875; }

    // port of lae_math.h:sinPiSmall
    public static double sinPiSmall(double y) {
        final double x = pi() * y, x2 = x * x;
        double s = 1.0;
        for (int k = 14; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k) * (2.0 * k + 1.0));
        return x * s;
    }

    // port of lae_math.h:cosPiSmall
    public static double cosPiSmall(double y) {
        final double x = pi() * y, x2 = x * x;
        double s = 1.0;
        for (int k = 14; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k - 1.0) * (2.0 * k));
        return s;
    }

    // port of lae_math.h:reduce (r = t - n exact; odd = fmod(n, 2) != 0); returns r, odd in odd[0]
    private static double reduce(double t, boolean[] odd) {
        final double n = Math.floor(t + 0.5);
        odd[0] = (n % 2.0) != 0;
        return t - n;
    }

    // port of lae_math.h:sinpi
    public static double sinpi(double t) {
        boolean[] odd = new boolean[1];
        double r = reduce(t, odd);
        final double a = Math.abs(r);   // MSVC x64 compiles "r < 0 ? -r : r" to fabs (+0 for -0.0); PORT_RULES 6
        double v = a <= 0.25 ? sinPiSmall(a) : cosPiSmall(0.5 - a);   // 0.5 - a is exact (Sterbenz)
        if (r < 0) v = -v;
        return odd[0] ? -v : v;
    }

    // port of lae_math.h:cospi
    public static double cospi(double t) {
        boolean[] odd = new boolean[1];
        double r = reduce(t, odd);
        final double a = Math.abs(r);   // MSVC x64 compiles "r < 0 ? -r : r" to fabs (+0 for -0.0); PORT_RULES 6
        final double v = a <= 0.25 ? cosPiSmall(a) : sinPiSmall(0.5 - a);
        return odd[0] ? -v : v;
    }

    // port of lae_math.h:log2 (x = m 2^e exactly, m in [sqrt(1/2), sqrt(2)), atanh series to s^25; exact for powers of two)
    public static double log2(double x) {
        if (!(x > 0)) return x == 0 ? Double.NEGATIVE_INFINITY : Double.NaN;
        if (x == Double.POSITIVE_INFINITY) return x;
        int e = Cv.frexpExp(x);
        double m = Cv.frexpMant(x);
        if (m < 0.70710678118654752440) { m = m * 2.0; e = e - 1; }
        final double s = (m - 1.0) / (m + 1.0), s2 = s * s;
        double p = 1.0 / 25.0;
        for (int k = 23; k >= 1; k -= 2) p = 1.0 / (double) k + s2 * p;
        return (double) e + 2.8853900817779268147 * (s * p);   // 2 / ln 2
    }

    // port of lae_math.h:log10
    public static double log10(double x) { return log2(x) * 0.30102999566398119521; }

    // port of lae_math.h:exp2 (y = n + f, 2^f by the Taylor series to z^22, then ldexp; (int)n as the C++ cast)
    public static double exp2(double y) {
        final double n = Math.floor(y), z = (y - n) * 0.69314718055994530942;
        double s = 1.0;
        for (int k = 22; k >= 1; k--) s = 1.0 + s * z / (double) k;
        return Math.scalb(s, Cv.d2i(n));
    }

    // port of lae_math.h:exp10
    public static double exp10(double x) { return exp2(x * 3.3219280948873623479); }
}
