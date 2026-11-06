// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import com.lae.core.Cv;
import com.lae.laey.LaeyEncoder;
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;

/**
 * Port of laes_tool.cpp:autoAlign for master.wav@auto, with LAEY's search run in-process: the Java LAEY encoder (com.lae.laey,
 * {@link LaeyEncoder#run}) with exactly the arguments the C++ hands to lae_enc ("&lt;master&gt; &lt;dummy&gt; 3 cd=&lt;core master&gt; cdcheck [bits=24]"), and
 * its printed lines read as the C++ reads lae_enc's output (ALIGN_SUGGEST, "align: &lt;search&gt; search: ", the phase line's pooled residual).
 * The numbers are taken from the same text the C++ parses (the offset with 7 decimals, the residual with 2), so T is the same double and
 * the LAES file is the same. --lae-enc is not needed (no external program runs); the C++ check that the lae_enc file exists is not made.
 * LaesEncoder.Aligner.ask: the C++ asks "use this alignment? [y/N]" on stdin; here a line is read from System.in (EOF: no).
 * PORT_RULES 7: the own check's residual uses the C library's log10 (laes_tool.cpp autoAlign: 10 * std::log10(er / eb)); here
 * Math.log10. It decides only a printed note (resid &gt; -40) and, when LAEY gave no phase line, the -60 / -54 dB question; never the file.
 */
public final class LaeyAutoAligner implements LaesEncoder.Aligner {
    // port of laes_tool.cpp:autoAlign
    @Override public LaesEncoder.AutoT align(String cdPath, StreamEnc.Core cd, String mPath, StreamEnc.Prep m, String enc) {
        LaesEncoder.AutoT A = new LaesEncoder.AutoT(); int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++;
        if ((cd.rate << st) != m.rate || st > 3) { A.why = "master rate must be the core master rate times 1, 2, 4 or 8"; return A; }
        if (st == 0) { A.why = "LAEY's search needs a master at 2, 4 or 8 times the core master rate; give T by hand (master.wav@T)"; return A; }
        final String dummy = new File(System.getProperty("java.io.tmpdir"), "laes_align_unused.lae").getPath();   // cdcheck encodes nothing
        final StringBuilder out = new StringBuilder();
        String[] args = m.bits == 32 ? new String[] {"lae_enc", mPath, dummy, "3", "cd=" + cdPath, "cdcheck", "bits=24"} : new String[] {"lae_enc", mPath, dummy, "3", "cd=" + cdPath, "cdcheck"};
        LaeyEncoder.run(args, null, out, null, null, null);
        final String[] nm = {"whitened", "plain"}; String sug = ""; String[] why = {"", ""}; double[] pooled = {1, 1};
        for (String s : out.toString().split("\n")) { while (!s.isEmpty() && (s.endsWith("\n") || s.endsWith("\r"))) s = s.substring(0, s.length() - 1); if (s.startsWith("ALIGN_SUGGEST ")) sug = s;
            for (int k = 0; k < 2; k++) { final String pre = "align: " + nm[k] + " search: ", ph = "align (" + nm[k] + "): phase: "; int p;
                if (s.startsWith(pre)) why[k] = s.substring(pre.length()); else if (s.startsWith(ph) && (p = s.indexOf("pooled residual ")) >= 0) pooled[k] = atof(s.substring(p + 16)); } }
        if (sug.isEmpty()) { A.why = "lae_enc gave no ALIGN_SUGGEST line (old lae_enc? design D is needed)"; return A; }
        boolean[] have = {false, false}; double[] T = {0, 0};
        for (int k = 0; k < 2; k++) { int p = sug.indexOf(nm[k] + "="); if (p < 0) continue; int c = sug.indexOf(':', p), sp = sug.indexOf(' ', p);
            String v = sp < 0 ? sug.substring(c + 1) : sug.substring(c + 1, sp); if (!v.equals("-")) { have[k] = true; T[k] = atof(v) / (double) (1 << st); } }
        A.notes = "LAEY's own placement checks (LAEY rules, not LAES): whitened: " + why[0] + "; plain: " + why[1];
        if (!have[0] && !have[1]) { A.why = "LAEY's search found no position; give T by hand. " + A.notes; return A; }
        final int k = have[0] && have[1] ? (pooled[1] < pooled[0] ? 1 : 0) : (have[0] ? 0 : 1); A.found = true; A.T = T[k]; A.search = nm[k]; A.err = pooled[k] <= 0 ? pooled[k] : 0;
        // own check: the master's base level against the core moved by T, worst of three blocks (head / middle / tail, up to 5 s each)
        final int w = m.w, up = (m.bits - 16) - w; Stream.Sig[] base = {StreamEnc.levelOf(m, 0, 0), StreamEnc.levelOf(m, 1, 0)};   // the base level (samples >> w)
        try {
            final long nb = base[0].n, bl = Math.max(1L, Math.min(nb / 3, 5L * cd.rate)); final long[] b0 = {0, (nb - bl) / 2, nb - bl};
            long wh = Cv.d2l(Math.floor(A.T)); int fr = (int) Cv.llround((A.T - Math.floor(A.T)) * 4294967296.0); if (fr == 0 && A.T != Math.floor(A.T)) wh++;
            final long[] h = LaesMath.refTaps(fr); A.resid = -200;
            for (int b = 0; b < 3; b++) { double eb = 0, er = 0; for (int c = 0; c < 2; c++) { final long n = cd.c[c].length; long[] bv = new long[(int) bl]; base[c].get(b0[b], b0[b] + bl, bv, 0);
                    for (long i = b0[b]; i < b0[b] + bl; i++) { LaesMath.Acc a = new LaesMath.Acc(); for (int q = -127; q <= 128; q++) a.mac(h[q + 127], cd.c[c][(int) LaesMath.mir(i + wh + q, n)]); final int s = 31 - up;
                        final double r = (double) (s > 0 ? a.shr(s) : a.lo << (-s)), x = (double) bv[(int) (i - b0[b])]; eb += x * x; er += (x - r) * (x - r); } }
                if (eb > 0 && er > 0) A.resid = Cv.max(A.resid, 10 * Math.log10(er / eb)); } }
        finally { StreamEnc.closeQuiet(base[0]); StreamEnc.closeQuiet(base[1]); }
        if (A.err == 0) A.err = A.resid;   // LAEY gave no phase line: use the own check
        return A; }

    // port of laes_tool.cpp acceptAlign, the question: a line from stdin; the first letter y or Y accepts
    @Override public boolean ask(String question) { System.out.print(question); System.out.flush();
        String l; try { l = new BufferedReader(new InputStreamReader(System.in)).readLine(); } catch (IOException e) { l = null; }
        if (l == null) { System.out.println("(no answer)"); return false; }
        int p = 0; while (p < l.length() && !Character.isLetter(l.charAt(p))) p++; return p < l.length() && (l.charAt(p) == 'y' || l.charAt(p) == 'Y'); }   // skips a byte-order mark or spaces

    // the C library's atof on the text after the number's start (the longest decimal prefix; correctly rounded as the UCRT strtod)
    static double atof(String s) { int i = 0, n = s.length(); while (i < n && Character.isWhitespace(s.charAt(i))) i++; int st = i; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
        int d = i; while (i < n && Character.isDigit(s.charAt(i))) i++; if (i < n && s.charAt(i) == '.') { i++; while (i < n && Character.isDigit(s.charAt(i))) i++; }
        if (i == d || (i == d + 1 && s.charAt(d) == '.')) return 0.0; String t = s.substring(st, i); if (t.endsWith(".")) t += "0"; return Double.parseDouble(t); }
}
