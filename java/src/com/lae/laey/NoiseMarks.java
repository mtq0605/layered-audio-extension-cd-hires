// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
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

    // port of noise_marks.h:readMarks (strict since 2026-11-07, SPEC_LAENOISE.md Section 4): a comment (# to the end of the line), CR and
    // blank lines are skipped. The first remaining line is exactly "LAENOISE 1". Then exactly one master line and exactly one grid line,
    // both before the first pos line. A line that begins with LAENOISE, master, grid or pos must have exactly its fields (a second
    // LAENOISE line is an error); any other line is ignored. why[0]: the reason when the file is refused (as noise_marks.h).
    static boolean readMarks(String path, Marks mk) { return readMarks(path, mk, null); }
    // the next line up to (not including) LF; a CR stays in it (a separator, as in noise_marks.h); null at the end of the file
    private static String lfLine(BufferedReader r) throws IOException { StringBuilder b = new StringBuilder(); int c; boolean any = false;
        while ((c = r.read()) >= 0) { any = true; if (c == '\n') return b.toString(); b.append((char) c); } return any ? b.toString() : null; }
    static boolean readMarks(String path, Marks mk, String[] why) {
        BufferedReader r = null; boolean magic = false, haveMaster = false, haveGrid = false; long ln = 0;
        try {
            r = new BufferedReader(new InputStreamReader(new FileInputStream(path), "ISO-8859-1"));
            String line;
            while ((line = lfLine(r)) != null) {   // lines end at LF only, as noise_marks.h (readLine also ended one at a lone CR)
                ln++; int h = line.indexOf('#'); if (h >= 0) line = line.substring(0, h);
                List<String> tl = new ArrayList<String>(); for (String x : line.split("[ \\t\\r]+")) if (!x.isEmpty()) tl.add(x);
                if (tl.isEmpty()) continue;
                String[] t = tl.toArray(new String[0]);
                if (!magic) { if (t.length == 2 && t[0].equals("LAENOISE") && t[1].equals("1")) { magic = true; continue; } return bad(why, ln, "the first line must be \"LAENOISE 1\" (not a marking file of version 1)"); }
                if (t[0].equals("LAENOISE")) return bad(why, ln, "a second LAENOISE line");
                if (t[0].equals("master")) {
                    if (haveMaster) return bad(why, ln, "a second master line");
                    if (!mk.pos.isEmpty()) return bad(why, ln, "the master line must come before the pos lines");
                    if (t.length != 7 || !t[1].equals("sha256") || !t[3].equals("frames") || !t[5].equals("rate")) return bad(why, ln, "expected \"master sha256 <64 hex digits> frames <L> rate <fs>\"");
                    if (t[2].length() != 64) return bad(why, ln, "the SHA-256 must have 64 hex digits");
                    for (int i = 0; i < 64; i++) { char c = t[2].charAt(i); if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return bad(why, ln, "the SHA-256 must be lower-case hex digits"); }
                    if (!digits(t[4], 18) || !digits(t[6], 9)) return bad(why, ln, "frames and rate must be whole numbers");
                    mk.sha = t[2]; mk.frames = Long.parseLong(t[4]); mk.rate = (double) Long.parseLong(t[6]); haveMaster = true;
                } else if (t[0].equals("grid")) {
                    if (haveGrid) return bad(why, ln, "a second grid line");
                    if (!mk.pos.isEmpty()) return bad(why, ln, "the grid line must come before the pos lines");
                    if (t.length != 5 || !t[1].equals("mdct") || !t[3].equals("cell") || !digits(t[2], 9) || !digits(t[4], 9)) return bad(why, ln, "expected \"grid mdct 512 cell 8\"");
                    if (Integer.parseInt(t[2]) != NM || Integer.parseInt(t[4]) != CW) return bad(why, ln, "unsupported grid mdct " + t[2] + " cell " + t[4] + " (version 1 has only mdct 512 cell 8)");
                    haveGrid = true;
                } else if (t[0].equals("pos")) {
                    if (!haveMaster || !haveGrid) return bad(why, ln, "a pos line before the master and grid lines");
                    if (t.length != 5 || !digits(t[1], 18) || !digits(t[2], 18) || !digits(t[3], 9) || !digits(t[4], 9)) return bad(why, ln, "expected \"pos <m0> <m1> <c0> <c1>\" with whole numbers");
                    mk.pos.add(new Pos(Long.parseLong(t[1]), Long.parseLong(t[2]), Integer.parseInt(t[3]), Integer.parseInt(t[4])));
                }
            }
        } catch (IOException e) { return bad(why, 0, "cannot open the file"); }
        finally { if (r != null) try { r.close(); } catch (IOException ig) { } }
        if (!magic) return bad(why, 0, "not a marking file (no \"LAENOISE 1\" line)");
        if (!haveMaster) return bad(why, 0, "no master line");
        if (!haveGrid) return bad(why, 0, "no grid line");
        return true;
    }
    private static boolean bad(String[] why, long ln, String w) { if (why != null && why.length > 0) why[0] = ln > 0 ? "line " + ln + ": " + w : w; return false; }
    private static boolean digits(String s, int maxLen) { if (s.isEmpty() || s.length() > maxLen) return false; for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); if (c < '0' || c > '9') return false; } return true; }

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
