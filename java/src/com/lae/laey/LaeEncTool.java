// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The command line of lae_enc in Java: the same arguments and messages as bin\lae_enc.exe (part 1 of the port: without cd= and plan).
 * usage: java com.lae.laey.LaeEncTool &lt;master.wav&gt; &lt;out.wav&gt; [mode] [s1 | s2] [zs] [energy] [equal] [bits=24] [mem=&lt;GB&gt;] [resume]
 * The environment variables of the C++ test hooks are read from the process (LAE_TESTFAIL, LAE_BLOCKUNITS, LAE_TESTSEQUENTIAL,
 * LAE_TESTMAXFRAMES, PARTTHREADS, KEEPTMP, TIMING); LAE_WORKDIR (Java only) sets the work folder.
 */
public final class LaeEncTool {
    private LaeEncTool() {}

    // port of lae_enc.cpp:main
    public static void main(String[] args) {
        String[] argv = new String[args.length + 1]; argv[0] = "lae_enc"; System.arraycopy(args, 0, argv, 1, args.length);
        Map<String, String> vars = new HashMap<String, String>();
        for (String k : new String[] {"LAE_TESTFAIL", "LAE_BLOCKUNITS", "LAE_TESTSEQUENTIAL", "LAE_TESTMAXFRAMES", "LAEY_TEST_PIECE", "PARTTHREADS", "KEEPTMP", "TIMING"}) { String v = System.getenv(k); if (v != null) vars.put(k, v); }
        final AtomicBoolean cancel = new AtomicBoolean(false); final Thread mainT = Thread.currentThread(); final boolean[] done = {false};
        Thread hook = new Thread(new Runnable() { @Override public void run() { if (done[0]) return; cancel.set(true); try { mainT.join(10000); } catch (InterruptedException e) { /* exiting */ } } });   // Ctrl+C: stop cleanly
        Runtime.getRuntime().addShutdownHook(hook);
        int rc = LaeyEncoder.run(argv, new LaeyEncoder.Listener() { @Override public void message(String t) { System.out.print(t); System.out.flush(); } @Override public void step(String s) { } }, null, System.getenv("LAE_WORKDIR"), vars, cancel);
        done[0] = true; System.out.flush(); System.exit(rc); }
}
