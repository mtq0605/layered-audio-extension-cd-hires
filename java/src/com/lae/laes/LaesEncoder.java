// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicBoolean;

import com.lae.core.LaeException;

/**
 * The LAES encoder: port of `laes_tool enc` (src/laes_tool.cpp main, the "enc" branch, with readWav, wavBytes, setT, selfCheckFile and the
 * memory rule pieceFor). The masters are read into a work folder next to the output (&lt;out&gt;.laes.tmp), their levels and layers are coded
 * piece by piece (StreamEnc), the file is written as &lt;out&gt;.part, read back (FLAC form: the APPLICATION blocks and the audio frames;
 * then every master decoded by the playback decoder Play and compared with its input), and only then renamed to &lt;out&gt;. On any failure
 * nothing is left behind (the .part file and the work folder are removed), as in the C++.
 *
 * The file written does not depend on the memory budget (piece length), the thread count or the cancellation checks.
 * Not ported: master.wav@auto (LAEY's alignment search, laes_tool autoAlign / acceptAlign, which runs lae_enc cdcheck): Options.aligner is
 * the hook; without one, @auto is refused with a message.
 *
 * Memory (Java heap): the core (int32 per sample: 8 bytes per base-rate frame), one piece of the lifting / reference / block coder (about
 * JAVA_BYTES_PER_TOP bytes per top-level sample of a piece, see pieceFor), the FLAC frames 256 at a time; everything else is in the work folder.
 * The self-check runs after the core is released and holds the playback decoder's core copy (int32, 8 bytes per frame) and its windows.
 */
public final class LaesEncoder {
    private LaesEncoder() {}

    /** the lines the C++ prints (without the line end) and the progress of the stages (fraction 0..1) */
    public interface Listener { void message(String line); void progress(String stage, double fraction); }
    /** one master argument: path, T (base samples), or @auto */
    public static final class MasterArg { public String path; public double T = 0; public boolean auto = false;
        public MasterArg(String p, double t, boolean a) { path = p; T = t; auto = a; } }
    /** port of laes_tool.cpp:AutoT (the result of LAEY's alignment search; not ported, see Aligner) */
    public static final class AutoT { public boolean found = false; public double T = 0, err = 0, resid = 0; public String search = "", notes = "", why = ""; }
    /**
     * Hook for master.wav@auto (laes_tool autoAlign: LAEY's search lae_enc cdcheck against the 16-bit core master, then the own check of the
     * residual). align() returns the result; ask() is the question "use this alignment?" when the error is worse than -60 dB and --accept
     * was not given (the C++ reads y/N from stdin). The LAEY alignment is ported separately (com.lae.laey); connect it here.
     */
    public interface Aligner { AutoT align(String corePath, StreamEnc.Core core, String masterPath, StreamEnc.Prep m, String laeEnc); boolean ask(String question); }

    /** the command line of `laes_tool enc` as an object */
    public static final class Options {
        public String out = "";
        /** --core=&lt;core.wav&gt; ("" = none) */
        public String core = "";
        /** --derive-core */
        public boolean deriveCore = false;
        /** --core-bits=16|24|32; 0 = the default (16 for WAV, for FLAC the highest master word length up to 24); any other value is refused */
        public int coreBits = 0;
        public List<MasterArg> masters = new ArrayList<MasterArg>();
        /** --mem=&lt;GB&gt;; negative = the default rule (pieceFor) */
        public double memGB = -1;
        /** test hook LAES_PIECE (base-level samples per piece); 0 = from the memory budget */
        public long piece = 0;
        /** threads; 0 = the C++ default (all logical processors) */
        public int threads = 0;
        /** --timing */
        public boolean timing = false;
        /** --accept (for @auto) */
        public boolean accept = false;
        /** --lae-enc=&lt;path&gt; (for @auto) */
        public String laeEnc = "";
        /** test hook LAES_FORCE_RF64: the WAV form as RF64 even below 4 GB */
        public boolean forceRf64 = false;
        /** test hook LAES_SELFCHECK_FLIP: one byte of the written file flipped at this offset before the self-check (from the end when negative) */
        public String selfCheckFlip = null;
        /** KEEPTMP: the work folder is kept */
        public boolean keepTmp = false;
        /** the @auto hook (null: @auto is refused) */
        public Aligner aligner = null;
        /** --tag / --cover / --picture (2026-11-05): written into the release file; empty = none (the file is the same as without) */
        public LaesTags.Tags tags = new LaesTags.Tags();
        /** fromArgs: the reason when --tag / --cover / --picture could not be read ("" = fine); reported after the form check, as the C++ */
        public String tagError = "";
        /** --form=flac|wav ("" = from the name; for .lae: flac) */
        public String form = "";
        /** --small-disk (2026-11-05): a FLAC master is not decoded into the work folder and the largest rate level of each master is not kept
         *  (both made again when read; slower, half the work folder); half the piece */
        public boolean smallDisk = false;
        /** --disk-stop / --disk-stop-remove: below the reserve the run stops (work folder kept / removed) instead of waiting; --force-disk:
         *  runs although the estimate is more than is free */
        public boolean diskStop = false, diskStopRemove = false, forceDisk = false;
        /** bytes kept free on the work folder's disk (test hook LAES_DISK_RESERVE; default 2 GB) */
        public long diskReserve = 2L << 30;
        /** the work folder (null: &lt;out&gt;.laes.tmp; Java only, the app's cache folder) */
        public String workDir = null;

        // port of laes_tool.cpp:arg  (args: the C++ argv without argv[0], i.e. args[0] = "enc")
        static String arg(String[] args, String key) { final String k = key + "="; for (String a : args) if (a.startsWith(k)) return a.substring(k.length()); return ""; }
        // port of laes_tool.cpp:flag
        static boolean flag(String[] args, String key) { for (String a : args) if (a.equals(key)) return true; return false; }
        /** the options from `laes_tool enc` arguments: args[0] = "enc", args[1] = out, then masters and options (as the C++ argv[1..]) */
        public static Options fromArgs(String[] args) {
            Options o = new Options(); o.out = args.length > 1 ? args[1] : ""; o.core = arg(args, "--core"); o.deriveCore = flag(args, "--derive-core");
            final String cb = arg(args, "--core-bits"); if (!cb.isEmpty()) { o.coreBits = CStd.atoi(cb); if (o.coreBits == 0) o.coreBits = -1; }
            final String mm = arg(args, "--mem"); if (!mm.isEmpty()) o.memGB = Math.max(0.0, CStd.atof(mm));
            o.timing = flag(args, "--timing"); o.accept = flag(args, "--accept"); o.laeEnc = arg(args, "--lae-enc"); o.form = arg(args, "--form");
            o.smallDisk = flag(args, "--small-disk"); o.diskStopRemove = flag(args, "--disk-stop-remove"); o.diskStop = o.diskStopRemove || flag(args, "--disk-stop"); o.forceDisk = flag(args, "--force-disk");
            { final String r = System.getenv("LAES_DISK_RESERVE"); if (r != null) o.diskReserve = CStd.atoll(r); }
            o.tagError = LaesTags.parseArgs(args, o.tags);
            final String th = arg(args, "--threads"); if (!th.isEmpty()) o.threads = CStd.atoi(th);   // Java only (the C++ skips arguments starting with '-')
            for (int i = 2; i < args.length; i++) { if (args[i].length() > 0 && args[i].charAt(0) == '-') continue; String a = args[i]; double T = 0; boolean au = false; final int at = a.lastIndexOf('@');
                if (at >= 0 && at > 2) { au = a.substring(at + 1).equals("auto"); T = au ? 0 : CStd.atof(a.substring(at + 1)); a = a.substring(0, at); }
                o.masters.add(new MasterArg(a, T, au)); }
            final String pe = System.getenv("LAES_PIECE"); if (pe != null) o.piece = Math.max(4096, CStd.atoll(pe) / 4096 * 4096);
            o.forceRf64 = System.getenv("LAES_FORCE_RF64") != null; o.selfCheckFlip = System.getenv("LAES_SELFCHECK_FLIP"); o.keepTmp = System.getenv("KEEPTMP") != null;
            return o; }
    }

