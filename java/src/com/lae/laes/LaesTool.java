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
 *   enc &lt;out.wav | out.flac&gt; (--core=&lt;core.wav&gt; | --derive-core) [--core-bits=16|24|32] &lt;master.wav&gt;[@T] ... [--mem=&lt;GB&gt;] [--timing] [--overwrite]
 *       (Java only: --threads=&lt;n&gt;; the C++ ignores it like every argument starting with '-')
 *   dec &lt;in.wav | in.flac&gt; &lt;out dir&gt; [--levels] [--pick=&lt;sel&gt;[,&lt;sel&gt;...]] [--overwrite] | --list | --list=all, where &lt;sel&gt; is an entry of --list
 *       (&lt;n&gt; or a range &lt;a&gt;-&lt;b&gt;), &lt;rate&gt;:&lt;bits&gt; (the masters of that rate and word length), master:&lt;k&gt; or name:&lt;text&gt; (a master by its
 *       number or by its exact name); a selection that matches several masters lists them and writes nothing
 *   verify &lt;in.wav | in.flac&gt; &lt;master.wav&gt; ...
 * Environment (test hooks, as the C++): LAES_PIECE, LAES_FORCE_RF64, LAES_SELFCHECK_FLIP, LAES_NO_SHORTLIST, LAES_DISK_RESERVE only with
 * LAE_TEST_MODE=1 (one line names them at the start); KEEPTMP always.
 * An existing output (enc: the file, dec: each WAV) is replaced only after two "yes" at a console, or with --overwrite.
 * Not ported: align, selftest, excerpt, reduce, mkcd (out of scope). enc with master.wav@auto: this tool uses LAEY's search (LaeyAutoAligner);
 * an app that calls LaesEncoder sets LaesEncoder.Options.aligner itself.
 */
public final class LaesTool {
    private LaesTool() {}

    // the texts of the questions (without a new line at the end)
    static final com.lae.core.Replace.Out OUT = new com.lae.core.Replace.Out() { public void print(String s) { System.out.print(s); System.out.flush(); } };
    public static void main(String[] args) { System.out.flush(); final int code = run(args); System.out.flush(); System.exit(code); }

    // port of laes_tool.cpp:main  (args = the C++ argv[1..])
    public static int run(String[] args) {
        if (args.length < 1) { System.out.println("laes enc | dec | verify  (see the source header; the other commands of the C++ laes are not in this port)"); return 1; }
        final String cmd = args[0]; { final String tl = LaesFile.testModeLine(); if (!tl.isEmpty()) System.out.println(tl); }
        if (cmd.equals("enc") && args.length >= 3) {
            final LaesEncoder.Options o = LaesEncoder.Options.fromArgs(args);
            o.aligner = new LaeyAutoAligner();   // master.wav@auto: LAEY's search in Java (com.lae.laey)
            return LaesEncoder.encode(o, new LaesEncoder.Listener() {
                public void message(String line) { System.out.println(line); }
                public void progress(String stage, double fraction) { } }, new AtomicBoolean(false)); }
        if ((cmd.equals("dec") || cmd.equals("verify")) && args.length >= 3) {
            try { return decVerify(args); }
            catch (LaesEncoder.WorkLocked x) { System.out.println(x.getMessage()); return 2; }
            catch (OutOfMemoryError x) { System.out.println("stopped: bad allocation"); return 2; }
            catch (RuntimeException x) { System.out.println("stopped: " + x.getMessage()); return 2; } }
        System.out.println("bad arguments"); return 1; }

