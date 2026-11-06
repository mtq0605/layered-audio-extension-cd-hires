// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

/** Port of src/pad_blk.cpp: block-wise pad_mirror.cpp (2026-10-29): the same file, written piece by piece from pieces of the input. */
final class PadBlk {
    private PadBlk() {}

    // port of pad_blk.cpp:main   usage: pad <in raw> <out raw> <P>
    static int main(EncEnv env, String[] argv) {
        BlkIo.ReaderI in = new BlkIo.ReaderI(argv[1], 2); BlkIo.Writer out = null;
        try { final long n = in.frames, P = Fmt.atoll(argv[3]), tot = n + 2 * P, BS = 1L << 20;
            out = new BlkIo.Writer(env, argv[2]);
            for (long a = 0; a < tot; a += BS) { env.checkCancel(); final long b = Math.min(tot, a + BS);
                long lo = n, hi = -1; long[] pts = {a, b - 1, Math.min(Math.max(a, P), b - 1), Math.min(Math.max(a, P + n - 1), b - 1)};
                for (long i : pts) { long j = mir(i, P, n); lo = Math.min(lo, j); hi = Math.max(hi, j); }
                if (a <= P && P < b) { lo = Math.min(lo, 0L); } if (a <= P + n - 1 && P + n - 1 < b) hi = Math.max(hi, n - 1);
                lo = Math.max(lo, 0L); hi = Math.min(hi, n - 1); int[] x = in.read(lo, hi + 1); int[] y = new int[(int) (b - a) * 2];
                for (long i = a; i < b; i++) { long j = mir(i, P, n); if (j < lo || j > hi) throw EncIo.fail("block-wise padding: index outside the block (internal error)");
                    y[(int) (i - a) * 2] = x[(int) (j - lo) * 2]; y[(int) (i - a) * 2 + 1] = x[(int) (j - lo) * 2 + 1]; }
                out.put(y); }
            out.finish(); out = null; return 0; }
        finally { if (out != null) out.close(); in.close(); } }
    // port of pad_blk.cpp:main lambda mir
    private static long mir(long i, long P, long n) { long j = i - P; if (j < 0) j = -j; if (j >= n) j = 2 * (n - 1) - j; return j; }
}
