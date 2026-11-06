// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import com.lae.core.LaeException;

/**
 * Port of src/laes_tool.cpp main for the commands enc, dec and verify (the same arguments and messages as `laes_tool`; for tests and as a
 * command-line tool):
 *   enc &lt;out.wav | out.flac&gt; (--core=&lt;core.wav&gt; | --derive-core) [--core-bits=16|24|32] &lt;master.wav&gt;[@T] ... [--mem=&lt;GB&gt;] [--timing]
 *       (Java only: --threads=&lt;n&gt;; the C++ ignores it like every argument starting with '-')
 *   dec &lt;in.wav | in.flac&gt; &lt;out dir&gt; [--levels] | --list | --pick=&lt;n&gt;[,&lt;n&gt;...] | --pick=&lt;rate&gt;:&lt;bits&gt;
 *   verify &lt;in.wav | in.flac&gt; &lt;master.wav&gt; ...
 * Environment (test hooks, as the C++): LAES_PIECE, LAES_FORCE_RF64, LAES_SELFCHECK_FLIP, KEEPTMP.
 * Not ported: align, selftest, excerpt, reduce, mkcd (out of scope); enc with master.wav@auto needs LaesEncoder.Options.aligner.
 */
public final class LaesTool {
    private LaesTool() {}

    public static void main(String[] args) { System.out.flush(); final int code = run(args); System.out.flush(); System.exit(code); }

    // port of laes_tool.cpp:main  (args = the C++ argv[1..])
    public static int run(String[] args) {
        if (args.length < 1) { System.out.println("laes enc | dec | verify | align | selftest | excerpt | mkcd  (see the source header)"); return 1; }
        final String cmd = args[0];
        if (cmd.equals("enc") && args.length >= 3) {
            final LaesEncoder.Options o = LaesEncoder.Options.fromArgs(args);
            o.aligner = new LaeyAutoAligner();   // master.wav@auto: LAEY's search in Java (com.lae.laey)
            return LaesEncoder.encode(o, new LaesEncoder.Listener() {
                public void message(String line) { System.out.println(line); }
                public void progress(String stage, double fraction) { } }, new AtomicBoolean(false)); }
        if ((cmd.equals("dec") || cmd.equals("verify")) && args.length >= 3) {
            try { return decVerify(args); }
            catch (OutOfMemoryError x) { System.out.println("stopped: bad allocation"); return 2; }
            catch (RuntimeException x) { System.out.println("stopped: " + x.getMessage()); return 2; } }
        System.out.println("bad arguments"); return 1; }

    // a choice's samples: the core (memory), a master (top level << w) or a rate level (level << w, limited to the word length)
    /** port of laes_tool.cpp:main::Src */
    static final class Src { int kind = 0; int m = 0; int k = 0; int rate = 0, bits = 0; long n = 0; }
    /** port of laes_tool.cpp:main::Choice */
    static final class Choice { String name, what; Src s; boolean derived = false; int from = -1; Choice(String n, String w, Src x) { name = n; what = w; s = x; } }
    // port of laes_tool.cpp:main::readSrc (the lambda)
    static long[] readSrc(LaesFile.Pcm core, List<Stream.DecMaster> dec, Src s, int c, long a, long b) { final long[] out = new long[(int) (b - a)];
        if (s.kind == 0) { System.arraycopy(core.c[c], (int) a, out, 0, (int) (b - a)); return out; }
        final Stream.DecMaster D = dec.get(s.m); final Stream.FileSig sg = Stream.levelOf(D, c, s.kind == 1 ? D.I.stages : s.k); try { sg.get(a, b, out, 0); } finally { sg.close(); } final int w = D.I.w;
        if (s.kind == 1) { for (int i = 0; i < out.length; i++) out[i] <<= w; return out; }
        final long mx = (1L << (s.bits - 1)) - 1, mn = -(1L << (s.bits - 1)); for (int i = 0; i < out.length; i++) out[i] = Math.max(mn, Math.min(mx, out[i] << w)); return out; }

