// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import com.lae.core.LaeException;

/**
 * The thread split of the encoder's block loops (laes_stream_enc.h LayerOut::feed, laes_codec_enc.h encodeLayer, laes_flac_enc.h
 * writeFlac): each thread takes the next block not yet taken; every block's result depends only on its own samples, so the results do
 * not depend on the threads. Thread count rule of the C++: th = threads (C++: hardware_concurrency); 1 when fewer than 4 blocks; at most
 * one thread per block. The calling thread works too; the first exception (by thread index, as the C++) is re-thrown after all ended.
 * Cancellation: when cancel is set, the threads take no new block and LaeException("cancelled") is thrown.
 */
public final class Par {
    private Par() {}
    /** one block k */
    public interface Block { void run(int k); }
    /** 0 or less: the C++ default (std::thread::hardware_concurrency) */
    public static int threadsOr(int threads) { if (threads > 0) return threads; final int h = Runtime.getRuntime().availableProcessors(); return h > 0 ? h : 1; }
    // port of the thread split in laes_stream_enc.h:LayerOut::feed
    public static void forBlocks(final int nb, int threads, final Block f, final AtomicBoolean cancel) {
        int th = threadsOr(threads); if (nb < 4) th = 1; th = Math.min(th, nb > 0 ? nb : 1);
        final AtomicInteger next = new AtomicInteger(0);
        if (th <= 1) { for (int k = next.getAndIncrement(); k < nb; k = next.getAndIncrement()) { if (cancel != null && cancel.get()) throw new LaeException("cancelled"); f.run(k); } return; }
        final Throwable[] xs = new Throwable[th];
        final Runnable[] work = new Runnable[th]; Thread[] pool = new Thread[th];
        for (int t = 0; t < th; t++) { final int tt = t;
            work[t] = new Runnable() { public void run() {
                try { for (int k = next.getAndIncrement(); k < nb; k = next.getAndIncrement()) { if (cancel != null && cancel.get()) throw new LaeException("cancelled"); f.run(k); } }
                catch (Throwable x) { xs[tt] = x; next.set(nb); } } }; }
        for (int t = 1; t < th; t++) { try { pool[t] = new Thread(work[t], "laes-enc"); pool[t].setDaemon(true); pool[t].start(); } catch (Throwable x) { pool[t] = null; break; } }
        work[0].run();
        boolean intr = false;   // an interruption is noted and set again after the joins (setting it at once made join throw again at once: a busy loop)
        for (int t = 1; t < th; t++) if (pool[t] != null) { boolean done = false; while (!done) { try { pool[t].join(); done = true; } catch (InterruptedException e) { intr = true; } } }
        if (intr) Thread.currentThread().interrupt();
        for (Throwable x : xs) if (x != null) throw Lift.rethrow(x);
    }
}
