// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;

/**
 * Port of noise_marks.h (2026-11-04): the LAEY noise marking format read by noise= of the encoder. Positions are rectangles of the
 * encoder's own cells (textbook MDCT of mdct_tb.h, N = 512; a cell = 8 bins x 1 frame; M and S together). Geometry only: the master's
 * SHA-256, length and rate, and the rectangles. Only what the encoder uses is ported (readMarks, masterSha, frameT0, colHz).
 */
final class NoiseMarks {
    private NoiseMarks() {}

    static final int NM = MdctTb.N, CW = 8, NCOL = NM / CW;

    /** port of noise_marks.h:Pos: frames [m0, m1), columns [c0, c1) */
    static final class Pos { long m0, m1; int c0, c1; Pos(long a, long b, int c, int d) { m0 = a; m1 = b; c0 = c; c1 = d; } }

    /** port of noise_marks.h:Marks (the comment text is not read back) */
    static final class Marks { String sha = ""; long frames = 0; double rate = 0; final List<Pos> pos = new ArrayList<Pos>(); }

    // port of noise_marks.h:frameT0: start of frame m's hop [mN - N/2, mN + N/2)
    static double frameT0(long m, double rate) { return (m * (double) NM - NM / 2.0) / rate; }
    // port of noise_marks.h:colHz
    static double colHz(int c, double rate) { return c * CW * rate / (2.0 * NM); }

    // port of noise_marks.h:readMarks (sscanf patterns: "LAENOISE 1", "master sha256 %79s frames %lld rate %lf", "grid mdct %d cell %d", "pos %ld %ld %d %d")
    static boolean readMarks(String path, Marks mk) {
        BufferedReader r = null; boolean magic = false;
        try {
            r = new BufferedReader(new InputStreamReader(new FileInputStream(path), "ISO-8859-1"));
            String line;
            while ((line = r.readLine()) != null) {
                int h = line.indexOf('#'); if (h >= 0) line = line.substring(0, h);
                String[] t = line.trim().split("\\s+");
                if (line.startsWith("LAENOISE 1")) magic = true;
                else if (t.length >= 6 && t[0].equals("master") && t[1].equals("sha256") && t[3].equals("frames") && t[5].equals("rate") && t.length >= 7) {
                    try { long fr = Long.parseLong(t[4]); double rt = Double.parseDouble(t[6]); mk.sha = t[2].length() > 79 ? t[2].substring(0, 79) : t[2]; mk.frames = fr; mk.rate = rt; } catch (NumberFormatException e) { }
                } else if (t.length >= 5 && t[0].equals("grid") && t[1].equals("mdct") && t[3].equals("cell")) {
                    try { int n = Integer.parseInt(t[2]), cw = Integer.parseInt(t[4]); if (n != NM || cw != CW) return false; } catch (NumberFormatException e) { }
                } else if (t.length >= 5 && t[0].equals("pos")) {
                    try { mk.pos.add(new Pos(Long.parseLong(t[1]), Long.parseLong(t[2]), Integer.parseInt(t[3]), Integer.parseInt(t[4]))); } catch (NumberFormatException e) { }
                }
            }
        } catch (IOException e) { return false; }
        finally { if (r != null) try { r.close(); } catch (IOException ig) { } }
        return magic && !mk.sha.isEmpty();
    }

    /** port of noise_marks.h:masterSha (WavStream of noise_io.h): SHA-256 of the data chunk's bytes, frames x 2 x bytes per sample; null when unreadable */
    static String masterSha(String path) {
        WavIn.Info wi = new WavIn.Info();
        if (!WavIn.probe(path, wi) || wi.ch != 2 || wi.dataOff < 0) return null;
        final int bps = wi.bits / 8; if (bps <= 0) return null;
        final long frames = wi.dataBytes / (2L * bps), total = frames * 2 * bps;
        RandomAccessFile f = null;
        try {
            f = new RandomAccessFile(path, "r");
            com.lae.laes.LaesFile.Sha256 h = new com.lae.laes.LaesFile.Sha256(); byte[] b = new byte[1 << 22];
            for (long off = 0; off < total; off += (1 << 22)) { int n = (int) Math.min(1 << 22, total - off); f.seek(wi.dataOff + off); f.readFully(b, 0, n); h.update(b, 0, n); }
            byte[] d = new byte[32]; h.finish(d);
            StringBuilder s = new StringBuilder(64); for (byte x : d) s.append(String.format("%02x", x & 0xFF));
            return s.toString();
        } catch (IOException e) { return null; }
        finally { if (f != null) try { f.close(); } catch (IOException ig) { } }
    }
}