    /** bytes of Java heap per top-level sample of a piece (measured: see the port report; the C++ uses 160) */
    public static final long JAVA_BYTES_PER_TOP = 160;
    // base-level samples per piece, a multiple of 4096. Piece length from the memory limit (--mem=<GB>, or by default 2 GB, less where the
    // machine has less); the files written do not depend on it.
    // Java: the default budget is min(2 GB, half the maximum heap) (the C++ rule uses the physical memory, which Java on Android cannot read);
    // the lower limit is 32 MB (C++: 512 MB, more than an Android app's heap).
    // port of laes_tool.cpp:main::pieceFor (the lambda)
    public static long pieceFor(Options o, int maxStages) {
        if (o.piece > 0) return Math.max(4096, o.piece / 4096 * 4096);   // test hook
        long budget; if (o.memGB >= 0) budget = (long) (o.memGB * 1073741824.0); else budget = Math.min(2L << 30, Runtime.getRuntime().maxMemory() / 2);
        if (budget < (32L << 20)) budget = 32L << 20;
        final long top = budget / JAVA_BYTES_PER_TOP;
        // --small-disk (2026-11-05): half the piece, so that its extra buffers (the level made again, FLAC frames decoded again) stay
        // within the memory of a normal run; the files written do not depend on the piece
        final long topUsed = o.smallDisk ? top / 2 : top;
        return Math.max(4096, (topUsed >> maxStages) / 4096 * 4096); }

    // T (base samples, decimal) -> whole + fraction in 2^-32 base samples; returns {whole, frac as uint32}
    // port of laes_tool.cpp:setT
    public static long[] setT(double T) { long whole = com.lae.core.Cv.d2l(Math.floor(T)); final int frac = (int) com.lae.core.Cv.llround((T - Math.floor(T)) * 4294967296.0); if (frac == 0 && T != Math.floor(T)) whole += 1; return new long[] {whole, frac & 0xFFFFFFFFL}; }

