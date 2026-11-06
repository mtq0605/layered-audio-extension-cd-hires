// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.Cv;

/**
 * Port of src/mdct_tb.h (namespace mdct). Textbook MDCT / IMDCT for the lossy tier: sine window, 50 % overlap, N = 512
 * coefficients/frame.
 *   forward: multiply by the window -> fold the 2N windowed samples into N (additions only: [-c_r - d, a - b_r]) -> DCT-IV
 *   inverse: DCT-IV -> unfold (N -> 2N) -> multiply by the window -> overlap-add
 * The DCT-IV is the classic algorithm via an N/2-point complex FFT (pre-twiddle, radix-2 FFT, post-twiddle); both twiddles are
 * exp(-i 2pi (n + 1/8) / 2N).
 * Determinism: only IEEE-754 double +, -, x in a fixed order with constants from mdct_tables.h (exact literals), final rounding by
 * llround. Java double arithmetic is strict IEEE; the expressions below keep the C++ order exactly (no fused multiply-add).
 * Frame layout: frame m covers padded samples [m N, m N + 2N) of [N zeros | signal | zeros], K = ceil(L / N) + 1 frames.
 * C++ `long` is 32-bit on MSVC x64; every length here is far below 2^31 (the decoder limits the master to 2e9 samples).
 */
public final class MdctTb {
    private MdctTb() {}

    public static final int N = 512, M = N / 2;
    static final double[] WIN = MdctTables.WIN;

    /** Java only (tests): a copy of the table of mdct_tables.h with this name (BITREV and SCALE as doubles) */
    public static double[] table(String name) {
        if (name.equals("WIN")) return MdctTables.WIN.clone(); if (name.equals("PRE_C")) return MdctTables.PRE_C.clone(); if (name.equals("PRE_S")) return MdctTables.PRE_S.clone();
        if (name.equals("POST_C")) return MdctTables.POST_C.clone(); if (name.equals("POST_S")) return MdctTables.POST_S.clone(); if (name.equals("FFT_C")) return MdctTables.FFT_C.clone();
        if (name.equals("FFT_S")) return MdctTables.FFT_S.clone(); if (name.equals("SCALE")) return new double[] {MdctTables.SCALE};
        if (name.equals("BITREV")) { double[] b = new double[MdctTables.BITREV.length]; for (int i = 0; i < b.length; i++) b[i] = MdctTables.BITREV[i]; return b; }
        return null; }

    // port of mdct_tb.h:frames
    public static long frames(long L) { return (L + N - 1) / N + 1; }

    // port of mdct_tb.h:fft
    // in-place radix-2 complex FFT of size M (decimation in time); the input is reordered by the bit-reversal table BITREV
    static void fft(double[] re, double[] im) {
        final int[] BITREV = MdctTables.BITREV; final double[] FFT_C = MdctTables.FFT_C, FFT_S = MdctTables.FFT_S;
        for (int i = 0; i < M; i++) { int j = BITREV[i]; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
        for (int len = 2; len <= M; len <<= 1) { int step = M / len;
            for (int i = 0; i < M; i += len) for (int k = 0; k < len / 2; k++) { double wr = FFT_C[k * step], wi = FFT_S[k * step];
                double xr = re[i + k + len / 2], xi = im[i + k + len / 2]; double tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + k + len / 2] = re[i + k] - tr; im[i + k + len / 2] = im[i + k] - ti; re[i + k] += tr; im[i + k] += ti; } } }

