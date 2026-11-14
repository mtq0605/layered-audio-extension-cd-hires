// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.LaeException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The LAEY (lossy LAE v5) encoder for the apps: pure Java, byte-identical to the standard C++ encoder (bin\lae_enc.exe; checked against
 * the C++ build of the same source state, tests/vectors). Options covers the common options; extension packs, master1name= and
 * --form=flac are reached through {@link #run} with the lae_enc arguments.
 * Options mirror the command line of lae_enc; the encoder writes &lt;out&gt;.part, reads it back, decodes it end to end and only then renames it
 * to &lt;out&gt;; on any failure or cancellation nothing is left behind. Block-wise work happens in a work folder (default &lt;out&gt;.tmp; on
 * Android give a folder in the app's cache dir), which bounds the memory.
 * <pre>
 *   LaeyEncoder enc = new LaeyEncoder();
 *   LaeyEncoder.Options o = new LaeyEncoder.Options(); o.master = in; o.out = out; o.mode = "3";
 *   enc.encode(o, new LaeyEncoder.Listener() { public void message(String t) { ... } public void step(String s) { ... } });
 *   // from another thread: enc.cancel();
 * </pre>
 * Questions the C++ asks on a console (bits=24 for a 32-bit master with content in its lowest 8 bits) end the run with the C++ text in an
 * {@link EncodeException}; the app asks the user and passes the option.
 */
public final class LaeyEncoder {
    /** the C++ commit this port reproduces, and the port's part. It is part of the resume key of the work folder: bump it (the part after
     *  "java-") whenever the format or the meaning of the intermediate files in the work folder changes, so that a resume never continues
     *  from files an older build wrote (decision 36, 2026-11-15). */
    public static final String VERSION = "1d96a60/java-1";

    /** options = the command line of lae_enc (see lae_enc.cpp) */
    public static final class Options {
        public String master, out;
        /** max | 1 | 3 | 5 | keep | keep20 */
        public String mode = "3";
        /** 1 or 2 (s1 / s2) */
        public int scheme = 1;
        public boolean zs = false, energy = false, equal = false, bits24 = false, resume = false;
        /** mem=&lt;GB&gt;: memory budget for coding two layers at once (0: the default rule); the output does not depend on it */
        public double memGB = 0;
        /** worker threads (C++ PARTTHREADS); 0: the processors of the device. The output does not depend on it */
        public int threads = 0;
        /** the work folder (null: &lt;out&gt;.tmp). Give every encoding its own folder: a folder in use by another run is refused (FU22-8) */
        public String workDir;
        /** independent CD (part 2 of the port): cd=, cdcheck, cdalign=, cdaccept, cdedge= */
        public String cd, cdAlign, cdEdge; public boolean cdCheck = false, cdAccept = false;
        /** noise=&lt;marks.laenoise&gt;: delete the marked cells (the marking must be made on this master); g: code G in the silent cells (2026-11-04) */
        public String noise; public boolean g = false;
        /** g strength (2026-11-05): 'd' = g (step 1, down to the 24-bit floor), 'c' = g=c (block peak - 4 bit), 'b' = g=b (block peak - 2 bit) */
        public char gLevel = 'd';
        /** tags and pictures (2026-11-06, a85dd40): one set for the whole file, as lae_enc --tag / --cover / --picture; null or empty = none */
        public com.lae.laes.LaesTags.Tags tags;
        // the command line of lae_enc for these options
        public String[] toArgs() { List<String> a = new ArrayList<String>(); a.add("lae_enc"); a.add(master); a.add(out); a.add(mode); a.add(scheme == 2 ? "s2" : "s1");
            if (zs) a.add("zs"); if (energy) a.add("energy"); if (equal) a.add("equal"); if (bits24) a.add("bits=24"); if (resume) a.add("resume");
            if (memGB > 0) a.add("mem=" + memGB);
            if (cd != null) a.add("cd=" + cd); if (cdCheck) a.add("cdcheck"); if (cdAlign != null) a.add("cdalign=" + cdAlign); if (cdAccept) a.add("cdaccept"); if (cdEdge != null) a.add("cdedge=" + cdEdge);
            if (noise != null) a.add("noise=" + noise); if (g) a.add(gLevel == 'd' ? "g" : "g=" + gLevel);
            return a.toArray(new String[0]); } }

    /** progress: the encoder's printed text (the C++ stdout) and the name of each finished step; called from the encoding thread */
    public interface Listener { void message(String text); void step(String name); }

    /** the run did not write a file: rc as the C++ exit code, the message the C++ printed last, the whole log */
    public static final class EncodeException extends LaeException {
        private static final long serialVersionUID = 1L;
        public final int rc; public final String log;
        public EncodeException(String m, int r, String l) { super(m); rc = r; log = l; } }

    // FU22-7: every encode() has its own flag (a cancel() once stopped every later encode() of the object at once); cancel() stops all running ones
    private final List<AtomicBoolean> running = new ArrayList<AtomicBoolean>();
    /** stops the encodes of this object that are running (from any thread): nothing is written, the work folder is removed */
    public void cancel() { synchronized (running) { for (AtomicBoolean c : running) c.set(true); } }

    /** encodes; returns the log. Throws {@link EncodeException} when no file was written */
    public String encode(Options o, Listener l) {
        if (o.master == null || o.out == null) throw new LaeException("master and out are required");
        Map<String, String> vars = new java.util.HashMap<String, String>(); if (o.threads > 0) vars.put("PARTTHREADS", Integer.toString(o.threads));
        final StringBuilder log = new StringBuilder();
        final AtomicBoolean cancel = new AtomicBoolean(false); synchronized (running) { running.add(cancel); }
        int rc; try { rc = run(o.toArgs(), l, log, o.workDir, vars, cancel, o.tags); } finally { synchronized (running) { running.remove(cancel); } }
        if (rc != 0) throw new EncodeException(lastLines(log.toString()), rc, log.toString());
        return log.toString(); }

    /**
     * the command-line encoder: args as lae_enc's argv (args[0] = program name); vars = the environment variables the C++ reads (test
     * hooks LAE_TESTFAIL, LAE_BLOCKUNITS, LAE_TESTSEQUENTIAL, LAE_TESTMAXFRAMES, PARTTHREADS, KEEPTMP, TIMING); returns the C++ exit code
     */
    public static int run(String[] args, final Listener l, final StringBuilder log, String workDir, Map<String, String> vars, AtomicBoolean cancel) { return run(args, l, log, workDir, vars, cancel, null); }
    /** as above; tags: tags and pictures given as objects (added after the --tag / --cover / --picture of args; null = none) */
    public static int run(String[] args, final Listener l, final StringBuilder log, String workDir, Map<String, String> vars, AtomicBoolean cancel, com.lae.laes.LaesTags.Tags tags) {
        EncEnv env = new EncEnv(new EncEnv.Sink() { @Override public void print(String t) { if (log != null) log.append(t); if (l != null) l.message(t); } }, cancel, Runtime.getRuntime().availableProcessors());
        if (l != null) env.steps = new EncEnv.Steps() { @Override public void step(String s) { l.step(s); } };
        if (vars != null) for (Map.Entry<String, String> e : vars.entrySet()) env.setVar(e.getKey(), e.getValue());
        return LaeEncoder.main(env, args, workDir, tags); }

    // the message of a failed run: the last printed line (and the one before it when that line is indented, as "  nothing encoded ...")
    static String lastLines(String log) { String[] ls = log.split("\n"); int i = ls.length - 1; while (i > 0 && ls[i].trim().isEmpty()) i--;
        if (ls[i].trim().isEmpty()) return "nothing encoded";   // FU22-7: an empty log (the former test i < 0 was never true)
        String m = ls[i].trim(); if (ls[i].startsWith("  ") && i > 0) m = ls[i - 1].trim() + " " + m; return m; }
}
