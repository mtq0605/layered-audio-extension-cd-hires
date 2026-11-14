// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.Halfband;

/**
 * Port of src/base_blk.cpp. Block-wise base_layer.cpp (2026-10-29): the same CD layer, 24-bit base and dither flags, computed one
 * base-rate block at a time. Every decimation stage is evaluated over the window its block needs (with the same mirrored edges, which
 * depend only on the stage's length); the dither decision is made per 4096-sample block as before, and the random numbers are drawn in
 * the same order.
 * usage: base &lt;raw s32le stereo, 24-bit left-aligned&gt; &lt;rate&gt; &lt;base_rate&gt; &lt;out s16le raw&gt;; environment BASE24, DITHERFLAGS
 */
final class BaseBlk {
    private BaseBlk() {}

    // port of base_blk.cpp:tab (half-length in half[0])
    static int[] tab(int s, int[] half) {
        switch (s) {
            case 0: half[0] = Halfband.kHalfLen0; return Halfband.kHalf0;
            case 1: half[0] = Halfband.kHalfLen1; return Halfband.kHalf1;
            case 2: half[0] = Halfband.kHalfLen2; return Halfband.kHalf2;
            default: half[0] = Halfband.kHalfLen3; return Halfband.kHalf3;
        } }

    /** port of base_blk.cpp:Cascade. len[k]: length of level k (level stages = the input &gt;&gt; 4) */
    static final class Cascade { int stages = 0; long[] len; BlkIo.ReaderI raw;
        // port of base_blk.cpp:Cascade::eval: level k (k = stages: the input in Q4; k < stages: after decimation stage k) over [a, b), channel c
        BlkIo.WinI eval(int k, long a, long b, int c) {
            BlkIo.WinI w = new BlkIo.WinI(); w.base = a; w.d = new int[(int) (b - a)];
            if (k == stages) { int[] x = raw.read(a, b); for (long i = a; i < b; i++) w.d[(int) (i - a)] = x[(int) (i - a) * 2 + c] >> 4; return w; }
            final int s = k; int[] hf = new int[1]; final int[] h = tab(s, hf); final int half = hf[0]; final long n = len[k + 1];
            long[] lh = BlkIo.cover(2 * a - half, 2 * (b - 1) + half, n);
            final BlkIo.WinI x = eval(k + 1, lh[0], lh[1] + 1, c);
            final int[] xd = x.d; final long xb = x.base;
            for (long i = a; i < b; i++) {   // decimate() of base_layer.cpp
                long cc = 2 * i;
                long acc = (long) x.at(cc) << 30;
                for (int j = 1, t = 0; j <= half; j += 2, t++) {
                    long aa0 = cc - j, bb0 = cc + j;
                    long aa = aa0 >= 0 ? aa0 : -aa0, bb = bb0 < n ? bb0 : 2 * (n - 1) - bb0; aa = Math.min(Math.max(aa, 0L), n - 1); bb = Math.min(Math.max(bb, 0L), n - 1);
                    long ka = aa - xb, kb = bb - xb;
                    if (ka < 0 || ka >= xd.length || kb < 0 || kb >= xd.length) throw EncIo.fail("block-wise encoder: window index outside the block (internal error)");
                    long sa = xd[(int) ka], sb = xd[(int) kb];
                    acc += (long) h[t] * (sa + sb);
                }
                long r = (acc + (1L << 30)) >> 31;
                if (r > 2147483647L) r = 2147483647L; if (r < -2147483647L) r = -2147483647L;
                w.d[(int) (i - a)] = (int) r;
            }
            return w; } }