    // port of mdct_tb.h:dct4
    // orthonormal DCT-IV (self-inverse): X[k] = sqrt(2/N) sum u[n] cos(pi/N (n+1/2)(k+1/2))
    // (re, im: scratch of M values each, supplied by the caller instead of the C++ stack arrays)
    static void dct4(double[] u, double[] X, double[] re, double[] im) {
        final double[] PRE_C = MdctTables.PRE_C, PRE_S = MdctTables.PRE_S, POST_C = MdctTables.POST_C, POST_S = MdctTables.POST_S;
        final double SCALE = MdctTables.SCALE;
        for (int n = 0; n < M; n++) { double a = u[2 * n], b = u[N - 1 - 2 * n]; re[n] = a * PRE_C[n] - b * PRE_S[n]; im[n] = a * PRE_S[n] + b * PRE_C[n]; }
        fft(re, im);
        for (int k = 0; k < M; k++) { double yr = (re[k] * POST_C[k] - im[k] * POST_S[k]) * SCALE, yi = (re[k] * POST_S[k] + im[k] * POST_C[k]) * SCALE; X[2 * k] = yr; X[N - 1 - 2 * k] = -yi; } }

    // startup self-test: a fixed coefficient pattern through the IMDCT; the FNV-1a-style hash of the integer output must equal the reference
    // value. A mismatch means this build does not follow IEEE-754 double arithmetic in the specified order (x87 extended precision,
    // fused multiply-add contraction, fast-math...) and its output would differ from other platforms.
    public static final long SELFTEST_REF = 0x729b547fbdbfe203L;   // reference value; verified builds are listed in docs/BUILD.md

    // port of mdct_tb.h:selftestHash
    public static long selftestHash() {
        final long L = 6 * N; int[] X = new int[(int) (frames(L) * N)];
        for (int i = 0; i < X.length; i++) X[i] = (int) (((((long) i * 2654435761L) & 0xFFFFFFFFL) >>> 7) % 2000001L) - 1000000;   // deterministic pattern up to +-1e6 (32-bit arithmetic on every platform)
        int[] y = inverse(X, L); long h = 1469598103934665603L;
        for (int v : y) { int u = v; for (int b = 0; b < 4; b++) { h ^= (u >>> (8 * b)) & 0xFF; h *= 1099511628211L; } }
        return h; }

    // port of mdct_tb.h:selftestOK
    public static boolean selftestOK() { return SELFTEST_REF == 0 || selftestHash() == SELFTEST_REF; }

    // port of mdct_tb.h:forward (not used by the decoder; kept for the encoder port, verified by the module test)
    public static int[] forward(int[] x) {
        long L = x.length, K = frames(L); int[] X = new int[(int) (K * N)]; double[] z = new double[2 * N], u = new double[N], c = new double[N];
        double[] re = new double[M], im = new double[M];
        for (long m = 0; m < K; m++) { long s = m * N;
            for (int n = 0; n < 2 * N; n++) { long i = s + n - N; z[n] = ((i >= 0 && i < L) ? (double) x[(int) i] : 0.0) * WIN[n]; }   // window (padded: N zeros first)
            for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }   // fold: [-c_r - d, a - b_r]
            dct4(u, c, re, im); for (int k = 0; k < N; k++) X[(int) (m * N + k)] = (int) Cv.llround(c[k]); }
        return X; }

    // port of mdct_tb.h:inverse
    public static int[] inverse(int[] X, long L) {
        long K = frames(L), LP = (K + 1) * N; double[] y = new double[(int) LP]; double[] c = new double[N], u = new double[N];
        double[] re = new double[M], im = new double[M];
        for (long m = 0; m < K && (m + 1) * N <= X.length; m++) {
            for (int k = 0; k < N; k++) c[k] = X[(int) (m * N + k)];
            dct4(c, u, re, im); int s = (int) (m * N);
            for (int n = 0; n < N / 2; n++) {                                                                          // unfold [u2, -u2_r, -u1_r, -u1], window, overlap-add
                y[s + n] += u[N / 2 + n] * WIN[n];
                y[s + N / 2 + n] += -u[N - 1 - n] * WIN[N / 2 + n];
                y[s + N + n] += -u[N / 2 - 1 - n] * WIN[N + n];
                y[s + 3 * N / 2 + n] += -u[n] * WIN[3 * N / 2 + n]; } }
        int[] out = new int[(int) L]; for (int i = 0; i < L; i++) out[i] = (int) Cv.llround(y[N + i]); return out; }
}