    // ---- WAV read (16/24/32-bit integer PCM, stereo; PCM or EXTENSIBLE with the PCM sub-format), for the core
    // port of laes_tool.cpp:readWav  (the C++ reads the whole file into memory; here the chunk headers, then the samples; the same checks)
    public static String readWav(String path, StreamEnc.Core p) {
        RandomAccessFile f; try { f = new RandomAccessFile(path, "r"); } catch (IOException e) { return "cannot read " + path; }
        try { final long size = f.length(); final byte[] h = new byte[12]; if (size < 12) return path + ": not RIFF/WAVE"; f.seek(0); f.readFully(h); if (!StreamEnc.eq4(h, 0, "RIFF") || !StreamEnc.eq4(h, 8, "WAVE")) return path + ": not RIFF/WAVE";
            long pos = 12; int tag = 0, ch = 0, rate = 0, bits = 0; long data = -1, dlen = 0; final byte[] c = new byte[8];
            while (pos + 8 <= size) { f.seek(pos); f.readFully(c); long n = (c[4] & 0xFF) | (c[5] & 0xFF) << 8 | (c[6] & 0xFF) << 16 | (long) (c[7] & 0xFF) << 24; final long body = pos + 8; if (n > size - body) n = size - body;
                if (StreamEnc.eq4(c, 0, "fmt ") && n >= 16) { final byte[] b = new byte[26]; f.seek(body); f.readFully(b, 0, (int) Math.min(26, n)); tag = (b[0] & 0xFF) | (b[1] & 0xFF) << 8; ch = (b[2] & 0xFF) | (b[3] & 0xFF) << 8;
                    rate = (b[4] & 0xFF) | (b[5] & 0xFF) << 8 | (b[6] & 0xFF) << 16 | (b[7] & 0xFF) << 24; bits = (b[14] & 0xFF) | (b[15] & 0xFF) << 8; if (tag == 0xFFFE && n >= 26) tag = (b[24] & 0xFF) | (b[25] & 0xFF) << 8; }
                else if (StreamEnc.eq4(c, 0, "data")) { data = body; dlen = n; }
                pos = body + n + (n & 1); }
            if (tag != 1 || ch != 2 || (bits != 16 && bits != 24 && bits != 32) || data < 0) return path + ": need 16/24/32-bit integer PCM, 2 channels";
            final int by = bits / 8; final long n = dlen / (2 * by); p.rate = rate; p.bits = bits;
            p.c = new int[][] {new int[(int) n], new int[(int) n]}; final int P = 1 << 16; final byte[] b = new byte[P * 2 * by];
            for (long a = 0; a < n; a += P) { final int k = (int) Math.min(P, n - a); f.seek(data + a * 2 * by); f.readFully(b, 0, k * 2 * by);
                for (int i = 0; i < k; i++) for (int cc = 0; cc < 2; cc++) { final int q = (i * 2 + cc) * by; int v = 0; for (int t = 0; t < by; t++) v |= (b[q + t] & 0xFF) << (8 * t);
                    if (bits < 32 && ((v >>> (bits - 1)) & 1) != 0) v |= -1 << bits; p.c[cc][(int) (a + i)] = v; } }
            return ""; }
        catch (IOException e) { return "cannot read " + path; }
        finally { try { f.close(); } catch (IOException e) { /* read only */ } } }
    // ---- WAV header: 16-bit PCM (for the core) or EXTENSIBLE for 24/32 bit, of an empty file (the sizes are patched by the caller)
    // port of laes_tool.cpp:wavBytes (with no samples and no "LAE " chunk)
    public static byte[] wavHeader(int rate, int bits) {
        final int by = bits / 8; final boolean ext = bits > 16; final int fmtLen = ext ? 40 : 16; final byte[] o = new byte[12 + 8 + fmtLen + 8]; int p = 0;
        o[p++] = 'R'; o[p++] = 'I'; o[p++] = 'F'; o[p++] = 'F'; com.lae.core.Le.put32(o, p, 4 + 8 + fmtLen + 8); p += 4; final byte[] wf = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}; System.arraycopy(wf, 0, o, p, 8); p += 8; com.lae.core.Le.put32(o, p, fmtLen); p += 4;
        com.lae.core.Le.put16(o, p, ext ? 0xFFFE : 1); p += 2; com.lae.core.Le.put16(o, p, 2); p += 2; com.lae.core.Le.put32(o, p, rate); p += 4; com.lae.core.Le.put32(o, p, rate * 2 * by); p += 4; com.lae.core.Le.put16(o, p, 2 * by); p += 2; com.lae.core.Le.put16(o, p, bits); p += 2;
        if (ext) { com.lae.core.Le.put16(o, p, 22); p += 2; com.lae.core.Le.put16(o, p, bits); p += 2; com.lae.core.Le.put32(o, p, 3); p += 4; final byte[] guid = {1, 0, 0, 0, 0, 0, 0x10, 0, (byte) 0x80, 0, 0, (byte) 0xAA, 0, 0x38, (byte) 0x9B, 0x71}; System.arraycopy(guid, 0, o, p, 16); p += 16; }
        o[p++] = 'd'; o[p++] = 'a'; o[p++] = 't'; o[p++] = 'a'; com.lae.core.Le.put32(o, p, 0); return o; }

    // ---- the work folder <out>.laes.tmp (removed when the run ends; kept when an encoding stops, for the same command to continue)
    // job (enc, 2026-11-05): a work folder made for the same job (job.txt equal) is used again: the run continues where an earlier one stopped
    // port of laes_tool.cpp:main::workDir (the lambda; wd: the folder, resumed[0] receives whether it was used again)
    static String workDir(String wd, String job, boolean[] resumed) { final File d = new File(wd);
        if (d.exists() && !new File(wd + "/laes.tmp").exists()) throw new LaeException(wd + " exists and is not a laes work folder: remove it or choose another output name");
        if (resumed != null) { resumed[0] = false; final String old = job.isEmpty() ? null : StreamEnc.getState(wd + "/job.txt"); if (old != null && old.equals(job)) { resumed[0] = true; return wd; } }
        removeAll(d); d.mkdirs(); if (!d.isDirectory()) throw new LaeException("cannot create the work folder " + wd);
        { Stream.Out o = new Stream.Out(wd + "/laes.tmp"); final byte[] t = "laes work folder (removed when the run ends; kept when an encoding stops, for the same command to continue)\n".getBytes(); o.put(t, 0, t.length); o.close(); }
        if (!job.isEmpty()) StreamEnc.putState(wd + "/job.txt", job); return wd; }
    // the work folder of another command (dec, verify): <out>.laes.tmp, made new
    static String workDir(String out) { return workDir(out + ".laes.tmp", "", null); }
    /** the work folder of an encode (Options.workDir, or &lt;out&gt;.laes.tmp) */
    public static String workDirOf(Options o) { return o.workDir != null ? o.workDir : o.out + ".laes.tmp"; }
    /** the version in job.txt: a work folder made by another version of this encoder is not used again */
    public static final String JOB_VERSION = "java 2026-11-06 (laes_tool 77dc9ef)";
    // std::filesystem::remove_all (errors ignored, as with the error_code overload): only ever called on a laes work folder (it holds "laes.tmp")
    static void removeAll(File f) { if (!f.exists()) return; if (f.isDirectory()) { final File[] k = f.listFiles(); if (k != null) for (File c : k) removeAll(c); } f.delete(); }

    // "  %-22s %8.2f s"
    static String stageLine(String s, double secs) { StringBuilder b = new StringBuilder("  ").append(s); while (b.length() < 2 + 22) b.append(' '); return b.append(' ').append(CStd.f(secs, 8, 2)).append(" s").toString(); }


    // port of laes_tool.cpp:main ("enc"): returns the exit code (0 written, 1 bad arguments, 2 refused / failed; nothing written)
    public static int encode(final Options opt, final Listener listener, AtomicBoolean cancel) {
        final StreamEnc.Ctx ctx = new StreamEnc.Ctx(); ctx.threads = Par.threadsOr(opt.threads); ctx.cancel = cancel != null ? cancel : new AtomicBoolean(false); ctx.listener = listener;
        try { return encodeImpl(opt, listener, ctx); }
        catch (OutOfMemoryError x) { stopped(opt, listener, "bad allocation"); return 2; }
        catch (RuntimeException x) { stopped(opt, listener, String.valueOf(x.getMessage())); return 2; }
        finally { StreamEnc.diskGuard(null); } }
    // the catch of laes_tool.cpp:main ("enc"): the .part removed; a work folder kept for the same command to continue is named
    static void stopped(Options opt, Listener L, String why) { new File(opt.out + ".part").delete(); say(L, "stopped: " + why + "; nothing written");
        final String wd = workDirOf(opt);
        if (new File(wd + "/job.txt").exists()) { say(L, "the work folder " + wd + " is kept: run the same command again to continue (--disk-stop-remove: removed instead)");
            say(L, "  工作文件夹 " + wd + " 已保留：再运行同一条命令，就从停下的地方接着做（加 --disk-stop-remove 时改为删除）"); } }
    static void say(Listener l, String s) { if (l != null) l.message(s); }
    static String gb(double v) { return CStd.f(v / 1073741824.0, 1); }
    // the job a work folder belongs to (port of laes_tool.cpp:main "enc", job): the arguments the files depend on and every master's size
    // and time; Java: from the options (the C++ takes the arguments except --timing, --disk-stop*, --force-disk, --mem and the tags)
    static String jobOf(Options o, List<String> paths) {
        final StringBuilder j = new StringBuilder("laes enc work folder, " + JOB_VERSION + "\n");
        j.append(o.out).append('\n').append("core=").append(o.core).append('\n').append("derive=").append(o.deriveCore).append('\n').append("core-bits=").append(o.coreBits).append('\n')
         .append("form=").append(o.form).append('\n').append("small-disk=").append(o.smallDisk).append('\n').append("accept=").append(o.accept).append('\n').append("lae-enc=").append(o.laeEnc).append('\n');
        for (MasterArg a : o.masters) j.append(a.path).append('@').append(a.auto ? "auto" : Double.toString(a.T)).append('\n');
        for (String p : paths) { final File f = new File(p); j.append(p).append('\t').append(f.length()).append('\t').append(f.lastModified()).append('\n'); }
        boolean anyAuto = false; for (MasterArg a : o.masters) anyAuto |= a.auto;
        for (String p : new String[] {o.core, anyAuto ? o.laeEnc : ""}) if (!p.isEmpty() && new File(p).isFile()) { final File f = new File(p); j.append(p).append('\t').append(f.length()).append('\t').append(f.lastModified()).append('\n'); }
        return j.toString(); }

    private static int encodeImpl(final Options opt, final Listener L, final StreamEnc.Ctx ctx) {
        final String out = opt.out, corePath = opt.core; final boolean derive = opt.deriveCore;
        // --timing: prints the time of each stage at the end (L5, 2026-11-03); the file written does not change
        final boolean timing = opt.timing; final StringBuilder tlog = new StringBuilder(); final long[] t0 = {System.nanoTime()};
        final List<String> paths = new ArrayList<String>(); final List<Double> Ts = new ArrayList<Double>(); final List<Boolean> autoT = new ArrayList<Boolean>(); final List<StreamEnc.WavInfo> wis = new ArrayList<StreamEnc.WavInfo>();
        for (MasterArg a : opt.masters) { StreamEnc.WavInfo wi = new StreamEnc.WavInfo(); final String e = StreamEnc.probeMaster(a.path, wi); if (!e.isEmpty()) { say(L, e); return 2; } paths.add(a.path); Ts.add(a.T); autoT.add(a.auto); wis.add(wi); }
        if (paths.isEmpty()) { say(L, "no master given"); return 1; }
        {   // file order: more bits first, then higher rate (a master can only refer to earlier ones; a shorter word refers to a longer one cheaply)
            final List<Integer> ord = new ArrayList<Integer>(); for (int i = 0; i < paths.size(); i++) ord.add(i);
            Collections.sort(ord, new Comparator<Integer>() { public int compare(Integer a, Integer b) { final StreamEnc.WavInfo x = wis.get(a), y = wis.get(b);   // std::stable_sort: Collections.sort is stable
                if (x.bits != y.bits) return x.bits > y.bits ? -1 : 1; if (x.rate != y.rate) return x.rate > y.rate ? -1 : 1; return 0; } });
            final List<String> p2 = new ArrayList<String>(); final List<Double> t2 = new ArrayList<Double>(); final List<Boolean> a2 = new ArrayList<Boolean>(); final List<StreamEnc.WavInfo> w2 = new ArrayList<StreamEnc.WavInfo>();
            for (int k : ord) { p2.add(paths.get(k)); t2.add(Ts.get(k)); a2.add(autoT.get(k)); w2.add(wis.get(k)); }
            paths.clear(); paths.addAll(p2); Ts.clear(); Ts.addAll(t2); autoT.clear(); autoT.addAll(a2); wis.clear(); wis.addAll(w2); }
        // the form: .flac = FLAC, .wav = WAV, .lae (the format's own name, 2026-11-05) = --form=flac (default) or --form=wav
        final String formArg = opt.form; final boolean laeName = out.length() > 4 && out.substring(out.length() - 4).equalsIgnoreCase(".lae");
        if (!formArg.isEmpty() && !formArg.equals("flac") && !formArg.equals("wav")) { say(L, "--form must be flac or wav"); return 1; }
        final boolean asFlac = laeName ? !formArg.equals("wav") : out.length() > 5 && out.substring(out.length() - 5).equalsIgnoreCase(".flac");
        if (!formArg.isEmpty() && !laeName && formArg.equals("flac") != asFlac) { say(L, "--form=" + formArg + " does not match the name " + out + " (use .lae to choose the form freely)"); return 1; }
        if (!opt.tagError.isEmpty()) { say(L, opt.tagError); return 1; }   // before any work
        final int base = wis.get(0).rate % 44100 == 0 ? 44100 : 48000;
        int maxBits = 0, maxSt = 0; for (StreamEnc.WavInfo w : wis) { maxBits = Math.max(maxBits, w.bits); int st = 0; while ((base << st) < w.rate && st < 4) st++; maxSt = Math.max(maxSt, st); }
        final int coreBits = opt.coreBits != 0 ? opt.coreBits : (asFlac ? Math.min(maxBits, 24) : 16);
        if (coreBits != 16 && coreBits != 24 && coreBits != 32) { say(L, "--core-bits must be 16, 24 or 32"); return 1; }
        if (corePath.isEmpty() && !derive) { say(L, "give --core=<core.wav> or --derive-core"); return 1; }
        final String wdPath = workDirOf(opt), job = jobOf(opt, paths);
        long oldWork = 0; { final String old = StreamEnc.getState(wdPath + "/job.txt"); if (old != null && old.equals(job)) oldWork = StreamEnc.folderBytes(wdPath); }   // counts as free
        final int[] stOf = new int[wis.size()]; for (int i = 0; i < wis.size(); i++) { int st = 0; while ((base << st) < wis.get(i).rate && st < 4) st++; stOf[i] = st; }
        {   // disk needed by the work folder, estimated before it is made (2026-11-05): a FLAC master's decoded copy and the kept rate levels
            // (4 bytes per value, 8 for a 32-bit master; one more level while the next is made) exactly; then two candidates of the largest
            // master at a time, and the two payloads and the output: 1.5 x the masters' PCM (a payload is usually far smaller)
            long need = 0, pcm = 0, big = 0;
            for (int i = 0; i < wis.size(); i++) { final StreamEnc.WavInfo w = wis.get(i); final int st = stOf[i]; final long by = w.bits == 32 ? 8 : 4; if (w.flac && !opt.smallDisk) need += w.dataBytes;
                final boolean[] keep = new boolean[st + 1]; keep[0] = true; for (int k = 0; k < stOf.length; k++) if (k != i) keep[Math.min(st, stOf[k])] = true; long lv = 0, top = 0;
                for (int k = 0; k < st; k++) { final long b = LaesFile.levLen(w.frames, st - k) * 2 * by; if (keep[k] && !(opt.smallDisk && k == st - 1)) lv += b; else top = Math.max(top, b); }
                need += lv + top; pcm += w.dataBytes; big = Math.max(big, w.dataBytes); }
            need += 2 * big + pcm * 3 / 2;
            File dir = new File(wdPath).getAbsoluteFile().getParentFile(); while (dir != null && !dir.exists()) dir = dir.getParentFile();
            final boolean ec = dir == null; final long avail = (ec ? 0 : dir.getUsableSpace()) + oldWork;
            if (!ec && need + opt.diskReserve > avail) { say(L, "the work folder needs about " + gb(need) + " GB (and 2 GB kept free), " + gb(avail) + " GB free on that disk" + (opt.forceDisk ? "; --force-disk: running anyway" : "; nothing written (--force-disk runs anyway)"));
                say(L, "  工作文件夹大约需要 " + gb(need) + " GB（另外保留 2 GB），这块磁盘剩 " + gb(avail)
                       + (opt.forceDisk ? " GB；--force-disk：照样开始" : " GB；没有开始，也没有写任何文件（加 --force-disk 可以照样开始）"));
                if (!opt.forceDisk) return 2; }
            if (timing) say(L, "work folder: about " + gb(need) + " GB needed, " + gb(ec ? 0 : avail) + " GB free"); }
        final boolean[] resumed = {false}; final String wd = workDir(wdPath, job, resumed);
        boolean ok = false;
        try { final int rc = encodeInWork(opt, L, ctx, wd, resumed[0], paths, Ts, autoT, wis, stOf, base, asFlac, coreBits, maxSt, tlog, t0); ok = true; return rc; }
        finally {   // the work folder goes at the end; kept when the run is stopped by an exception (disk full, closed: the same command continues),
                    // not with --disk-stop-remove and not when the user cancelled (Java: the app's cancel button means "stop and leave nothing")
            final boolean cancelled = ctx.cancel != null && ctx.cancel.get();
            if (!opt.keepTmp && (ok || opt.diskStopRemove || cancelled)) removeAll(new File(wd)); } }

    // "  %-22s %8.2f s   peak memory so far %7.0f MB" (C++ 3c0f89b, --timing); Java: the largest Java heap in use seen at the stage ends
    // (the C++ reads the process's peak working set): only for the timing report, the file does not change
    static long heapPeak = 0;
    private static void stage(StringBuilder tlog, long[] t0, String s) { final long t = System.nanoTime(); final Runtime rt = Runtime.getRuntime(); heapPeak = Math.max(heapPeak, rt.totalMemory() - rt.freeMemory());
        tlog.append(stageLine(s, (t - t0[0]) / 1e9)).append("   peak memory so far ").append(CStd.f(heapPeak / 1048576.0, 7, 0)).append(" MB").append('\n'); t0[0] = t; }

    private static int encodeInWork(final Options opt, final Listener L, final StreamEnc.Ctx ctx, final String wd, final boolean resumed, List<String> paths, List<Double> Ts, List<Boolean> autoT, List<StreamEnc.WavInfo> wis,
                                    final int[] stOf, final int base, final boolean asFlac, final int coreBits, final int maxSt, StringBuilder tlog, long[] t0) {
        final String out = opt.out, corePath = opt.core; final long piece = pieceFor(opt, maxSt);
        if (resumed) { say(L, "continuing from the work folder " + wd); say(L, "  从工作文件夹 " + wd + " 接着做"); }
        final StreamEnc.DiskGuard dg = new StreamEnc.DiskGuard(); dg.dir = wd; dg.track = opt.timing; dg.reserve = opt.diskReserve; dg.stop = opt.diskStop; dg.removeNote = opt.diskStopRemove; dg.listener = L; dg.cancel = ctx.cancel; StreamEnc.diskGuard(dg);
        List<StreamEnc.Prep> pm = new ArrayList<StreamEnc.Prep>(); for (int i = 0; i < paths.size(); i++) pm.add(new StreamEnc.Prep());
        final boolean[] restoredPrep = new boolean[paths.size()];
        for (int i = 0; i < paths.size(); i++) { final String stem = wd + "/m" + i; pm.get(i).derive = opt.smallDisk; pm.get(i).threads = ctx.threads;
            final String t = resumed ? StreamEnc.getState(stem + ".prep") : null; if (t != null && StreamEnc.prepFromState(t, pm.get(i)) && pm.get(i).path.equals(paths.get(i))) restoredPrep[i] = true;   // made by an earlier run
            else if (t != null) { final StreamEnc.Prep q = new StreamEnc.Prep(); q.derive = opt.smallDisk; q.threads = ctx.threads; pm.set(i, q); } }
        long totalFrames = 0; for (int i = 0; i < wis.size(); i++) if (!restoredPrep[i]) totalFrames += wis.get(i).frames;
        ctx.stage = "read, levels, octaves"; ctx.done = 0; ctx.total = Math.max(1, 4.5 * totalFrames);
        for (int i = 0; i < paths.size(); i++) if (!restoredPrep[i]) { final String stem = wd + "/m" + i; String e = StreamEnc.prepRead(paths.get(i), stem, pm.get(i), ctx);
            final int[] others = new int[stOf.length - 1]; for (int k = 0, q = 0; k < stOf.length; k++) if (k != i) others[q++] = stOf[k];
            if (e.isEmpty()) e = StreamEnc.prepLevels(pm.get(i), base, stem, piece, ctx, others); if (!e.isEmpty()) { say(L, e); return 2; }
            StreamEnc.putState(stem + ".prep", StreamEnc.prepState(pm.get(i))); }
        stage(tlog, t0, "read, levels, octaves");
        int coreSrc = -1; StreamEnc.Core core = new StreamEnc.Core();
        if (!corePath.isEmpty()) { final String e = readWav(corePath, core); if (!e.isEmpty()) { say(L, e); return 2; } if (core.rate != base) { say(L, "the core master must be at " + base + " Hz"); return 2; } }
        else { final boolean[] dith = new boolean[1]; final int[] src = new int[1]; core = StreamEnc.deriveCoreS(pm, base, coreBits, src, dith, 20261028); coreSrc = src[0];
            say(L, "core master: " + coreBits + "-bit base level of " + paths.get(coreSrc) + (dith[0] ? " (TPDF-dithered)" : "")); }
        for (int i = 0; i < paths.size(); i++) if (autoT.get(i)) {   // master.wav@auto: T from LAEY's search against the given core
            if (corePath.isEmpty() || core.bits != 16) { say(L, "@auto needs --core=<cd.wav>, a 16-bit CD / DVD (a derived core is at T = 0)"); return 1; }
            if (wis.get(i).flac) { say(L, paths.get(i) + ": @auto needs the master as a WAV (LAEY's search reads WAV)"); return 1; }
            if (opt.aligner == null) { say(L, paths.get(i) + ": LAEY's alignment search is not available in this build (LaesEncoder.Options.aligner); give T by hand (master.wav@T); nothing written"); return 2; }
            final AutoT A = opt.aligner.align(corePath, core, paths.get(i), pm.get(i), opt.laeEnc); if (!A.found) { say(L, paths.get(i) + ": " + A.why + "; nothing written"); return 2; }
            if (!acceptAlign(A, paths.get(i), opt.accept, opt.aligner, L)) { say(L, "alignment not accepted; nothing written"); return 2; } Ts.set(i, A.T); }
        stage(tlog, t0, "core master, alignment");
        final List<StreamEnc.MasterSel> sel = new ArrayList<StreamEnc.MasterSel>(); for (int i = 0; i < paths.size(); i++) { final long[] t = setT(Ts.get(i)); StreamEnc.MasterSel s = new StreamEnc.MasterSel(); s.storage = -1; s.whole = t[0]; s.frac = (int) t[1]; sel.add(s); }
        // file order, tried two ways, the smaller kept: (a) more bits first; (b) the master the core was derived from first, then the others more bits first
        final List<List<Integer>> orders = new ArrayList<List<Integer>>(); { List<Integer> a = new ArrayList<Integer>(); for (int i = 0; i < paths.size(); i++) a.add(i); orders.add(a); }
        if (coreSrc >= 0 && coreSrc != 0) { List<Integer> b = new ArrayList<Integer>(); b.add(coreSrc); for (int i = 0; i < paths.size(); i++) if (i != coreSrc) b.add(i); orders.add(b); }
        List<LaesFile.MasterInfo> info = new ArrayList<LaesFile.MasterInfo>(); String bodyPath = ""; long bodySize = -1; List<Integer> pick = null;
        final Map<String, StreamEnc.CachedCand> cache = new TreeMap<String, StreamEnc.CachedCand>();   // with two orders, the second takes the sizes of the candidates the first coded (only its chosen ones are coded again)
        final String cacheLog = wd + "/cache.txt";   // the cache's entries as they are made (resume)
        // resume checks (2026-11-05, review): the T of every master as this run has it - a state made with other T values is not used
        final StringBuilder tsb = new StringBuilder(); for (StreamEnc.MasterSel s : sel) tsb.append(s.whole).append(':').append(s.frac & 0xFFFFFFFFL).append(';'); final String tsig = tsb.toString();
        if (resumed) {   // the cache's sizes as made before: whole lines only (key, two numbers), and the file rewritten with just those
            final String t = StreamEnc.getState(cacheLog); final StringBuilder good = new StringBuilder();
            if (t != null) { int a = 0; for (int e; (e = t.indexOf('\n', a)) >= 0; a = e + 1) { final String ln = t.substring(a, e);
                    final int p1 = ln.indexOf('\t'), p2 = p1 < 0 ? -1 : ln.indexOf('\t', p1 + 1); if (p1 <= 0 || p2 < 0 || ln.indexOf('\t', p2 + 1) >= 0) continue;
                    final double b0, b1; try { b0 = Double.parseDouble(ln.substring(p1 + 1, p2)); b1 = Double.parseDouble(ln.substring(p2 + 1)); } catch (NumberFormatException x) { continue; }
                    StreamEnc.CachedCand cc = cache.get(ln.substring(0, p1)); if (cc == null) { cc = new StreamEnc.CachedCand(); cache.put(ln.substring(0, p1), cc); } cc.b[0] = b0; cc.b[1] = b1; good.append(ln).append('\n'); } }
            StreamEnc.putState(cacheLog, good.toString()); }
        final long[] osz = new long[orders.size()]; Arrays.fill(osz, -1); final List<List<LaesFile.MasterInfo>> oinfo = new ArrayList<List<LaesFile.MasterInfo>>(); for (int i = 0; i < orders.size(); i++) oinfo.add(new ArrayList<LaesFile.MasterInfo>());
        final String pickP = wd + "/pick.done"; int picked = -1;   // the order kept, once both are done (the other is not made again)
        { final String t = resumed ? StreamEnc.getState(pickP) : null; if (t != null) { final StreamEnc.StateR r = new StreamEnc.StateR(t); final long pi = r.i(); final String ts = r.str(); if (!r.bad && ts.equals(tsig) && pi >= 0 && pi < orders.size()) picked = (int) pi; } }
        ctx.stage = "candidates, payload"; ctx.done = 0; { double tot = 0; for (StreamEnc.Prep m : pm) tot += (double) m.frames / (double) (1 << m.stages) * (1 + 2 * paths.size()) * 2; ctx.total = Math.max(1, tot * orders.size()); }
        for (int oi = 0; oi < orders.size(); oi++) { if (picked >= 0 && oi != picked) continue;
            final List<Integer> ord = orders.get(oi); final List<StreamEnc.Prep> m2 = new ArrayList<StreamEnc.Prep>(); final List<StreamEnc.MasterSel> s2 = new ArrayList<StreamEnc.MasterSel>(); for (int k : ord) { m2.add(pm.get(k)); s2.add(sel.get(k)); }
            final List<LaesFile.MasterInfo> i2 = oinfo.get(oi); final String bp = wd + "/body" + oi, stO = wd + "/o" + oi + ".done";
            if (resumed) { final String t = StreamEnc.getState(stO);
                if (t != null) { final StreamEnc.StateR r = new StreamEnc.StateR(t); final long sz = r.i(); final String ts = r.str(); final long n = r.i(); i2.clear(); for (long q = 0; q < Math.max(0, Math.min(n, 16)); q++) { final LaesFile.MasterInfo I = new LaesFile.MasterInfo(); StreamEnc.getInfo(r, I); i2.add(I); }
                    if (!r.bad && ts.equals(tsig) && n == ord.size() && new File(bp).isFile() && sz == new File(bp).length()) { osz[oi] = sz; continue; }   // made by an earlier run
                    // a stale o<oi>.done (other T, damaged, body gone): the whole order is made again. Without one the order was not
                    // finished: its masters' choice states stay, each checked (files' sizes, T) when encodeLaesS restores it (C++ e6eab9f).
                    new File(stO).delete(); for (int mi = 0; mi < ord.size(); mi++) new File(wd + "/o" + oi + "_m" + mi + ".done").delete(); } }
            final List<String> kept = new ArrayList<String>();
            final String err = StreamEnc.encodeLaesS(core, base, m2, s2, i2, bp, wd + "/o" + oi, asFlac ? 1 : 0, piece, ctx, orders.size() > 1 ? cache : null, cacheLog, kept);
            if (!err.isEmpty()) { say(L, err); return 2; } osz[oi] = new File(bp).length();
            { final StreamEnc.StateW w = new StreamEnc.StateW(); w.i(osz[oi]); w.str(tsig); w.i(i2.size()); for (LaesFile.MasterInfo I : i2) StreamEnc.putInfo(w, I); StreamEnc.putState(stO, w.s.toString()); }
            for (String f : kept) new File(f).delete(); }   // the chosen layers, now in the payload
        for (int oi = 0; oi < orders.size(); oi++) if (osz[oi] >= 0 && (bodySize < 0 || osz[oi] < bodySize)) { bodyPath = wd + "/body" + oi; bodySize = osz[oi]; info = oinfo.get(oi); pick = orders.get(oi); picked = oi; }
        { final StreamEnc.StateW w = new StreamEnc.StateW(); w.i(picked); w.str(tsig); StreamEnc.putState(pickP, w.s.toString()); }   // before the other body goes
        for (int oi = 0; oi < orders.size(); oi++) { final String bp = wd + "/body" + oi; if (!bp.equals(bodyPath)) new File(bp).delete(); }
        StreamEnc.diskCheck(true);   // the largest size: before the work folder goes
        stage(tlog, t0, "candidates, payload");
        { final List<String> p2 = new ArrayList<String>(); final List<StreamEnc.Prep> q2 = new ArrayList<StreamEnc.Prep>(); for (int k : pick) { p2.add(paths.get(k)); q2.add(pm.get(k)); } paths = p2; pm = q2; }
        { StringBuilder s = new StringBuilder("file order:"); for (int k = 0; k < paths.size(); k++) s.append(' ').append(k + 1).append(" = ").append(paths.get(k)).append(';'); s.append(orders.size() > 1 ? " (the smaller of two orders)" : ""); say(L, s.toString()); }
        final long coreFrames = core.frames(); final int coreBitsW = core.bits;
        ctx.stage = "core master frames, file"; ctx.done = 0; ctx.total = 1; ctx.step(0);
        if (asFlac) { final String why = writeFlacForm(out, wd, core, base, bodyPath, bodySize, ctx, opt.tags); if (why != null) { new File(out + ".part").delete(); say(L, "FLAC self-check failed" + (why.isEmpty() ? "" : ": ") + why + "; nothing written"); return 2; } }
        else writeWavForm(out, core, bodyPath, bodySize, opt.forceRf64, opt.tags);
        core = null;   // released before the self-check (the playback decoder holds its own copy)
        stage(tlog, t0, "core master frames, file");
        // self-check of the written file, read exactly as a player reads it (the playback decoder, laes_play.h; 2026-11-03): the whole file
        // (file SHA-256, payload CRC-32, core), then every master decoded and compared sample by sample with its input; any difference:
        // the file is removed and nothing is written
        if (opt.selfCheckFlip != null) flipByte(out + ".part", CStd.atoll(opt.selfCheckFlip));   // tests only
        ctx.stage = "self-check"; ctx.done = 0; ctx.total = 0; for (StreamEnc.Prep m : pm) ctx.total += m.frames;
        { final String why = selfCheckFile(out + ".part", pm, ctx);
          if (!why.isEmpty()) { new File(out + ".part").delete(); say(L, "self-check failed: " + why + "; nothing written"); return 2; } }
        stage(tlog, t0, "self-check");
        { final File o = new File(out); o.delete(); if (!new File(out + ".part").renameTo(o)) { say(L, "cannot write " + out); return 2; } }
        final double secs = (double) coreFrames / base; final long fsz = new File(out).length();
        say(L, out + ": " + (asFlac ? "FLAC" : "WAV") + " form, core master " + base + " Hz " + coreBitsW + "-bit, " + pm.size() + " master(s), LAES block " + CStd.f(bodySize * 8 / secs / 1000, 1)
               + " kb/s (file " + CStd.f((double) fsz / 1e6, 1) + " MB, " + CStd.f((double) fsz * 8 / secs / 1000, 1) + " kb/s)");
        for (int i = 0; i < info.size(); i++) { final LaesFile.MasterInfo I = info.get(i); final String tier = I.refTier == 0 ? "the core master" : "master " + I.refTier;
            final String how = I.storage == 1 ? "full" : I.storage == 0 ? "difference to " + tier + " at the base level"
                             : "difference to " + tier + " at " + (base << Math.min(I.stages, info.get(I.refTier - 1).stages)) + " Hz";
            say(L, "  master " + (i + 1) + ": " + I.rate + " Hz " + I.bits + "-bit, " + how + ", layers " + CStd.f(I.layerBits / secs / 1000, 1) + " kb/s"); }
        if (opt.timing) { say(L, "timing:"); for (String s : tlog.toString().split("\n")) if (!s.isEmpty()) say(L, s); say(L, "  work folder, largest   " + CStd.f(dg.peak / 1048576.0, 8, 1) + " MB"); }
        if (L != null) L.progress("done", 1.0);
        return 0; }

    // the -60 / -54 dB bands: within -60 dB the alignment passes; otherwise it is shown and passes only when chosen by hand
    // port of laes_tool.cpp:acceptAlign
    static boolean acceptAlign(AutoT A, String mPath, boolean acceptFlag, Aligner al, Listener L) {
        say(L, mPath + ": T = " + CStd.f(A.T, 10) + " base samples (" + A.search + " search), alignment error " + CStd.f(A.err, 1) + " dB, residual re the master's base level " + CStd.f(A.resid, 1) + " dB");
        if (A.resid > -40) say(L, "  note: the core master is not a close copy of this master (other mix / level / EQ / edit?); the encoder will likely store it in full");
        if (A.err <= -60) return true;
        say(L, "  alignment error " + CStd.f(A.err, 1) + " dB is " + (A.err <= -54 ? "between -60 and" : "worse than") + " -54 dB: check it (" + A.notes + ")");
        if (acceptFlag) { say(L, "  accepted (--accept)"); return true; }
        return al.ask("  use this alignment? [y/N] "); }

    // tests only: one byte of the written file flipped at this offset (from the end when negative)
    // port of laes_tool.cpp:main (LAES_SELFCHECK_FLIP)
    static void flipByte(String path, long at) {
        try { RandomAccessFile t = new RandomAccessFile(path, "rw"); try { final long n = t.length(); if (at < 0) at += n; if (at >= 0 && at < n) { t.seek(at); final int c = t.read(); t.seek(at); t.write(c ^ 0x10); } } finally { t.close(); } }
        catch (IOException e) { /* as the C++: a file that cannot be opened is left alone */ } }

    // WAV form: the core, then the "LAE " chunk with the payload
    // port of laes_tool.cpp:main (the WAV form)
    static void writeWavForm(String out, StreamEnc.Core core, String bodyPath, long bodySize, boolean forceRf64, LaesTags.Tags tags) {
        final int by = core.bits / 8; final long n = core.frames(), db = n * 2 * by; final boolean ext = core.bits > 16; final long fmtLen = ext ? 40 : 16; final long extra = bodySize != 0 ? 8 + bodySize + (bodySize & 1) : 0;
        // beyond 4 GB: RF64 (EBU Tech 3306 / ITU-R BS.2088): "RF64", a "ds64" chunk with the 64-bit sizes (RIFF, data, the LAE chunk in its
        // table), the 32-bit size fields set to 0xFFFFFFFF; everything else as in RIFF
        final byte[] tagChunks = LaesTags.wavChunks(tags); final long tc = tagChunks.length;   // LIST/INFO + "id3 " between "fmt " and "data" (none: nothing)
        final long riff = 4 + 8 + fmtLen + tc + 8 + db + (db & 1) + extra; final boolean rf64 = riff > 0xFFFFFFFFL || forceRf64;   // the variable: tests on small files
        byte[] h = wavHeader(core.rate, core.bits);   // header of an empty file: sizes patched below
        if (tc != 0) { final byte[] h2 = new byte[h.length + (int) tc]; System.arraycopy(h, 0, h2, 0, h.length - 8); System.arraycopy(tagChunks, 0, h2, h.length - 8, (int) tc); System.arraycopy(h, h.length - 8, h2, h.length - 8 + (int) tc, 8); h = h2; }
        com.lae.core.Le.put32(h, 4, (int) riff); com.lae.core.Le.put32(h, h.length - 4, (int) db);
        if (rf64) { final byte[] r = new byte[20 + 8 * 3 + 8 + 8 + (h.length - 12)]; int p = 0; final byte[] r0 = {'R', 'F', '6', '4', (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 40, 0, 0, 0};
            System.arraycopy(r0, 0, r, 0, 20); p = 20; com.lae.core.Le.put64(r, p, riff + 48); p += 8; com.lae.core.Le.put64(r, p, db); p += 8; com.lae.core.Le.put64(r, p, n); p += 8;
            final byte[] r1 = {1, 0, 0, 0, 'L', 'A', 'E', ' '}; System.arraycopy(r1, 0, r, p, 8); p += 8; com.lae.core.Le.put64(r, p, bodySize); p += 8;
            System.arraycopy(h, 12, r, p, h.length - 12); p += h.length - 12; for (int i = r.length - 4; i < r.length; i++) r[i] = (byte) 0xFF; h = r; }   // the data size: in ds64
        Stream.Out o = new Stream.Out(out + ".part");
        try { o.put(h, 0, h.length); final int P = 1 << 18; final byte[] b = new byte[P * 2 * by];
            for (long a = 0; a < n; a += P) { final long e = Math.min(n, a + P); int f = 0; for (long i = a; i < e; i++) for (int c = 0; c < 2; c++) { final int v = core.c[c][(int) i]; for (int k = 0; k < by; k++) b[f++] = (byte) (v >>> (8 * k)); } o.put(b, 0, f); }
            if ((db & 1) != 0) o.put(new byte[1], 0, 1);
            if (bodySize != 0) { final byte[] ch = {'L', 'A', 'E', ' ', rf64 ? (byte) 0xFF : (byte) bodySize, rf64 ? (byte) 0xFF : (byte) (bodySize >> 8), rf64 ? (byte) 0xFF : (byte) (bodySize >> 16), rf64 ? (byte) 0xFF : (byte) (bodySize >> 24)}; o.put(ch, 0, 8);
                Stream.In body = new Stream.In(bodyPath); try { final byte[] q = new byte[1 << 22]; for (long a = 0; a < bodySize; a += 1 << 22) { final int k = (int) Math.min(1 << 22, bodySize - a); body.read(a, q, 0, k); o.put(q, 0, k); } } finally { body.close(); }
                if ((bodySize & 1) != 0) o.put(new byte[1], 0, 1); }
            o.close(); o = null; }
        finally { StreamEnc.closeQuiet(o); } }

    // FLAC form, then read back: core, MD5 and LAES block must come back unchanged. Returns null when written and read back unchanged, else
    // the reason ("" when there is none to show)
    // port of laes_tool.cpp:main (the FLAC form). Java: the audio frames are coded into a work-folder file first (the C++ holds them in
    // memory), and the read-back decodes the frames one at a time (the C++ reads them all into memory, laesflac::readFlac); the same checks.
    static String writeFlacForm(String out, String wd, StreamEnc.Core core, int base, String bodyPath, long bodySize, StreamEnc.Ctx ctx, LaesTags.Tags tags) {
        final FlacEnc.Ch[] ch = {new FlacEnc.IntCh(core.c[0]), new FlacEnc.IntCh(core.c[1])}; final String framesPath = wd + "/core.frames";
        final FlacEnc.Frames F; { Stream.Out fo = new Stream.Out(framesPath); try { F = FlacEnc.writeFlacFrames(ch, base, core.bits, fo, ctx.threads, ctx.cancel); fo.close(); fo = null; } finally { StreamEnc.closeQuiet(fo); } }
        final byte[] fl0 = FlacEnc.flacHead(ch, base, core.bits, F, 0);   // "fLaC", STREAMINFO (the audio frames: in framesPath)
        final long pieceA = (1L << 24) - 1 - 4; final long napp = bodySize == 0 ? 0 : (bodySize + pieceA - 1) / pieceA;
        final byte[] tagBlocks = LaesTags.flacBlocks(tags, bodySize == 0);
        { Stream.Out o = new Stream.Out(out + ".part"); Stream.In body = null, fr = null;
          try { body = new Stream.In(bodyPath); final byte[] h = fl0.clone(); if (napp != 0 || !tags.empty()) h[4] = 0x00; o.put(h, 0, h.length);   // STREAMINFO is not the last block
              o.put(tagBlocks, 0, tagBlocks.length);   // tags and pictures, before the LAES block (none: nothing)
              final byte[] b = new byte[1 << 22];
              for (long i = 0; i < napp; i++) { final long a = i * pieceA, len = Math.min(pieceA, bodySize - a);
                  final long Ln = len + 4; final byte[] bh = {(byte) ((i + 1 == napp ? 0x80 : 0) | 2), (byte) (Ln >> 16), (byte) (Ln >> 8), (byte) Ln, 'L', 'A', 'E', '2'}; o.put(bh, 0, 8);
                  for (long q = 0; q < len; q += b.length) { final int k = (int) Math.min(b.length, len - q); body.read(a + q, b, 0, k); o.put(b, 0, k); } }
              fr = new Stream.In(framesPath); for (long q = 0; q < fr.n; q += b.length) { final int k = (int) Math.min(b.length, fr.n - q); fr.read(q, b, 0, k); o.put(b, 0, k); }
              o.close(); o = null; }
          finally { StreamEnc.closeQuiet(o); StreamEnc.closeQuiet(body); StreamEnc.closeQuiet(fr); } }
        new File(framesPath).delete();
        // read back: the APPLICATION blocks against the payload, the rest through the FLAC reader
        Stream.In f = new Stream.In(out + ".part"), body = new Stream.In(bodyPath);
        try { long p = 4; long got = 0; boolean ok = true, last = false; final byte[] bh = new byte[4]; final int C = 1 << 20; final byte[] b = new byte[C], c = new byte[C];
            while (!last && ok) { f.read(p, bh, 0, 4); last = (bh[0] & 0x80) != 0; final int type = bh[0] & 0x7F; final long len = (long) (bh[1] & 0xFF) << 16 | (bh[2] & 0xFF) << 8 | (bh[3] & 0xFF); p += 4;
                if (type == 2) { if (len < 4) { ok = false; break; }
                    if (p + 4 < 0 || p + 4 + (len - 4) > f.n) Stream.fail("read failed: " + f.path); if (got + (len - 4) > body.n) Stream.fail("read failed: " + body.path);   // the C++ reads both ranges whole first
                    for (long q = 0; q < len - 4 && ok; q += C) { final int k = (int) Math.min(C, len - 4 - q); f.read(p + 4 + q, b, 0, k); body.read(got + q, c, 0, k); for (int i = 0; i < k; i++) if (b[i] != c[i]) { ok = false; break; } }
                    if (!ok) { got += len - 4; break; } got += len - 4; }
                p += len; }
            boolean tagsOk = true; if (tagBlocks.length != 0) { final byte[] t = new byte[tagBlocks.length]; f.read(42, t, 0, t.length); tagsOk = java.util.Arrays.equals(t, tagBlocks); }
            if (!tagsOk) return "tag blocks differ";   // as the C++: this reason replaces "LAES block differs"
            if (!(ok && got == bodySize)) return "LAES block differs";
            return readBackFlac(fl0, f, p, core); }
        finally { f.close(); body.close(); } }

    // laesflac::readFlac on "fLaC" + STREAMINFO (last) + the file from p on, compared with the core as it goes: null when the core and the MD5
    // come back unchanged and there is no APPLICATION data, else the reason ("" when the reader found none)
    // port of laes_flac.h:readFlac (the checks the read-back needs, frame by frame)
    static String readBackFlac(byte[] fl0, Stream.In f, long p, StreamEnc.Core core) {
        final Flac.Info in = new Flac.Info(); { Flac.BR r = new Flac.BR(fl0, 8, 34); in.minBs = (int) r.get(16); in.maxBs = (int) r.get(16); r.get(24); r.get(24); in.rate = (int) r.get(20); in.channels = (int) r.get(3) + 1; in.bps = (int) r.get(5) + 1; in.total = r.get(36); System.arraycopy(fl0, 8 + 18, in.md5, 0, 16); }
        if (in.total == 0) return "STREAMINFO gives no sample count (an LAES core master always has one)"; if (in.maxBs < 16) return "bad STREAMINFO block size"; if (in.channels != 2) return "LAES core master must have 2 channels"; if (in.bps != 16 && in.bps != 24 && in.bps != 32) return "core master must be 16, 24 or 32 bit";
        final com.lae.core.AlignHash.Md5 md = new com.lae.core.AlignHash.Md5(); final int by = (in.bps + 7) / 8; final byte[] mb = new byte[4096 * 2 * 4];
        final int W = 1 << 22, M = 1 << 20; byte[] buf = new byte[W]; long bufAt = p; int bufLen = 0, fp = 0; long size = 0, frameNo = 0; boolean same = true; final long nCore = core.frames();
        while (bufAt + fp < f.n) {
            if (bufLen - fp < M && bufAt + bufLen < f.n) { final int keep = bufLen - fp; System.arraycopy(buf, fp, buf, 0, keep); bufAt += fp; fp = 0; final int k = (int) Math.min(W - keep, f.n - (bufAt + keep)); f.read(bufAt + keep, buf, keep, k); bufLen = keep + k; }
            final Flac.Frame F = Flac.decodeFrame(buf, fp, bufLen - fp, in, frameNo, size); if (!F.err.isEmpty()) return F.err;
            int q = 0; for (int i = 0; i < F.n; i++) for (int c = 0; c < 2; c++) { final long v = F.ch[c][i]; for (int k = 0; k < by; k++) mb[q++] = (byte) (v >>> (8 * k)); if (same && (size + i >= nCore || core.c[c][(int) (size + i)] != v)) same = false; }
            md.update(mb, 0, q); size += F.n; fp += F.used; frameNo++; }
        if (in.total != 0 && in.total != size) return "sample count differs from STREAMINFO";
        final byte[] m = Flac.hexBytes(md.hex(), 16); boolean zero = true; for (int i = 0; i < 16; i++) zero &= in.md5[i] == 0; final boolean md5ok = !zero && java.util.Arrays.equals(m, in.md5);
        if (!md5ok || !same || size != nCore) return "";
        return null; }

    // the self-check of a written LAES file, read exactly as a player reads it (laes_play.h): open (file SHA-256, payload CRC-32, core), then
    // every master decoded in time order and compared sample by sample with its input (pm: in file order; read where they are: StreamEnc.masterFrames).
    // "" when everything matches, else the reason.
    // port of laes_tool.cpp:selfCheckFile
    static String selfCheckFile(String path, List<StreamEnc.Prep> pm, StreamEnc.Ctx ctx) {
        final RandomAccessFile fp; try { fp = new RandomAccessFile(path, "r"); } catch (IOException e) { return "cannot read " + path; }
        try { final long size; try { size = fp.length(); } catch (IOException e) { return "cannot read " + path; }
            Play.FnSrc src = new Play.FnSrc(); src.n = size; src.fn = new Play.ReadFn() { public int read(long at, byte[] d, int off, int k) { try { fp.seek(at); fp.readFully(d, off, k); return 0; } catch (IOException e) { return 1; } } };
            Play.File f = new Play.File(); final String[] mg = {""}; String e; try { e = f.open(src, mg, false); } catch (RuntimeException x) { e = String.valueOf(x.getMessage()); } catch (OutOfMemoryError x) { e = "bad allocation"; }
            if (e.isEmpty() && !f.why.isEmpty()) e = f.why;   // the payload failed a check
            if (!e.isEmpty()) return e;
            final int P = 1 << 16; final int[] a = new int[P * 2];
            for (int i = 0; i < pm.size(); i++) {
                int j = -1; for (int k = 0; k < f.choices.size(); k++) if (f.choices.get(k).kind == 1 && f.choices.get(k).m == i) j = k;
                if (j < 0) return "master " + (i + 1) + " is not in the file";
                Play.Player p = new Play.Player(); Stream.In raw = null;
                try { e = p.start(f, j, ctx.threads, false, 0); if (!e.isEmpty()) return "master " + (i + 1) + ": " + e;
                    final StreamEnc.Prep pi = pm.get(i); if (!pi.flacLive) raw = new Stream.In(pi.raw); long at = 0; final int[] want = new int[P * 2]; final byte[] tmp = new byte[P * 2 * 4];
                    for (;;) { final long k = p.read(a, P); if (!p.err.isEmpty()) return "master " + (i + 1) + ": " + p.err; if (k == 0) break;
                        if (at + k > pm.get(i).frames) return "master " + (i + 1) + " is longer than its input";
                        StreamEnc.masterFrames(raw, pi, at, (int) k, want, tmp); for (int q = 0; q < k * 2; q++) if (a[q] != want[q]) return "master " + (i + 1) + " differs from its input near frame " + at;
                        at += k; ctx.step((double) k); }
                    if (at != pm.get(i).frames) return "master " + (i + 1) + " is shorter than its input";
                    if (!p.verified()) return "master " + (i + 1) + ": SHA-256 mismatch"; }
                finally { p.close(); StreamEnc.closeQuiet(raw); } }
            return ""; }
        finally { try { fp.close(); } catch (IOException x) { /* read only */ } } }

}
