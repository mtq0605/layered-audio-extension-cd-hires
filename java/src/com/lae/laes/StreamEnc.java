// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.CRC32;

import com.lae.core.LaeException;

/**
 * Port of src/laes_stream_enc.h (namespace laess). LAES encoder, block-wise (2026-10-30): the same payload as laes::encodeLaes
 * (laes_file_enc.h), byte for byte, with the memory of a few pieces. A master is read where it is (a WAV in place, a FLAC decoded once
 * into the work folder, or with --small-disk decoded again whenever it is read); its rate levels and octave layers are computed piece by
 * piece (laes_stream.h, Stream) and do not depend on the file order, so they are made once for both orders the tool tries. Every candidate
 * storage of a master is coded to a file; only the cheapest is kept. The payload is written to a file with its CRC-32 added up piece by piece.
 * Disk (2026-11-05, C++ 3c0f89b / d1c71e8 / 77dc9ef; Java port 2026-11-06): levels in 4 bytes when they fit, only the levels a candidate
 * reads kept, candidates keep only their sizes, a guard on the free space (wait, or stop with the work folder kept), state files for a run
 * that continues where an earlier one stopped.
 * Java: every file is closed explicitly (the C++ destructors); Ctx carries the thread count, the cancel flag and the progress listener;
 * the core is held as int32 (Core) instead of int64 (the values are the same: a core sample has at most 32 bits). The two channels are
 * made one after the other and the masters read one after the other (the C++ does them at once for speed; the files are the same).
 */
public final class StreamEnc {
    private StreamEnc() {}

    /** what the encoder's loops need besides their data: threads (resolved, >= 1), cancellation, progress */
    public static final class Ctx { public int threads = 1; public AtomicBoolean cancel = new AtomicBoolean(false); public LaesEncoder.Listener listener = null;
        String stage = ""; double done = 0, total = 1;
        void check() { if (cancel != null && cancel.get()) throw new LaeException("cancelled"); }
        void step(double add) { done += add; if (listener != null) listener.progress(stage, total > 0 ? Math.min(1.0, done / total) : 0); check(); }
        void say(String s) { if (listener != null) listener.message(s); } }

    // ---- disk guard (2026-11-05): while the work folder is written, the free space of its disk is looked at (at most every 0.5 s); below
    // the reserve the run waits until there is room again (stop, --disk-stop: it stops, the work folder kept; removeNote, --disk-stop-remove:
    // the tool removes it). With track, the largest size of the work folder seen is kept (--timing). The files written do not change.
    /** port of laes_stream_enc.h:DiskGuard (cancel: Java only, a wait for room ends when the run is cancelled) */
    public static final class DiskGuard { public String dir = ""; public long reserve = 2L << 30; public boolean track = false, stop = false, removeNote = false; public long peak = 0; long last = 0;
        public LaesEncoder.Listener listener = null; public AtomicBoolean cancel = null; }
    static volatile DiskGuard guard = null;
    /** port of laes_stream_enc.h:diskGuard (the guard of the running encode; null = none) */
    public static void diskGuard(DiskGuard g) { guard = g; }
    // port of laes_stream_enc.h:folderBytes
    public static long folderBytes(String dir) { return folderBytes(new File(dir)); }
    static long folderBytes(File f) { if (f.isFile()) return f.length(); long s = 0; final File[] k = f.listFiles(); if (k != null) for (File c : k) s += folderBytes(c); return s; }
    static void sayG(DiskGuard g, String s) { if (g.listener != null) g.listener.message(s); }
    // port of laes_stream_enc.h:diskCheck  (Java: File.getUsableSpace for std::filesystem::space().available)
    public static void diskCheck() { diskCheck(false); }
    public static void diskCheck(boolean now) {
        final DiskGuard g = guard; if (g == null) return;
        synchronized (g) {
            final long t = System.nanoTime(); if (!now && g.last != 0 && t - g.last < 500_000_000L) return; g.last = t;
            final File d = new File(g.dir); final long avail = d.getUsableSpace(); final boolean ec = avail == 0 && !d.exists();
            if (g.track) g.peak = Math.max(g.peak, folderBytes(d));
            if (ec || avail >= g.reserve) return;
            final String left = (avail >> 20) + " MB left, " + (g.reserve >> 20) + " MB kept free";
            if (g.stop) { sayG(g, "  磁盘快满（剩 " + (avail >> 20) + (g.removeNote ? " MB）：--disk-stop-remove，已停下，工作文件夹已删除，没有写出文件"
                                                                                       : " MB）：--disk-stop，已停下，工作文件夹保留；之后再运行同一条命令，从停下的地方接着做"));
                throw new LaeException("the disk of the work folder is almost full (" + left + (g.removeNote ? "); --disk-stop-remove: stopped, the work folder removed" : "); --disk-stop: stopped")); }
            // the default (2026-11-05): wait, nothing removed; the run goes on once the disk has room again (other threads wait at their next check)
            sayG(g, "disk almost full (" + left + "): free some space on the disk of " + g.dir + " and the encoding continues by itself;");
            sayG(g, "  or close this program and run the same command again later: it continues from the work folder");
            sayG(g, "  磁盘快满（剩 " + (avail >> 20) + " MB）：请在 " + g.dir + " 所在的磁盘上清理一些空间，编码会自动接着做；也可以关掉程序，之后再运行同一条命令，从工作文件夹接着做");
            long a2;
            do { for (int i = 0; i < 50; i++) { if (g.cancel != null && g.cancel.get()) throw new LaeException("cancelled"); try { Thread.sleep(100); } catch (InterruptedException x) { throw new LaeException("cancelled"); } }
                 a2 = d.getUsableSpace(); } while (a2 < g.reserve + (256L << 20));
            sayG(g, "disk has room again (" + (a2 >> 20) + " MB free): continuing");
            sayG(g, "  磁盘有空间了（剩 " + (a2 >> 20) + " MB）：接着做"); g.last = System.nanoTime(); } }

    // ---- resume (2026-11-05): small state files in the work folder, each written whole (a .new file, then renamed), so that a run stopped at
    // any point (disk full, closed) continues where it was. They hold only what a full run computes again the same way.
    // port of laes_stream_enc.h:putState
    public static void putState(String p, String text) { final byte[] b = text.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        { Stream.Out o = new Stream.Out(p + ".new"); try { o.put(b, 0, b.length); o.close(); o = null; } finally { closeQuiet(o); } }
        final File to = new File(p); to.delete(); if (!new File(p + ".new").renameTo(to)) Stream.fail("cannot write " + p); }
    // port of laes_stream_enc.h:sizeOrMinus (-1: missing)
    public static long sizeOrMinus(String p) { final File f = new File(p); return f.isFile() ? f.length() : -1; }
    // port of laes_stream_enc.h:getState (null: missing)
    public static String getState(String p) { if (!new File(p).exists()) return null; Stream.In in = new Stream.In(p);
        try { final byte[] b = new byte[(int) in.n]; if (in.n != 0) in.read(0, b, 0, b.length); return new String(b, java.nio.charset.StandardCharsets.UTF_8); } finally { in.close(); } }
    /** one value per line (port of laes_stream_enc.h:StateW) */
    public static final class StateW { public final StringBuilder s = new StringBuilder();
        public void i(long v) { s.append(v).append('\n'); } public void d(double v) { s.append(g17(v)).append('\n'); }
        public void str(String v) { s.append(v).append('\n'); } public void hex(byte[] p, int n) { for (int k = 0; k < n; k++) s.append(String.format("%02x", p[k] & 0xFF)); s.append('\n'); } }
    /** port of laes_stream_enc.h:StateR */
    public static final class StateR { final String s; int at = 0; public boolean bad = false; public StateR(String t) { s = t; }
        public String str() { final int e = s.indexOf('\n', at); if (e < 0) { bad = true; return ""; } final String v = s.substring(at, e); at = e + 1; return v; }
        public long i() { final String v = str(); if (v.isEmpty()) { bad = true; return 0; } return CStd.atoll(v); } public double d() { return CStd.atof(str()); }
        public void hex(byte[] p, int n) { final String v = str(); if (v.length() != 2 * n) { bad = true; return; } for (int k = 0; k < n; k++) p[k] = (byte) Integer.parseInt(v.substring(2 * k, 2 * k + 2), 16); } }
    // "%.17g": enough digits to read the same double back (only this port reads its state files)
    static String g17(double v) { return Double.toString(v); }
    // port of laes_stream_enc.h:putInfo / getInfo
    public static void putInfo(StateW w, LaesFile.MasterInfo I) { w.i(I.bits); w.i(I.stages); w.i(I.w); w.i(I.storage); w.i(I.refTier); w.i(I.rate); w.i(I.frames); w.i(I.whole); w.i(I.frac & 0xFFFFFFFFL); w.hex(I.sha, 32); w.d(I.layerBits); }
    public static void getInfo(StateR r, LaesFile.MasterInfo I) { I.bits = (int) r.i(); I.stages = (int) r.i(); I.w = (int) r.i(); I.storage = (int) r.i(); I.refTier = (int) r.i(); I.rate = (int) r.i(); I.frames = r.i(); I.whole = r.i(); I.frac = (int) r.i(); r.hex(I.sha, 32); I.layerBits = r.d(); }

