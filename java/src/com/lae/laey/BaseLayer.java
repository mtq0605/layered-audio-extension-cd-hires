// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

/**
 * Port of src/base_layer.cpp, the part the encoder uses (namespace m_base: decimate() for the independent-CD alignment, part 2 of the
 * port; the CD layer itself is derived by base_blk.cpp = {@link BaseBlk}). Deterministic base (CD) layer derivation, integer only.
 */
final class BaseLayer {
    private BaseLayer() {}

    // port of base_layer.cpp:decimate
    // one decimate-by-2 stage, stage index counted from the base rate upwards. x in Q4 of the 24-bit domain (28-bit).
    static int[] decimate(int[] x, int s) {
        int[] hf = new int[1]; final int[] h = BaseBlk.tab(s, hf); final int half = hf[0];
        long n = x.length, m = n / 2;
        int[] y = new int[(int) m];
        for (long i = 0; i < m; i++) {
            long c = 2 * i;
            long acc = (long) x[(int) c] << 30;      // centre tap 2^30 (Q31 0.5)
            for (int j = 1, t = 0; j <= half; j += 2, t++) {
                long a = c - j, b = c + j;
                long aa = a >= 0 ? a : -a, bb = b < n ? b : 2 * (n - 1) - b; aa = Math.min(Math.max(aa, 0L), n - 1); bb = Math.min(Math.max(bb, 0L), n - 1);
                long sa = x[(int) aa], sb = x[(int) bb];
                acc += (long) h[t] * (sa + sb);
            }
            long r = (acc + (1L << 30)) >> 31;    // round half up, Q31 -> integer
            if (r > 2147483647L) r = 2147483647L; if (r < -2147483647L) r = -2147483647L;   // only guards int32 (Q4 has 24 dB headroom)
            y[(int) i] = (int) r;
        }
        return y; }
}
