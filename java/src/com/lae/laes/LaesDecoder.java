// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Port of the LAE Lossless part of the C API (src/laes_api.cpp, src/lae_api.h: the laes_* functions, API version 9), on top of Play.
 * Differences forced by Java (no pointers):
 *   - the read callback is Play.ReadFn (offset, buffer, offset in the buffer, length) -&gt; 0 on success (lae_read_fn without ctx);
 *   - out-parameters (lae_magic, err, key / value) are String[1] arrays (may be null); strings are not truncated to errlen;
 *   - laes_index_layers takes an AtomicBoolean as the cancel flag (C++: const volatile int*, polled every 20 ms by a watcher thread);
 *   - laes_read writes into an int[] from index 0 and returns -1 when the array is shorter than 2 x frames;
 *   - laes_stop ends a player's threads, laes_close those of every player of the file not stopped (the C++ destructors do that); a
 *     player is not usable after laes_close of its file.
 *   - Java only: a memory budget (laes_open_reader with memory_bytes, laes_set_memory; default 64 MB) bounds the heap of a file and its players.
 * Threads: one laes_file must not be used by two threads at once (as in C++).
 */
public final class LaesDecoder {
    private LaesDecoder() {}

    /** port of lae_api.h:laes_file_info */
    public static final class FileInfo { public int is_flac, base_rate, core_bits, choices; public long core_frames; public int payload_ok; public byte[] file_sha256 = new byte[32]; public int quick; }
    /** port of lae_api.h:laes_choice_info */
    public static final class ChoiceInfo { public int kind, master, level, rate, bits; public long frames; public String name, what; }
    /** port of laes_api.cpp:laes_file */
    public static final class LaesFileH { final Play.FnSrc src = new Play.FnSrc(); final Play.File f = new Play.File();
        final java.util.List<LaesPlayH> players = new java.util.ArrayList<LaesPlayH>(); }   // FU25-4: the players started and not stopped (laes_close stops them)
    /** port of laes_api.cpp:laes_play */
    public static final class LaesPlayH { final Play.Player p = new Play.Player(); LaesFileH owner = null; }

    static void setErrS(String[] err, String m) { if (err != null && err.length > 0) err[0] = m; }