    /** the core, int32 per sample (laes::Pcm with int64 samples in the C++; laess::Core32) */
    public static final class Core { public int rate = 0, bits = 0; public int[][] c = {new int[0], new int[0]}; public long frames() { return c[0].length; } }
    /** a core channel as a signal (laess::Core32Sig) */
    // port of laes_stream_enc.h:Core32Sig
    public static final class IntSig extends Stream.Sig { final int[] v; public IntSig(int[] x) { v = x; n = x.length; }
        // port of laes_stream_enc.h:Core32Sig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) Stream.fail("internal error: signal range"); for (int i = (int) a; i < b; i++) out[outOff + i - (int) a] = v[i]; } }
    // port of laes_stream_enc.h:hashCore32 (= laes_file.h:hashAnyCore: 16 bit as int16 LE, else int32 LE)
    public static void hashAnyCore(Core p, byte[] out) { LaesFile.Sha256 s = new LaesFile.Sha256(); byte[] b = new byte[1 << 16]; int fill = 0; final boolean b16 = p.bits == 16;
        for (int i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { final int v = p.c[c][i]; b[fill++] = (byte) v; b[fill++] = (byte) (v >>> 8); if (!b16) { b[fill++] = (byte) (v >>> 16); b[fill++] = (byte) (v >>> 24); } if (fill > (1 << 16) - 8) { s.update(b, 0, fill); fill = 0; } }
        s.update(b, 0, fill); s.finish(out); }
    /** a signal that reads a file: closed when the C++ unique_ptr would be destroyed */
    interface Closing { void close(); }
    static void close(Stream.Sig s) { if (s instanceof Closing) ((Closing) s).close(); else if (s instanceof Stream.FileSig) ((Stream.FileSig) s).close(); else if (s instanceof Stream.RawSig) ((Stream.RawSig) s).in.close(); }
    static void closeQuiet(Stream.Sig s) { try { if (s != null) close(s); } catch (RuntimeException x) { /* already failing */ } }
    static void closeQuiet(Stream.Out o) { try { if (o != null) o.close(); } catch (RuntimeException x) { /* already failing */ } }
    static void closeQuiet(Stream.In i) { try { if (i != null) i.close(); } catch (RuntimeException x) { /* already failing */ } }

    // a layer (one channel) coded in pieces: blocks of B; a piece is a whole number of blocks (only the last piece may end in a short block).
    // As laes::encodeLayer: the blocks' parameters are chosen in parallel threads, then written in order; the coded bytes go to a file.
    /** port of laes_stream_enc.h:LayerOut */
    public static final class LayerOut { final long B; final CodecEnc.BitW w = new CodecEnc.BitW(); final Stream.Out o; long done = 0; final Ctx ctx;
        public LayerOut(String p, long b, Ctx cx) { B = b; ctx = cx; o = new Stream.Out(p); }
        // port of laes_stream_enc.h:LayerOut::feed  (x[0 .. n))
        public void feed(final long[] x, final long n) {
            final int nb = (int) ((n + B - 1) / B); final CodecEnc.BlockPlan[] plans = new CodecEnc.BlockPlan[Math.max(0, nb)];
            // each thread takes the next block not yet taken (L5, 2026-11-03; blocks cost very different amounts, so fixed ranges left threads
            // waiting). Each block's plan depends only on its own samples, so the plans and the bytes written are the same.
            Par.forBlocks(nb, ctx.threads, new Par.Block() { public void run(int k) { final long s = (long) k * B; plans[k] = CodecEnc.planBlock(x, (int) s, (int) Math.min(B, n - s)); } }, ctx.cancel);
            for (int k = 0; k < nb; k++) { final long s = (long) k * B; CodecEnc.writeBlock(w, x, (int) s, (int) Math.min(B, n - s), plans[k]); }
            o.put(w.b, 0, w.len); w.len = 0; done += n; diskCheck(); }   // the whole bytes so far; the bits of a byte not yet full stay in w
        // port of laes_stream_enc.h:LayerOut::finish
        public long finish() { final long bits = w.bits; w.flush(); o.put(w.b, 0, w.len); w.len = 0; o.close(); return bits; }
        void abandon() { closeQuiet(o); } }
    // copies a byte range of one file into an output, with the payload's running CRC-32 (java.util.zip.CRC32: the same polynomial 0xEDB88320,
    // initial value and final complement as the C++ loop)
    /** port of laes_stream_enc.h:CrcOut */
    static final class CrcOut { final Stream.Out o; final CRC32 crc = new CRC32(); final byte[] t = new byte[8];
        CrcOut(Stream.Out out) { o = out; }
        // port of laes_stream_enc.h:CrcOut::put
        void put(byte[] p, int off, int n) { crc.update(p, off, n); o.put(p, off, n); }
        // port of laes_stream_enc.h:CrcOut::u
        void u(long v, int n) { for (int i = 0; i < n; i++) t[i] = (byte) (v >>> (8 * i)); put(t, 0, n); }
        // port of laes_stream_enc.h:CrcOut::file
        void file(String p) { Stream.In in = new Stream.In(p); try { byte[] b = new byte[0]; for (long a = 0; a < in.n; a += 1 << 22) { final int k = (int) Math.min(1 << 22, in.n - a); if (b.length < k) b = new byte[k]; in.read(a, b, 0, k); put(b, 0, k); diskCheck(); } } finally { in.close(); } } }

    // ---- a master (disk, 2026-11-05): its samples are read where they are, a WAV in place (no copy), a FLAC decoded once into the work
    // folder at its own word length (raw: the sample file, rawOff: where the samples start, rawBy: bytes per sample); SHA-256, unused low
    // bits w; its levels 0..stages-1 (samples >> w; 4 bytes per value when all fit, else 8; lby) - only the levels a candidate can read are
    // kept, the others are removed once the level below is made - and its octave layers 1..stages coded (they do not depend on the file order)
    /** port of laes_stream_enc.h:Prep */
    public static final class Prep { public String path = "", raw = ""; public long rawOff = 0; public int rawBy = 4, bits = 0, rate = 0, stages = 0, w = 0; public boolean derive = false; public int threads = 1; public long frames = 0; public byte[] sha = new byte[32];
        public String[][] lev = new String[2][0], dfile = new String[2][0]; public int[][] lby = new int[2][0]; public long[][] dbits = new long[2][0];
        // --small-disk, a FLAC master (flacLive): no decoded copy; raw is the FLAC itself, its frames decoded again when read. fOff: the
        // byte offset of every frame and the end, fFirst: the first sample of every frame (kept in memory, 16 bytes per frame)
        public boolean flacLive = false; public Flac.Info fi = new Flac.Info(); public long[] fOff = new long[0], fFirst = new long[0]; }
    // little-endian signed samples of by bytes (2, 3, 4) as int32 (a 32-bit sample keeps all 32 bits)
    // port of laes_stream_enc.h:unpackPcm
    static void unpackPcm(byte[] q, int qOff, int count, int by, int[] out, int outOff) {
        for (int i = 0; i < count; i++) { final int p = qOff + i * by; int v = 0; for (int k = 0; k < by; k++) v |= (q[p + k] & 0xFF) << (8 * k);
            if (by < 4 && ((v >>> (8 * by - 1)) & 1) != 0) v |= -1 << (8 * by); out[outOff + i] = v; } }
    // frames [a, b) of a flacLive master as int32 L R: the frames that hold them decoded again, split over threads
    // port of laes_stream_enc.h:flacFrames
    static void flacFrames(final Prep m, final long a, final long b, final int[] out) {
        if (a >= b) return; final long[] F = m.fFirst;
        int f0 = upperBound(F, a) - 1; final int f1 = lowerBound(F, b); if (f0 < 0) f0 = 0; final int nf = f1 - f0; final int T = Math.max(1, Math.min(Math.max(1, m.threads), nf / 2));
        final String[] err = new String[T]; final int ff0 = f0;
        Lift.parallelFor(T, T, new Lift.Range() { public void run(long t0, long t1) { for (long tt = t0; tt < t1; tt++) { final int t = (int) tt;
            final int i0 = ff0 + (int) ((long) nf * t / T), i1 = ff0 + (int) ((long) nf * (t + 1) / T); if (i0 >= i1) continue;
            Stream.In in = new Stream.In(m.raw);
            try { final byte[] fb = new byte[(int) (m.fOff[i1] - m.fOff[i0])]; in.read(m.fOff[i0], fb, 0, fb.length); int at = 0; final Flac.Frame Fr = new Flac.Frame();
                for (int i = i0; i < i1; i++) { final String e = masterFlacFrame(fb, at, fb.length - at, m.fi, i, F[i], Fr); if (!e.isEmpty()) { err[t] = m.path + ": " + e; return; } at += Fr.used;
                    for (int k = 0; k < Fr.n; k++) { final long s = F[i] + k; if (s < a || s >= b) continue; out[(int) (s - a) * 2] = (int) Fr.ch[0][k]; out[(int) (s - a) * 2 + 1] = (int) Fr.ch[1][k]; } } }
            catch (RuntimeException x) { err[t] = String.valueOf(x.getMessage()); }
            finally { closeQuiet(in); } } } });
        for (String e : err) if (e != null && !e.isEmpty()) Stream.fail(e); }
    static int upperBound(long[] F, long v) { int lo = 0, hi = F.length; while (lo < hi) { final int mid = (lo + hi) >>> 1; if (F[mid] <= v) lo = mid + 1; else hi = mid; } return lo; }
    static int lowerBound(long[] F, long v) { int lo = 0, hi = F.length; while (lo < hi) { final int mid = (lo + hi) >>> 1; if (F[mid] < v) lo = mid + 1; else hi = mid; } return lo; }
    // one channel of a master's interleaved samples, shifted right by w (as RawSig over the int32 copy before)
    /** port of laes_stream_enc.h:PcmSig */
    static final class PcmSig extends Stream.Sig implements Closing { Stream.In in = null; final Prep pm; final long off; final int by, c, w; byte[] buf = new byte[0]; int[] x = new int[0];
        PcmSig(Prep m, int ch) { pm = m; off = m.rawOff; by = m.rawBy; c = ch; w = m.w; n = m.frames;
            if (!m.flacLive) { in = new Stream.In(m.raw); if (off + n * 2 * by > in.n) { closeQuiet(in); Stream.fail("master samples shorter than the header says: " + m.raw); } } }
        // port of laes_stream_enc.h:PcmSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) Stream.fail("internal error: signal range"); final int k = (int) (b - a); if (x.length < k * 2) x = new int[k * 2];
            if (pm.flacLive) flacFrames(pm, a, b, x);
            else { if (buf.length < k * 2 * by) buf = new byte[k * 2 * by]; in.read(off + a * 2 * by, buf, 0, k * 2 * by); unpackPcm(buf, 0, k * 2, by, x, 0); }
            for (int i = 0; i < k; i++) out[outOff + i] = (long) x[i * 2 + c] >> w; }
        public void close() { if (in != null) in.close(); } }
    // frames [a, a + k) of a master as int32 L R (in: the master's sample file; not read for a flacLive master)
    // port of laes_stream_enc.h:masterFrames
    public static void masterFrames(Stream.In in, Prep m, long a, int k, int[] out, byte[] tmp) { if (m.flacLive) { flacFrames(m, a, a + k, out); return; }
        in.read(m.rawOff + a * 2 * m.rawBy, tmp, 0, k * 2 * m.rawBy); unpackPcm(tmp, 0, k * 2, m.rawBy, out, 0); }
    // a level file: by = 4 (int32) or 8 (int64) bytes per value
    // port of laes_stream_enc.h:keptLevel
    static String keptLevel(String p) { if (p == null || p.isEmpty()) Stream.fail("internal error: a level that was not kept"); return p; }
    /** port of laes_stream_enc.h:LevSig */
    static final class LevSig extends Stream.Sig implements Closing { final Stream.In in; final int by; byte[] buf = new byte[0];
        LevSig(String p, int b) { in = new Stream.In(keptLevel(p)); by = b; if (in.n % by != 0) { closeQuiet(in); Stream.fail("bad signal file " + p); } n = in.n / by; }
        // port of laes_stream_enc.h:LevSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) Stream.fail("internal error: signal range"); final int k = (int) (b - a); if (buf.length < k * by) buf = new byte[k * by];
            in.read(a * by, buf, 0, k * by);
            if (by == 8) ByteBuffer.wrap(buf, 0, k * 8).order(ByteOrder.LITTLE_ENDIAN).asLongBuffer().get(out, outOff, k);
            else for (int i = 0; i < k; i++) out[outOff + i] = com.lae.core.Le.i32(buf, i * 4); }
        public void close() { in.close(); } }
    // int64 values written in order: 4 bytes each while all fit in int32; at the first that does not, the file so far is widened to 8 bytes
    /** port of laes_stream_enc.h:LevOut */
    static final class LevOut { final String p; Stream.Out o; int by = 4; byte[] t = new byte[0];
        LevOut(String path) { p = path; o = new Stream.Out(path); }
        // port of laes_stream_enc.h:LevOut::put
        void put(long[] d, int off, int k) {
            if (by == 4) { boolean fit = true; for (int i = 0; i < k && fit; i++) fit = d[off + i] >= Integer.MIN_VALUE && d[off + i] <= Integer.MAX_VALUE;
                if (fit) { if (t.length < k * 4) t = new byte[k * 4]; for (int i = 0; i < k; i++) com.lae.core.Le.put32(t, i * 4, (int) d[off + i]); o.put(t, 0, k * 4); return; }
                widen(); }
            if (t.length < k * 8) t = new byte[k * 8]; ByteBuffer.wrap(t, 0, k * 8).order(ByteOrder.LITTLE_ENDIAN).asLongBuffer().put(d, off, k); o.put(t, 0, k * 8); }
        // port of laes_stream_enc.h:LevOut::widen
        void widen() { o.close(); o = null; final String q = p + ".i32"; new File(q).delete(); if (!new File(p).renameTo(new File(q))) Stream.fail("cannot rename " + p);
            Stream.In in = new Stream.In(q);
            try { o = new Stream.Out(p); final long P = 4L << 20; byte[] a = new byte[0], b = new byte[0];
                for (long x = 0; x < in.n; x += P) { final int k = (int) (Math.min(P, in.n - x) / 4); if (a.length < k * 4) { a = new byte[k * 4]; b = new byte[k * 8]; } in.read(x, a, 0, k * 4);
                    for (int i = 0; i < k; i++) com.lae.core.Le.put64(b, i * 8, com.lae.core.Le.i32(a, i * 4)); o.put(b, 0, k * 8); diskCheck(); } }
            finally { in.close(); }
            new File(q).delete(); by = 8; }
        void close() { o.close(); }
        void abandon() { closeQuiet(o); } }
    // --small-disk (D6, 2026-11-05): the largest level (stages - 1) is not kept; it is made again from the master whenever it is read (one
    // lifting stage, the same values: fwdPiece gives the same samples for any range)
    /** port of laes_stream_enc.h:DerivedSig */
    static final class DerivedSig extends Stream.Sig implements Closing { final Stream.Sig up; final int th;
        DerivedSig(Prep m, int c, int k) { up = levelOf(m, c, k + 1); th = m.threads; n = (up.n + 1) / 2; }
        // port of laes_stream_enc.h:DerivedSig::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n) Stream.fail("internal error: signal range"); final long[][] sd = Stream.fwdPiece(up, a, b, th); System.arraycopy(sd[0], 0, out, outOff, sd[0].length); }
        public void close() { StreamEnc.close(up); } }
    // port of laes_stream_enc.h:levelOf
    public static Stream.Sig levelOf(Prep m, int c, int k) { if (k >= m.stages) return new PcmSig(m, c);
        if (m.derive && (m.lev[c][k] == null || m.lev[c][k].isEmpty())) return new DerivedSig(m, c, k); return new LevSig(m.lev[c][k], m.lby[c][k]); }

