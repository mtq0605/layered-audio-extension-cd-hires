// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.AlignHash;
import com.lae.core.Cv;
import com.lae.core.LMath;
import com.lae.core.LaeException;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Port of src/lae_enc.cpp (encode(), the run state encrun, call(), env()): the LAE standalone encoder (lossy tier), part 1 of the port:
 * everything except the independent CD (cd=, cdcheck, cdalign=, cdaccept, cdedge=: part 2, {@link CdHook}) and the planner (lae_enc plan:
 * excluded). Every module runs as in the C++ (its own main with arguments and module variables), so the Java runs the same steps in the
 * same order and writes the same files in its work folder.
 *   input : PCM WAV master, stereo, 16/24/32-bit integer, rate = 44.1k or 48k family x 2, 4 or 8
 *   output: WAV whose audio is the CD layer (16 bit at 44.1/48 kHz, plays everywhere) + a "LAE " chunk with the depth layer (L, R) and the
 *           frequency layer (M, S), each range-coded with partition levels
 * The encoder decodes every layer again and refuses to write the file unless the decoded coefficient planes are identical.
 * Fail-safe: every file operation is checked, every module result is checked, the output is first written as &lt;out&gt;.part, read back and
 * verified, decoded end to end, and only then renamed to &lt;out&gt;. On any failure, including an internal error, an out-of-memory condition
 * in a worker thread or a cancellation, nothing is written and the work folder is removed.
 * Differences to the C++ (reported): no console questions (bits=24, cdaccept, cdedge: the run stops with the C++ text for scripts, the app
 * asks and passes the option); the work folder may be given (Android: the app's cache dir; default &lt;out&gt;.tmp as the C++); the physical
 * memory of the memory plan is the Java heap limit (Runtime.maxMemory); the resume key names this port instead of the C++ build date.
 */
public final class LaeEncoder {
    private LaeEncoder() {}

    /** a module's main (C++: int (*fn)(int, char**)) */
    interface Module { int main(EncEnv env, String[] argv); }

    /**
     * Part 2 of the port plugs the independent-CD path in here: lae_enc.cpp lines 308-492 (the cd= block between the CD layer derivation
     * and the route split). Arguments are the run's state; returns -1 to continue encoding, or the rc encode() returns (0 for cdcheck: nothing encoded).
     */
    public interface CdHook { int run(Ctx x); }
    /** the hook installed by part 2 (null: cd= is refused) */
    public static volatile CdHook cdHook = new LaeCd();

    /** port of lae_enc.cpp:encrun: run state for the cleanup (what this run created and must remove) */
    static final class Run { String out, part, tmp; boolean hadOld = false, partMine = false, tmpMine = false, cleaned = false, finished = false, keepTmp = false; }

    /** the state of one encode() (the C++ locals the lambdas capture); public for part 2 */
    public static final class Ctx {
        public EncEnv env; Run run = new Run(); public String[] argv; public String outPath, partPath, tmp;
        public int P = 4096; public long Lm; public int rate, bits, base, stages, mi; public String cdPath; public boolean cdCheck, cdAccept, manual; public int cdEdge = -1;
        public String manualText; public int manualSg = 1; public long manualI, manualF, manualFs = 1, manualPlace, manualPhi, manualT; public double manualMove;
        public String alignNote = "", alignReport = ""; WavIn.Info wi = new WavIn.Info();
        // --tag / --cover / --picture (2026-11-06, a85dd40): the LIST/INFO + "id3 " chunks written after "fmt "; presetTags = tags the app gives directly
        public boolean asFlac = false;   // the FLAC form (2026-11-10): tagChunks then holds the VORBIS_COMMENT / PICTURE blocks
        public byte[] tagChunks = new byte[0]; public com.lae.laes.LaesTags.Tags presetTags;
        // noise=<marks.laenoise>, g (2026-11-04): the marking file, the master length it was checked against, the whole offset the master was placed at
        public String noisePath; public boolean gOpt; public char gLevel = 'd'; NoiseMarks.Marks noiseMk = new NoiseMarks.Marks(); long LmIn; public long placeT = 0;
        boolean resume, partLeft = false; Set<String> doneSteps = new HashSet<String>(); String manifest;
        // extension packs (2026-11-10; port of lae_enc.cpp: PackOpt pk[8], nPacks, alignOut, rawRate, cdAddLead / cdAddTail, packRecs, packReport)
        final PackOpt[] pk = new PackOpt[8]; int nPacks = 0; String alignOut; int rawRate = 0; public long cdAddLead = 0, cdAddTail = 0;
        final List<byte[]> packRecs = new ArrayList<byte[]>(); String packReport = ""; String mainName = "";   // mainName: master1name=<text>, the main body's name ("" = none stored)
        // port of lae_enc.cpp:encode lambda T
        public String T(String n) { return new File(tmp, n).getPath(); }
        // port of lae_enc.cpp:encrun::oldNote (the lambda oldNote of encode)
        public void oldNote() { if (run.hadOld) env.printf("note: the existing %s was NOT written by this run\n", run.out); }
        // port of lae_enc.cpp:encode lambda isDone
        synchronized boolean isDone(String s) { return doneSteps.contains(s); }
        // port of lae_enc.cpp:encode lambda markDone
        synchronized void markDone(String s) { if (!resume) return; doneSteps.add(s);
            boolean okw = false; FileOutputStream f = null; try { f = new FileOutputStream(manifest, true); f.write(EncIo.latin1("done " + s + "\n")); okw = true; } catch (IOException e) { okw = false; } finally { if (f != null) try { f.close(); } catch (IOException e) { okw = false; } }
            if (!okw) throw EncIo.fail("cannot write " + manifest); }
        // port of lae_enc.cpp:encode lambda derive: padded master and the derived CD layer (16 bit), 24-bit base and dither flags, all from m0.raw
        public boolean derive() {
            if (call(env, new Module() { @Override public int main(EncEnv e, String[] a) { return PadBlk.main(e, a); } }, "pad", T("m0.raw"), T("m.raw"), Integer.toString(P)) != 0) { env.printf("padding failed\n"); oldNote(); return false; }
            env.set("DITHERFLAGS", T("dither.flags")); env.set("BASE24", T("b24.s32"));
            if (call(env, new Module() { @Override public int main(EncEnv e, String[] a) { return BaseBlk.main(e, a); } }, "base", T("m.raw"), Integer.toString(rate), Integer.toString(base), T("b16.s16")) != 0) { env.printf("CD layer derivation failed\n"); oldNote(); return false; } env.set("BASE24", "");
            return true; }
        // port of lae_enc.cpp:encode lambda lengthOk
        // length limit: the v5 header (decoder check: Lm <= 2,000,000,000 - 2P); every module indexes the master with 64-bit positions
        public boolean lengthOk(String what) {
            if (Lm < (long) rate) { env.printf("%s is shorter than 1 second: not supported\n", what); oldNote(); return false; }
            long maxFrames = 2000000000L - 2L * P; String who = "the v5 format"; String hint = "";
            String t = env.get("LAE_TESTMAXFRAMES"); if (t != null) { long v = Fmt.atoll(t); if (v > 0) maxFrames = Math.min(maxFrames, v); }   // test hook: can only lower the limit
            if (Lm > maxFrames) { env.printf("%s is %.2f minutes long; at %d Hz %s allows at most %.2f minutes%s: nothing encoded\n", what, Lm / (60.0 * rate), rate, who, maxFrames / (60.0 * rate), hint); oldNote(); return false; }
            return true; }
        // port of lae_enc.cpp:encode lambda fitsLength
        public boolean fitsLength(long n) { long mf = 2000000000L - 2L * P; String e = env.get("LAE_TESTMAXFRAMES"); if (e != null) { long v = Fmt.atoll(e); if (v > 0) mf = Math.min(mf, v); } return n >= (long) rate && n <= mf; }
        // port of lae_enc.cpp:encode lambda spaceOk
        // free space for the intermediates (about 65-85 bytes per padded master frame, measured; about 20 for cdcheck) + the output; reuse = work files about to be overwritten
        public boolean spaceOk(long reuse) {
            long avail = new File(tmp).getUsableSpace(); boolean ec = avail <= 0 && !new File(tmp).exists(); final double perFrame = cdCheck ? 45.0 : (cdPath != null ? 121.0 : 105.0); if (!ec) avail += reuse;
            long needB = Cv.d2l((Lm + 2 * P) * perFrame) + (64L << 20), hardB = Cv.d2l((Lm + 2 * P) * perFrame * 0.7);
            if ((!ec && avail < hardB) || EncIo.testFail(env, "diskspace")) { env.printf("not enough free disk space next to %s: about %.0f MB needed, %.0f MB free: nothing encoded\n", outPath, needB / 1e6, ec ? 0.0 : avail / 1e6); oldNote(); return false; }
            if (!ec && avail < needB) env.printf("WARNING: free disk space is tight (%.0f MB free, about %.0f MB needed); every write is checked, so a full disk stops the run cleanly\n", avail / 1e6, needB / 1e6);
            return true; }
    }

    // port of lae_enc.cpp:call
    // runs one module; a checked-I/O failure, an exhausted memory or any other exception inside it becomes a non-zero result with the reason printed
    static int call(EncEnv env, Module fn, String... a) {
        try { return fn.main(env, a); }   // the messages go to the layer's own text when the module runs in a layer thread
        catch (EncEnv.Cancelled c) { throw c; }   // not a module failure: the run stops (C++: the signal handler)
        catch (LaeException e) { env.printf("ENCODER STOPPED in module %s: %s\n", a[0], e.getMessage()); return 90; }
        catch (OutOfMemoryError e) { env.printf("ENCODER STOPPED in module %s: out of memory\n", a[0]); return 91; }
        catch (RuntimeException e) { env.printf("ENCODER STOPPED in module %s: %s\n", a[0], String.valueOf(e.getMessage())); return 92; }
        catch (Throwable e) { env.printf("ENCODER STOPPED in module %s: internal error\n", a[0]); return 93; } }

    // port of lae_enc.cpp:encrun::cleanup: the .part only while it is unfinished; the work folder always (it holds only this run's files)
    static void cleanup(Ctx x) { Run r = x.run; if (r.cleaned) return; r.cleaned = true;
        if (r.partMine) new File(r.part).delete();
        if (r.tmpMine && x.env.get("KEEPTMP") == null && !r.keepTmp) removeAll(new File(r.tmp)); }
    // std::filesystem::remove_all of the work folder (created and marked by this run, see the marker check in encode())
    static void removeAll(File d) { File[] l = d.listFiles(); if (l != null) for (File f : l) { if (f.isDirectory()) removeAll(f); else f.delete(); } d.delete(); }
    // port of lae_enc.cpp:encrun::emergency
    static void emergency(Ctx x, String why) { if (x.run.finished) return; x.run.finished = true; x.env.printf("ENCODER STOPPED: %s; nothing written\n", why); x.oldNote(); cleanup(x); }

    // port of lae_enc.cpp:main (the encoder only; "plan" is not part of this port)
    public static int main(EncEnv env, String[] argv, String workDir) { return main(env, argv, workDir, null); }
    // tags: tags and pictures the app gives as objects (added after those of --tag / --cover / --picture in argv; null = none)
    public static int main(EncEnv env, String[] argv, String workDir, com.lae.laes.LaesTags.Tags tags) {
        Ctx x = new Ctx(); x.env = env; x.presetTags = tags; int rc;
        try { if (argv.length > 1 && argv[1].equals("plan")) { env.printf("lae_enc plan (the independent-CD planner) is not part of the Java port\n"); return 1; }
              rc = argv.length > 1 && argv[1].equals("tag") ? LaeRetag.retag(env, argv, tags) : encode(x, argv, workDir); }
        catch (EncEnv.Cancelled c) { emergency(x, "interrupted"); return 130; }
        catch (LaeException e) { emergency(x, e.getMessage()); return 2; }
        catch (OutOfMemoryError e) { emergency(x, "out of memory"); return 2; }
        catch (RuntimeException e) { emergency(x, String.valueOf(e.getMessage())); return 2; }
        catch (Error e) { emergency(x, "internal error"); return 2; }
        x.run.finished = true; return rc; }

    static boolean exists(String p) { return new File(p).exists(); }
    static long fileSize(String p) { File f = new File(p); return f.isFile() ? f.length() : -1; }
    static byte[] slurp(String p) { return EncIo.readAll(p); }   // a missing or unreadable file stops the encoder

    /** port of lae_enc.cpp:PackOpt: one further master (master2= .. master9=) */
    static final class PackOpt { String path = "", align = "", name = "", edge = ""; boolean accept = false, any = false; }
    // port of lae_enc.cpp:packOptArg
    static boolean packOptArg(String a) { if (!a.startsWith("master") || a.length() < 8 || a.charAt(6) < '2' || a.charAt(6) > '9') return false;
        final String r = a.substring(7); return r.startsWith("=") || r.startsWith("align=") || r.startsWith("name=") || r.startsWith("edge=") || r.equals("accept"); }
    // port of lae_enc.cpp:packOptParse (pk[0] = master2 .. pk[7] = master9; "" or the reason)
    static String packOptParse(String a, PackOpt[] pk) {
        final int k = a.charAt(6) - '2'; if (pk[k] == null) pk[k] = new PackOpt(); final PackOpt q = pk[k]; final String r = a.substring(7), key = a.substring(0, 7); q.any = true;
        if (r.startsWith("=")) { if (r.length() == 1) return key + "=: give the master's file"; q.path = r.substring(1); return ""; }
        if (r.startsWith("align=")) { if (r.length() == 6) return key + "align=: give the offset, as for cdalign="; q.align = r.substring(6); return ""; }
        if (r.startsWith("name=")) { if (utf8(r.substring(5)).length > 255) return key + "name=: a name has at most 255 bytes"; q.name = r.substring(5); return ""; }
        if (r.startsWith("edge=")) { final String v = r.substring(5); if (!v.equals("cut") && !v.equals("pad")) return key + "edge=: pad or cut, as for cdedge="; q.edge = v; return ""; }
        q.accept = true; return ""; }
    static byte[] utf8(String s) { try { return s.getBytes("UTF-8"); } catch (java.io.UnsupportedEncodingException e) { return new byte[0]; } }
    // std::filesystem::copy_file (overwrite): the aligned master of rawin=
    static void copyFile(String from, String to) {
        try { java.io.FileInputStream in = new java.io.FileInputStream(from); try { FileOutputStream out = new FileOutputStream(to); try { byte[] b = new byte[1 << 20]; for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n); } finally { out.close(); } } finally { in.close(); } }
        catch (IOException e) { throw EncIo.fail("cannot copy " + from + ": " + e.getMessage()); } }
    // port of lae_enc.cpp:encode lambda padRawFile: a zero frames + the file + b zero frames
    static void padRawFile(EncEnv env, String f, long a, long b) {
        final String nf = f + ".new"; { final long n = fileSize(f) / 8; BlkIo.View m = new BlkIo.View(f); BlkIo.Writer wr = new BlkIo.Writer(env, nf); try { int[] z = new int[1 << 19]; final long BS = 1L << 18;
              for (long k = 0; k < a; k += BS) wr.put(z, 0, (int) (Math.min(BS, a - k) * 2));
              for (long i = 0; i < n; i += BS) { final long e = Math.min(n, i + BS); int[] xx = new int[(int) (e - i) * 2]; for (long j = i; j < e; j++) { xx[(int) (j - i) * 2] = m.get(j * 2); xx[(int) (j - i) * 2 + 1] = m.get(j * 2 + 1); } wr.put(xx); }
              for (long k = 0; k < b; k += BS) wr.put(z, 0, (int) (Math.min(BS, b - k) * 2));
              wr.finish(); wr = null; } finally { if (wr != null) wr.close(); m.close(); } }
        if (!rename(nf, f)) throw EncIo.fail("cannot replace " + f + ": rename failed"); }
    // the CD layer of the work folder's b16.s16 without its padding (C++: the vector core)
    static short[] coreOf(String b16, long pbA) { byte[] b = EncIo.readAll(b16); final int n = b.length / 2 - (int) (4 * pbA); short[] c = new short[n]; EncIo.toShorts(b, (int) (4 * pbA), c, 0, n); return c; }

    // port of lae_enc.cpp:encode
    static int encode(final Ctx x, final String[] argv, String workDir) {
        final EncEnv env = x.env; final int argc = argv.length; x.argv = argv;
        if (argc < 3) { env.printf("usage: lae_enc <master.wav> <out.wav> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2, default s1] [zs] [cd=<independent CD.wav>] [energy] [equal] [cdcheck] [cdalign=<offset>] [cdaccept] [cdedge=pad | cdedge=cut] [bits=24] [mem=<GB>]\n"); return 1; }
        final long T0 = System.nanoTime();
        final String outPath = argv[2], partPath = outPath + ".part", tmp = workDir != null ? workDir : outPath + ".tmp";
        x.outPath = outPath; x.partPath = partPath; x.tmp = tmp; x.run.out = outPath; x.run.part = partPath; x.run.tmp = tmp; x.run.hadOld = exists(outPath);
        if (!MdctTb.selftestOK() || !AlignHash.md5SelfTest() || EncIo.testFail(env, "selftest")) { env.printf("encoder self-test FAILED: this build does not compute the IMDCT in the IEEE-754 order the format requires, or its MD5 is wrong (see docs/BUILD.md); nothing encoded\n"); return 4; }
        final String modeName = argc > 3 ? argv[3] : "3"; final int mi = PartitionFormat.modeIndex(modeName); if (mi < 0) { env.printf("mode must be max, 1, 3, 5, keep or keep20\n"); return 1; }
        x.mi = mi;
        boolean equal = false, zsOpt = false, bits24 = false; int wm = 0, scheme = 1; long memBudget = 0;   // mem=<GB>: memory budget for coding two layers at once
        final int formSel = LaeRetag.outputForm(env, outPath, argv, 4); if (formSel < 0) { env.printf("nothing encoded\n"); return 1; } x.asFlac = formSel == 1;
        { final com.lae.laes.LaesTags.Tags tags = new com.lae.laes.LaesTags.Tags(); final String e = LaeRetag.tagArgs(argv, 4, tags); if (!e.isEmpty()) { env.printf("%s: nothing encoded\n", e); return 1; }   // read and checked before any work
          if (x.presetTags != null) { tags.kv.addAll(x.presetTags.kv); tags.pics.addAll(x.presetTags.pics); final String e2 = com.lae.laes.LaesTags.checkAll(tags); if (!e2.isEmpty()) { env.printf("%s: nothing encoded\n", e2); return 1; } }
          x.tagChunks = x.asFlac ? com.lae.laes.LaesTags.flacBlocks(tags, false, LaeRetag.kLaeyEncoder) : com.lae.laes.LaesTags.wavChunks(tags, LaeRetag.kLaeyEncoder); }   // FLAC form: VORBIS_COMMENT + PICTURE blocks, before the "LAE1" blocks
        for (int i = 4; i < argc; i++) { final String a = argv[i];
            if (LaeRetag.isTagOpt(a) || LaeRetag.isFormOpt(a)) continue;
            if (a.startsWith("master1name=")) { if (utf8(a.substring(12)).length > 255) { env.printf("master1name=: a name has at most 255 bytes\n"); return 1; } x.mainName = a.substring(12); continue; }   // port of lae_enc.cpp: the main body's name
            if (a.equals("equal")) equal = true; else if (a.equals("energy")) wm = 1; else if (a.equals("s2")) scheme = 2; else if (a.equals("s1")) scheme = 1;
            else if (a.equals("zs")) zsOpt = true; else if (a.startsWith("cd=") && a.length() > 3) x.cdPath = a.substring(3); else if (a.equals("cdcheck")) x.cdCheck = true;
            else if (a.equals("cdaccept")) x.cdAccept = true; else if (a.equals("cdedge=pad")) x.cdEdge = EDGE_PAD; else if (a.equals("cdedge=cut")) x.cdEdge = EDGE_CUT; else if (a.equals("bits=24")) bits24 = true; else if (a.equals("resume")) x.resume = true;
            else if (a.startsWith("noise=") && a.length() > 6) x.noisePath = a.substring(6); else if (a.equals("g") || a.equals("g=d")) { x.gOpt = true; x.gLevel = 'd'; } else if (a.equals("g=c")) { x.gOpt = true; x.gLevel = 'c'; } else if (a.equals("g=b")) { x.gOpt = true; x.gLevel = 'b'; }
            else if (a.startsWith("alignout=") && a.length() > 9) x.alignOut = a.substring(9); else if (a.startsWith("rawin=") && Fmt.atoi(a.substring(6)) > 0) x.rawRate = Fmt.atoi(a.substring(6));
            else if (packOptArg(a)) { final String e = packOptParse(a, x.pk); if (!e.isEmpty()) { env.printf("%s: nothing encoded\n", e); return 1; } }   // extension packs (2026-11-10)
            else if (a.startsWith("mem=")) { final double g = Fmt.atof(a.substring(4)); if (!(g > 0) || g > 1e6) { env.printf("mem=%s: give the memory budget in GB (for example mem=8)\n", a.substring(4)); return 1; } memBudget = Cv.d2ul(g * 1073741824.0); }
            else if (a.equals("cdalign=none")) { env.printf("cdalign=none was removed (2026-10-22): every CD is checked. A small remaining difference can be accepted with cdaccept, a known offset given with cdalign=<offset>; nothing encoded\n"); return 1; }
            else if (a.startsWith("cdalign=")) {   // master samples, whole or with a fraction (v6): digits, optionally '.' and up to 9 digits
                final String v = a.substring(8); int p = 0; int sg = 1; if (p < v.length() && (v.charAt(p) == '-' || v.charAt(p) == '+')) { sg = v.charAt(p) == '-' ? -1 : 1; p++; }
                long I = 0, F = 0, Fs = 1; int ni = 0, nf = 0; boolean ok = true;
                while (p < v.length() && v.charAt(p) >= '0' && v.charAt(p) <= '9' && ni < 11) { I = I * 10 + (v.charAt(p++) - '0'); ni++; }
                if (p < v.length() && v.charAt(p) == '.') { p++; while (p < v.length() && v.charAt(p) >= '0' && v.charAt(p) <= '9' && nf < 9) { F = F * 10 + (v.charAt(p++) - '0'); Fs *= 10; nf++; } ok = nf > 0; }
                if (ni == 0 || !ok || p < v.length() || I > 2000000000L) { env.printf(v.indexOf(',') >= 0 ? "cdalign=%s: write the fraction with a point (for example cdalign=3.5)\n"
                                                                                  : "cdalign=%s: give the offset in master samples, a whole number or with a fraction of up to 9 digits (for example cdalign=-3 or cdalign=3.4999828)\n", v); return 1; }
                x.manual = true; x.manualText = v; x.manualSg = sg; x.manualI = I; x.manualF = F; x.manualFs = Fs; }
            else { env.printf("unknown option '%s': nothing encoded (options: s1 s2 zs energy equal cd=<CD.wav> cdcheck cdalign=<offset> cdaccept cdedge=pad cdedge=cut bits=24 mem=<GB> noise=<marks.laenoise> g g=b g=c g=d --tag=KEY=VALUE --cover=<image> --picture=<type>:<image> --form=flac --form=wav resume)\n", a); return 1; } }
        x.run.keepTmp = x.resume;
        if ((x.cdCheck || x.cdAccept || x.manual || x.cdEdge >= 0) && x.cdPath == null) { env.printf("cdcheck, cdalign, cdaccept and cdedge= need cd=<CD.wav>\n"); return 1; }
        for (int k = 0; k < 8; k++) { if (x.pk[k] == null || !x.pk[k].any) continue;
            if (x.pk[k].path.isEmpty()) { env.printf("master%d...: master%d=<master.wav> is missing: nothing encoded\n", k + 2, k + 2); return 1; }
            if (k != x.nPacks) { env.printf("master%d=: the further masters are numbered without a gap (master%d= is missing): nothing encoded\n", k + 2, x.nPacks + 2); return 1; }
            x.nPacks++; }
        if (x.alignOut != null && (x.cdPath == null || x.nPacks != 0 || x.resume || x.cdCheck)) { env.printf("alignout= is an internal step of the extension packs: nothing encoded\n"); return 1; }
        if (x.nPacks != 0 && (x.noisePath != null || x.resume || x.cdCheck)) { env.printf("further masters (master2= ...) cannot be combined with noise=, resume or cdcheck in this version: nothing encoded\n"); return 1; }
        if (mi >= PartitionFormat.MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
        x.partLeft = exists(partPath);   // with resume, decided once the work folder's notes are read (port of lae_enc.cpp, 2026-11-05)
        if (x.partLeft && !x.resume) { env.printf("%s already exists and was not created by this run: remove it or choose another output name; nothing encoded\n", partPath); return 1; }
        final String marker = new File(tmp, "lae_enc.tmp").getPath();
        if (exists(tmp) && !exists(marker)) { env.printf("%s already exists and is not an lae_enc work folder: remove it or choose another output name; nothing encoded\n", tmp); return 1; }
        clearModuleEnv(env);   // opt-in diagnostics excepted
        env.set("ZSIGN", zsOpt ? "1" : "");   // opt-in zero level with true signs (partition.h)
        env.set("RULE_F", scheme == 2 ? "2" : "1"); env.set("RULE_D", mi == PartitionFormat.MODE_KEEP20 ? "3" : "1");   // precision rules (partition.h); recorded in every stream header
        if (mi == PartitionFormat.MODE_KEEP20 && !x.cdCheck) env.printf("NOTE keep20: the depth layer is coded at 20 bit instead of 18 bit. The CD band gets about 13 dB closer to the master\n"
                                            + "  (about -124 instead of -111 dBFS error), which is very unlikely to be audible at normal listening levels; the file grows by about\n"
                                            + "  84 MB per hour of music (44.1 kHz family; about 94 MB for 48 kHz), i.e. about 9 minutes less music per CD-Extra disc.\n");
        // only the header is read here; the samples go straight to m0.raw once the work folder exists (the master is never held whole in memory)
        final WavIn.Info wi = x.wi;
        if (x.rawRate > 0) { final long rb = fileSize(argv[1]); if (rb < 8) { env.printf("rawin=: the aligned master is missing\n"); return 2; }   // internal: an aligned master of this run (stereo int32, 24 bit left-aligned)
            wi.rate = x.rawRate; wi.ch = 2; wi.bits = 24; wi.dataBytes = rb / 8 * 6; }
        else if (!(WavIn.probe(argv[1], wi) && wi.ch == 2)) { env.printf("need a stereo integer PCM WAV\n"); x.oldNote(); return 1; }
        x.rate = wi.rate; x.bits = wi.bits; final int wrate = wi.rate;
        String note32 = "";
        if (x.bits == 32) {   // LAE v5 carries at most 24 bit: say what reducing the master would drop, then end or continue as the user chooses
            long lost = WavIn.low8(argv[1], wi);
            if (lost == 0) note32 = "input 32 bit with 24-bit content (nothing dropped)";
            else {
                env.printf("NOTE: the master is a 32-bit file. LAE v5 carries at most 24 bit, so the lowest 8 bits would be dropped (%s).\n",
                           lost > 0 ? (lost + " samples carry information there") : "their content could not be checked");
                if (!bits24) { env.printf("  nothing encoded. To encode with 24-bit output, add the option bits=24.\n"); x.oldNote(); return 1; }   // C++: a console asks; the app asks and passes bits=24
                note32 = "input 32 bit, encoded as 24 bit"; } }
        final double base = (double) wrate % 44100.0 == 0 ? 44100.0 : 48000.0; final int stages = Cv.lround32(LMath.log2(wrate / base));
        if (Math.abs(base * (1 << stages) - wrate) > 0.5 || stages < 1 || stages > 3) { env.printf("rate %d is not 2/4/8 x 44.1 or 48 kHz\n", wrate); x.oldNote(); return 1; }
        x.base = (int) base; x.stages = stages;
        if (x.manual) {   // cdalign=<o>: parsed as o = manualPlace - manualPhi / U (U units per master sample), then the nearest whole offset and the fraction
            final long Ux = FracDelay.unitsPerMaster(stages);
            if (x.manualF == 0) { x.manualPlace = x.manualSg * x.manualI; x.manualPhi = 0; }
            else { final long rest = x.manualSg > 0 ? x.manualFs - x.manualF : x.manualF; x.manualPlace = x.manualSg > 0 ? x.manualI + 1 : -x.manualI; x.manualPhi = (rest * Ux + x.manualFs / 2) / x.manualFs;
                   if (x.manualPhi >= Ux) { x.manualPlace--; x.manualPhi = 0; } }
            x.manualT = x.manualPhi * 2 > Ux ? x.manualPlace - 1 : x.manualPlace;   // the whole offset nearest to o: where the first check runs and the master is placed
            x.manualMove = (double) (x.manualT - x.manualPlace) + (double) x.manualPhi / (double) Ux; }   // and the fraction it is moved by: o = manualT - manualMove
        final int P = x.P;   // mirror padding at the master rate (decoder trims it)
        x.Lm = wi.dataBytes / (wi.bits / 8) / 2;   // changes only when the master is placed on an independent CD's time line
        if (!x.lengthOk("the master")) return 1;
        x.LmIn = x.Lm;   // noise=: the marking file belongs to this master (SHA-256, length, rate) and lies inside its grid
        if (x.noisePath != null) { final NoiseMarks.Marks mk = x.noiseMk;
            String[] why = {""}; if (!NoiseMarks.readMarks(x.noisePath, mk, why)) { env.printf("cannot read the marking file %s (%s): nothing encoded\n", x.noisePath, why[0]); x.oldNote(); return 1; }
            if (mk.frames != x.Lm || Math.abs(mk.rate - wi.rate) > 0.5) { env.printf("the marking file %s is for another master (length or rate differ): nothing encoded\n", x.noisePath); x.oldNote(); return 1; }
            final String sha = NoiseMarks.masterSha(argv[1]); if (sha == null) { env.printf("cannot read %s for the SHA-256 check of noise=: nothing encoded\n", argv[1]); x.oldNote(); return 1; }
            if (!sha.equals(mk.sha)) { env.printf("the marking file %s is for another master (SHA-256 differs): nothing encoded\n", x.noisePath); x.oldNote(); return 1; }
            final long Kn = MdctTb.frames(x.Lm);
            for (int i = 0; i < mk.pos.size(); i++) { final NoiseMarks.Pos p = mk.pos.get(i);
                if (p.m0 < 0 || p.m1 > Kn || p.m0 >= p.m1 || p.c0 < 0 || p.c1 > NoiseMarks.NCOL || p.c0 >= p.c1) { env.printf("the marking file %s: position %zu lies outside the grid: nothing encoded\n", x.noisePath, (long) (i + 1)); x.oldNote(); return 1; } }
            env.printf("noise: %zu position(s) from %s (the master's SHA-256 matches); the marked cells are deleted in the frequency layer\n", (long) mk.pos.size(), x.noisePath); }
        if (x.gOpt) env.printf("g: on, strength %c (%s) - deleted or silent A cells code -G where G lies above the 24-bit rounding floor (docs/NOISE.md)\n",
                               x.gLevel, x.gLevel == 'd' ? "step 1, down to the 24-bit floor" : x.gLevel == 'c' ? "C steps: block peak - 4 bit" : "B steps: block peak - 2 bit");
        { File td = new File(tmp); boolean made = td.mkdirs(); if (!td.isDirectory()) { env.printf("cannot create the work folder %s: nothing encoded\n", tmp); x.oldNote(); return 2; }
          if (made || exists(marker)) x.run.tmpMine = true; EncIo.writeText(env, marker, "lae_enc work folder (removed at the end of each run)\n"); }
        try {   // struct Guard: removes the work folder always, the .part on failure
            return encodeBody(x, argv, T0, base, stages, P, wm, equal, scheme, memBudget, modeName, note32); }
        finally { cleanup(x); } }

    static final int EDGE_STOP = 0, EDGE_PAD = 1, EDGE_CUT = 2;   // m_align::EDGE_* (cd_align.cpp)

    // port of lae_enc.cpp:clearModuleEnv: module variables come from this program only: anything inherited from the calling environment is removed
    static void clearModuleEnv(EncEnv env) {
        for (String k : new String[] {"ZSIGN", "RULE_F", "RULE_D", "DITHERFLAGS", "BASE24", "CDEXT", "CUTOFFOUT", "PART", "PARTEQ", "PARTW", "CHSEL", "DUMP", "PLANE", "SEGCOUNT",
                                      "PARTBYTES", "FULLBAND", "MASTERREF", "DEPTHADAPT", "K0FRAC", "OCTSPLIT", "LEAKIN", "LEAKOUT", "CLIPMAP", "NOISE", "NOISEPAD", "NOISEM", "GMODE"}) if (env.get(k) != null) env.set(k, ""); }

    /** the streams of one layer (C++ std::vector&lt;std::vector&lt;uint8_t&gt;&gt;) */
    static final class Streams { List<byte[]> s = new ArrayList<byte[]>(); }

    // port of lae_enc.cpp:encode lambda nested: one further master, this encoder run as a step of this run with its own work folder
    static int nested(Ctx x, List<String> a, String zs, String rf, String rd, boolean hadUnits) {
        final EncEnv env = x.env; Ctx y = new Ctx(); y.env = env; int rc;
        try { rc = encode(y, a.toArray(new String[0]), null); }
        catch (EncEnv.Cancelled c) { throw c; }
        catch (LaeException e) { env.printf("further master: stopped: %s\n", e.getMessage()); rc = 2; }
        catch (OutOfMemoryError e) { env.printf("further master: stopped: out of memory\n"); rc = 2; }
        clearModuleEnv(env); env.set("ZSIGN", zs != null ? zs : ""); env.set("RULE_F", rf != null ? rf : ""); env.set("RULE_D", rd != null ? rd : ""); env.set("DITHERFLAGS", x.T("dither.flags"));   // this run's own module settings again
        if (!hadUnits) env.set("LAE_BLOCKUNITS", "");
        return rc; }
    static void writeCore(EncEnv env, String coreW, short[] core, int base) {
        byte[] h = WavIo.hdr(base, 2, 16, core.length * 2L, 0), d = new byte[core.length * 2]; EncIo.toBytes(core, 0, core.length, d);
        byte[] o = new byte[h.length + d.length]; System.arraycopy(h, 0, o, 0, h.length); System.arraycopy(d, 0, o, h.length, d.length); EncIo.writeVec(env, coreW, o); }
    // port of lae_enc.cpp:encode, the extension-pack block (2026-11-10): every further master is aligned to the core by the master's own
    // alignment, the largest need of digital silence on each side goes to the core once and to every master, then each aligned master
    // is encoded against the final core as a run of its own. Returns -1 to continue with the main body, or the rc encode() returns.
    static int packPhase(final Ctx x, final String[] argv, final double base, final int stages, final int P, final int scheme, final String modeName) {
        final EncEnv env = x.env; final long pbA = P >> stages; short[] core = coreOf(x.T("b16.s16"), pbA); final int n = x.nPacks;
        final String coreW = x.T("core.wav"); writeCore(env, coreW, core, (int) base);
        List<String> common = new ArrayList<String>(); common.add(modeName); common.add(scheme == 2 ? "s2" : "s1");   // the coding options are the same for every pack
        for (int i = 4; i < argv.length; i++) { final String a = argv[i]; if (a.equals("zs") || a.equals("energy") || a.equals("equal") || a.equals("bits=24") || a.startsWith("mem=") || a.equals("g") || a.startsWith("g=")) common.add(a); }   // g: every pack on its own master (2026-11-11)
        final boolean hadUnits = env.get("LAE_BLOCKUNITS") != null; boolean stopA = false;
        final String zs = env.get("ZSIGN"), rf = env.get("RULE_F"), rd = env.get("RULE_D");
        final long upM = 1L << stages; final boolean bits24c = common.contains("bits=24");
        String[] raws = new String[n]; int[] rates = new int[n]; long[] ups = new long[n], leads = new long[n], tails = new long[n];
        for (int k = 0; k < n; k++) { final PackOpt q = x.pk[k]; final String num = Integer.toString(k + 2), pre = "master" + num + " (pack " + (k + 1) + "): ", raw = x.T("al" + num + ".raw");
            env.printf("%saligning %s to the core (%zu frames) with the master's alignment. In its lines below, \"the master\" is this master, \"the CD\" / \"independent CD\" is the core, cdalign= stands for master%salign=, cdaccept for master%saccept, cdedge= for master%sedge=\n",
                       pre, q.path, (long) (core.length / 2), num, num, num);
            List<String> a = new ArrayList<String>(); a.add("lae_enc"); a.add(q.path); a.add(x.T("al" + num + ".wav")); a.add("3"); a.add("cd=" + coreW); a.add("alignout=" + raw);
            if (!q.align.isEmpty()) a.add("cdalign=" + q.align); if (q.accept) a.add("cdaccept"); if (!q.edge.isEmpty()) a.add("cdedge=" + q.edge); if (bits24c) a.add("bits=24");
            final int rc = nested(x, a, zs, rf, rd, hadUnits);
            if (rc != 0 || !exists(raw + ".txt")) { stopA = true;
                if (q.align.isEmpty()) env.printf("%snot placed yet: choose its position from the two searches above and give it as master%salign=<offset>\n", pre, num);
                else env.printf("%snot aligned at master%salign=%s (see the lines above)\n", pre, num, q.align);
                continue; }
            long Lx, aL, aT; int rx;
            try { final String[] t = EncIo.readText(raw + ".txt").trim().split(" "); if (t.length != 4) throw new NumberFormatException(); Lx = Long.parseLong(t[0]); aL = Long.parseLong(t[1]); aT = Long.parseLong(t[2]); rx = Integer.parseInt(t[3]); }
            catch (NumberFormatException e) { throw EncIo.fail("internal error: alignment result of master" + num); }
            if (aL < 0 || aT < 0 || rx <= 0) throw EncIo.fail("internal error: alignment result of master" + num);
            x.packReport += "pack " + (k + 1) + " (master" + num + "): " + q.path + "\n" + EncIo.readText(raw + ".report.txt");
            new File(raw + ".txt").delete(); new File(raw + ".report.txt").delete();
            final long upX = rx / (int) base;
            if (upX < 2 || Lx / upX != (long) (core.length / 2) + aL + aT) { env.printf("%sinternal error: the aligned master has %lld frames at %d Hz, the core %zu + %ld + %ld; nothing encoded\n", pre, Lx, rx, (long) (core.length / 2), aL, aT); x.oldNote(); return 2; }
            if (aL != 0 || aT != 0) env.printf("%sits alignment needs %ld frame(s) of digital silence before the core and %ld after\n", pre, aL, aT);
            raws[k] = raw; rates[k] = rx; ups[k] = upX; leads[k] = aL; tails[k] = aT; }
        if (stopA) { env.printf("further masters: nothing encoded\n"); x.oldNote(); return 1; }
        long maxL = 0, maxT = 0; for (int k = 0; k < n; k++) { maxL = Math.max(maxL, leads[k]); maxT = Math.max(maxT, tails[k]); }
        if (maxL != 0 || maxT != 0) {   // the largest need on each side: the core gets that digital silence once, and the master and every further master the same
            env.printf("further masters: the core gets %ld frame(s) of digital silence before and %ld after (the largest need of any master); the master and every further master get the same\n", maxL, maxT);
            { short[] o = new short[(int) (core.length + 2 * maxL + 2 * maxT)]; System.arraycopy(core, 0, o, (int) (2 * maxL), core.length); core = o; }
            padRawFile(env, x.T("m0.raw"), maxL * upM, maxT * upM); x.Lm += (maxL + maxT) * upM;
            if (!x.fitsLength(x.Lm)) { env.printf("further masters: the time line (%lld frames) is outside this encoder's length limits: nothing encoded\n", x.Lm); x.oldNote(); return 1; }
            if (!x.derive()) return 2;
            if (x.cdPath != null) {   // the given CD with its digital silence is the CD layer again, as after the master's own alignment
                byte[] b = slurp(x.T("b16.s16")); if (b.length / 4 != core.length / 2 + 2 * pbA) throw EncIo.fail("internal error: core length after padding");
                byte[] cb = new byte[core.length * 2]; EncIo.toBytes(core, 0, core.length, cb); System.arraycopy(cb, 0, b, (int) (4 * pbA), cb.length); EncIo.writeVec(env, x.T("b16.s16"), b);
                byte[] df = slurp(x.T("dither.flags")); java.util.Arrays.fill(df, (byte) 0); EncIo.writeVec(env, x.T("dither.flags"), df); }
            else { final int want = core.length; core = coreOf(x.T("b16.s16"), pbA);   // derived core: derived again from the padded master
                if (core.length != want) throw EncIo.fail("internal error: derived core length after padding"); }
            for (int k = 0; k < n; k++) if (maxL - leads[k] != 0 || maxT - tails[k] != 0) padRawFile(env, raws[k], (maxL - leads[k]) * ups[k], (maxT - tails[k]) * ups[k]);
            writeCore(env, coreW, core, (int) base); }
        for (int k = 0; k < n; k++) { final PackOpt q = x.pk[k]; final String num = Integer.toString(k + 2), pre = "master" + num + " (pack " + (k + 1) + "): ", outK = x.T("pk" + num + ".wav");
            env.printf("%sencoding the aligned master against the core (%zu frames) as an extension of its own (position 0: nothing is moved again)\n", pre, (long) (core.length / 2));
            List<String> a = new ArrayList<String>(); a.add("lae_enc"); a.add(raws[k]); a.add(outK); a.addAll(common); a.add("cd=" + coreW); a.add("cdalign=0"); a.add("rawin=" + rates[k]);
            if (q.accept) a.add("cdaccept");
            final int rc = nested(x, a, zs, rf, rd, hadUnits);
            if (rc != 0 || !exists(outK)) { stopA = true; env.printf("%snot encoded (see the lines above)\n", pre); continue; }
            WavIo.Wav pw = new WavIo.Wav(); if (!WavIo.readCD(outK, pw) || pw.lae.length < 60) throw EncIo.fail("internal error: the pack of master" + num + " could not be read back");
            if (!java.util.Arrays.equals(pw.pcm16, core)) { env.printf("%sinternal error: the pack's core differs from the file's core (%zu frames instead of %zu, or other samples); nothing encoded\n", pre, (long) (pw.pcm16.length / 2), (long) (core.length / 2)); x.oldNote(); return 2; }
            new File(raws[k]).delete();
            final byte[] nmb = utf8(q.name); final int rn = 1 + nmb.length + pw.lae.length - 4; byte[] r = new byte[8 + rn];
            r[0] = 'L'; r[1] = 'P'; r[2] = 'A'; r[3] = 'K'; for (int i = 0; i < 4; i++) r[4 + i] = (byte) (rn >>> (8 * i));
            r[8] = (byte) nmb.length; System.arraycopy(nmb, 0, r, 9, nmb.length); System.arraycopy(pw.lae, 0, r, 9 + nmb.length, pw.lae.length - 4);   // the pack's payload without its CRC: one CRC covers the whole chunk
            env.printf("%sencoded: %zu B\n", pre, (long) r.length); x.packRecs.add(r);
            new File(outK).delete(); new File(outK + ".align.txt").delete(); }
        new File(coreW).delete();
        if (stopA) { env.printf("further masters: nothing encoded\n"); x.oldNote(); return 1; }
        return -1; }

    // the part of encode() after the work folder exists (inside the C++ Guard's scope)
    static int encodeBody(final Ctx x, final String[] argv, final long T0, final double base, final int stages, final int P, final int wm, final boolean equal, final int scheme,
                          final long memBudget, final String modeName, final String note32) {
        final EncEnv env = x.env; final int mi = x.mi; final int wrate = x.rate; final String outPath = x.outPath, partPath = x.partPath;
        if (!x.spaceOk(0)) return 2;
        String masterMd5;   // MD5 of the master's data bytes, computed once while reading (RM-1)
        if (x.rawRate > 0) { copyFile(argv[1], x.T("m0.raw")); masterMd5 = ""; }
        else { long[] fr = new long[2]; masterMd5 = WavIn.toRaw(env, argv[1], x.wi, x.T("m0.raw"), fr);
            if (fr[0] != x.Lm) throw EncIo.fail("internal error: master length"); }
        // resume (block-wise, without cd=): every finished step is noted in resume.txt in the work folder, together with a key made of this
        // encoder build, the options and the master's MD5. A run with the same key continues after the last finished step; any other key
        // starts from the beginning. The notes are read only by this encoder and never go into the LAE file.
        x.manifest = x.T("resume.txt");
        if (x.resume) { StringBuilder key = new StringBuilder("lae_enc java-port " + LaeyEncoder.VERSION + " | options");
            for (int i = 3; i < argv.length; i++) if (!argv[i].equals("resume") && !LaeRetag.isTagOpt(argv[i]) && !LaeRetag.isFormOpt(argv[i])) key.append(" ").append(argv[i]);
            key.append(" | master ").append(masterMd5);
            if (x.noisePath != null) { final byte[] t = EncIo.readAll(x.noisePath); long h = 0xcbf29ce484222325L;   // FNV-1a of the marking file's bytes (1469598103934665603)
                for (int i = 0; i < t.length; i++) { h ^= (t[i] & 0xFF); h *= 1099511628211L; }
                key.append(" | noise ").append(String.format("%016x", h)); }
            String old = ""; if (exists(x.manifest)) old = EncIo.readText(x.manifest);
            final String head = "key " + key + "\n";
            if (!old.isEmpty() && old.startsWith(head)) { int p = head.length();
                while (p < old.length()) { int e = old.indexOf('\n', p); if (e < 0) break; if (old.startsWith("done ", p)) x.doneSteps.add(old.substring(p + 5, e)); p = e + 1; }
                env.printf("resume: continuing the earlier run (%zu step(s) already done)\n", (long) x.doneSteps.size()); }
            else { if (!old.isEmpty()) env.printf("resume: the work folder belongs to another run (other master, options or encoder build): starting from the beginning\n");
                EncIo.writeText(env, x.manifest, head); } }
        if (x.partLeft) {   // resume (2026-11-05): a run killed after writing its .part leaves it; the run with the same key writes it again
            if (x.resume && x.isDone("part")) { new File(partPath).delete();
                if (exists(partPath)) { env.printf("cannot remove the unfinished %s of the earlier run: nothing encoded\n", partPath); return 1; }
                env.printf("resume: the unfinished %s of the earlier run is written again\n", partPath); }
            else { env.printf("%s already exists and was not created by this run: remove it or choose another output name; nothing encoded\n", partPath); return 1; } }
        if (x.cdPath == null && x.isDone("derive")) { env.printf("resume: CD layer derivation already done\n"); env.set("DITHERFLAGS", x.T("dither.flags")); }   // the second route split reads the dither flags derive() names (fixed in lae_enc 2026-11-05)
        else { if (!x.derive()) return 2; if (x.cdPath == null) x.markDone("derive"); }   // cd=: the alignment may place and move the master and derive again
        stamp(env, T0, "padding + CD layer derived");
        if (x.cdPath != null) {   // independent CD: lae_enc.cpp lines 308-492 (part 2 of the port)
            CdHook h = cdHook; if (h == null) { env.printf("cd= (independent CD) is not part of this build of the Java encoder: nothing encoded\n"); x.oldNote(); return 1; }
            int r = h.run(x); if (r >= 0) return r; }
        if (x.alignOut != null) {   // internal step of the extension packs: this master now lies on the given core's time line; hand it to the calling run
            if (!rename(x.T("m0.raw"), x.alignOut)) throw EncIo.fail("cannot write " + x.alignOut + ": rename failed");
            EncIo.writeText(env, x.alignOut + ".report.txt", x.alignReport.isEmpty() ? "\n" : x.alignReport);
            EncIo.writeText(env, x.alignOut + ".txt", x.Lm + " " + x.cdAddLead + " " + x.cdAddTail + " " + wrate + "\n");
            env.printf("further master aligned: %lld frames on the core's time line (digital silence added to the core: %ld frame(s) before, %ld after)\n", x.Lm, x.cdAddLead, x.cdAddTail); return 0; }
        if (x.nPacks != 0) { final int rp = packPhase(x, argv, base, stages, P, scheme, modeName); if (rp >= 0) return rp; }
        if (x.noisePath != null && (x.placeT != 0 || x.Lm != x.LmIn)) {   // the user (2026-11-04): alignment and its confirmation come first, the marking is made on the aligned master
            env.printf("noise=: the master was placed on the CD's time line here (whole offset %+lld master samples, length %lld -> %lld frames), so the marked cells no longer lie on the grid they were marked on.\n"
                     + "  Make the marking on the aligned master (lae_enc plan writes it) and encode that one with cdalign=0; nothing encoded\n", x.placeT, x.LmIn, x.Lm); x.oldNote(); return 1; }
        env.set("CDEXT", x.cdPath != null ? "1" : "");   // cut-off detection by coherence for an independent CD (routes.cpp)
        env.set("CUTOFFOUT", x.T("cutoff.txt"));
        final Module routes = new Module() { @Override public int main(EncEnv e, String[] a) { return RoutesBlk.main(e, a); } };
        if (x.isDone("routes")) env.printf("resume: route split already done\n");
        else { if (call(env, routes, "routes", x.T("m.raw"), Integer.toString(wrate), x.T("b16.s16"), x.T("b24.s32"), x.T("T.s32"), x.T("F.s32")) != 0) { env.printf("route split failed\n"); x.oldNote(); return 2; } x.markDone("routes"); }
        env.set("CUTOFFOUT", "");
        stamp(env, T0, "route split"); final double fc = Fmt.atof(EncIo.readText(x.T("cutoff.txt")));
        env.set("PART", modeName); env.set("PARTEQ", equal ? "1" : ""); env.set("PARTW", wm != 0 ? "1" : "0");
        // one layer = one encoder run (possibly several octave segments) + a self-check of every segment's stream
        final AtomicBoolean ok = new AtomicBoolean(true);   // shared by the two layer threads of a pair
        final int[] depthPass = {1};   // resume: the depth layers are coded twice (before and after leak compensation), each pass is its own step
        final Layer layer = new Layer() { @Override public Streams run(EncEnv e, String in, int rate, String ch, String tag) {   // port of lae_enc.cpp:encode lambda layer
            if (EncIo.testFail(e, "layerthrow:" + tag)) throw EncIo.fail("forced by the test hook in layer " + tag);   // test: an exception outside the modules
            final String step = "layer " + tag + (tag.charAt(0) == 'd' ? Integer.toString(depthPass[0]) : "");
            Streams out = new Streams();
            if (x.isDone(step)) { int nseg = Fmt.atoi(EncIo.readText(x.T(tag + ".seg")));
                if (nseg < 1 || nseg > 8) { ok.set(false); e.printf("layer %s: bad segment count\n", tag); return out; }
                for (int i = 0; i < nseg; i++) out.s.add(slurp(x.T(tag + ".bin" + (nseg > 1 ? "." + i : ""))));
                e.printf("resume: %s already coded\n", step); return out; }
            e.set("CHSEL", ch); e.set("DUMP", x.T(tag + ".lae")); e.set("PLANE", x.T(tag + ".plane")); e.set("SEGCOUNT", x.T(tag + ".seg"));
            int rc = call(e, new Module() { @Override public int main(EncEnv ee, String[] a) { return LossyBlk.main(ee, a); } }, "lossy", x.T(in), Integer.toString(rate)); e.set("SEGCOUNT", "");
            if (rc != 0) { ok.set(false); e.printf("layer %s: coding failed (code %d)\n", tag, rc); return out; }
            int nseg = Fmt.atoi(EncIo.readText(x.T(tag + ".seg"))); if (nseg < 1 || nseg > 8) { ok.set(false); e.printf("layer %s: bad segment count\n", tag); return out; }
            for (int i = 0; i < nseg; i++) { String suf = nseg > 1 ? "." + i : "";
                e.set("PARTBYTES", x.T(tag + ".bin" + suf));
                if (call(e, new Module() { @Override public int main(EncEnv ee, String[] a) { return PartBlk.main(ee, a); } }, "part", x.T(tag + ".lae" + suf), x.T(tag + ".plane" + suf)) != 0) ok.set(false);
                e.set("PARTBYTES", ""); if (!ok.get()) return out; out.s.add(slurp(x.T(tag + ".bin" + suf))); }
            x.markDone(step); return out; } };
        // memory plan (2026-10-29 / 30): made once at the start from the track length and the memory available then; the whole encode follows it.
        // Budget: mem=<GB>, or by default min(1/2, max(1/4, all - 28 GB)) of the memory (Java: the heap limit). A layer needs about 1 byte per
        // layer sample (the cell records) + 20 bytes per sample of one block + 128 MB. The block length (whole floor segments and root rows, a memory
        // unit only) is the largest that fits the budget with two layers at once, at most 4 units; if not even one unit fits two at once, the layers
        // run one after the other. Never two at once with PARTTHREADS=1. LAE_BLOCKUNITS sets the block length by hand (tests). The output depends on
        // neither: every block length and both orders give byte-identical files.
        final Runtime rt = Runtime.getRuntime(); final long memTotal = rt.maxMemory() == Long.MAX_VALUE ? 0 : rt.maxMemory(); final long memAvail = memTotal > 0 ? memTotal - (rt.totalMemory() - rt.freeMemory()) : 0;
        final long G28 = 28L << 30, memRule = Math.min(memTotal / 2, Math.max(memTotal / 4, memTotal > G28 ? memTotal - G28 : 0L));
        final long memUse = memBudget > 0 ? memBudget : memRule;
        final boolean oneThread = env.partThreads() == 1;
        final long sF = x.Lm + 2 * P, sD = sF >> stages, uF = unitSamples(wrate), uD = unitSamples((int) base), M128 = 128L << 20;
        final boolean mayPair = !oneThread && env.get("LAE_TESTSEQUENTIAL") == null;
        long units = 0; final String buEnv = env.get("LAE_BLOCKUNITS");
        if (buEnv != null) units = Fmt.atoll(buEnv);
        else { final long lim = Math.min(memUse, memAvail != 0 ? memAvail : memUse);
            units = mayPair ? fit(lim / 2, sF, M128, uF) : 0; if (units < 1) units = fit(lim, sF, M128, uF); units = Math.max(1L, Math.min(Math.min(4L, units), (sF + uF - 1) / uF));   // at most 4 units (about 32 s): larger blocks are not faster and only take memory; never longer than the track
            env.set("LAE_BLOCKUNITS", Long.toString(units)); }
        final boolean[] planD = planFor(sD, uD, units, M128, mayPair, memUse, memAvail), planF = planFor(sF, uF, units, M128, mayPair, memUse, memAvail);
        final long needD = 2 * layerNeed(sD, uD, units > 0 ? units : (sD + uD - 1) / uD, M128), needF = 2 * layerNeed(sF, uF, units > 0 ? units : (sF + uF - 1) / uF, M128);
        env.printf("memory plan: %.2f min at %d Hz; blocks of %lld unit(s) (%.0f s); depth layers %s (two at once about %.0f MB), frequency layers %s (two at once about %.0f MB); budget %.0f MB%s, available %.0f MB\n",
                   x.Lm / (60.0 * wrate), wrate, units, units > 0 ? (double) (uF * units) / wrate : x.Lm / (double) wrate,
                   planD[0] ? "two at once" : "coded one after the other", needD / 1e6, planF[0] ? "two at once" : "coded one after the other", needF / 1e6,
                   memUse / 1e6, memBudget > 0 ? " (mem=)" : "", memAvail / 1e6);
        env.set("FULLBAND", "1"); env.set("MASTERREF", x.T("b24.s32")); env.set("DEPTHADAPT", "1");
        // resume: once the leak step is done, the first depth pass is not needed any more (its only use is the file the leak is decoded from)
        final boolean skipD1 = x.isDone("leak"); Streams[] dL = {new Streams()}, dR = {new Streams()}; if (!skipD1) layerPair(env, layer, ok, "T.s32", (int) base, "L", "dL", "R", "dR", dL, dR, planD[0]); stamp(env, T0, "depth layers");
        env.set("FULLBAND", ""); env.set("MASTERREF", ""); env.set("DEPTHADAPT", "");
        final String kf = Fmt.c("%.6f", (fc - 1000.0) / (base / 2)); env.set("K0FRAC", kf); env.set("OCTSPLIT", "1");
        if (x.noisePath != null) {   // cells below k0 lie in the CD layer: listed, not deleted (the user, 2026-11-04: the default stays as before)
            final double k0f = Fmt.atof(kf); int k0b = Cv.d2i(Math.floor(k0f * (base / 2) / (wrate / 2.0 / MdctTb.N))); k0b -= k0b % NoiseMarks.CW; long below = 0;   // as lossy_blk.cpp computes k0 with noise=
            for (int i = 0; i < x.noiseMk.pos.size(); i++) { final NoiseMarks.Pos p = x.noiseMk.pos.get(i); if (p.c0 * NoiseMarks.CW >= k0b) continue; below++; final boolean whole = p.c1 * NoiseMarks.CW <= k0b;
                env.printf("noise: position %zu (%.3f-%.3f s, %.1f-%.1f Hz) lies %s below %.1f Hz, the frequency layer's start: in the CD layer, not deleted%s\n", (long) (i + 1),
                           NoiseMarks.frameT0(p.m0, wrate), NoiseMarks.frameT0(p.m1, wrate), NoiseMarks.colHz(p.c0, wrate), NoiseMarks.colHz(p.c1, wrate), whole ? "wholly" : "partly", k0b * (wrate / 2.0 / MdctTb.N), whole ? "" : " there (deleted above it)"); }
            if (below != 0) env.printf("noise: %ld position(s) reach below the frequency layer's start; deleting there would change the CD layer (not done)\n", below);
            env.set("NOISE", x.noisePath); env.set("NOISEPAD", Integer.toString(P)); }
        if (x.noisePath != null || x.gOpt) env.set("NOISEM", x.T("m.raw")); if (x.gOpt) env.set("GMODE", String.valueOf(x.gLevel));
        Streams[] fM = {new Streams()}, fS = {new Streams()}; if (ok.get()) layerPair(env, layer, ok, "F.s32", wrate, "M", "fM", "S", "fS", fM, fS, planF[0]); env.set("K0FRAC", ""); env.set("OCTSPLIT", ""); stamp(env, T0, "frequency layers");
        env.set("NOISE", ""); env.set("NOISEPAD", ""); env.set("NOISEM", ""); env.set("GMODE", "");
        if (x.gOpt && ok.get() && fM[0].s.size() == fS[0].s.size() && fM[0].s.size() % 2 == 0) {   // the user is told what g costs (2026-11-04): the G streams are the second half of each plane's streams
            long all = 0, gb = 0; final int half = fM[0].s.size() / 2;
            for (int i = 0; i < fM[0].s.size(); i++) { final long b = fM[0].s.get(i).length + fS[0].s.get(i).length; all += b; if (i >= half) gb += b; }
            env.printf("g: the G streams take %.2f MB of the frequency layer's %.2f MB (%.1f %%); without g the file would be about that much smaller\n", gb / 1e6, all / 1e6, all != 0 ? 100.0 * gb / all : 0.0); }
        if (!ok.get() || (!skipD1 && (dL[0].s.size() != 1 || dR[0].s.size() != 1)) || fM[0].s.isEmpty() || fM[0].s.size() != fS[0].s.size()) { env.printf("ENCODER SELF-CHECK FAILED: a decoded layer differs from the encoder's plane or a layer could not be coded; nothing written\n"); x.oldNote(); return 2; }
        // CD layer: padded base trimmed by P >> stages at each end; the trimmed edges go into the LAE chunk (the decoder needs them).
        // The CD samples are not held in memory: they are copied from b16.s16 piece by piece when the file is written and checked.
        final long nb; final long pb = P >> stages; final short[] edge = new short[(int) (4 * pb)];
        { BlkIo.ReaderS r = new BlkIo.ReaderS(x.T("b16.s16")); try { nb = r.frames;
            short[] e1 = r.read(0, pb); System.arraycopy(e1, 0, edge, 0, e1.length); short[] e2 = r.read(nb - pb, nb); System.arraycopy(e2, 0, edge, (int) (2 * pb), e2.length); } finally { r.close(); } }
        final long cdBytes = (nb - 2 * pb) * 4;
        final byte[] dfl = slurp(x.T("dither.flags"));
        final Chunk ck = new Chunk(); ck.x = x; ck.wrate = wrate; ck.base = (int) base; ck.stages = stages; ck.fc = fc; ck.mi = mi; ck.equal = equal; ck.wm = wm; ck.nb = nb; ck.pb = pb; ck.edge = edge; ck.dfl = dfl; ck.cdBytes = cdBytes; ck.fM = fM[0]; ck.fS = fS[0];
        long chunk = 0; if (!skipD1) { ck.dL = dL[0]; ck.dR = dR[0]; chunk = ck.writeFile(); if (chunk == 0) { x.oldNote(); return 2; } stamp(env, T0, "file written"); }
        long nclip = -1;
        final boolean[] streamErr = {false};   // C++ thread_local g_streamErr of the dec module (it runs on this thread)
        final Module dec = new Module() { @Override public int main(EncEnv e, String[] a) { return LaeDec.main(e, a, streamErr); } };
        {   // leak compensation (v5, always on): decode the final frequency layer, let the depth layer cancel its leak into the CD band
          if (skipD1) env.printf("resume: leak decode already done\n");
          else {
            streamErr[0] = false; new File(x.T("leak.s32")).delete();
            env.set("LEAKOUT", x.T("leak.s32")); int rc = call(env, dec, "dec", partPath, x.T("check.wav"), "full"); env.set("LEAKOUT", "");
            long lsz = fileSize(x.T("leak.s32")); final boolean forced = EncIo.testFail(env, "leakdecode"); final boolean se = streamErr[0];
            if (rc != 0 || lsz != nb * 8 || se || forced) { env.printf("leak compensation FAILED (decoder code %d, leak file %lld bytes, expected %lld%s%s): nothing written\n", rc, lsz, nb * 8,
                                                                                    se ? ", decoded values out of range" : "", forced ? ", forced by the test hook" : ""); x.oldNote(); return 2; }
            x.markDone("leak"); }
            if (x.isDone("routes2")) env.printf("resume: second route split already done\n");
            else { String fcs = Fmt.c("%.0f", fc); env.set("LEAKIN", x.T("leak.s32"));
              int rc = call(env, routes, "routes", x.T("m.raw"), Integer.toString(wrate), x.T("b16.s16"), x.T("b24.s32"), x.T("T.s32"), x.T("F2.s32"), fcs); env.set("LEAKIN", "");
              stamp(env, T0, "leak decoded + route split again");
              if (rc != 0 || EncIo.testFail(env, "routes2")) { env.printf("leak compensation FAILED in the route split: nothing written\n"); x.oldNote(); return 2; }
              x.markDone("routes2"); }
            env.set("FULLBAND", "1"); env.set("MASTERREF", x.T("b24.s32")); env.set("DEPTHADAPT", "1");
            depthPass[0] = 2; dL[0] = new Streams(); dR[0] = new Streams(); layerPair(env, layer, ok, "T.s32", (int) base, "L", "dL", "R", "dR", dL, dR, planD[0]);
            env.set("FULLBAND", ""); env.set("MASTERREF", ""); env.set("DEPTHADAPT", "");
            if (!ok.get() || dL[0].s.size() != 1 || dR[0].s.size() != 1) { env.printf("ENCODER SELF-CHECK FAILED after leak compensation; nothing written\n"); x.oldNote(); return 2; }
            ck.dL = dL[0]; ck.dR = dR[0]; chunk = ck.writeFile(); if (chunk == 0) { x.oldNote(); return 2; }
            env.printf("leak compensation: depth layer re-encoded against the decoded frequency layer\n"); }
        // no clip control: an overshoot beyond full scale is bounded by the coding error (16-bit backstop; peak error <= -76.6 dBFS measured on
        // hard-clipped +12 dB masters) and the decoder's hard limiter returns it to the master's own full-scale value. This decode of the final
        // file is also the end-to-end check: if the decoder refuses it, nothing is written.
        { streamErr[0] = false; new File(x.T("clip.bin")).delete();
          env.set("CLIPMAP", x.T("clip.bin")); int rc = call(env, dec, "dec", partPath, x.T("check.wav"), "full"); env.set("CLIPMAP", "");
          if (rc != 0 || EncIo.testFail(env, "finaldecode")) { env.printf("end-to-end check FAILED: the decoder could not decode the final file (code %d): nothing written\n", rc); x.oldNote(); return 2; }
          final long wantWav = 44 + x.Lm * 6 + ((x.Lm * 6) & 1), clipB = fileSize(x.T("clip.bin"));
          if (fileSize(x.T("check.wav")) == wantWav && clipB >= 0 && !EncIo.testFail(env, "clipdecode")) nclip = clipB / 4;
          if (nclip >= 0) env.printf("samples beyond full scale before the decoder's hard limiter: %ld\n", nclip);
          else env.printf("samples beyond full scale before the decoder's hard limiter: not measured (the check files are incomplete)\n"); }
        new File(x.T("check.wav")).delete();
        {   // only a complete, verified file gets the output name; a briefly locked target (scanner, indexer, player) is retried
            boolean renamed = false; for (int k = 0; k < 20; k++) { if (rename(partPath, outPath)) { renamed = true; break; } sleep(250); }
            if (!renamed) { env.printf("cannot rename %s to %s: %s (the output may be open in another program)\n", partPath, outPath, "rename failed");
                if (x.run.hadOld && !exists(outPath) && exists(partPath)) {   // FU22-2: only without java.nio.file the old file can be gone: the new one is kept
                    x.run.partMine = false; env.printf("the existing %s was removed before the rename failed; the new, checked file is kept as %s: rename it by hand\n", outPath, partPath); return 2; }
                x.oldNote(); return 2; }
            x.run.partMine = false; x.run.keepTmp = false; }   // finished: the work folder goes, also after a resumed run
        if (x.nPacks != 0) {   // the file's packs: printed, and their alignment reports go into the report beside the file
            long all = 0; for (byte[] r : x.packRecs) all += r.length;
            env.printf("extension packs: %d further pack(s), %zu B in all (pack 0 = the main body, from %s)\n", x.nPacks, all, argv[1]);
            for (int k = 0; k < x.nPacks; k++) { final PackOpt q = x.pk[k]; env.printf("  pack %d: %s%s%s%s, %zu B\n", k + 1, q.path, q.name.isEmpty() ? "" : " (\"", q.name, q.name.isEmpty() ? "" : "\")", (long) x.packRecs.get(k).length); }
            if (!x.packReport.isEmpty()) { if (x.alignReport.isEmpty()) x.alignReport = "LAE alignment report (further masters)\noutput: " + outPath + "\n"; x.alignReport += x.packReport; } }
        if (!x.alignReport.isEmpty()) { final String rp = outPath + ".align.txt"; boolean okr = true;
            try { FileOutputStream f = new FileOutputStream(rp); try { f.write(EncIo.latin1(x.alignReport)); } finally { f.close(); } } catch (IOException e) { okr = false; }
            env.printf(okr ? "alignment report: %s\n" : "WARNING: the alignment report %s could not be written (the LAE file itself is complete)\n", rp); }
        long sd = dL[0].s.get(0).length + dR[0].s.get(0).length, sf = 0; for (byte[] s : fM[0].s) sf += s.length; for (byte[] s : fS[0].s) sf += s.length;
        String extra = ""; if (!note32.isEmpty()) extra += " | " + note32; if (!x.alignNote.isEmpty()) extra += " | " + x.alignNote;
        if (x.asFlac) env.printf("FLAC form: CD layer %llu B as FLAC frames (%.1f %% of its %llu B of PCM), extension in %zu LAE1 block(s)\n", ck.cdfBytes, cdBytes != 0 ? 100.0 * ck.cdfBytes / cdBytes : 0.0, cdBytes, ck.flacPieces);
        env.printf("LAE encode OK: %s | CD %d Hz cut-off %.0f Hz | scheme %d, mode %s (share by %s) %s | CD PCM %zu B, depth %zu B, frequency %zu B in %zu octave segment(s), chunk %zu B | beyond full scale before limiter %s | %.1f s%s\n",
                   argv[2], (int) base, fc, scheme, PartitionFormat.MODES[mi].name, wm == 2 ? "cells+energy" : (wm != 0 ? "energy" : "cells"), equal ? "equal" : "unequal", cdBytes, sd, sf, (long) fM[0].s.size(), chunk,
                   nclip >= 0 ? Long.toString(nclip) : "not measured", (System.nanoTime() - T0) / 1e9, extra);
        return 0; }


    static WavIn.Info readInfo(String path) { WavIn.Info i = new WavIn.Info(); if (!WavIn.probe(path, i)) throw EncIo.fail("cannot open " + path); return i; }
    // port of lae_enc.cpp:encode lambda stamp
    static void stamp(EncEnv env, long T0, String w) { env.step(w); if (env.get("TIMING") != null) env.printf("[enc %.1f s] %s\n", (System.nanoTime() - T0) / 1e9, w); }
    // port of lae_enc.cpp:encode lambda unitSamples (as lossy_blk.cpp)
    static long unitSamples(int rate) { long seg = Math.max(1L, (long) (rate / MdctTb.N)); long u = seg; while (u % 8 != 0) u += seg; return u * MdctTb.N; }
    // port of lae_enc.cpp:encode lambda layerNeed
    static long layerNeed(long samples, long unitS, long units, long M128) { return samples + 20L * Math.min(samples, unitS * units) + M128; }
    // port of lae_enc.cpp:encode lambda fit
    static long fit(long room, long sF, long M128, long uF) { return room > sF + M128 ? (room - sF - M128) / (20L * uF) : 0L; }
    // port of lae_enc.cpp:encode lambda planFor (returns {both})
    static boolean[] planFor(long samples, long unitS, long units, long M128, boolean mayPair, long memUse, long memAvail) {
        final long u = units > 0 ? units : (samples + unitS - 1) / unitS; long need = 2 * layerNeed(samples, unitS, u, M128);
        return new boolean[] {mayPair && need <= memUse && (memAvail == 0 || need <= memAvail)}; }
    static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    // std::filesystem::rename (replaces an existing target, as MoveFileEx with MOVEFILE_REPLACE_EXISTING does)
    // FU22-2: the target is replaced in one step (java.nio.file.Files.move with ATOMIC_MOVE: MoveFileEx with MOVEFILE_REPLACE_EXISTING on
    // Windows, rename(2) elsewhere), so a rename that fails (the source held open by a scanner or indexer) leaves an existing target as it was.
    // Only where java.nio.file is missing (Android below 8.0) the target is deleted first, as before; the caller handles that case.
    static boolean rename(String from, String to) { File f = new File(from), t = new File(to); if (f.renameTo(t)) return true;
        final Boolean moved = moveReplacing(f, t); if (moved != null) return moved.booleanValue();
        if (t.isFile() && t.delete()) return f.renameTo(t); return false; }
    // Files.move(f.toPath(), t.toPath(), ATOMIC_MOVE, REPLACE_EXISTING) through reflection (the library is checked against Android 7.0, API
    // 24, which has no java.nio.file): true moved, false failed, null not available on this system
    static Boolean moveReplacing(File f, File t) {
        final java.lang.reflect.Method move; final Object a, b, opts;
        try { final java.lang.reflect.Method toPath = File.class.getMethod("toPath"); a = toPath.invoke(f); b = toPath.invoke(t);
              final Class<?> pathC = Class.forName("java.nio.file.Path"), optC = Class.forName("java.nio.file.CopyOption"), stdC = Class.forName("java.nio.file.StandardCopyOption");
              opts = java.lang.reflect.Array.newInstance(optC, 2); java.lang.reflect.Array.set(opts, 0, stdC.getField("ATOMIC_MOVE").get(null)); java.lang.reflect.Array.set(opts, 1, stdC.getField("REPLACE_EXISTING").get(null));
              move = Class.forName("java.nio.file.Files").getMethod("move", pathC, pathC, opts.getClass()); }
        catch (ReflectiveOperationException e) { return null; }
        catch (RuntimeException e) { return null; }
        try { move.invoke(null, a, b, opts); return Boolean.TRUE; }
        catch (java.lang.reflect.InvocationTargetException e) { return Boolean.FALSE; }   // IOException (in use, AtomicMoveNotSupportedException ...)
        catch (IllegalAccessException e) { return null; } }

    /** port of lae_enc.cpp:encode lambda layer */
    interface Layer { Streams run(EncEnv e, String in, int rate, String ch, String tag); }

    // port of lae_enc.cpp:encode lambda layerPair
    // two layers of the same kind at once (L and R, M and S): the second runs in its own thread with its own module variables and printed text.
    // Same arithmetic as one after the other, so the streams are byte-identical.
    static void layerPair(final EncEnv env, final Layer layer, final AtomicBoolean ok, final String in, final int rate, final String c1, final String t1, final String c2, final String t2,
                          final Streams[] o1, final Streams[] o2, boolean both) {
        if (!both) { o1[0] = layer.run(env, in, rate, c1, t1); if (ok.get()) o2[0] = layer.run(env, in, rate, c2, t2); return; }
        final EncEnv e1 = new EncEnv(env), e2 = new EncEnv(env); final Throwable[] xs = new Throwable[2];
        // one layer on this thread; an exception is kept and re-thrown on the main thread after both have finished, so the encoder stops
        // exactly as it does when the layers run one after the other (nothing written, work folder removed)
        Thread th = new Thread(new Runnable() { @Override public void run() { try { o2[0] = layer.run(e2, in, rate, c2, t2); } catch (Throwable t) { xs[1] = t; ok.set(false); } } });
        try { th.start(); } catch (Throwable t) { o1[0] = layer.run(env, in, rate, c1, t1); if (ok.get()) o2[0] = layer.run(env, in, rate, c2, t2); return; }   // no thread could be started: one after the other
        try { o1[0] = layer.run(e1, in, rate, c1, t1); } catch (Throwable t) { xs[0] = t; ok.set(false); }
        RoutesBlk.join(th);
        env.print(e1.out.toString()); env.print(e2.out.toString());
        if (xs[0] != null) throw RoutesBlk.rethrowable(xs[0]); if (xs[1] != null) throw RoutesBlk.rethrowable(xs[1]); }

    /** port of lae_enc.cpp:encode lambdas chunkPieces, fileBytes and writeFile */
    static final class Chunk {
        Ctx x; int wrate, base, stages, mi, wm; double fc; boolean equal; long nb, pb, cdBytes; short[] edge; byte[] dfl; Streams dL, dR, fM, fS;
        interface Out { boolean put(byte[] b, int off, int n); }
        // port of lae_enc.cpp:encode lambda chunkPieces
        // LAE chunk v5 (v5 = v4 + precision rule per stream, reconstruction offset 0.20, 6 modes; v4 = v3 + 16-bit backstop): header, base edges, dither
        // flags, depth L, depth R, number of frequency segments, M segments, S segments. The chunk is a list of pieces pointing at the coded streams
        // (no second copy); its CRC-32 is added up piece by piece.
        long pieces(List<byte[]> pc) {
            pc.clear();
            put32(pc, 0x3145414C); put32(pc, 5); put32(pc, wrate); put32(pc, base); put32(pc, stages); put32(pc, (int) x.Lm); put32(pc, x.P);
            put32(pc, Cv.d2i(fc)); put32(pc, mi); put32(pc, equal ? 1 : 0); put32(pc, wm); put32(pc, (int) nb); put32(pc, (int) pb);
            byte[] eb = new byte[edge.length * 2]; EncIo.toBytes(edge, 0, edge.length, eb); pc.add(eb); put32(pc, dfl.length); pc.add(dfl);
            putS(pc, dL.s.get(0)); putS(pc, dR.s.get(0)); put32(pc, fM.s.size()); for (byte[] s : fM.s) putS(pc, s); for (byte[] s : fS.s) putS(pc, s);
            for (byte[] r : x.packRecs) pc.add(r);   // extension packs: one record each, after the main body's last stream and inside the one CRC
            if (!x.mainName.isEmpty()) { final byte[] nb = utf8(x.mainName), r = new byte[5 + nb.length]; r[0] = 'L'; r[1] = 'N'; r[2] = 'A'; r[3] = 'M'; r[4] = (byte) nb.length;   // the optional "LNAM" record: the main body's name, display only
                System.arraycopy(nb, 0, r, 5, nb.length); pc.add(r); }
            int c = 0xFFFFFFFF;
            for (byte[] q : pc) for (int i = 0; i < q.length; i++) c = CRC_T[(c ^ q[i]) & 0xFF] ^ (c >>> 8);   // = crc32() of part_stream.h over the whole chunk
            put32(pc, c ^ 0xFFFFFFFF);   // v3: CRC-32 of everything before it (integrity check, whole chunk)
            long n = 0; for (byte[] q : pc) n += q.length; return n; }
        static void put32(List<byte[]> pc, int v) { byte[] b = new byte[4]; com.lae.core.Le.put32(b, 0, v); pc.add(b); }
        static void putS(List<byte[]> pc, byte[] s) { put32(pc, s.length); pc.add(s); }
        // FLAC form (2026-11-10): the CD layer coded once into FLAC frames (block size 4096, two independent channels, 16 bits: the frame
        // writer of the LAES FLAC form), kept in the work folder as cd.flacframes; STREAMINFO needs the smallest and the largest frame and
        // the MD5 of the samples. Coded in batches; each frame depends only on its own samples, so the bytes do not depend on the threads.
        // port of lae_enc.cpp:encode struct CdFlac and lambda makeCdFlac
        boolean cdfMade = false; long cdfMin = 0, cdfMax = 0, cdfBytes = 0, flacPieces = 0; byte[] cdfMd5 = new byte[16];
        boolean makeCdFlac() { if (cdfMade) return true; final EncEnv env = x.env; final String path = x.T("cd.flacframes");
            final long n = nb - 2 * pb; final int B = 4096; final long nf = (n + B - 1) / B; if (n < 16) { env.printf("the CD layer is too short for the FLAC form (fewer than 16 samples): nothing written\n"); return false; }
            BlkIo.ReaderS r = new BlkIo.ReaderS(x.T("b16.s16")); AlignHash.Md5 md = new AlignHash.Md5(); BufferedOutputStream f;
            try { f = new BufferedOutputStream(new FileOutputStream(path), 1 << 16); } catch (IOException e) { r.close(); env.printf("cannot write %s\n", path); return false; }
            final int batch = 512; long fmin = 0xFFFFFF, fmax = 0, bytes = 0; boolean okw = true; byte[] xb = new byte[0];
            final int th = Math.min(16, Math.max(1, Runtime.getRuntime().availableProcessors()));
            try {
                for (long k0 = 0; k0 < nf && okw; k0 += batch) { env.checkCancel(); final long k1 = Math.min(nf, k0 + batch), a = k0 * B, b = Math.min(n, k1 * B); final int cnt = (int) (k1 - k0), len = (int) (b - a);
                    final short[] s = r.read(pb + a, pb + b); if (xb.length != s.length * 2) xb = new byte[s.length * 2]; EncIo.toBytes(s, 0, s.length, xb); md.update(xb, 0, xb.length);   // 16-bit little-endian, interleaved: the bytes STREAMINFO's MD5 is defined on
                    final int[] c0 = new int[len], c1 = new int[len]; for (int i = 0; i < len; i++) { c0[i] = s[i * 2]; c1[i] = s[i * 2 + 1]; }
                    final com.lae.laes.FlacEnc.Ch[] ch = {new com.lae.laes.FlacEnc.IntCh(c0), new com.lae.laes.FlacEnc.IntCh(c1)}; final byte[][] fr = new byte[cnt][]; final long first = k0; final int rateF = base;
                    com.lae.laes.Par.forBlocks(cnt, th, new com.lae.laes.Par.Block() { public void run(int k) { fr[k] = com.lae.laes.FlacEnc.encodeFrame(ch, (long) k * B, (int) Math.min((long) B, len - (long) k * B), first + k, rateF, 16); } }, null);
                    for (byte[] q : fr) { fmin = Math.min(fmin, q.length); fmax = Math.max(fmax, q.length); bytes += q.length; try { f.write(q); } catch (IOException e) { okw = false; } } }
            } finally { r.close(); try { f.close(); } catch (IOException e) { okw = false; } }
            if (!okw) { env.printf("cannot write %s\n", path); return false; }
            final String h = md.hex(); for (int i = 0; i < 16; i++) cdfMd5[i] = (byte) Integer.parseInt(h.substring(i * 2, i * 2 + 2), 16);
            cdfMin = fmin; cdfMax = fmax; cdfBytes = bytes; cdfMade = true; return true; }
        // the bytes of the FLAC form: "fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (the tags, if any) | the chunk body in "LAE1" APPLICATION
        // blocks of at most 2^24 - 5 bytes each (the last one carries the "last metadata block" flag) | the audio frames. No SEEKTABLE,
        // no CUESHEET, no padding, nothing between the pieces.
        // port of lae_enc.cpp:encode lambda fileBytesFlac
        boolean fileBytesFlac(List<byte[]> pc, long laeN, Out f) {
            final long n = nb - 2 * pb; final int bs = n >= 4096 ? 4096 : (int) Math.max(16L, n);
            final byte[] o = new byte[42]; o[0] = 'f'; o[1] = 'L'; o[2] = 'a'; o[3] = 'C'; o[7] = 34;   // block header: not last, type 0, length 34
            o[8] = (byte) (bs >> 8); o[9] = (byte) bs; o[10] = (byte) (bs >> 8); o[11] = (byte) bs;
            o[12] = (byte) (cdfMin >> 16); o[13] = (byte) (cdfMin >> 8); o[14] = (byte) cdfMin; o[15] = (byte) (cdfMax >> 16); o[16] = (byte) (cdfMax >> 8); o[17] = (byte) cdfMax;
            final long v = (long) base << 44 | 1L << 41 | 15L << 36 | n; for (int i = 0; i < 8; i++) o[18 + i] = (byte) (v >>> (8 * (7 - i)));   // rate (20 bits), channels - 1 (3), bits - 1 (5), samples (36)
            System.arraycopy(cdfMd5, 0, o, 26, 16);
            if (!f.put(o, 0, o.length)) return false; if (x.tagChunks.length != 0 && !f.put(x.tagChunks, 0, x.tagChunks.length)) return false;
            long left = laeN, room = 0, blocks = 0; long pieceMax = LaeyFlac.kPiece;
            { final String tp = x.env.get("LAEY_TEST_PIECE"); if (tp != null) { final long tv = Fmt.atoi(tp); if (tv >= 1 && tv < LaeyFlac.kPiece) pieceMax = tv; } }   // test only: smaller pieces, to exercise the split
            for (byte[] q : pc) { int p = 0, k = q.length;
                while (k > 0) { if (room == 0) { room = Math.min(pieceMax, left); final long L = room + 4; final byte[] bh = {(byte) ((left == room ? 0x80 : 0) | 2), (byte) (L >> 16), (byte) (L >> 8), (byte) L, 'L', 'A', 'E', '1'};
                        blocks++; if (!f.put(bh, 0, 8)) return false; }
                    final int t = (int) Math.min((long) k, room); if (!f.put(q, p, t)) return false; p += t; k -= t; room -= t; left -= t; } }
            if (left != 0 || room != 0) return false; flacPieces = blocks;
            FileInputStream ff; try { ff = new FileInputStream(x.T("cd.flacframes")); } catch (IOException e) { return false; }
            try { final byte[] buf = new byte[1 << 20]; long total = 0;
                  for (;;) { final int k = ff.read(buf); if (k < 0) break; if (k == 0) continue; total += k; if (!f.put(buf, 0, k)) return false; }
                  return total == cdfBytes; }
            catch (IOException e) { return false; }
            finally { try { ff.close(); } catch (IOException e) { /* a reader */ } } }
        boolean fileBytes(List<byte[]> pc, long laeN, Out f) { return x.asFlac ? fileBytesFlac(pc, laeN, f) : fileBytesWav(pc, laeN, f); }
        // port of lae_enc.cpp:encode lambda fileBytesWav: the bytes of the output file (as wavio::writeCD writes them), handed to f piece by piece
        boolean fileBytesWav(List<byte[]> pc, long laeN, Out f) {
            final long extra = laeN == 0 ? 0 : 8 + laeN + (laeN & 1); byte[] o = WavIo.hdr(base, 2, 16, cdBytes, extra + x.tagChunks.length);
            if (x.tagChunks.length != 0) { if (!f.put(o, 0, 36) || !f.put(x.tagChunks, 0, x.tagChunks.length) || !f.put(o, 36, o.length - 36)) return false; }   // tags after "fmt " (RIFF 12 + fmt 24 bytes), before "data"
            else if (!f.put(o, 0, o.length)) return false;
            BlkIo.ReaderS r = new BlkIo.ReaderS(x.T("b16.s16")); try { final long BS = 1L << 18; byte[] b = new byte[0];
                for (long a = pb; a < nb - pb; a += BS) { x.env.checkCancel(); short[] s = r.read(a, Math.min(nb - pb, a + BS)); if (b.length != s.length * 2) b = new byte[s.length * 2]; EncIo.toBytes(s, 0, s.length, b); if (!f.put(b, 0, b.length)) return false; } }
            finally { r.close(); }
            if ((cdBytes & 1) != 0 && !f.put(new byte[1], 0, 1)) return false;
            if (laeN != 0) { byte[] h = {'L', 'A', 'E', ' ', 0, 0, 0, 0}; com.lae.core.Le.put32(h, 4, (int) laeN); if (!f.put(h, 0, 8)) return false; }
            for (byte[] q : pc) if (q.length != 0 && !f.put(q, 0, q.length)) return false;
            return (laeN & 1) == 0 || f.put(new byte[1], 0, 1); }
        // port of lae_enc.cpp:encode lambda writeFile: written to <out>.part and read back: the file on disk must hold exactly this CD layer and this chunk
        long writeFile() { final EncEnv env = x.env; List<byte[]> pc = new ArrayList<byte[]>(); final long laeN = pieces(pc);
            if (x.asFlac) { if (laeN == 0 || laeN > 0x7FFFFFFFL || !makeCdFlac()) { if (laeN > 0x7FFFFFFFL) env.printf("the extension would exceed 2 GB: nothing written\n"); return 0; } }
            else if (!WavIo.fits32(cdBytes, 8L + laeN + 1 + x.tagChunks.length)) { env.printf("output would exceed the 4 GB limit of WAV: nothing written\n"); return 0; }
            x.run.partMine = true; x.markDone("part");   // resume: this run owns the .part from here on
            boolean okw = false; { BufferedOutputStream[] f = {null}; try { f[0] = new BufferedOutputStream(new FileOutputStream(x.partPath), 1 << 16); } catch (IOException e) { f[0] = null; }
                if (f[0] != null) { final BufferedOutputStream fo = f[0];
                    try { okw = fileBytes(pc, laeN, new Out() { @Override public boolean put(byte[] b, int off, int n) { try { fo.write(b, off, n); return true; } catch (IOException e) { return false; } } }); }
                    finally { try { fo.close(); } catch (IOException e) { okw = false; } } } }
            if (!okw || EncIo.testFail(env, "partwrite")) { env.printf("cannot write %s\n", x.partPath); return 0; }
            boolean okr = false; { BufferedInputStream fi = null; try { fi = new BufferedInputStream(new FileInputStream(x.partPath), 1 << 16); } catch (IOException e) { fi = null; }
                if (fi != null) { final BufferedInputStream g = fi; final long[] total = {0};
                    try { okr = fileBytes(pc, laeN, new Out() { @Override public boolean put(byte[] b, int off, int n) { total[0] += n; byte[] r = new byte[n]; int got = 0;
                              try { while (got < n) { int k = g.read(r, got, n - got); if (k < 0) return false; got += k; } } catch (IOException e) { return false; }
                              for (int i = 0; i < n; i++) if (r[i] != b[off + i]) return false; return true; } });
                          okr = okr && fileSize(x.partPath) == total[0]; }
                    finally { try { g.close(); } catch (IOException e) { /* a reader */ } } } }
            if (!okr || EncIo.testFail(env, "partverify")) { env.printf("read-back check FAILED: %s does not hold what was written (disk full or failing?)\n", x.partPath); return 0; }
            return laeN; }
    }
    private static final int[] CRC_T = new int[256];
    static { for (int i = 0; i < 256; i++) { int v = i; for (int k = 0; k < 8; k++) v = (v & 1) != 0 ? 0xEDB88320 ^ (v >>> 1) : v >>> 1; CRC_T[i] = v; } }

    static String hex(byte[] d) { return WavIn.hex(d); }
    static boolean same(byte[] a, byte[] b) { return Arrays.equals(a, b); }
}
