// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.LMath;
import java.util.HashMap;
import java.util.Map;

/**
 * Port of src/fft.h (namespace fftlib). Complex FFT (forward, unnormalised) for the encoder's analysis stages; n must be a power of two.
 * A plan per size holds a bit-reversal table and one twiddle table exp(-2 pi i k / n), k &lt; n/2.
 * The transform reorders the input from the table and then runs log2(n) passes of radix-2 butterflies, taking every twiddle from the
 * table with a stride. Identical on every platform (2026-11-03): the twiddles come from lae_math.h (cospi / sinpi of -2k/n, exact
 * argument) and the butterfly's product is written out (re = a c - b d, im = a d + b c, in this order), not left to the library's
 * complex multiplication.
 * std::complex&lt;double&gt; is held as two arrays (re, im). The C++ butterfly uses only the written-out product and the complex
 * operator- / operator+= (component-wise, exact as in Java).
 * C++ plans are cached per thread; here one immutable plan per size is shared (a plan depends only on n).
 */
final class Fft {
    private Fft() {}

    /** port of fft.h:Plan */
    static final class Plan { final int n; final int[] rev; final double[] twr, twi;
        // port of fft.h:Plan::Plan
        Plan(int size) { n = size; rev = new int[size]; twr = new double[size / 2]; twi = new double[size / 2];
            int bits = 0; while ((1L << bits) < n) bits++;
            for (int i = 1; i < n; i++) rev[i] = (rev[i >> 1] >>> 1) | ((i & 1) << (bits - 1));
            for (int k = 0; k < n / 2; k++) { final double t = -2.0 * (double) k / (double) n; twr[k] = LMath.cospi(t); twi[k] = LMath.sinpi(t); } } }
    private static final Map<Integer, Plan> CACHE = new HashMap<Integer, Plan>();
    // port of fft.h:planFor
    static Plan planFor(int n) { synchronized (CACHE) { Plan p = CACHE.get(n); if (p == null) { p = new Plan(n); CACHE.put(n, p); } return p; } }

    // port of fft.h:fft (in place on re[0 .. n), im[0 .. n))
    static void fft(double[] re, double[] im, int n) {
        if (n < 2) return; final Plan p = planFor(n); final int[] rev = p.rev; final double[] twr = p.twr, twi = p.twi;
        for (int i = 0; i < n; i++) { int j = rev[i]; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
        for (int half = 1, stride = n / 2; half < n; half <<= 1, stride >>= 1)
            for (int blk = 0; blk < n; blk += 2 * half)
                for (int k = 0; k < half; k++) { final int lo = blk + k, hi = blk + half + k;
                    final double hr = re[hi], hm = im[hi], wr = twr[k * stride], wm = twi[k * stride];
                    final double tr = hr * wr - hm * wm, ti = hr * wm + hm * wr;
                    re[hi] = re[lo] - tr; im[hi] = im[lo] - ti; re[lo] += tr; im[lo] += ti; } }
}