    // port of base_blk.cpp:main
    static int main(EncEnv env, String[] argv) {
        if (argv.length < 5) return 1;
        int R = Fmt.atoi(argv[2]), R0 = Fmt.atoi(argv[3]);
        int stages = 0; while ((R0 << stages) < R) stages++;
        if ((R0 << stages) != R) { env.printf("rate %d is not base %d * 2^n\n", R, R0); return 2; }   // FU23-5: said, as the C++ (there on stderr)
        BlkIo.ReaderI raw = new BlkIo.ReaderI(argv[1], 2); BlkIo.Writer out = null, o24 = null;
        try { Cascade C = new Cascade(); C.stages = stages; C.raw = raw; C.len = new long[stages + 1]; C.len[stages] = raw.frames;
            for (int k = stages - 1; k >= 0; k--) C.len[k] = C.len[k + 1] / 2;
            final long n = C.len[0];
            final int DB = 4096; long nbk = (n + DB - 1) / DB; byte[] dflag = new byte[(int) (2 * nbk)];
            final String b24p = env.get("BASE24"); out = new BlkIo.Writer(env, argv[4]); if (b24p != null) o24 = new BlkIo.Writer(env, b24p);
            // each CD sample needs only the dither flag of its own 4096-block, decided within the same base block; the flag file is written at the end
            int[] st = {0x12345678, 0x12345678 + 0x9E3779B9};
            final long BB = 64L * DB;   // base samples per block (a multiple of the dither block)
            for (long a0 = 0; a0 < n; a0 += BB) { env.checkCancel(); final long a1 = Math.min(n, a0 + BB);
                BlkIo.WinI[] ch = {C.eval(0, a0, a1, 0), C.eval(0, a0, a1, 1)};
                for (int c = 0; c < 2; c++) for (long k = a0 / DB; k < (a1 + DB - 1) / DB; k++) {   // adaptive dither decision (base_layer.cpp)
                    long s0 = k * DB, s1 = Math.min(n, s0 + DB); int P = 16; double[] r = new double[P + 1], a = new double[P + 1], t = new double[P + 1]; double[] xx = new double[(int) (s1 - s0)];
                    for (long i = s0; i < s1; i++) xx[(int) (i - s0)] = ch[c].at(i) / 16.0;
                    for (int q = 0; q <= P; q++) for (int i = q; i < xx.length; i++) r[q] += xx[i] * xx[i - q];
                    double rms = 0;
                    if (r[0] > 0) { r[0] *= 1 + 1e-9; a[0] = 1; double e = r[0];
                        for (int i = 1; i <= P; i++) { double acc = r[i]; for (int j = 1; j < i; j++) acc += a[j] * r[i - j]; double kk = -acc / e; for (int j = 1; j < i; j++) t[j] = a[j] + kk * a[i - j]; for (int j = 1; j < i; j++) a[j] = t[j]; a[i] = kk; e *= 1 - kk * kk; }
                        double se = 0; for (int i = P; i < xx.length; i++) { double p = 0; for (int j = 0; j <= P; j++) p += a[j] * xx[i - j]; se += p * p; } rms = Math.sqrt(se / Math.max(1L, (long) xx.length - P)); }
                    dflag[(int) (c * nbk + k)] = (byte) (rms < 128.0 && r[0] > 0 ? 1 : 0); }
                short[] o16 = new short[(int) (a1 - a0) * 2];
                for (long i = a0; i < a1; i++)
                    for (int c = 0; c < 2; c++) {
                        int s = st[c];
                        s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u1 = (s >>> 20) & 0xFFF;
                        s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u2 = (s >>> 20) & 0xFFF;
                        st[c] = s;
                        int d = dflag[(int) (c * nbk + i / DB)] != 0 ? u1 + u2 - 4096 : 0;
                        long v = ((long) ch[c].at(i) + d + 2048) >> 12;
                        if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                        o16[(int) (i - a0) * 2 + c] = (short) v;
                    }
                out.put(o16);
                if (o24 != null) { int[] ob24 = new int[(int) (a1 - a0) * 2]; for (long i = a0; i < a1; i++) for (int c = 0; c < 2; c++) { long v = ((long) ch[c].at(i) + 8) >> 4; ob24[(int) (i - a0) * 2 + c] = (int) v; } o24.put(ob24); } }
            if (env.get("DITHERFLAGS") != null) EncIo.writeVec(env, env.get("DITHERFLAGS"), dflag);
            { long nd = 0; for (byte v : dflag) nd += v; env.printf("adaptive dither: %ld of %ld blocks dithered\n", nd, (long) dflag.length); }
            out.finish(); out = null; if (o24 != null) { o24.finish(); o24 = null; }
            env.printf("base layer: %ld frames @ %d Hz, %d stages\n", n, R0, stages);
            return 0; }
        finally { if (out != null) out.close(); if (o24 != null) o24.close(); raw.close(); } }
}