    // reads a 16 / 24 / 32-bit integer PCM WAV (PCM or EXTENSIBLE) as laes_tool readWav does (all 32 bits of a 32-bit file), header only first;
    // or (probeMaster) a FLAC master's STREAMINFO: dataOff = its first audio frame, dataBytes = the size of its samples as PCM
    /** port of laes_stream_enc.h:WavInfo */
    public static final class WavInfo { public int rate = 0, bits = 0; public long dataOff = -1, dataBytes = 0; public long frames = 0; public boolean flac = false; public Flac.Info fi = new Flac.Info(); }
    static boolean eq4(byte[] b, int o, String s) { for (int i = 0; i < 4; i++) if (b[o + i] != (byte) s.charAt(i)) return false; return true; }
    // port of laes_stream_enc.h:probeWav  (lae: null, or {offset, length} of the "LAE " chunk (offset -1: none))
    public static String probeWav(String path, WavInfo wi, long[] lae) {
        Stream.In in = new Stream.In(path);
        try { return probeWavImpl(path, in, wi, lae); } finally { in.close(); } }
    private static String probeWavImpl(String path, Stream.In in, WavInfo wi, long[] lae) {
        final byte[] h = new byte[12]; if (in.n < 12) return path + ": not RIFF/WAVE"; in.read(0, h, 0, 12);
        final boolean rf64 = eq4(h, 0, "RF64") || eq4(h, 0, "BW64");   // beyond 4 GB (EBU Tech 3306 / ITU-R BS.2088): 64-bit sizes in "ds64"
        if ((!eq4(h, 0, "RIFF") && !rf64) || !eq4(h, 8, "WAVE")) return path + ": not RIFF/WAVE";
        long pos = 12; int tag = 0, ch = 0; if (lae != null) lae[0] = -1;
        long dsData = 0, dsLae = 0;
        if (rf64) { final byte[] d = new byte[48]; if (in.n < 20 + 28) return path + ": RF64 without ds64"; in.read(12, d, 0, 8); if (!eq4(d, 0, "ds64")) return path + ": RF64 without ds64";
            final long dl = (d[4] & 0xFF) | (d[5] & 0xFF) << 8 | (d[6] & 0xFF) << 16 | (long) (d[7] & 0xFF) << 24; if (dl < 28) return path + ": RF64 ds64 too short"; in.read(20, d, 0, (int) Math.min(dl, 40));
            dsData = le64(d, 8);
            if (dl >= 40 && ((d[24] & 0xFF) | (d[25] & 0xFF) << 8 | (d[26] & 0xFF) << 16 | (long) (d[27] & 0xFF) << 24) >= 1 && eq4(d, 28, "LAE ")) dsLae = le64(d, 32); }
        final byte[] c = new byte[8];
        while (pos + 8 <= in.n) { in.read(pos, c, 0, 8); long n = (c[4] & 0xFF) | (c[5] & 0xFF) << 8 | (c[6] & 0xFF) << 16 | (long) (c[7] & 0xFF) << 24; final long body = pos + 8;
            if (rf64 && n == 0xFFFFFFFFL) n = eq4(c, 0, "data") ? dsData : eq4(c, 0, "LAE ") ? dsLae : n;
            if (n > in.n - body) n = in.n - body;   // C++ (long long)n: the same signed comparison
            if (eq4(c, 0, "fmt ") && com.lae.core.U.ge(n, 16)) { final byte[] f = new byte[26]; in.read(body, f, 0, com.lae.core.U.ge(n, 26) ? 26 : 16); tag = (f[0] & 0xFF) | (f[1] & 0xFF) << 8; ch = (f[2] & 0xFF) | (f[3] & 0xFF) << 8;
                wi.rate = (f[4] & 0xFF) | (f[5] & 0xFF) << 8 | (f[6] & 0xFF) << 16 | (f[7] & 0xFF) << 24; wi.bits = (f[14] & 0xFF) | (f[15] & 0xFF) << 8; if (tag == 0xFFFE && com.lae.core.U.ge(n, 26)) tag = (f[24] & 0xFF) | (f[25] & 0xFF) << 8; }
            else if (eq4(c, 0, "data")) { wi.dataOff = body; wi.dataBytes = n; }
            else if (eq4(c, 0, "LAE ") && lae != null) { lae[0] = body; lae[1] = n; }
            pos = body + n + (n & 1); }
        if (tag != 1 || ch != 2 || (wi.bits != 16 && wi.bits != 24 && wi.bits != 32) || wi.dataOff < 0) return path + ": need 16/24/32-bit integer PCM, 2 channels";
        wi.frames = wi.dataBytes / (2 * (wi.bits / 8)); return ""; }
    static long le64(byte[] d, int at) { long v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | (d[at + i] & 0xFF); return v; }
    // the samples of frames [a, b) of a probed WAV as int32 L R (out[0 .. 2 (b - a)); buf: scratch, returned grown)
    // port of laes_stream_enc.h:wavFrames
    public static byte[] wavFrames(Stream.In in, WavInfo wi, long a, long b, int[] out, byte[] buf) {
        final int by = wi.bits / 8; final int nb = (int) ((b - a) * 2 * by); if (buf == null || buf.length < nb) buf = new byte[nb]; in.read(wi.dataOff + a * 2 * by, buf, 0, nb);
        unpackPcm(buf, 0, (int) ((b - a) * 2), by, out, 0); return buf; }
    // a FLAC master (2026-11-05): STREAMINFO (2 channels, 16 / 24 / 32 bits, a sample count) and where the audio frames start
    // port of laes_stream_enc.h:probeFlac
    static String probeFlac(String path, WavInfo wi) {
        Stream.In in = new Stream.In(path);
        try { long p = 4; boolean last = false, haveSi = false; final Flac.Info fi = wi.fi; final byte[] bh = new byte[4];
            while (!last) { if (p + 4 > in.n) return path + ": FLAC metadata truncated"; in.read(p, bh, 0, 4); last = (bh[0] & 0x80) != 0; final int type = bh[0] & 0x7F; final long len = (long) (bh[1] & 0xFF) << 16 | (bh[2] & 0xFF) << 8 | (bh[3] & 0xFF); p += 4;
                if (p + len > in.n) return path + ": FLAC metadata truncated";
                if (type == 0) { if (len != 34) return path + ": bad STREAMINFO"; final byte[] s = new byte[34]; in.read(p, s, 0, 34); final Flac.BR r = new Flac.BR(s, 0, 34); fi.minBs = (int) r.get(16); fi.maxBs = (int) r.get(16); r.get(24); r.get(24);
                    fi.rate = (int) r.get(20); fi.channels = (int) r.get(3) + 1; fi.bps = (int) r.get(5) + 1; fi.total = r.get(36); System.arraycopy(s, 18, fi.md5, 0, 16); haveSi = true; }
                p += len; }
            if (!haveSi) return path + ": no STREAMINFO";
            if (fi.channels != 2 || (fi.bps != 16 && fi.bps != 24 && fi.bps != 32) || fi.total == 0 || fi.maxBs < 16) return path + ": need a 2-channel 16/24/32-bit FLAC with a sample count";
            wi.flac = true; wi.rate = fi.rate; wi.bits = fi.bps; wi.frames = fi.total; wi.dataOff = p; wi.dataBytes = fi.total * 2 * (fi.bps / 8); return ""; }
        finally { in.close(); } }
    // port of laes_stream_enc.h:probeMaster
    public static String probeMaster(String path, WavInfo wi) { final byte[] h = new byte[4]; { Stream.In in = new Stream.In(path); try { if (in.n >= 4) in.read(0, h, 0, 4); } finally { in.close(); } }
        return eq4(h, 0, "fLaC") ? probeFlac(path, wi) : probeWav(path, wi, null); }
    // one FLAC frame of a master (laesflac::decodeFrame, plus the stereo decorrelations a FLAC encoder uses: left / side, side / right,
    // mid / side, the side channel one bit wider). Into F (its arrays reused when long enough): F.n samples per channel, F.used its size in bytes.
    // port of laes_stream_enc.h:masterFlacFrame
    static String masterFlacFrame(byte[] buf, int fp, long avail, Flac.Info in, long frameNo, long have, Flac.Frame F) {
        final Flac.BR r = new Flac.BR(buf, fp, avail); F.n = 0; F.used = 0;
        if (r.get(15) != 0x7FFC) return "lost frame sync"; final int strategy = (int) r.get(1); final int bsc = (int) r.get(4), src = (int) r.get(4), chc = (int) r.get(4), ssc = (int) r.get(3);
        if (r.get(1) != 0) return "frame header reserved bit set";
        if (chc != 1 && chc != 8 && chc != 9 && chc != 10) return "frame without 2 channels";
        long num = r.get(8); { int extra = 0; if (num == 0xFF) return "bad frame number"; if (num >= 0xFE) { if (strategy == 0) return "bad frame number"; extra = 6; } else if (num >= 0xFC) extra = 5; else if (num >= 0xF8) extra = 4; else if (num >= 0xF0) extra = 3; else if (num >= 0xE0) extra = 2; else if (num >= 0xC0) extra = 1; else if (num >= 0x80) return "bad frame number";
            if (extra != 0) { num &= (0x3F >> extra); for (int i = 0; i < extra; i++) { final long b = r.get(8); if ((b & 0xC0) != 0x80) return "bad frame number"; num = num << 6 | (b & 0x3F); } } }
        long n; if (bsc == 1) n = 192; else if (bsc >= 2 && bsc <= 5) n = 576L << (bsc - 2); else if (bsc == 6) n = r.get(8) + 1; else if (bsc == 7) { n = r.get(16) + 1; if (n == 65536) return "bad block size"; } else if (bsc >= 8) n = 256L << (bsc - 8); else return "bad block size";
        if (n > in.maxBs || com.lae.core.U.gt(have + n, in.total)) return "block larger than STREAMINFO allows";
        if (src == 12) r.get(8); else if (src == 13 || src == 14) r.get(16); else if (src == 15) return "bad sample rate code";
        final int bps = ssc == 0 ? in.bps : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 4 ? 16 : ssc == 5 ? 20 : ssc == 6 ? 24 : ssc == 7 ? 32 : 0; if (bps != in.bps) return "frame bit depth differs from STREAMINFO";
        if (r.fail || (r.pos & 7) != 0) return "frame header truncated"; final int hb = (int) (r.pos / 8); final int c8 = (int) r.get(8); if (r.fail || c8 != Flac.crc8(buf, fp, hb)) return "frame header CRC-8 mismatch";
        if (strategy == 0 && num != frameNo) return "frame number out of order";
        final int ni = (int) n;
        for (int c = 0; c < 2; c++) { if (F.ch[c] == null || F.ch[c].length < ni) F.ch[c] = new long[ni];
            final boolean side = (chc == 8 && c == 1) || (chc == 9 && c == 0) || (chc == 10 && c == 1); if (!Flac.readSubframe(r, F.ch[c], 0, ni, bps + (side ? 1 : 0), F)) return "damaged subframe"; }
        final long[] a = F.ch[0], b = F.ch[1];
        if (chc == 8) for (int i = 0; i < ni; i++) b[i] = a[i] - b[i];
        else if (chc == 9) for (int i = 0; i < ni; i++) a[i] = a[i] + b[i];
        else if (chc == 10) for (int i = 0; i < ni; i++) { final long md = (a[i] << 1) | (b[i] & 1); a[i] = (md + b[i]) >> 1; b[i] = (md - b[i]) >> 1; }
        r.pos = (r.pos + 7) & ~7L; final long fb = r.pos / 8; if (fb + 2 > r.n) return "frame truncated";
        if (Flac.crc16(buf, fp, (int) fb) != (((buf[fp + (int) fb] & 0xFF) << 8) | (buf[fp + (int) fb + 1] & 0xFF))) return "frame CRC-16 mismatch";
        F.n = ni; F.used = (int) (fb + 2); return ""; }
    // step 1: the master's samples (one pass: SHA-256 of the samples as int32 LE, unused low bits). A WAV stays where it is; a FLAC is decoded
    // into stem_pcm.raw at its own word length and checked against its STREAMINFO MD5.
    // port of laes_stream_enc.h:prepRead
    public static String prepRead(String path, String stem, Prep m, Ctx ctx) {
        final WavInfo wi = new WavInfo(); String e = probeMaster(path, wi); if (!e.isEmpty()) return e;
        m.path = path; m.rate = wi.rate; m.bits = wi.bits; m.frames = wi.frames; m.rawBy = wi.bits / 8; final LaesFile.Sha256 s = new LaesFile.Sha256(); long orv = 0;
        if (!wi.flac) { m.raw = path; m.rawOff = wi.dataOff; Stream.In in = new Stream.In(path);
            try { final long P = 1 << 18; final int[] x = new int[(int) P * 2]; byte[] buf = null; final byte[] le = new byte[(int) P * 8];
                for (long a = 0; a < wi.frames; a += P) { final long b = Math.min(wi.frames, a + P); buf = wavFrames(in, wi, a, b, x, buf); final int cnt = (int) (b - a) * 2;
                    for (int i = 0; i < cnt; i++) { final int v = x[i]; orv |= (long) v; com.lae.core.Le.put32(le, 4 * i, v); }
                    s.update(le, 0, cnt * 4); ctx.step((double) (b - a)); } }
            finally { in.close(); } }
        else { m.flacLive = m.derive; m.raw = m.flacLive ? path : stem + "_pcm.raw"; m.rawOff = 0; m.fi = wi.fi; final Flac.Info fi = wi.fi; final int by = m.rawBy;
            final List<long[]> offs = new ArrayList<long[]>(); Stream.In in = new Stream.In(path); Stream.Out o = null;
            try { if (!m.flacLive) o = new Stream.Out(m.raw); final com.lae.core.AlignHash.Md5 md5 = new com.lae.core.AlignHash.Md5();
                byte[] fb = new byte[0]; int fbLen = 0, head = 0; long at = wi.dataOff, fbOff = wi.dataOff, frameNo = 0, have = 0; final Flac.Frame F = new Flac.Frame(); int[] x = new int[0]; byte[] le = new byte[0], pb = new byte[0];
                for (;;) {
                    if (at < in.n && fbLen - head < (8 << 20)) { final int k = (int) Math.min(16 << 20, in.n - at); if (fb.length < fbLen + k) fb = Arrays.copyOf(fb, fbLen + k); in.read(at, fb, fbLen, k); fbLen += k; at += k; continue; }
                    if (head >= fbLen || have == fi.total) break;
                    if (m.flacLive) offs.add(new long[] {fbOff + head, have});
                    e = masterFlacFrame(fb, head, fbLen - head, fi, frameNo, have, F); if (!e.isEmpty()) return path + ": " + e;
                    head += F.used; frameNo++; final int k = F.n; have += k; if (x.length < k * 2) { x = new int[k * 2]; le = new byte[k * 8]; pb = new byte[k * 2 * 4]; }
                    for (int i = 0; i < k; i++) { x[i * 2] = (int) F.ch[0][i]; x[i * 2 + 1] = (int) F.ch[1][i]; }
                    for (int i = 0; i < k * 2; i++) { orv |= (long) x[i]; com.lae.core.Le.put32(le, 4 * i, x[i]); } s.update(le, 0, k * 8);
                    for (int i = 0; i < k * 2; i++) for (int q = 0; q < by; q++) pb[i * by + q] = (byte) (x[i] >>> (8 * q));
                    md5.update(pb, 0, k * 2 * by); if (o != null) o.put(pb, 0, k * 2 * by); ctx.step((double) k);
                    if (head > (32 << 20)) { System.arraycopy(fb, head, fb, 0, fbLen - head); fbLen -= head; fbOff += head; head = 0; diskCheck(); } }
                if (o != null) { o.close(); o = null; } if (m.flacLive) offs.add(new long[] {fbOff + head, -1});
                if (have != fi.total) return path + ": sample count differs from STREAMINFO";
                boolean zero = true; for (int i = 0; i < 16; i++) zero &= fi.md5[i] == 0;
                if (zero) ctx.say("note: " + path + " has no MD5 in its STREAMINFO: its decoding is checked only by this encoder's own FLAC reader");
                if (!zero && !Arrays.equals(Flac.hexBytes(md5.hex(), 16), fi.md5)) return path + ": decoded audio differs from its STREAMINFO MD5"; }
            finally { closeQuiet(o); in.close(); }
            if (m.flacLive) { m.fOff = new long[offs.size()]; m.fFirst = new long[offs.size() - 1]; for (int i = 0; i < offs.size(); i++) { m.fOff[i] = offs.get(i)[0]; if (i + 1 < offs.size()) m.fFirst[i] = offs.get(i)[1]; } }
            else { m.fOff = new long[0]; m.fFirst = new long[0]; } }
        s.finish(m.sha);
        m.w = 0; if (orv != 0) { while (m.w < 31 && ((orv >> m.w) & 1) == 0) m.w++; }   // laes::unusedBits
        return ""; }
    // step 2: levels and octave layers (stages from the base rate). piece: base-level samples per piece (a multiple of 4096).
    // otherStages: the stages of the other masters (disk, 2026-11-05): level k is kept when a candidate can read it - the base level (0), or
    // the highest level this master shares with another (min of the two stages); the others are removed once the level below is made.
    // port of laes_stream_enc.h:prepLevels
    public static String prepLevels(Prep m, int baseRate, String stem, long piece, Ctx ctx, int[] otherStages) {
        int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
        m.stages = st; final boolean[] keep = new boolean[st + 1]; keep[0] = true; for (int o : otherStages) keep[Math.min(st, Math.max(0, o))] = true;
        if (m.derive && st >= 1) keep[st - 1] = false;   // --small-disk: the largest level is not kept (made again from the master, one stage)
        for (int c = 0; c < 2; c++) { m.lev[c] = new String[st]; Arrays.fill(m.lev[c], ""); m.lby[c] = new int[st]; Arrays.fill(m.lby[c], 4); m.dfile[c] = new String[st + 1]; Arrays.fill(m.dfile[c], ""); m.dbits[c] = new long[st + 1];
            for (int k = st; k > 0; k--) { Stream.Sig x = levelOf(m, c, k); LevOut so = null; LayerOut lo = null;
                try { final long ne = (x.n + 1) / 2;
                    final String sc = stem + "_c" + c; m.lev[c][k - 1] = sc + "_L" + (k - 1); m.dfile[c][k] = sc + "_D" + k;
                    so = new LevOut(m.lev[c][k - 1]); lo = new LayerOut(m.dfile[c][k], 4096L << (k - 1), ctx); final long P = piece << (k - 1);
                    for (long a = 0; a < ne; a += P) { final long[][] sd = Stream.fwdPiece(x, a, Math.min(ne, a + P), ctx.threads); so.put(sd[0], 0, sd[0].length); diskCheck(); if (sd[1].length != 0) lo.feed(sd[1], sd[1].length);
                        ctx.step((double) (Math.min(ne, a + P) - a) * 2); }
                    so.close(); m.lby[c][k - 1] = so.by; so = null; m.dbits[c][k] = lo.finish(); lo = null; }
                finally { if (so != null) so.abandon(); if (lo != null) lo.abandon(); closeQuiet(x); }
                if (k < st && !keep[k]) { new File(m.lev[c][k]).delete(); m.lev[c][k] = ""; } }   // read by no candidate
            if (st > 0 && !keep[0]) { new File(m.lev[c][0]).delete(); m.lev[c][0] = ""; } }
        return ""; }
    // resume: a prepared master (steps 1 and 2) as a state file, and back (false: missing, damaged or a file of it gone)
    // port of laes_stream_enc.h:prepState
    public static String prepState(Prep m) { final StateW w = new StateW(); w.str(m.path); w.str(m.raw); w.i(m.rawOff); w.i(m.rawBy); w.i(m.bits); w.i(m.rate); w.i(m.stages); w.i(m.w); w.i(m.frames); w.hex(m.sha, 32);
        for (int c = 0; c < 2; c++) { w.i(m.lev[c].length); for (int k = 0; k < m.lev[c].length; k++) { w.str(m.lev[c][k]); w.i(m.lby[c][k]); w.i(m.lev[c][k].isEmpty() ? 0 : sizeOrMinus(m.lev[c][k])); }
            w.i(m.dfile[c].length); for (int k = 0; k < m.dfile[c].length; k++) { w.str(m.dfile[c][k]); w.i(m.dbits[c][k]); w.i(m.dfile[c][k].isEmpty() ? 0 : sizeOrMinus(m.dfile[c][k])); } }
        w.i(m.flacLive ? 1 : 0); w.i(m.fi.rate); w.i(m.fi.channels); w.i(m.fi.bps); w.i(m.fi.minBs); w.i(m.fi.maxBs); w.i(m.fi.total); w.hex(m.fi.md5, 16);
        w.i(m.fOff.length); for (long v : m.fOff) w.i(v); w.i(m.fFirst.length); for (long v : m.fFirst) w.i(v);
        w.i(sizeOrMinus(m.raw)); w.str("end"); return w.s.toString(); }
    // port of laes_stream_enc.h:prepFromState
    public static boolean prepFromState(String t, Prep m) { final StateR r = new StateR(t);
        m.path = r.str(); m.raw = r.str(); m.rawOff = r.i(); m.rawBy = (int) r.i(); m.bits = (int) r.i(); m.rate = (int) r.i(); m.stages = (int) r.i(); m.w = (int) r.i(); m.frames = r.i(); r.hex(m.sha, 32);
        for (int c = 0; c < 2; c++) { final long nl = r.i(); if (r.bad || nl < 0 || nl > 4) return false; m.lev[c] = new String[(int) nl]; m.lby[c] = new int[(int) nl];
            for (int k = 0; k < nl; k++) { m.lev[c][k] = r.str(); m.lby[c][k] = (int) r.i(); final long n = r.i(); if (!m.lev[c][k].isEmpty() && sizeOrMinus(m.lev[c][k]) != n) return false; }
            final long nd = r.i(); if (r.bad || nd < 0 || nd > 5) return false; m.dfile[c] = new String[(int) nd]; m.dbits[c] = new long[(int) nd];
            for (int k = 0; k < nd; k++) { m.dfile[c][k] = r.str(); m.dbits[c][k] = r.i(); final long n = r.i(); if (!m.dfile[c][k].isEmpty() && sizeOrMinus(m.dfile[c][k]) != n) return false; } }
        m.flacLive = r.i() != 0; m.fi.rate = (int) r.i(); m.fi.channels = (int) r.i(); m.fi.bps = (int) r.i(); m.fi.minBs = (int) r.i(); m.fi.maxBs = (int) r.i(); m.fi.total = r.i(); r.hex(m.fi.md5, 16);
        final long no = r.i(); if (r.bad || no < 0 || no > (1L << 28)) return false; m.fOff = new long[(int) no]; for (int k = 0; k < no; k++) m.fOff[k] = r.i();
        final long nf = r.i(); if (r.bad || nf < 0 || nf > (1L << 28)) return false; m.fFirst = new long[(int) nf]; for (int k = 0; k < nf; k++) m.fFirst[k] = r.i();
        final long nr = r.i(); return !r.bad && r.str().equals("end") && nr >= 0 && sizeOrMinus(m.raw) == nr; }   // every file whole, as it was written

