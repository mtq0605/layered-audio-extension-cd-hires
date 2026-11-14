// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Port of lae_enc.cpp namespace encenv plus the process environment the C++ modules read with getenv(): the module variables of one
 * encoder run. The C++ encoder hands settings to its modules through environment variables (env() / getenv()); the Java port keeps
 * that design with an explicit object, so several encoders can run in one process.
 * <p>
 * C++: "per-thread module settings and output: two layers (L and R, M and S) are coded at once, each in its own thread. A layer thread
 * keeps its own copy of the variables a layer sets (env() below) and its own printed text, which is shown after the pair in the usual
 * order, so the log reads as if the layers ran one after the other. Outside a layer thread both go to the process as before."
 * A layer thread gets a child EncEnv (the C++ encenv::over map and encenv::out string).
 */
public final class EncEnv {
    /** where the printed text of a run goes (the C++ stdout) */
    public interface Sink { void print(String text); }
    /** progress: the name of each finished step (the points the C++ marks with TIMING) */
    public interface Steps { void step(String name); }
    Steps steps;

    /** thrown when the caller cancels the run (the C++ Ctrl+C); never turned into a module failure */
    public static final class Cancelled extends RuntimeException {
        private static final long serialVersionUID = 1L;
        public Cancelled() { super("interrupted"); }
    }

    final EncEnv parent;                       // null: the process environment
    private final Map<String, String> map = new HashMap<String, String>();
    StringBuilder out;                         // C++ encenv::out (a layer thread's text), null: straight to the sink
    final Sink sink;
    final AtomicBoolean cancel;
    final int hwThreads;                       // std::thread::hardware_concurrency()

    /** the process environment of one run */
    public EncEnv(Sink s, AtomicBoolean c, int hw) { parent = null; sink = s; cancel = c != null ? c : new AtomicBoolean(false); hwThreads = Math.max(1, hw); }
    // a layer thread's overlay (C++ encenv::over = &e; encenv::out = &s)
    EncEnv(EncEnv p) { parent = p; sink = p.sink; cancel = p.cancel; hwThreads = p.hwThreads; out = new StringBuilder(); }

    // port of lae_enc.cpp:encenv::get
    synchronized String get(String k) {
        if (map.containsKey(k)) { String v = map.get(k); return v.isEmpty() ? null : v; }
        if (parent != null) return parent.get(k);
        return null; }
    // port of lae_enc.cpp:env (an empty value removes it in the process environment; an overlay keeps the empty value, which hides the process one)
    synchronized void set(String k, String v) { if (parent == null && v.isEmpty()) map.remove(k); else map.put(k, v); }
    /** sets a process variable (test hooks such as LAE_TESTFAIL, LAE_BLOCKUNITS, PARTTHREADS) */
    public void setVar(String k, String v) { set(k, v == null ? "" : v); }
    // port of lae_enc.cpp:encenv::print
    void print(String s) { if (out != null) out.append(s); else if (sink != null) sink.print(s); }
    void printf(String f, Object... a) { print(Fmt.c(f, a)); }
    // the cancellation point (C++: the signal handler)
    void step(String name) { EncEnv r = this; while (r.parent != null) r = r.parent; if (r.steps != null) r.steps.step(name); }
    void checkCancel() { if (cancel.get()) throw new Cancelled(); }
    // getenv("PARTTHREADS") ? atoi(...) : -1 (-1 = not set; the C++ uses 0 for that)
    int partThreads() { String p = get("PARTTHREADS"); return p == null ? -1 : Fmt.atoi(p); }
}