    // a choice's samples: the core (memory), a master (top level << w) or a rate level (level << w, limited to the word length)
    /** port of laes_tool.cpp:main::Src */
    static final class Src { int kind = 0; int m = 0; int k = 0; int rate = 0, bits = 0; long n = 0; }
    /** port of laes_tool.cpp:main::Choice */
    static final class Choice { String name, what; Src s; boolean derived = false; int from = -1; int kind = 0; int no = 0; Choice(String n, String w, Src x) { name = n; what = w; s = x; } }   // no: the master's number in the menu order
    // port of laes_pick.h:parseRanges  ("6,12" / "6-12" / "2-4, 9": every number; null: not such a list)
    static List<Long> parseRanges(String s) { final List<Long> out = new ArrayList<Long>(); int i = 0; boolean any = false; final int n = s.length();
        for (;;) { while (i < n && s.charAt(i) == ' ') i++; if (i >= n) return any ? out : null;
            if (s.charAt(i) < '0' || s.charAt(i) > '9') return null; long a = 0; while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') { a = a * 10 + (s.charAt(i) - '0'); if (a > 100000) return null; i++; }
            long b = a; while (i < n && s.charAt(i) == ' ') i++;
            if (i < n && s.charAt(i) == '-') { i++; while (i < n && s.charAt(i) == ' ') i++; if (i >= n || s.charAt(i) < '0' || s.charAt(i) > '9') return null; b = 0;
                while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') { b = b * 10 + (s.charAt(i) - '0'); if (b > 100000) return null; i++; } while (i < n && s.charAt(i) == ' ') i++; }
            if (a < 1 || b < a) return null; for (long v = a; v <= b; v++) out.add(v); any = true;
            if (i >= n) return out; if (s.charAt(i) != ',') return null; i++; } }
    // port of laes_tool.cpp:main::readSrc (the lambda)
    static long[] readSrc(LaesFile.Pcm core, List<Stream.DecMaster> dec, Src s, int c, long a, long b) { final long[] out = new long[(int) (b - a)];
        if (s.kind == 0) { System.arraycopy(core.c[c], (int) a, out, 0, (int) (b - a)); return out; }
        final Stream.DecMaster D = dec.get(s.m); final Stream.FileSig sg = Stream.levelOf(D, c, s.kind == 1 ? D.I.stages : s.k); try { sg.get(a, b, out, 0); } finally { sg.close(); } final int w = D.I.w;
        if (s.kind == 1) { for (int i = 0; i < out.length; i++) out[i] <<= w; return out; }
        final long mx = (1L << (s.bits - 1)) - 1, mn = -(1L << (s.bits - 1)); for (int i = 0; i < out.length; i++) out[i] = Math.max(mn, Math.min(mx, out[i] << w)); return out; }

    // port of laes_tool.cpp:main ("dec" / "verify")
    static int decVerify(String[] args) {
        final boolean isDec = args[0].equals("dec"); final String inPath = args[1];
        if (isDec && args[2].startsWith("-") && !args[2].startsWith("--list")) {   // as laes_tool.cpp 5af9e2b: an option is not taken as the output folder
            System.out.println("give the output folder: laes dec <in> <out dir> [--levels] [--pick=...] (or laes dec <in> --list)"); return 1; }
        if (isDec && (LaesEncoder.Options.flag(args, "--list") || LaesEncoder.Options.arg(args, "--list").equals("all"))) {   // 2026-11-15, as laes_tool.cpp: the list reads the records only and decodes nothing (the checks belong to decoding and playback)
            final Stream.In lin = new Stream.In(inPath); final Play.File lf = new Play.File(); final String[] lm = {""}; String le;
            try { le = lf.open(new Play.ByteSrc() { public long size() { return lin.n; } public void read(long at, byte[] d, int off, int n) { lin.read(at, d, off, n); } }, lm, true); }
            catch (RuntimeException x) { le = String.valueOf(x.getMessage()); } finally { lin.close(); }
            if (!le.isEmpty() && lm[0].equals("LAE1")) { System.out.println(inPath + ": this is an LAEY file (lossy, extension LAE1): decode it with lae_dec; laes reads LAES (extension LAE2)"); return 2; }
            if (!le.isEmpty()) { System.out.println(inPath + ": " + le); return 2; } if (!lf.why.isEmpty()) { System.out.println(inPath + ": " + lf.why + " (the core master is still playable)"); return 2; }
            final String fnote = Flac.foreignNote(lf.foreignFront, lf.foreignBehind); if (!fnote.isEmpty()) System.out.println("note: " + fnote);
            final List<LaesFile.MasterInfo> infos = new ArrayList<LaesFile.MasterInfo>(); for (Play.Master M : lf.ms) infos.add(M.I);
            final List<Choices.Spec> specs = Choices.build(lf.baseRate, lf.coreBits, lf.coreFrames, infos, null, lf.head); final int defJ = Choices.defaultIndex(specs, lf.head); final boolean listAll = LaesEncoder.Options.arg(args, "--list").equals("all");
            System.out.println("choices (use --pick=<n>[,<n>...], --pick=<rate>:<bits> (masters only), --pick=master:<k> or --pick=name:<text>; listed from the records, nothing decoded or checked here):");
            int hidden = 0;
            for (int j = 0; j < specs.size(); j++) { final Choices.Spec s = specs.get(j); if (!listAll && s.kind >= 2) { hidden++; continue; } String extra = ""; if (s.kind <= 1) { final long sec = s.rate != 0 ? s.frames / s.rate : 0; extra = String.format("  %d:%02d", sec / 60, sec % 60); if (j == defJ) extra += "  [default]"; }
                System.out.println(String.format("  %3d  %6d Hz %2d-bit  %s%s%s", j + 1, s.rate, s.bits, s.kind >= 2 ? "  - " : "", s.what, extra)); }
            if (hidden != 0) System.out.println("(" + hidden + " rate levels and derived versions not shown: --list=all)");
            return 0; }
        final String wd = LaesEncoder.workDir(inPath + (isDec ? ".dec" : ".verify"));
        try { return decVerifyIn(args, isDec, inPath, wd); } finally { LaesEncoder.unlockWork(wd); if (System.getenv("KEEPTMP") == null) LaesEncoder.removeAll(new File(wd)); } }   // unlocked first
    private static int decVerifyIn(String[] args, boolean isDec, String inPath, String wd) {
        final String bodyPath = wd + "/body.bin"; final LaesFile.Pcm core = new LaesFile.Pcm(); long bodySize = 0;
        { final Stream.In in = new Stream.In(inPath);
          try { final byte[] mg = new byte[4]; final long s0 = Flac.flacStart(StreamEnc.rdOf(in), in.n);   // ID3v2 tags in front
            if (in.n >= s0 + 4) in.read(s0, mg, 0, 4); if (s0 != 0 && !StreamEnc.eq4(mg, 0, "fLaC")) { System.out.println(inPath + ": no FLAC stream after the ID3v2 tag"); return 2; }
            if (StreamEnc.eq4(mg, 0, "fLaC")) {   // metadata: STREAMINFO kept, the "LAE2" APPLICATION blocks into the payload file; the audio through the FLAC reader
                { final long tl = Flac.trailerLen(StreamEnc.rdOf(in), s0 + 4, in.n); final String fn = Flac.foreignNote(s0, tl); if (!fn.isEmpty()) System.out.println("note: " + fn); }
                long p = s0 + 4; boolean last = false; java.io.ByteArrayOutputStream meta = new java.io.ByteArrayOutputStream(); meta.write('f'); meta.write('L'); meta.write('a'); meta.write('C'); final Stream.Out bo = new Stream.Out(bodyPath);
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
        final List<Stream.DecMaster> dec = new ArrayList<Stream.DecMaster>(); final LaesFile.Head head = new LaesFile.Head(); final String de = Stream.decodeLaesS(bodyPath, core, core.rate, wd, dec, piece, threads, head);
        if (!de.isEmpty()) { System.out.println(inPath + ": " + de + " (the core master is still playable)"); return 2; }
        boolean all = true; final List<LaesFile.MasterInfo> mi0 = new ArrayList<LaesFile.MasterInfo>(); for (Stream.DecMaster D : dec) mi0.add(D.I); {   // every master line below gives the menu number (Choices.displayNo)
          final int[] mo = Choices.menuOrder(mi0);   // in the menu order, numbered as in menus
          for (int p = 0; p < mo.length; p++) { final int i = mo[p]; final LaesFile.MasterInfo I = dec.get(i).I;
              System.out.println("master " + (p + 1) + (I.name.length() == 0 ? "" : " \"" + I.name + "\"") + ": " + I.rate + " Hz " + I.bits + "-bit, " + I.frames + " frames, SHA-256 " + (dec.get(i).shaOk ? "ok" : "MISMATCH")
                                 + (mo[p] != p ? " (master " + (i + 1) + " in the file)" : "")); all &= dec.get(i).shaOk; }
          if (head.def != 0) { final LaesFile.MasterInfo D = dec.get(head.def - 1).I; System.out.println("default: master " + Choices.displayNo(mi0, head.def - 1) + (D.name.length() == 0 ? "" : " \"" + D.name + "\"") + " (set when encoded)"); }
          else System.out.println("default: the core master");
          if ((head.coreFlags & LaesFile.NO_DERIVE) != 0) System.out.println("the core master: no derived 16-bit version"); }
        if (isDec) {
            // every choice the file offers (Choices = laes_choices.h, SPEC 10; the same list as the players): the core master, then per master its
            // own group - the master, its rate levels (lossless), its derived 16-bit versions (unless the file turns them off for it)
            final List<LaesFile.MasterInfo> infos = new ArrayList<LaesFile.MasterInfo>(); final boolean[] offered = new boolean[dec.size()];
            for (int i = 0; i < dec.size(); i++) { infos.add(dec.get(i).I); offered[i] = dec.get(i).shaOk;
                if (!dec.get(i).shaOk) System.out.println("master " + Choices.displayNo(mi0, i) + ": SHA-256 mismatch: not offered (nor its rate levels or derived versions)"); }
            final List<Choice> ch = new ArrayList<Choice>();
            final List<Choices.Spec> specs = Choices.build(core.rate, core.bits, core.frames(), infos, offered, head); final int defJ = Choices.defaultIndex(specs, head);
            for (Choices.Spec sp : specs) { Src s = new Src(); s.kind = sp.kind == 3 ? 0 : sp.kind; s.rate = sp.rate; s.bits = sp.bits; s.n = sp.frames; if (sp.m >= 0) s.m = sp.m; s.k = sp.level;
                final Choice c = new Choice(sp.name, sp.what, s); c.kind = sp.kind; c.no = sp.no;
                if (sp.kind == 3) { c.derived = true; c.from = sp.from; final Src f = ch.get(sp.from).s; final Src d = new Src(); d.kind = f.kind; d.m = f.m; d.k = f.k; d.rate = f.rate; d.bits = 16; d.n = f.n; c.s = d; } ch.add(c); }
            final List<Integer> pick = new ArrayList<Integer>(); final String pk = LaesEncoder.Options.arg(args, "--pick");
            if (pk.isEmpty()) { pick.add(0); for (int j = 1; j < ch.size(); j++) if (ch.get(j).kind == 1 || (ch.get(j).kind == 2 && LaesEncoder.Options.flag(args, "--levels"))) pick.add(j); }
            else { int a = 0; for (;;) { final int b = pk.indexOf(',', a); final String t = pk.substring(a, b < 0 ? pk.length() : b); final int col = t.indexOf(':');
                    // <n> or <a>-<b>: entries of --list. <rate>:<bits>: the masters (never a rate level or a derived version) with that rate and
                    // word length. master:<k>: master k. name:<text>: the masters with exactly this name.
                    final List<Integer> hit = new ArrayList<Integer>();
                    if (col < 0) { final List<Long> v = parseRanges(t); if (v != null) { boolean okR = true; for (long j : v) okR = okR && j >= 1 && j <= ch.size(); if (okR) for (long j : v) hit.add((int) j - 1); } }
                    else if (t.startsWith("master:")) { final long k = CStd.strtolFull(t.substring(7)); for (int j = 0; j < ch.size(); j++) if (ch.get(j).kind == 1 && ch.get(j).no == k) hit.add(j); }
                    else if (t.startsWith("name:")) { final String nm = t.substring(5); for (int j = 0; j < ch.size(); j++) if (ch.get(j).kind == 1 && nm.length() != 0 && infos.get(ch.get(j).s.m).name.equals(nm)) hit.add(j); }
                    else { final int r = CStd.atoi(t), bb = CStd.atoi(t.substring(col + 1)); for (int j = 0; j < ch.size(); j++) if (ch.get(j).kind == 1 && ch.get(j).s.rate == r && ch.get(j).s.bits == bb) hit.add(j); }
                    if (hit.isEmpty()) { System.out.println("--pick: '" + t + "' is not one of the choices (see --list)"); return 2; }
                    // several masters fit: the user chooses. The C++ tool shows a tick list in a console; this port has none and
                    // lists the candidates as the C++ does outside a console, nothing written
                    if (hit.size() > 1 && col >= 0) { System.out.println("--pick: '" + t + "' fits " + hit.size() + " masters. Tick the ones to write:");
                        for (int j : hit) System.out.println(String.format("  %4d  %6d Hz %2d-bit  %s", j + 1, ch.get(j).s.rate, ch.get(j).s.bits, Choices.masterLabel(ch.get(j).no, infos.get(ch.get(j).s.m))));
                        System.out.println("Not in a console: give the numbers with --pick=<n>, several --pick=<n>,<n>, a range --pick=<a>-<b>. Nothing written."); return 1; }
                    for (int j : hit) if (!pick.contains(j)) pick.add(j); if (b < 0) break; a = b + 1; } }
            // as laes_tool.cpp: an existing WAV only after two "yes" at a console, or with --overwrite; asked before anything is written
            for (int j : pick) if (!com.lae.core.Replace.mayReplace(new File(args[2], ch.get(j).name + ".wav").getPath(), LaesEncoder.Options.flag(args, "--overwrite"), OUT)) return 1;
            {   // 2026-11-14, as laes_tool.cpp: the WAVs' size against the free space, before anything is written (as enc does for its work folder)
                long need = 0; for (int j : pick) { final long db = ch.get(j).s.n * 2 * (ch.get(j).s.bits / 8); need += 128 + db + (db & 1); }
                File fsAt = new File(args[2]).getAbsoluteFile(); while (fsAt != null && !fsAt.exists()) fsAt = fsAt.getParentFile();   // the folder may not exist yet
                final long avail = fsAt != null ? fsAt.getUsableSpace() : 0;   // 0: not known (as the C++ error code): no check
                if (avail > 0 && need > avail) { final boolean force = LaesEncoder.Options.flag(args, "--force-disk");
                    System.out.println(String.format(java.util.Locale.ROOT, "the WAVs need about %.1f GB, %.1f GB free on that disk%s", need / 1073741824.0, avail / 1073741824.0, force ? "; --force-disk: running anyway" : "; nothing written (--force-disk runs anyway)"));
                    System.out.println(String.format(java.util.Locale.ROOT, "  输出大约需要 %.1f GB，这块磁盘剩 %.1f GB%s", need / 1073741824.0, avail / 1073741824.0, force ? "；--force-disk：照样开始" : "；没有开始，也没有写任何文件（加 --force-disk 可以照样开始）"));
                    if (!force) return 2; } }
            boolean wroteAll = true;
            new File(args[2]).mkdirs(); final String d = args[2] + "/";
            for (int j : pick) { final Choice C = ch.get(j); final Src S = C.derived ? ch.get(C.from).s : C.s; final int by = C.s.bits / 8; final long n = C.s.n, db = n * 2 * by;
                byte[] h = LaesEncoder.wavHeader(C.s.rate, C.s.bits); final long fmtLen = C.s.bits > 16 ? 40 : 16;
                com.lae.core.Le.put32(h, 4, (int) (4 + 8 + fmtLen + 8 + db + (db & 1))); com.lae.core.Le.put32(h, h.length - 4, (int) db);
                // beyond 4 GB RF64 as the encoder writes it (ds64 with the 64-bit RIFF and data sizes, no table; 32-bit fields
                // 0xFFFFFFFF), as laes_tool.cpp dec (2026-11-04); the header lengths wrapped before. Test hook LAES_FORCE_RF64 as the C++
                final long riff = 4 + 8 + fmtLen + 8 + db + (db & 1);
                if (riff > 0xFFFFFFFFL || LaesFile.testEnv("LAES_FORCE_RF64") != null) { final byte[] r = new byte[20 + 28 + h.length - 12];
                    final byte[] r0 = {'R', 'F', '6', '4', (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 28, 0, 0, 0}; System.arraycopy(r0, 0, r, 0, 20);
                    com.lae.core.Le.put64(r, 20, riff + 36); com.lae.core.Le.put64(r, 28, db); com.lae.core.Le.put64(r, 36, n);   // then the table length 0
                    System.arraycopy(h, 12, r, 48, h.length - 12); for (int i = r.length - 4; i < r.length; i++) r[i] = (byte) 0xFF; h = r; }   // the data size: in ds64
                boolean okw = true; Stream.Out o = null; String why = "";
                try { o = new Stream.Out(d + C.name + ".wav"); o.put(h, 0, h.length); final long P = 1 << 18;
                    final long[] stc = {20261029L + 0x9E3779B97F4A7C15L * 1, 20261029L + 0x9E3779B97F4A7C15L * 2}; final int sft = C.derived ? S.bits - 16 : 0;   // derive16: one stream per channel
                    for (long a = 0; a < n; a += P) { final long e = Math.min(n, a + P); final long[] x0 = readSrc(core, dec, S, 0, a, e), x1 = readSrc(core, dec, S, 1, a, e); final byte[] b = new byte[(int) (e - a) * 2 * by]; int f = 0;
                        for (long i = a; i < e; i++) for (int c = 0; c < 2; c++) { long v = (c != 0 ? x1 : x0)[(int) (i - a)];
                            if (C.derived) { long st = stc[c]; st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r1 = st >>> (64 - sft); st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r2 = st >>> (64 - sft); stc[c] = st;
                                v = Math.max(-32768L, Math.min(32767L, (v + r1 - r2 + (1L << (sft - 1))) >> sft)); }
                            final int u = (int) v; for (int k = 0; k < by; k++) b[f++] = (byte) (u >>> (8 * k)); }
                        o.put(b, 0, f); }
                    if ((db & 1) != 0) o.put(new byte[1], 0, 1); o.close(); o = null; }
                catch (LaeException x) { okw = false; why = String.valueOf(x.getMessage()); }
                finally { StreamEnc.closeQuiet(o); }
                if (!okw) { new File(d + C.name + ".wav").delete(); System.out.println(C.name + ".wav: FAILED, the file was removed: " + why); }   // said, and the part written is removed (the file is closed)
                all &= okw; wroteAll &= okw; if (okw) System.out.println(C.name + ".wav: " + C.s.rate + " Hz " + C.s.bits + "-bit, " + C.what); }
            if (wroteAll) System.out.println("written to " + args[2]); else System.out.println("not every file was written: see above (" + args[2] + ")"); }
        else { final boolean[] used = new boolean[dec.size()];   // each given file must equal one decoded master (the file order may differ from the command line)
            for (int i = 2; i < args.length; i++) { if (args[i].length() > 0 && args[i].charAt(0) == '-') continue; final StreamEnc.WavInfo wi = new StreamEnc.WavInfo(); final String re = StreamEnc.probeWav(args[i], wi, null); int hit = -1;
                for (int k = 0; k < dec.size() && hit < 0 && re.isEmpty(); k++) { if (used[k] || wi.rate != dec.get(k).I.rate || wi.bits != dec.get(k).I.bits || wi.frames != dec.get(k).I.frames) continue;
                    Src s = new Src(); s.kind = 1; s.m = k; final Stream.In in = new Stream.In(args[i]); boolean same = true; final long P = 1 << 20; int[] r = new int[0]; byte[] buf = null;
                    try { for (long a = 0; a < wi.frames && same; a += P) { final long e = Math.min(wi.frames, a + P); if (r.length < (e - a) * 2) r = new int[(int) (e - a) * 2]; buf = StreamEnc.wavFrames(in, wi, a, e, r, buf); final long[] x0 = readSrc(core, dec, s, 0, a, e), x1 = readSrc(core, dec, s, 1, a, e);
                            for (long q = 0; q < e - a && same; q++) same = r[(int) q * 2] == x0[(int) q] && r[(int) q * 2 + 1] == x1[(int) q]; } }
                    finally { in.close(); }
                    if (same) hit = k; }
                // a file the WAV reader refused is named with the reason (it was only "DIFFERENT (no decoded master equals it)")
                if (hit >= 0) { used[hit] = true; System.out.println("master " + Choices.displayNo(mi0, hit) + " vs " + args[i] + ": IDENTICAL"); }
                else if (!re.isEmpty()) { System.out.println(args[i] + ": DIFFERENT (not compared: " + (re.startsWith(args[i] + ": ") ? re.substring(args[i].length() + 2) : re) + ")"); all = false; }
                else { System.out.println(args[i] + ": DIFFERENT (no decoded master equals it)"); all = false; } }
            for (int k = 0; k < dec.size(); k++) if (!used[k] && args.length > 2) System.out.println("master " + Choices.displayNo(mi0, k) + ": not compared"); }
        return all ? 0 : 2; }
}