    // port of laes_tool.cpp:main ("dec" / "verify")
    static int decVerify(String[] args) {
        final boolean isDec = args[0].equals("dec"); final String inPath = args[1];
        final String wd = LaesEncoder.workDir(inPath + (isDec ? ".dec" : ".verify"));
        try { return decVerifyIn(args, isDec, inPath, wd); } finally { if (System.getenv("KEEPTMP") == null) LaesEncoder.removeAll(new File(wd)); } }
    private static int decVerifyIn(String[] args, boolean isDec, String inPath, String wd) {
        final String bodyPath = wd + "/body.bin"; final LaesFile.Pcm core = new LaesFile.Pcm(); long bodySize = 0;
        { final Stream.In in = new Stream.In(inPath);
          try { final byte[] mg = new byte[4]; if (in.n >= 4) in.read(0, mg, 0, 4);
            if (StreamEnc.eq4(mg, 0, "fLaC")) {   // metadata: STREAMINFO kept, the "LAE2" APPLICATION blocks into the payload file; the audio through the FLAC reader
                long p = 4; boolean last = false; java.io.ByteArrayOutputStream meta = new java.io.ByteArrayOutputStream(); meta.write('f'); meta.write('L'); meta.write('a'); meta.write('C'); final Stream.Out bo = new Stream.Out(bodyPath);
                try { while (!last) { if (p + 4 > in.n) { System.out.println(inPath + ": metadata truncated"); return 2; } final byte[] bh = new byte[4]; in.read(p, bh, 0, 4); last = (bh[0] & 0x80) != 0; final int type = bh[0] & 0x7F; final long len = (long) (bh[1] & 0xFF) << 16 | (bh[2] & 0xFF) << 8 | (bh[3] & 0xFF);
                        if (p + 4 + len > in.n) { System.out.println(inPath + ": metadata truncated"); return 2; }
                        if (type == 0 || type != 2) { final byte[] b = new byte[(int) len + 4]; in.read(p, b, 0, b.length); if (type == 0) meta.write(b, 0, b.length); }
                        else { final byte[] b = new byte[(int) len]; in.read(p + 4, b, 0, b.length); if (len >= 4 && StreamEnc.eq4(b, 0, "LAE2")) { bo.put(b, 4, b.length - 4); bodySize += len - 4; } }
                        p += 4 + len; }
                    bo.close(); }
                finally { StreamEnc.closeQuiet(bo); }
                byte[] mb = meta.toByteArray(); if (mb.length > 4) mb[4] |= (byte) 0x80; final byte[] rest = new byte[(int) (in.n - p)]; in.read(p, rest, 0, rest.length);
                final byte[] all = new byte[mb.length + rest.length]; System.arraycopy(mb, 0, all, 0, mb.length); System.arraycopy(rest, 0, all, mb.length, rest.length);
                final Flac.Whole W = Flac.readFlac(all);
                if (!W.err.isEmpty()) { System.out.println(inPath + ": " + W.err); return 2; } core.rate = W.in.rate; core.bits = W.in.bps; core.c = W.ch;
                System.out.println("FLAC core master: " + core.rate + " Hz " + core.bits + "-bit, " + core.frames() + " frames, MD5 " + (W.md5ok ? "ok" : "MISMATCH")); if (!W.md5ok) return 2; }
            else { final StreamEnc.WavInfo wi = new StreamEnc.WavInfo(); final long[] lae = {-1, 0}; final String e = StreamEnc.probeWav(inPath, wi, lae); if (!e.isEmpty()) { System.out.println(e); return 2; }
                core.rate = wi.rate; core.bits = wi.bits; final int[] x = new int[(int) wi.frames * 2]; StreamEnc.wavFrames(in, wi, 0, wi.frames, x, null); core.c = new long[][] {new long[(int) wi.frames], new long[(int) wi.frames]};
                for (int c = 0; c < 2; c++) for (int i = 0; i < wi.frames; i++) core.c[c][i] = x[i * 2 + c];
                final Stream.Out bo = new Stream.Out(bodyPath);
                try { if (lae[0] >= 0) { final byte[] b = new byte[1 << 22]; for (long a = 0; a < lae[1]; a += 1 << 22) { final int k = (int) Math.min(1 << 22, lae[1] - a); in.read(lae[0] + a, b, 0, k); bo.put(b, 0, k); } } bo.close(); }
                finally { StreamEnc.closeQuiet(bo); }
                bodySize = lae[0] >= 0 ? lae[1] : 0; } }
          finally { in.close(); } }
        // the extension's first 4 bytes name the format: "LAE2" = LAES (lossless, this tool), "LAE1" = LAEY (lossy, decoded by lae_dec)
        final byte[] mg = new byte[4]; if (bodySize >= 4) { final Stream.In b = new Stream.In(bodyPath); try { b.read(0, mg, 0, 4); } finally { b.close(); } }
        if (bodySize >= 4 && StreamEnc.eq4(mg, 0, "LAE1")) { System.out.println(inPath + ": this is an LAEY file (lossy, extension LAE1): decode it with lae_dec; laes reads LAES (extension LAE2)"); return 2; }
        if (bodySize < 8 || !StreamEnc.eq4(mg, 0, "LAE2")) { System.out.println(inPath + ": no LAES extension (LAE2) found"); return 2; }
        System.out.println("format: LAES (lossless, extension LAE2)");
        final int maxSt = 3;   // pieces sized for the highest rate a master can have
        final LaesEncoder.Options po = LaesEncoder.Options.fromArgs(args); final long piece = LaesEncoder.pieceFor(po, maxSt); final int threads = Par.threadsOr(po.threads);
        final List<Stream.DecMaster> dec = new ArrayList<Stream.DecMaster>(); final String de = Stream.decodeLaesS(bodyPath, core, core.rate, wd, dec, piece, threads);
        if (!de.isEmpty()) { System.out.println(inPath + ": " + de + " (the core master is still playable)"); return 2; }
        boolean all = true; for (int i = 0; i < dec.size(); i++) { System.out.println("master " + (i + 1) + ": " + dec.get(i).I.rate + " Hz " + dec.get(i).I.bits + "-bit, " + dec.get(i).I.frames + " frames, SHA-256 " + (dec.get(i).shaOk ? "ok" : "MISMATCH")); all &= dec.get(i).shaOk; }
        if (isDec) {
            // every choice the file offers: the core, each master, each master's rate levels (lossless), and a 16-bit version derived
            // from the longest-word choice at each rate that has no 16-bit choice of its own (a new master: lossy against its source)
            final List<Choice> ch = new ArrayList<Choice>(); { Src s = new Src(); s.rate = core.rate; s.bits = core.bits; s.n = core.frames(); ch.add(new Choice("core", "the core master (lossless)", s)); }
            for (int i = 0; i < dec.size(); i++) { final String mk = "master_" + (i + 1); final LaesFile.MasterInfo I = dec.get(i).I;
                if (!dec.get(i).shaOk) { System.out.println("master " + (i + 1) + ": SHA-256 mismatch: not offered (nor its rate levels)"); continue; }
                { Src s = new Src(); s.kind = 1; s.m = i; s.rate = I.rate; s.bits = I.bits; s.n = I.frames; ch.add(new Choice(mk + "_" + I.rate + "_" + I.bits, "master " + (i + 1) + " (lossless)", s)); }
                for (int k = I.stages - 1; k >= 0; k--) { Src s = new Src(); s.kind = 2; s.m = i; s.k = k; s.rate = I.rate >> (I.stages - k); s.bits = I.bits; s.n = LaesFile.levLen(I.frames, I.stages - k);
                    ch.add(new Choice(mk + "_level_" + s.rate + "_" + s.bits, "rate level of master " + (i + 1) + " (lossless)", s)); } }
            { final List<Integer> rates = new ArrayList<Integer>(); for (Choice c : ch) if (!rates.contains(c.s.rate)) rates.add(c.s.rate);
              for (int r : rates) { int best = -1; boolean has16 = false; for (int j = 0; j < ch.size(); j++) if (ch.get(j).s.rate == r) { has16 |= ch.get(j).s.bits == 16; if (best < 0 || ch.get(j).s.bits > ch.get(best).s.bits) best = j; }
                  if (has16 || best < 0 || ch.get(best).s.bits <= 16) continue;
                  final Choice sc = ch.get(best); Src s = new Src(); s.kind = sc.s.kind; s.m = sc.s.m; s.k = sc.s.k; s.rate = sc.s.rate; s.bits = 16; s.n = sc.s.n;
                  Choice dv = new Choice("derived16_" + r + "_from_" + sc.name, "16-bit master derived from " + (sc.name.equals("core") ? "the core master" : sc.name) + " by TPDF requantisation: lossless relative to its definition (bit-identical on every decoder), not lossless relative to its " + sc.s.bits + "-bit source", s); dv.derived = true; dv.from = best;
                  ch.add(dv); } }
            if (LaesEncoder.Options.flag(args, "--list")) { System.out.println("choices (use --pick=<n>[,<n>...] or --pick=<rate>:<bits>):");
                for (int j = 0; j < ch.size(); j++) System.out.println(String.format("  %2d  %6d Hz %2d-bit  %s", j + 1, ch.get(j).s.rate, ch.get(j).s.bits, ch.get(j).what)); return all ? 0 : 2; }
            final List<Integer> pick = new ArrayList<Integer>(); final String pk = LaesEncoder.Options.arg(args, "--pick");
            if (pk.isEmpty()) { pick.add(0); for (int j = 1; j < ch.size(); j++) if (!ch.get(j).derived && (ch.get(j).what.startsWith("master") || LaesEncoder.Options.flag(args, "--levels"))) pick.add(j); }
            else { int a = 0; for (;;) { final int b = pk.indexOf(',', a); final String t = pk.substring(a, b < 0 ? pk.length() : b); final int col = t.indexOf(':'); boolean found = false;
                    if (col < 0) { final long j = CStd.atoi(t); if (j >= 1 && j <= ch.size()) { pick.add((int) j - 1); found = true; } }
                    else { final int r = CStd.atoi(t), bb = CStd.atoi(t.substring(col + 1)); for (int j = 0; j < ch.size() && !found; j++) if (ch.get(j).s.rate == r && ch.get(j).s.bits == bb) { pick.add(j); found = true; } }
                    if (!found) { System.out.println("--pick: '" + t + "' is not one of the choices (see --list)"); return 2; } if (b < 0) break; a = b + 1; } }
            new File(args[2]).mkdirs(); final String d = args[2] + "/";
            for (int j : pick) { final Choice C = ch.get(j); final Src S = C.derived ? ch.get(C.from).s : C.s; final int by = C.s.bits / 8; final long n = C.s.n, db = n * 2 * by;
                final byte[] h = LaesEncoder.wavHeader(C.s.rate, C.s.bits); final long fmtLen = C.s.bits > 16 ? 40 : 16;
                com.lae.core.Le.put32(h, 4, (int) (4 + 8 + fmtLen + 8 + db + (db & 1))); com.lae.core.Le.put32(h, h.length - 4, (int) db);
                boolean okw = true; Stream.Out o = null;
                try { o = new Stream.Out(d + C.name + ".wav"); o.put(h, 0, h.length); final long P = 1 << 18;
                    final long[] stc = {20261029L + 0x9E3779B97F4A7C15L * 1, 20261029L + 0x9E3779B97F4A7C15L * 2}; final int sft = C.derived ? S.bits - 16 : 0;   // derive16: one stream per channel
                    for (long a = 0; a < n; a += P) { final long e = Math.min(n, a + P); final long[] x0 = readSrc(core, dec, S, 0, a, e), x1 = readSrc(core, dec, S, 1, a, e); final byte[] b = new byte[(int) (e - a) * 2 * by]; int f = 0;
                        for (long i = a; i < e; i++) for (int c = 0; c < 2; c++) { long v = (c != 0 ? x1 : x0)[(int) (i - a)];
                            if (C.derived) { long st = stc[c]; st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r1 = st >>> (64 - sft); st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r2 = st >>> (64 - sft); stc[c] = st;
                                v = Math.max(-32768L, Math.min(32767L, (v + r1 - r2 + (1L << (sft - 1))) >> sft)); }
                            final int u = (int) v; for (int k = 0; k < by; k++) b[f++] = (byte) (u >>> (8 * k)); }
                        o.put(b, 0, f); }
                    if ((db & 1) != 0) o.put(new byte[1], 0, 1); o.close(); o = null; }
                catch (LaeException x) { okw = false; }
                finally { StreamEnc.closeQuiet(o); }
                all &= okw; System.out.println(C.name + ".wav: " + C.s.rate + " Hz " + C.s.bits + "-bit, " + C.what); }
            System.out.println("written to " + args[2]); }
        else { final boolean[] used = new boolean[dec.size()];   // each given file must equal one decoded master (the file order may differ from the command line)
            for (int i = 2; i < args.length; i++) { if (args[i].length() > 0 && args[i].charAt(0) == '-') continue; final StreamEnc.WavInfo wi = new StreamEnc.WavInfo(); final String re = StreamEnc.probeWav(args[i], wi, null); int hit = -1;
                for (int k = 0; k < dec.size() && hit < 0 && re.isEmpty(); k++) { if (used[k] || wi.rate != dec.get(k).I.rate || wi.bits != dec.get(k).I.bits || wi.frames != dec.get(k).I.frames) continue;
                    Src s = new Src(); s.kind = 1; s.m = k; final Stream.In in = new Stream.In(args[i]); boolean same = true; final long P = 1 << 20; int[] r = new int[0]; byte[] buf = null;
                    try { for (long a = 0; a < wi.frames && same; a += P) { final long e = Math.min(wi.frames, a + P); if (r.length < (e - a) * 2) r = new int[(int) (e - a) * 2]; buf = StreamEnc.wavFrames(in, wi, a, e, r, buf); final long[] x0 = readSrc(core, dec, s, 0, a, e), x1 = readSrc(core, dec, s, 1, a, e);
                            for (long q = 0; q < e - a && same; q++) same = r[(int) q * 2] == x0[(int) q] && r[(int) q * 2 + 1] == x1[(int) q]; } }
                    finally { in.close(); }
                    if (same) hit = k; }
                if (hit >= 0) { used[hit] = true; System.out.println("master " + (hit + 1) + " vs " + args[i] + ": IDENTICAL"); } else { System.out.println(args[i] + ": DIFFERENT (no decoded master equals it)"); all = false; } }
            for (int k = 0; k < dec.size(); k++) if (!used[k] && args.length > 2) System.out.println("master " + (k + 1) + ": not compared"); }
        return all ? 0 : 2; }
}