    /** Java only (2026-11-04): the default memory budget of a decoder, in bytes (the heap its open file and players may use, about) */
    public static final long LAES_DEFAULT_MEMORY = 64L << 20;
    /**
     * laes_open_reader with a memory budget in bytes (0 or less: LAES_DEFAULT_MEMORY). Java only: the decoder's heap then stays near the
     * budget whatever the track's length (the core is read on demand; look-ahead windows follow the budget). An app can pass a share of
     * Runtime.getRuntime().maxMemory(); about half of it leaves the garbage collector room (with the heap no larger than the budget, decoding
     * gets about 2 x slower). The decoded values never depend on the budget; a small one makes decoding slower, not different.
     */
    // port of laes_api.cpp:laes_open_reader (with the Java memory budget)
    public static LaesFileH laes_open_reader(Play.ReadFn read, long size, int quick_info, String[] lae_magic, String[] err, long memory_bytes) {
        String[] mg = {""}; if (lae_magic != null && lae_magic.length > 0) lae_magic[0] = "";
        if (read == null) { setErrS(err, "no read function"); return null; }
        try { LaesFileH h = new LaesFileH(); h.src.n = size; h.src.fn = read; h.f.memBudget = memory_bytes > 0 ? memory_bytes : LAES_DEFAULT_MEMORY;
            final String e = h.f.open(h.src, mg, quick_info != 0); if (lae_magic != null && lae_magic.length > 0) lae_magic[0] = mg[0];
            if (!e.isEmpty()) { setErrS(err, e); return null; }
            return h; }
        catch (OutOfMemoryError x) { setErrS(err, "out of memory"); }
        catch (RuntimeException x) { setErrS(err, String.valueOf(x.getMessage())); }
        return null; }
    /** Java only: changes the memory budget (bytes; 0 or less: the default) for the players started after this call */
    public static int laes_set_memory(LaesFileH h, long memory_bytes) { if (h == null) return -1; h.f.memBudget = memory_bytes > 0 ? memory_bytes : LAES_DEFAULT_MEMORY; return 0; }
    // port of laes_api.cpp:laes_open_reader
    public static LaesFileH laes_open_reader(Play.ReadFn read, long size, int quick_info, String[] lae_magic, String[] err) { return laes_open_reader(read, size, quick_info, lae_magic, err, LAES_DEFAULT_MEMORY); }
    // port of laes_api.cpp:laes_get_info
    public static int laes_get_info(LaesFileH h, FileInfo out) {
        if (h == null || out == null) return -1; final Play.File f = h.f;
        out.is_flac = f.flac ? 1 : 0; out.base_rate = f.baseRate; out.core_bits = f.coreBits; out.choices = f.choices.size(); out.core_frames = f.coreFrames;
        out.payload_ok = f.why.isEmpty() ? 1 : 0; System.arraycopy(f.fileSha, 0, out.file_sha256, 0, 32); out.quick = f.quick ? 1 : 0; return 0; }
    // port of laes_api.cpp:laes_file_problem
    public static String laes_file_problem(LaesFileH h) { return h != null ? h.f.why : "no file"; }
    // port of laes_api.cpp:laes_get_choice
    public static int laes_get_choice(LaesFileH h, int i, ChoiceInfo out) {
        if (h == null || out == null || i < 0 || i >= h.f.choices.size()) return -1; final Play.Choice c = h.f.choices.get(i);
        out.kind = c.kind; out.master = c.m; out.level = c.level; out.rate = c.rate; out.bits = c.bits; out.frames = c.frames; out.name = c.name; out.what = c.what; return 0; }
    // port of laes_api.cpp:laes_get_tag
    public static int laes_get_tag(LaesFileH h, int i, String[] key, String[] value) {
        if (h == null || i < 0 || i >= h.f.tags.size()) return -1; setErrS(key, h.f.tags.get(i).key); setErrS(value, h.f.tags.get(i).value); return 0; }   // FU25-7: an empty array is skipped like null
    /** port of lae_api.h:LAES_MASTER_NO_DERIVE (API 7) */
    public static final int LAES_MASTER_NO_DERIVE = LaesFile.NO_DERIVE;
    // port of laes_api.cpp:laes_master_name  (API 7; m = the file index of choice_info.master; "" = no name; null = no such master)
    public static String laes_master_name(LaesFileH h, int m) {
        if (h == null || !h.f.why.isEmpty() || m < 0 || m >= h.f.ms.size()) return null; return h.f.ms.get(m).I.name; }
    // port of laes_api.cpp:laes_master_flags  (API 7)
    public static int laes_master_flags(LaesFileH h, int m) {
        if (h == null || !h.f.why.isEmpty() || m < 0 || m >= h.f.ms.size()) return -1; return h.f.ms.get(m).I.flags; }
    // port of laes_api.cpp:laes_master_number  (API 7; the number shown in menus: the place in the menu order; 0 = no such master)
    public static int laes_master_number(LaesFileH h, int m) {
        if (h == null || !h.f.why.isEmpty() || m < 0 || m >= h.f.ms.size()) return 0; final java.util.List<LaesFile.MasterInfo> mi = new java.util.ArrayList<LaesFile.MasterInfo>();
        for (Play.Master M : h.f.ms) mi.add(M.I); return Choices.displayNo(mi, m); }
    // port of laes_api.cpp:laes_default_choice  (API 9, 2026-11-12: the default master written when encoded, else the core master, 0;
    // a player that cannot play it plays the core master; never chosen by bit rate, size or device state)
    public static int laes_default_choice(LaesFileH h) { return h != null ? h.f.defaultChoice : 0; }
    // port of laes_api.cpp:laes_core_flags  (API 9: bit 0 = the core master offers no derived 16-bit version)
    public static int laes_core_flags(LaesFileH h) { return h != null && h.f.why.isEmpty() ? h.f.head.coreFlags : 0; }
    // port of laes_api.cpp:laes_choice_from  (API 7; a derived 16-bit choice: the choice it is made from; else -1)
    public static int laes_choice_from(LaesFileH h, int i) {
        if (h == null || i < 0 || i >= h.f.choices.size()) return -1; final Play.Choice c = h.f.choices.get(i); return c.kind == 3 ? c.from : -1; }
    // port of laes_api.cpp:startImpl
    static LaesPlayH startImpl(LaesFileH h, int choice, int threads, boolean all, String[] err, long frame) {
        if (h == null) { setErrS(err, "no file"); return null; }
        LaesPlayH p = null;
        try { p = new LaesPlayH(); if (threads <= 0) threads = Math.max(1, Lift.defaultThreads() / 2);
            final String e = p.p.start(h.f, choice, threads, all, frame); if (!e.isEmpty()) { setErrS(err, e); p.p.close(); return null; }
            p.owner = h; synchronized (h.players) { h.players.add(p); } return p; }
        catch (OutOfMemoryError x) { setErrS(err, "out of memory"); }
        catch (RuntimeException x) { setErrS(err, String.valueOf(x.getMessage())); }
        if (p != null) p.p.close(); return null; }
    // port of laes_api.cpp:laes_start
    public static LaesPlayH laes_start(LaesFileH h, int choice, int threads, String[] err) { return startImpl(h, choice, threads, false, err, 0); }
    // port of laes_api.cpp:laes_start_all
    public static LaesPlayH laes_start_all(LaesFileH h, int choice, int threads, String[] err) { return startImpl(h, choice, threads, true, err, 0); }
    // port of laes_api.cpp:laes_start_at  (frame: uint64 in C; a negative long here is a frame beyond 2^63, refused as outside the track)
    public static LaesPlayH laes_start_at(LaesFileH h, int choice, long frame, int threads, String[] err) { return startImpl(h, choice, threads, false, err, frame); }
    // port of laes_api.cpp:laes_index_layers
    public static void laes_index_layers(LaesFileH h, AtomicBoolean cancel, int threads) {
        if (h == null) return;
        try { h.f.indexLayers(cancel, threads > 0 ? threads : Math.max(1, Lift.defaultThreads() / 2)); } catch (RuntimeException x) { /* as the C++: catch (...) {} */ } catch (OutOfMemoryError x) { /* as the C++ */ } }
    // port of laes_api.cpp:laes_index_save
    public static long laes_index_save(LaesFileH h, byte[] buf, long cap) {
        if (h == null) return 0; try { final byte[] r = h.f.exportIndex(); if (buf != null && cap >= r.length && buf.length >= r.length) System.arraycopy(r, 0, buf, 0, r.length); return r.length; }
        catch (RuntimeException x) { return 0; } catch (OutOfMemoryError x) { return 0; } }
    // port of laes_api.cpp:laes_index_load
    public static int laes_index_load(LaesFileH h, byte[] data, long len) {
        if (h == null || data == null || len <= 0 || len > data.length) return -1; try { return h.f.importIndex(data, (int) len) ? 0 : -1; }
        catch (RuntimeException x) { return -1; } catch (OutOfMemoryError x) { return -1; } }
    // port of laes_api.cpp:laes_read
    public static long laes_read(LaesPlayH p, int[] out, long frames) {
        if (p == null || out == null || frames < 0) return -1;
        if ((long) out.length < frames * 2) return -1;   // Java: the C++ trusts the caller's buffer size
        try { final long k = p.p.read(out, frames); return p.p.err.isEmpty() ? k : -1; }
        catch (OutOfMemoryError x) { p.p.err = "out of memory"; } catch (RuntimeException x) { p.p.err = String.valueOf(x.getMessage()); }
        return -1; }
    // port of laes_api.cpp:laes_verified
    public static int laes_verified(LaesPlayH p) { if (p == null) return -1; if (!p.p.err.isEmpty()) return -1; return p.p.verified() ? 1 : 0; }
    // port of laes_api.cpp:laes_play_error
    public static String laes_play_error(LaesPlayH p) {
        if (p == null) return "no decoder"; if (!p.p.err.isEmpty()) return p.p.err; if (p.p.done && !p.p.ok) return "SHA-256 mismatch: the decoded master differs from the original"; return ""; }
    // port of laes_api.cpp:laes_master_ok
    public static int laes_master_ok(LaesPlayH p, int m) { return p != null && p.p.err.isEmpty() ? p.p.masterOk(m) : -1; }
    // port of laes_api.cpp:laes_stop
    public static void laes_stop(LaesPlayH p) { if (p != null) { p.p.close(); final LaesFileH o = p.owner; if (o != null) { synchronized (o.players) { o.players.remove(p); } p.owner = null; } } }
    // port of laes_api.cpp:laes_close
    // FU25-4: also stops the players of this file that were not stopped (the C++ destructors end their threads; here they would wait for ever)
    public static void laes_close(LaesFileH h) { if (h == null) return; final LaesPlayH[] ps; synchronized (h.players) { ps = h.players.toArray(new LaesPlayH[0]); h.players.clear(); }
        for (LaesPlayH p : ps) { p.p.close(); p.owner = null; } }
}