    // ---- the core derived from a master's base level (laes_tool deriveCore, same values and the same random numbers in the same order)
    /** port of laes_stream_enc.h:deriveCore32::Rng (xorshift64; uni in [-0.5, 0.5)) */
    public static final class Rng { long s; public Rng(long sd) { s = sd * 2654435761L + 88172645463325252L; }
        public long next() { s ^= s << 13; s ^= s >>> 7; s ^= s << 17; return s; }
        public double uni() { return (double) (next() >>> 11) / 9007199254740992.0 - 0.5; } }
    // port of laes_stream_enc.h:deriveCore32 (src[0], dithered[0] receive the source master and whether it was dithered): straight into int32
    // a piece at a time, as the Java port always did
    public static Core deriveCoreS(List<Prep> ms, int base, int coreBits, int[] src, boolean[] dithered, long seed) {
        src[0] = 0; for (int i = 0; i < ms.size(); i++) if (ms.get(i).bits > ms.get(src[0]).bits) src[0] = i;
        for (int i = 0; i < ms.size(); i++) if (ms.get(i).bits == coreBits) { src[0] = i; break; }
        final Prep m = ms.get(src[0]); final Rng r = new Rng(seed);
        Core core = new Core(); core.rate = base; core.bits = coreBits; final int sig = m.bits - m.w, drop = sig - coreBits; final long mx = (1L << (coreBits - 1)) - 1, mn = -mx - 1; dithered[0] = drop > 0;
        final double div = com.lae.core.Cv.ldexp(1.0, drop);
        for (int c = 0; c < 2; c++) { Stream.Sig x = levelOf(m, c, 0);
            try { core.c[c] = new int[(int) x.n]; final long P = 1 << 18; long[] t = new long[(int) Math.min(P, x.n)];
                for (long a = 0; a < x.n; a += P) { final long b = Math.min(x.n, a + P); x.get(a, b, t, 0);
                    for (int i = 0; i < b - a; i++) { final long xv = t[i]; long v;
                        // C++ (fa5f031): u1 = r.uni(), u2 = r.uni(), in this order, then std::floor((double)xv / ldexp(1, drop) + u1 + u2 + 0.5)
                        if (drop <= 0) v = xv << (-drop); else { final double u1 = r.uni(); final double u2 = r.uni(); v = com.lae.core.Cv.d2l(Math.floor((double) xv / div + u1 + u2 + 0.5)); }
                        core.c[c][(int) (a + i)] = (int) Math.max(mn, Math.min(mx, v)); } } }
            finally { close(x); } }
        return core; }
    // ---- the payload (laes::encodeLaes, block-wise). ms: the masters in file order with their storage choice (MasterIn without samples).
    /** port of laes_stream_enc.h:MasterSel (frac: uint32 in an int) */
    public static final class MasterSel { public int storage = -1, refTier = 0; public long whole = 0; public int frac = 0;
        public String name = ""; public int flags = 0; public int menu = 0; }   // name, flags, menu position: payload version 2 (laes_file.h, 2026-11-11)
    // coded candidates shared between the file orders the tool tries (L5, 2026-11-03): a candidate's layers depend only on the master, its
    // storage, the tier signal (the core or a given master), the level j and T, never on the file order. Key -> layer bits and the two layer
    // sizes only (disk, 2026-11-05): a candidate's files are removed as soon as a smaller one is found, with or without a cache; the one chosen
    // is coded again when its files are gone (the same bytes: the coding depends only on the key's inputs; its bits are checked to match).
    /** port of laes_stream_enc.h:CachedCand (CandCache = TreeMap&lt;String, CachedCand&gt;, as std::map) */
    public static final class CachedCand { public double[] b = {0, 0}; }
    /** port of laes_stream_enc.h:encodeLaesS::Cand */
    static final class Cand { int storage, tier, j; long whole; int frac; double bits; String[] e = {"", ""}; boolean coded = false;
        Cand(int s, int t, int jj) { storage = s; tier = t; j = jj; } }
    static void removeFile(String p) { if (p != null && !p.isEmpty()) new File(p).delete(); }   // C++ remove(): the result is not checked
    // port of laes_stream_enc.h:encodeLaesS  (kept != null: resume, 2026-11-05 - the chosen layer files are listed there instead of removed,
    // and each master's choice is kept in a state file; cacheLog: the cache's entries appended as they are made)
    public static String encodeLaesS(Core core, int baseRate, final List<Prep> ms, final List<MasterSel> sel, List<LaesFile.MasterInfo> info,
                                     String bodyPath, String stem, int coreMode, long piece, Ctx ctx, Map<String, CachedCand> cache, String cacheLog, List<String> kept) {
        return encodeLaesS(core, baseRate, ms, sel, info, bodyPath, stem, coreMode, piece, ctx, cache, cacheLog, kept, new LaesFile.Head()); }
    // head: the default master and the core master's flags (2026-11-12, header bytes 56 / 57)
    public static String encodeLaesS(Core core, int baseRate, final List<Prep> ms, final List<MasterSel> sel, List<LaesFile.MasterInfo> info,
                                     String bodyPath, String stem, int coreMode, long piece, Ctx ctx, Map<String, CachedCand> cache, String cacheLog, List<String> kept, LaesFile.Head head) {
        info.clear(); for (int i = 0; i < ms.size(); i++) info.add(new LaesFile.MasterInfo()); final byte[] sh = new byte[32]; hashAnyCore(core, sh);
        if (core.bits != 16 && core.bits != 24 && core.bits != 32) return "core master word length must be 16, 24 or 32"; if (coreMode != 0 && coreMode != 1) return "core master mode must be 0 or 1";
        Stream.Out bo = new Stream.Out(bodyPath); boolean okClose = false;
        try { CrcOut o = new CrcOut(bo);
            if (ms.isEmpty() || ms.size() > 255) return "1 to 255 masters";
            int ver = ms.size() > 16 ? 2 : 1; for (MasterSel x : sel) { if (!LaesFile.validMasterName(x.name)) return "a master name must be UTF-8 of at most 255 bytes without control characters"; if ((x.flags & ~LaesFile.NO_DERIVE) != 0) return "unknown master flag"; if (x.name.length() != 0 || x.flags != 0 || x.menu != 0) ver = 2; }
            { final List<LaesFile.MasterInfo> t = new ArrayList<LaesFile.MasterInfo>(); for (MasterSel x : sel) { LaesFile.MasterInfo q = new LaesFile.MasterInfo(); q.menu = x.menu; t.add(q); } if (!LaesFile.menuOk(t)) return "menu positions must be 1 ... M, each once"; }
            if (head.def < 0 || head.def > ms.size()) return "the default master must be 0 (the core master) or 1 ... M"; if ((head.coreFlags & ~LaesFile.NO_DERIVE) != 0) return "unknown core master flag";
            if (head.def != 0 || head.coreFlags != 0) ver = 2;
            o.u(0x3245414CL, 4); o.u(ver, 2); o.u(64, 2); o.u(baseRate, 4); o.u(core.bits, 1); o.u(2, 1); o.u(ms.size(), 1); o.u(coreMode, 1); o.u(core.frames(), 8); o.put(sh, 0, 32);
            { final byte[] hb = new byte[8]; LaesFile.headTo(ver, head, hb, 0); o.put(hb, 0, 8); }
            for (int mi = 0; mi < ms.size(); mi++) { final Prep m = ms.get(mi); final LaesFile.MasterInfo I = info.get(mi);
                if (m.bits != 16 && m.bits != 24 && m.bits != 32) return "master word length must be 16, 24 or 32";
                int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
                I.bits = m.bits; I.stages = st; I.w = m.w; I.rate = m.rate; I.frames = m.frames; System.arraycopy(m.sha, 0, I.sha, 0, 32);
                final List<Cand> cs = new ArrayList<Cand>(); final boolean autoSel = sel.get(mi).storage < 0;
                if (autoSel || sel.get(mi).storage == 1) cs.add(new Cand(1, 0, 0));
                for (int t = 0; t <= mi; t++) {
                    if (!autoSel && t != sel.get(mi).refTier) continue; if (t > mi) break;
                    if (autoSel || sel.get(mi).storage == 0) cs.add(new Cand(0, t, 0));
                    if (t >= 1 && (autoSel || sel.get(mi).storage == 2)) { final int j = Math.min(st, info.get(t - 1).stages); if (j >= 1 || !autoSel) cs.add(new Cand(2, t, j)); } }
                if (!autoSel && (sel.get(mi).refTier < 0 || sel.get(mi).refTier > mi || sel.get(mi).storage > 2 || (sel.get(mi).storage == 2 && sel.get(mi).refTier == 0))) return "reference tier must be the core master or an earlier master (storage 2: an earlier master)";
                int best = -1; final String stP = stem + "_m" + mi + ".done"; boolean restored = false;
                final String stT = kept != null ? getState(stP) : null;
                if (stT != null) {   // resume: this master's choice was made before (its two layer files kept)
                    final StateR r = new StateR(stT); final long q = r.i(); final long rWhole = r.i(); final int rFrac = (int) r.i(); final double rBits = r.d(); final String e0 = r.str(); final long n0 = r.i(); final String e1 = r.str(); final long n1 = r.i();
                    boolean tOk = false; if (!r.bad && q >= 0 && q < cs.size()) { final Cand C = cs.get((int) q); if (C.storage == 1) tOk = rWhole == 0 && rFrac == 0; else { final long[] wf = relT(sel, mi, autoSel, C.tier); tOk = wf != null && wf[0] == rWhole && (int) wf[1] == rFrac; } }   // the T of this run
                    if (tOk && n0 >= 0 && sizeOrMinus(e0) == n0 && n1 >= 0 && sizeOrMinus(e1) == n1) {   // the files whole, as they were written
                        final Cand C = cs.get((int) q); C.whole = rWhole; C.frac = rFrac; C.bits = rBits; C.e[0] = e0; C.e[1] = e1; C.coded = true; best = (int) q; restored = true; } }
                final boolean[] inShort = best < 0 ? makeShort(autoSel, cs, mi, m, st, I, core, info, ms, sel, stem, ctx) : null;
                if (best < 0) for (int q = 0; q < cs.size(); q++) { if (inShort != null && !inShort[q]) continue; final Cand C = cs.get(q); C.bits = above(m, st, C.j);
                    for (int c = 0; c < 2; c++) C.e[c] = stem + "_m" + mi + "_k" + q + "_c" + c + ".lay";
                    if (C.storage != 1) { final long[] wf = relT(sel, mi, autoSel, C.tier); if (wf == null) { C.e[0] = ""; continue; } C.whole = wf[0]; C.frac = (int) wf[1]; }
                    String key = ""; if (cache != null) { key = m.raw + "|" + C.storage + "|" + (C.storage == 1 ? "-" : C.tier == 0 ? "core" : ms.get(C.tier - 1).raw) + "|" + C.j;
                        if (C.storage != 1) key += "|" + C.whole + "|" + (C.frac & 0xFFFFFFFFL); }
                    final CachedCand hit = cache != null ? cache.get(key) : null;
                    if (hit != null) { for (int c = 0; c < 2; c++) C.bits += hit.b[c]; }
                    else { final double[] lb = new double[2]; final String ce = code(C, lb, m, st, I, core, info, ms, piece, ctx); if (ce != null) return ce;
                        for (int c = 0; c < 2; c++) { C.bits += lb[c]; if (cache != null) { CachedCand cc = cache.get(key); if (cc == null) { cc = new CachedCand(); cache.put(key, cc); } cc.b[c] = lb[c]; } }
                        if (cache != null && cacheLog != null && !cacheLog.isEmpty()) appendLine(cacheLog, key + "\t" + g17(lb[0]) + "\t" + g17(lb[1]) + "\n"); }
                    if (best < 0 || C.bits < cs.get(best).bits) { if (best >= 0 && cs.get(best).coded) for (int c = 0; c < 2; c++) removeFile(cs.get(best).e[c]); best = q; }
                    else if (C.coded) for (int c = 0; c < 2; c++) removeFile(C.e[c]); }
                if (best < 0) return "no usable storage for master " + (mi + 1);
                if (!cs.get(best).coded) { final Cand B = cs.get(best); final double[] lb = new double[2]; final String ce = code(B, lb, m, st, I, core, info, ms, piece, ctx); if (ce != null) return ce;
                    double b = above(m, st, B.j); for (int c = 0; c < 2; c++) b += lb[c];
                    if (b != B.bits) return "internal error: a candidate coded again differs in size"; }   // its files were removed in the other order
                if (kept != null && !restored) { final Cand B = cs.get(best); final StateW w = new StateW(); w.i(best); w.i(B.storage == 1 ? 0 : B.whole); w.i(B.storage == 1 ? 0 : (B.frac & 0xFFFFFFFFL)); w.d(B.bits);
                    w.str(B.e[0]); w.i(sizeOrMinus(B.e[0])); w.str(B.e[1]); w.i(sizeOrMinus(B.e[1])); putState(stP, w.s.toString()); }
                final Cand B = cs.get(best); I.storage = B.storage; I.refTier = B.storage == 1 ? 0 : B.tier; I.whole = B.storage == 1 ? 0 : B.whole; I.frac = B.storage == 1 ? 0 : B.frac; I.layerBits = B.bits;
                I.name = sel.get(mi).name; I.flags = sel.get(mi).flags; I.menu = sel.get(mi).menu; final byte[] nb = LaesFile.nameBytes(I.name);
                o.u(I.bits, 1); o.u(I.stages, 1); o.u(I.w, 1); o.u(I.storage, 1); o.u(I.refTier, 1); o.u(ver == 2 ? nb.length : 0, 1); o.u(ver == 2 ? I.flags : 0, 1); o.u(ver == 2 ? I.menu : 0, 1); o.u(I.rate, 4); o.u(0, 4); o.u(I.frames, 8);
                o.u(I.whole, 8); o.u(I.frac & 0xFFFFFFFFL, 4); o.u(0, 4); o.put(I.sha, 0, 32); if (ver == 2) o.put(nb, 0, nb.length);
                for (int c = 0; c < 2; c++) { { Stream.In e = new Stream.In(B.e[c]); final long en = e.n; e.close(); o.u(en, 4); } o.file(B.e[c]); if (kept != null) kept.add(B.e[c]); else removeFile(B.e[c]);
                    for (int k = B.j + 1; k <= st; k++) { { Stream.In d = new Stream.In(m.dfile[c][k]); final long dn = d.n; d.close(); o.u(dn, 4); } o.file(m.dfile[c][k]); } } }
            final long crc = o.crc.getValue(); final byte[] t = {(byte) crc, (byte) (crc >> 8), (byte) (crc >> 16), (byte) (crc >> 24)}; bo.put(t, 0, 4); bo.close(); okClose = true;
            return ""; }
        finally { if (!okClose) closeQuiet(bo); } }
    // codes a candidate into its two layer files; lb: the bits of each channel. null when coded, else the reason
    // port of laes_stream_enc.h:encodeLaesS::code (the lambda)
    // port of laes_stream_enc.h:encodeLaesS::makeShort  (more than 16 masters, 2026-11-11: every candidate is first coded on 4 excerpts of
    // 2^15 base-rate samples (x 2^j at level j) at positions given only by the length (the centres of the four quarters, aligned down to
    // the block length); the 3 with the smallest estimate are coded in full. null: every candidate in full (up to 16 masters, as before).
    // LAES_NO_SHORTLIST set: the full search, for tests.)
    static boolean[] makeShort(boolean autoSel, List<Cand> cs, int mi, Prep m, int st, LaesFile.MasterInfo I, Core core, List<LaesFile.MasterInfo> info, List<Prep> ms,
                               List<MasterSel> sel, String stem, Ctx ctx) {
        final int kShort = 3;
        if (!(autoSel && ms.size() > 16 && cs.size() > kShort) || System.getenv("LAES_NO_SHORTLIST") != null) return null;
        final List<double[]> est = new ArrayList<double[]>();   // {estimate, q}
        for (int q = 0; q < cs.size(); q++) { final Cand C = cs.get(q); long wh = 0; int fr = 0; if (C.storage != 1) { final long[] wf = relT(sel, mi, autoSel, C.tier); if (wf == null) continue; wh = wf[0]; fr = (int) wf[1]; }
            final int j = C.j; final long nj = LaesFile.levLen(I.frames, st - j), B = 4096L << j, L = (1L << 15) << j;
            final List<long[]> segs = new ArrayList<long[]>(); if (nj <= 4 * L) segs.add(new long[] {0, nj});
            else for (int s = 0; s < 4; s++) { long a = (2 * s + 1) * (nj / 8) + ((2 * s + 1) * (nj % 8)) / 8 - L / 2; a = a / B * B; a = Math.max(0L, Math.min(a, nj - L) / B * B); segs.add(new long[] {a, a + L}); }
            long got = 0; double pb = 0; final String pf = stem + "_m" + mi + "_probe.lay";
            int up = 0; if (C.storage != 1) up = C.tier == 0 ? (m.bits - I.w) - core.bits : (m.bits - I.w) - (info.get(C.tier - 1).bits - info.get(C.tier - 1).w);
            final long wj = wh * (1L << j) + (j != 0 ? ((fr & 0xFFFFFFFFL) >>> (32 - j)) : 0); final int fj = (int) ((fr & 0xFFFFFFFFL) << j); final long[] h = LaesMath.refTaps(fj);
            for (int c = 0; c < 2; c++) { Stream.Sig x = null, src = null; LayerOut lo = null;
                try { x = levelOf(m, c, j); if (C.storage != 1) { if (C.tier == 0) src = new IntSig(core.c[c]); else src = levelOf(ms.get(C.tier - 1), c, j); }
                    lo = new LayerOut(pf, B, ctx);
                    for (long[] sg : segs) { final long[] v = new long[(int) (sg[1] - sg[0])]; x.get(sg[0], sg[1], v, 0);
                        if (C.storage != 1) { final long[] r = Stream.refPiece(src, h, wj, up, sg[0], sg[1], ctx.threads); for (int i = 0; i < v.length; i++) v[i] -= r[i]; }
                        lo.feed(v, v.length); if (c == 0) got += v.length; }
                    pb += (double) lo.finish(); lo = null; removeFile(pf); }
                finally { if (lo != null) lo.abandon(); closeQuiet(x); closeQuiet(src); } }
            est.add(new double[] {above(m, st, j) + pb * (double) nj / (double) Math.max(1L, got), q}); }
        java.util.Collections.sort(est, new java.util.Comparator<double[]>() { public int compare(double[] a, double[] b) { return a[0] < b[0] ? -1 : b[0] < a[0] ? 1 : 0; } });   // stable, as std::stable_sort
        final boolean[] in = new boolean[cs.size()]; for (int k = 0; k < est.size() && k < kShort; k++) in[(int) est.get(k)[1]] = true; return in; }
    static String code(Cand C, double[] lb, Prep m, int st, LaesFile.MasterInfo I, Core core, List<LaesFile.MasterInfo> info, List<Prep> ms, long piece, Ctx ctx) {
        if (C.storage == 1) { for (int c = 0; c < 2; c++) { Stream.Sig x = levelOf(m, c, 0); LayerOut lo = null;
                try { lo = new LayerOut(C.e[c], 4096, ctx); final long P = piece; long[] v = new long[0];
                    for (long a = 0; a < x.n; a += P) { final long b = Math.min(x.n, a + P); if (v.length != b - a) v = new long[(int) (b - a)]; x.get(a, b, v, 0); lo.feed(v, v.length); ctx.step((double) (b - a)); }
                    lb[c] = (double) lo.finish(); lo = null; }
                finally { if (lo != null) lo.abandon(); closeQuiet(x); } } }
        else { final int j = C.j; final long nj = LaesFile.levLen(I.frames, st - j);
            int up; if (C.tier == 0) up = (m.bits - I.w) - core.bits; else up = (m.bits - I.w) - (info.get(C.tier - 1).bits - info.get(C.tier - 1).w);
            final long wj = C.whole * (1L << j) + (j != 0 ? ((C.frac & 0xFFFFFFFFL) >>> (32 - j)) : 0); final int fj = (int) ((C.frac & 0xFFFFFFFFL) << j);
            final long[] h = LaesMath.refTaps(fj);
            for (int c = 0; c < 2; c++) { Stream.Sig src = null, x = null; LayerOut lo = null;
                try { if (C.tier == 0) src = new IntSig(core.c[c]); else src = levelOf(ms.get(C.tier - 1), c, j);
                    x = levelOf(m, c, j); if (x.n != nj) return "internal error: level length";
                    lo = new LayerOut(C.e[c], 4096L << j, ctx); long[] v = new long[0]; final long P = piece << j;
                    for (long a = 0; a < nj; a += P) { final long b = Math.min(nj, a + P); if (v.length != b - a) v = new long[(int) (b - a)]; x.get(a, b, v, 0); final long[] r = Stream.refPiece(src, h, wj, up, a, b, ctx.threads);
                        for (int i = 0; i < v.length; i++) v[i] -= r[i]; lo.feed(v, v.length); ctx.step((double) (b - a) / (double) (1L << j)); }
                    lb[c] = (double) lo.finish(); lo = null; }
                finally { if (lo != null) lo.abandon(); closeQuiet(x); closeQuiet(src); } } }
        C.coded = true; return null; }
    // the cache log (resume): one line appended, the file closed again (a run stopped later keeps every whole line)
    static void appendLine(String path, String line) { final byte[] b = line.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        try { FileOutputStream f = new FileOutputStream(path, true); try { f.write(b); } finally { f.close(); } } catch (IOException e) { Stream.fail("cannot write " + path); } }
    // the octave layers kept above level j (bits)
    // port of laes_stream_enc.h:encodeLaesS::above (the lambda)
    static double above(Prep m, int st, int j) { double b = 0; for (int c = 0; c < 2; c++) for (int k = j + 1; k <= st; k++) b += (double) m.dbits[c][k]; return b; }
    // T relative to the tier, from the absolute T of both (2^-32 base samples, int64); null when out of range
    // port of laes_stream_enc.h:encodeLaesS::relT (the lambda); returns {whole, frac as uint32}
    static long[] relT(List<MasterSel> sel, int mi, boolean autoSel, int tier) {
        if (!autoSel) return new long[] {sel.get(mi).whole, sel.get(mi).frac & 0xFFFFFFFFL};
        final long lim = 1L << 30;
        if (sel.get(mi).whole <= -lim || sel.get(mi).whole >= lim || (tier != 0 && (sel.get(tier - 1).whole <= -lim || sel.get(tier - 1).whole >= lim))) return null;
        final long x = X(sel, mi) - (tier != 0 ? X(sel, tier - 1) : 0); return new long[] {x >> 32, x & 0xFFFFFFFFL}; }
    // port of laes_stream_enc.h:encodeLaesS::relT::X (the lambda)
    static long X(List<MasterSel> sel, int k) { return sel.get(k).whole * 4294967296L + (sel.get(k).frac & 0xFFFFFFFFL); }
}
