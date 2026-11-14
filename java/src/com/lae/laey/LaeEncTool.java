// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The command line of lae_enc in Java: the same arguments and messages as bin\lae_enc.exe (without lae_enc plan).
 * usage: java com.lae.laey.LaeEncTool &lt;master.wav&gt; &lt;out.wav&gt; [mode] [s1 | s2] [zs] [energy] [equal] [bits=24] [mem=&lt;GB&gt;] [resume]
 *        [cd=&lt;CD.wav&gt; [cdcheck] [cdalign=&lt;offset&gt;] [cdaccept] [cdedge=pad | cut]] [master2=&lt;master.wav&gt; .. master9=, master&lt;k&gt;align=,
 *        master&lt;k&gt;name=, master&lt;k&gt;edge=, master&lt;k&gt;accept, master1name=] [noise=&lt;marks.laenoise&gt;] [g | g=b | g=c | g=d]
 *        [--tag=KEY=VALUE] [--cover=&lt;image&gt;] [--picture=&lt;type&gt;:&lt;image&gt;] [--form=flac | --form=wav]; lae_enc tag ... (LaeRetag)
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
        // Ctrl+C: stop cleanly. FU22-6: the hook waits until the run has cleaned up (at most 120 s), told by a latch: the former join on the main
        // thread always ran its full 10 s (the main thread's System.exit blocks while the JVM shuts down) and a longer cleanup was cut off
        final AtomicBoolean cancel = new AtomicBoolean(false), hooked = new AtomicBoolean(false); final CountDownLatch fin = new CountDownLatch(1);
        Thread hook = new Thread(new Runnable() { @Override public void run() { if (fin.getCount() == 0) return; hooked.set(true); cancel.set(true);
            try { fin.await(120, TimeUnit.SECONDS); } catch (InterruptedException e) { /* exiting */ } } });
        Runtime.getRuntime().addShutdownHook(hook);
        int rc = LaeyEncoder.run(argv, new LaeyEncoder.Listener() { @Override public void message(String t) { System.out.print(t); System.out.flush(); } @Override public void step(String s) { } }, null, System.getenv("LAE_WORKDIR"), vars, cancel);
        System.out.flush(); fin.countDown(); if (hooked.get()) return;   // the JVM is already shutting down (Ctrl+C): no System.exit
        System.exit(rc); }
}
