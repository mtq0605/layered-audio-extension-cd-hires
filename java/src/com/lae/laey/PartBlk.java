// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.io.File;
import java.util.Arrays;

/**
 * Port of src/part_blk.cpp. Block-wise part_coder.cpp (2026-10-29): the same range-coded stream and the same checks, one row of roots at
 * a time. The tokens are read from the dump in order; the coded plane of each row goes to a temporary file; then the stream is decoded
 * row by row and its tokens and plane are compared with the dump, the coded plane and the encoder's plane. Only the coded bytes (the
 * stream itself) stay in memory.
 * usage: part &lt;dump&gt; [encoder plane] [decoded plane out]; environment PARTBYTES (TIERSTAT: statistics not ported)
 */
final class PartBlk {
    private PartBlk() {}

    /** port of part_blk.cpp:TokIn: tokens of the dump, read in order through a window (token i = int32 number 11 + i of the file) */
    static final class TokIn implements PartStreamEnc.Tokens { BlkIo.ReaderI r; long n = 0, wb = 0; int[] w = new int[0];
        // port of part_blk.cpp:TokIn::operator[]
        @Override public int get(long i) { if (i >= n) throw EncIo.fail("part coder: the stream asks for more tokens than the dump holds");
            if (i < wb || i >= wb + w.length) { wb = i; final long e = Math.min(n, i + (1L << 20)); w = r.read(11 + i, 11 + e); } return w[(int) (i - wb)]; }
        // port of part_blk.cpp:TokIn::push_back
        @Override public void push(int v) { throw EncIo.fail("internal error: token input written"); } }
    /** port of part_blk.cpp:TokCheck: decoded tokens, compared with the dump as they come */
    static final class TokCheck implements PartStreamEnc.Tokens { TokIn ref; long cnt = 0; boolean same = true;
        @Override public int get(long i) { return 0; }
        // port of part_blk.cpp:TokCheck::push_back
        @Override public void push(int v) { if (cnt >= ref.n || ref.get(cnt) != v) same = false; cnt++; } }

    // port of part_blk.cpp:main
    static int main(EncEnv env, String[] argv) {
        BlkIo.ReaderI dumpR = new BlkIo.ReaderI(argv[1], 1); final String pePath = argv[1] + ".pe";
        BlkIo.ReaderI encR = null, peR = null; BlkIo.Writer pw = null, outW = null;
        try {
            if (dumpR.frames < 11) { env.printf("not a partition dump\n"); return 2; }   // said, as the C++ (there on stderr)
            int[] hd = dumpR.read(0, 11);
            if (hd[0] != 0x50415254 || hd[10] < 0 || dumpR.frames != 11 + (long) hd[10]) { env.printf("not a partition dump\n"); return 2; }   // said, as the C++ (there on stderr)
            PartStream.Hdr h = new PartStream.Hdr(); h.K = hd[1]; h.N = hd[2]; h.k0 = hd[3]; h.k1 = hd[4]; h.rate = hd[5]; h.L = hd[6]; h.cw = hd[7]; h.lv = hd[8]; h.eq = hd[9];
            TokIn tin = new TokIn(); tin.r = dumpR; tin.n = hd[10];
            int[] row;
            PartStreamEnc.Enc e = new PartStreamEnc.Enc();
            { pw = new BlkIo.Writer(env, pePath); PartStreamEnc.Walker w = new PartStreamEnc.Walker(h, true);
              while (!w.done()) { env.checkCancel(); final long f0 = w.M0; row = new int[w.rowFrames() * h.N]; w.row(e, null, tin, row, f0); pw.put(row); }
              pw.finish(); pw = null; }
            e.flush();
            TokIn tref = new TokIn(); tref.r = dumpR; tref.n = tin.n; TokCheck tchk = new TokCheck(); tchk.ref = tref;
            boolean planeOk = true;
            if (argv.length > 2) { encR = new BlkIo.ReaderI(argv[2], 1); if (encR.frames != h.K * h.N) planeOk = false; }
            if (argv.length > 3) { outW = new BlkIo.Writer(env, argv[3]); int[] ph = {0x504C4E45, (int) h.K, h.k0, h.rate, (int) h.L}; outW.put(ph); }
            boolean pdSame = true; { peR = new BlkIo.ReaderI(pePath, 1); PartStream.Dec dc = new PartStream.Dec(); dc.p = e.out; dc.off = 0; dc.n = e.size; dc.init(); PartStreamEnc.Walker w = new PartStreamEnc.Walker(h, false);
                while (!w.done()) { env.checkCancel(); final long f0 = w.M0; final long rf = w.rowFrames(); row = new int[(int) rf * h.N]; w.row(null, dc, tchk, row, f0);
                    int[] pe = peR.read(f0 * h.N, (f0 + rf) * h.N); if (!Arrays.equals(pe, row)) pdSame = false;
                    if (encR != null && planeOk) { int[] en = encR.read(f0 * h.N, (f0 + rf) * h.N); if (!Arrays.equals(en, row)) planeOk = false; }
                    if (outW != null) outW.put(row); } }
            if (outW != null) { outW.finish(); outW = null; }
            boolean ok = tchk.same && tchk.cnt == tin.n && pdSame;
            if (env.get("PARTBYTES") != null) {
                int[] ph = {(int) h.K, h.N, h.k0, h.k1, h.rate, (int) h.L, h.cw, h.lv, h.eq, e.size};
                byte[] buf = new byte[40 + e.size]; EncIo.toBytes(ph, 0, 10, buf); System.arraycopy(e.out, 0, buf, 40, e.size); EncIo.writeVec(env, env.get("PARTBYTES"), buf); }
            double secs = (double) h.L / h.rate;
            env.printf("partition mode %d (%s): %zu bytes = %.1f kb/s (mono) | symbols %s | decoded plane %s encoder plane\n", h.lv, h.eq != 0 ? "equal" : "unequal",
                       (long) e.size, e.size * 8.0 / secs / 1000, ok ? "BIT-EXACT" : "MISMATCH", planeOk ? "==" : "!=");
            return ok && planeOk ? 0 : 1; }
        finally { if (pw != null) pw.close(); if (outW != null) outW.close(); if (encR != null) encR.close(); if (peR != null) peR.close(); dumpR.close(); new File(pePath).delete(); } }
}
