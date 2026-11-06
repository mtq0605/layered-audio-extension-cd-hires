// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.Cv;

/**
 * Port of src/frac_delay.h (namespace lae_fd). Fractional delay by d = phi / U1 master samples (U1 = 2^22 &gt;&gt; stages, so the unit
 * is 2^-22 of a CD sample; 0 &lt;= d &lt; 1). Encoder only (design A, 2026-10-23): the independent-CD alignment (part 2 of the port)
 * uses it to try fractions of a master sample through the encoder's own derivation; the master is then moved by the measured fraction
 * and the file stays v5. The decoder does not use it.
 * The delay is one time-invariant FIR at the master rate: y[n] = sum_k h[k] x[n - k], k = -15 .. 16. Its taps are the Lagrange
 * interpolation coefficients h[k] = prod over m != k of (d - m) / (k - m), in IEEE double in this fixed order, rounded to Q28 (the
 * centre tap h[0] takes the rounding remainder so that the taps sum to exactly 2^28: DC gain 1).
 */
final class FracDelay {
    private FracDelay() {}

    static final int TAPS = 32, K0 = -15;              // taps for k = -15 .. 16
    static final long CD_UNITS = 1L << 22;             // phi units per CD sample
    // port of frac_delay.h:unitsPerMaster
    static long unitsPerMaster(int stages) { return CD_UNITS >> stages; }
    // port of frac_delay.h:taps: taps for a delay of d = phi / unitsPerMaster(stages) master samples (0 <= phi < unitsPerMaster(stages))
    static void taps(long phi, int stages, int[] h) {
        final double d = (double) phi / (double) unitsPerMaster(stages); long sum = 0;
        for (int i = 0; i < TAPS; i++) { final int k = K0 + i; double p = 1.0;
            for (int j = 0; j < TAPS; j++) { final int m = K0 + j; if (m != k) p = p * ((d - (double) m) / (double) (k - m)); }
            h[i] = (int) Cv.llround(p * 268435456.0); sum += h[i]; }
        h[-K0] += (int) (268435456L - sum); }
    /** a source of int64 samples get(i) (any index; the caller mirrors at the edges) */
    interface Get { long get(long i); }
    // port of frac_delay.h:apply: one output sample of the delay. Inputs are limited to +-2^30 first (valid signals stay far below)
    static long apply(int[] h, long n, Get get) {
        long a = 0; for (int i = 0; i < TAPS; i++) { long x = get.get(n - (K0 + i)); x = x > (1L << 30) ? (1L << 30) : (x < -(1L << 30) ? -(1L << 30) : x); a += (long) h[i] * x; }
        return (a + (1L << 27)) >> 28; }
}
