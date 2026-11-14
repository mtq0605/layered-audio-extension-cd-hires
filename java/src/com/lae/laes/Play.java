// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.io.UnsupportedEncodingException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32;

import com.lae.core.AlignHash;
import com.lae.core.LaeException;
import com.lae.core.U;

/**
 * Port of src/laes_play.h (namespace laesp). LAES playback decoder (2026-11-01): one choice of an LAES file (the core, a master, a rate
 * level of a master, or a derived 16-bit version) decoded in time order, piece by piece, for players. The values are those of
 * laes::decodeLaes / laess::decodeLaesS and of `laes dec`: the same lifting, reference and layer code (laes_stream.h) applied to pieces
 * that are computed when they are first asked for.
 *   - Only the masters the choice needs are decoded: the chosen one and the tiers it refers to (its chain).
 *   - Signals are nodes that compute values [a, b) on request and keep a window of recent values; requests go forward in time, so a
 *     layer's bit stream is read once from its start (no index or entry table is read or made; LAES rules T-1, T-2).
 *   - The file is read through a callback (no whole-file buffer). open() reads it once from start to end: the SHA-256 of the whole file
 *     (a player can remember a verified file by it), the payload CRC-32, the core (its SHA-256, and the STREAMINFO MD5 of the FLAC form).
 *   - A master is presented as that master only when its SHA-256 matches (spec 1.3): the SHA-256 of every master of the chain is computed
 *     while the choice is decoded and is known after the last sample (verified()). A player shows a choice only after a verified run.
 * The CRC-32 (slicing-by-8 in the C++) is java.util.zip.CRC32: the same polynomial (IEEE 802.3, reflected), initial value and final xor.
 * Memory (Java, 2026-11-04; phones): the heap stays near File.memBudget (default 64 MB) whatever the track's length. Differences from the
 * C++, none of which changes a value: the core is read on demand (CoreStore) instead of being held; window, look-ahead and read sizes follow
 * the budget and the node's level (C++: fixed 2^17 / 2^19 values, 1 MiB, 4 MiB); the masters are hashed at the same time position in small
 * steps (C++: the same share of their lengths), so that the readers of a shared tier stay together; a layer drops the blocks a request has
 * passed while it waits. What still grows with the length (small): the FLAC frame index (16 bytes per frame) and the noted block positions.
 */
public final class Play {
    private Play() {}

    static RuntimeException fail(String m) { throw new LaeException(m); }

    // ---- byte sources
    /** port of laes_play.h:ByteSrc (read throws LaeException) */
    public interface ByteSrc { long size(); void read(long at, byte[] d, int off, int n); }
    /** the read callback: fills buf[off .. off + len) with the bytes at offset; 0 on success (lae_read_fn) */
    public interface ReadFn { int read(long offset, byte[] buf, int off, int len); }
    // a file given by a read callback: fn(offset, buffer, bytes) returns 0 on success
    /** port of laes_play.h:FnSrc */
    public static final class FnSrc implements ByteSrc { public ReadFn fn; public long n = 0;   // reads from both channel threads, one at a time
        // port of laes_play.h:FnSrc::size
        public long size() { return n; }
        // port of laes_play.h:FnSrc::read
        public synchronized void read(long at, byte[] d, int off, int k) { if (k == 0) return; if (U.gt(at, n) || U.gt(k, n - at) || fn.read(at, d, off, k) != 0) fail("read failed"); } }
    // the concatenation of byte ranges of another source (the payload: one range in the WAV form, the APPLICATION pieces in the FLAC form)
    /** port of laes_play.h:SegSrc */
    public static final class SegSrc implements ByteSrc { ByteSrc base = null; long[] off = new long[4], len = new long[4], start = new long[4]; int cnt = 0; long total = 0;
        // port of laes_play.h:SegSrc::add
        void add(long o, long l) { if (l == 0) return; if (cnt == off.length) { off = java.util.Arrays.copyOf(off, cnt * 2); len = java.util.Arrays.copyOf(len, cnt * 2); start = java.util.Arrays.copyOf(start, cnt * 2); }
            off[cnt] = o; len[cnt] = l; start[cnt] = total; cnt++; total += l; }
        // port of laes_play.h:SegSrc::size
        public long size() { return total; }
        // port of laes_play.h:SegSrc::read
        public void read(long at, byte[] d, int p, int k) { if (U.gt(at, total) || U.gt(k, total - at)) fail("read failed: payload range");
            int lo = 0, hi = cnt; while (lo < hi) { int mid = (lo + hi) >>> 1; if (start[mid] <= at) lo = mid + 1; else hi = mid; } int s = lo - 1;   // upper_bound - 1
            while (k != 0) { final long in = at - start[s], t = Math.min(k, len[s] - in); base.read(off[s] + in, d, p, (int) t); p += t; at += t; k -= (int) t; if (k != 0) s++; } } }
    // the bit reader of laes_codec.h (MSB first) over a byte range of a source, read through a window of 1 MiB; a read of up to 57 bits
    // takes the 8 bytes around the position at once. Bit positions are 64-bit.
    /** port of laes_play.h:BitRS */
    public static final class BitRS extends Codec.BitIn { ByteSrc src = null; long off = 0, len = 0, pos = 0; byte[] w = new byte[0]; int wsize = 0; long wb = 0, we = 0; int win = 1 << 20;   // win: window bytes (Java: set from the memory budget; C++ 1 MiB)
        // port of laes_play.h:BitRS::left
        long left() { return len * 8 - pos; }
        // port of laes_play.h:BitRS::fill  (the window from byte on (at most 1 MiB, at most to the end))
        void fill(long bt) { wb = bt; we = Math.min(len, bt + win); wsize = (int) (we - wb); if (w.length < wsize) w = new byte[wsize]; if (we > wb) src.read(off + wb, w, 0, wsize); }
        // port of laes_play.h:BitRS::get
        public long get(int n) { if (fail || n < 0 || n > 64 || (long) n > left()) { fail = true; return 0; } if (n == 0) return 0;
            if (n > 57) { final long hi = get(n - 32); return (hi << 32) | get(32); }
            final long v = peek(n); pos += n; return v; }
        // the next n bits (1 <= n <= 57, n <= left()) without consuming them
        // port of laes_play.h:BitRS::peek
        long peek(int n) { final long bt = pos >>> 3; final long need = Math.min(8, len - bt);
            if (wsize == 0 || bt < wb || bt + need > we) fill(bt);
            final int p = (int) (bt - wb); long v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | (i < need ? (w[p + i] & 0xFF) : 0);
            v <<= (pos & 7); return v >>> (64 - n); }
        // Java (2026-11-04): the 8 bytes from the byte that holds pos, shifted so that the bit at pos is the top bit (at least 57 valid bits);
        // only when left() >= 64. The window is filled as peek fills it.
        long word() { final long bt = pos >>> 3; if (wsize == 0 || bt < wb || bt + 8 > we) fill(bt); final int p = (int) (bt - wb); final byte[] b = w;
            return ((b[p] & 0xFFL) << 56 | (b[p + 1] & 0xFFL) << 48 | (b[p + 2] & 0xFFL) << 40 | (b[p + 3] & 0xFFL) << 32
                | (b[p + 4] & 0xFFL) << 24 | (b[p + 5] & 0xFFL) << 16 | (b[p + 6] & 0xFFL) << 8 | (b[p + 7] & 0xFFL)) << (int) (pos & 7); }
        // the zeros before the next 1, which is consumed too (laes::unaryOf); fails past qmax or at the end of the data
        // port of laes_play.h:BitRS::unary
        public long unary(long qmax) { long q = 0;
            for (;;) { if (fail) return q; final long l = left(); if (l == 0) { fail = true; return q; }
                final int n = (int) Math.min(57, l); final long v = peek(n);
                if (v == 0) { pos += n; q += n; if (U.gt(q, qmax)) { fail = true; return q; } continue; }
                int z = 0; while (((v >>> (n - 1 - z)) & 1) == 0) z++;
                q += z; pos += z + 1; if (U.gt(q, qmax)) fail = true; return q; } }
        // port of laes_play.h:BitRS::reset
        void reset() { pos = 0; fail = false; } }

    // ---- nodes: signals computed on request, keeping the values [c0, c0 + size) of recent requests
    /** port of laes_play.h:Node */
    abstract static class Node extends Stream.Sig { long[] cbuf = new long[0]; int chead = 0, csize = 0; long c0 = 0, keep = 1 << 17, chunk = 1 << 17; int threads = 1; int sh = 0;   // sh (Java): stages from this node's level up to its master's top; sizes are scaled by 2^-sh
        abstract long[] compute(long a, long b);   // values [a, b), a pure function of the inputs (layers: see LayerNode)
        void close() {}
        // Java (2026-11-04): the values [a, b) written to dst[off, off + b - a) (nodes override it to write there directly, without a new array)
        void computeInto(long a, long b, long[] dst, int off) { long[] t = compute(a, b); System.arraycopy(t, 0, dst, off, (int) (b - a)); }
        final Stream.Work ws = new Stream.Work();   // Java: the node's reusable temporaries (a node is used by one thread at a time)
        // port of laes_play.h:Node::get
        public void get(long a, long b, long[] out, int outOff) {
            if (a < 0 || b > n || a > b) fail("internal error: signal range"); if (a == b) return;
            final long c1 = c0 + csize;
            if (a < c0) { computeInto(a, b, out, outOff); return; }   // behind the window: computed again, not kept
            if (a > c1) { csize = 0; chead = 0; c0 = a; }   // a jump forward: the window starts again
            final long e1 = c0 + csize;
            // no read-ahead: exactly what is asked (port of laes_play.h, 2026-11-12: any rounding up grows level by level along a reference
            // chain, and the SHA-256 passes then recomputed dropped samples, level after level)
            if (b > e1) { final long e = b; final int k = (int) (e - e1); reserve(k); computeInto(e1, e, cbuf, chead + csize); csize += k; }
            System.arraycopy(cbuf, chead + (int) (a - c0), out, outOff, (int) (b - a));
            final long drop = a - keep - c0; if (drop > chunk) { chead += (int) drop; csize -= (int) drop; c0 += drop; } }
        // room for k values after the kept ones (as the C++ append: the kept values moved to the front, or a larger buffer)
        private void reserve(int k) {
            if (chead + csize + k > cbuf.length) {
                if (csize + k <= cbuf.length) { System.arraycopy(cbuf, chead, cbuf, 0, csize); chead = 0; }
                else { long[] nb = new long[Math.max(csize + k, cbuf.length + cbuf.length / 2)]; System.arraycopy(cbuf, chead, nb, 0, csize); cbuf = nb; chead = 0; } } } }
    // the core (one channel). C++: held in memory as int32. Java (2026-11-04, memory bound for phones): read on demand through CoreStore;
    // the values are the same (WAV: the "data" samples; FLAC: the audio frames decoded again, located by the frame index made in open())
    /** port of laes_play.h:CoreNode */
    static final class CoreNode extends Stream.Sig { CoreStore v = null; int c = 0;
        // port of laes_play.h:CoreNode::get
        public void get(long a, long b, long[] out, int outOff) { if (a < 0 || b > n || a > b) fail("internal error: signal range"); v.get(c, a, b, out, outOff); } }
    /**
     * Java only (2026-11-04): the core read from the file when asked for, instead of being held whole (the C++ keeps two int32 vectors of
     * the core's length, about 213 MB for a 10-minute 44.1 kHz core in Java's memory model). A window of decoded frames (both channels) is
     * kept; WAV: the samples are read from the "data" chunk; FLAC: the frames are found by the index of frame positions noted while open()
     * decoded and checked the whole core (STREAMINFO MD5, SHA-256 of the LAES header), and decoded again with the same reader. The file must
     * not change while it is open (the C++ reads the core once; here a FLAC frame's CRC-16 is checked again on every read, a WAV is not).
     */
    static final class CoreStore { ByteSrc src; boolean flac; int bits; long frames = 0, audioOff = 0; Flac.Info fi;
        long[] frOff = new long[256], frStart = new long[256]; int frN = 0; long frEnd = 0;   // FLAC: file offset and first sample of each frame; end of the last
        int cap = 1 << 16; int[][] w = null; long w0 = 0; int wn = 0; byte[] tmp = new byte[0]; final Flac.Frame fr = new Flac.Frame();   // fr: Java, reused
        void addFrame(long off, long start) { if (frN == frOff.length) { frOff = java.util.Arrays.copyOf(frOff, frN * 2); frStart = java.util.Arrays.copyOf(frStart, frN * 2); } frOff[frN] = off; frStart[frN] = start; frN++; }
        synchronized void get(int c, long a, long b, long[] out, int off) {
            if (a < 0 || b > frames || a > b) fail("internal error: signal range");
            while (a < b) { if (w == null || a < w0 || a >= w0 + wn) refill(a); final int k = (int) Math.min(b - a, w0 + wn - a); final int[] s = w[c]; final int p = (int) (a - w0);
                for (int i = 0; i < k; i++) out[off + i] = s[p + i]; a += k; off += k; } }
        private void refill(long a) {
            if (w == null) { if (flac) cap = Math.max(cap, fi.maxBs); w = new int[2][cap]; }
            if (!flac) { final int by = bits / 8; final int n = (int) Math.min(cap, frames - a); if (tmp.length < n * 2 * by) tmp = new byte[n * 2 * by]; src.read(audioOff + a * 2 * by, tmp, 0, n * 2 * by);
                for (int i = 0; i < n; i++) for (int c = 0; c < 2; c++) { final int q = (i * 2 + c) * by; int v = 0; for (int t = 0; t < by; t++) v |= (tmp[q + t] & 0xFF) << (8 * t);
                    if (bits < 32 && ((v >>> (bits - 1)) & 1) != 0) v |= -1 << bits; w[c][i] = v; }
                w0 = a; wn = n; return; }
            int lo = 0, hi = frN; while (hi - lo > 1) { final int mid = (lo + hi) >>> 1; if (frStart[mid] <= a) lo = mid; else hi = mid; }   // the frame that holds a
            int k = lo; w0 = frStart[k]; wn = 0;
            while (k < frN) { final long e = k + 1 < frN ? frOff[k + 1] : frEnd; final int len = (int) (e - frOff[k]); final long fn = (k + 1 < frN ? frStart[k + 1] : frames) - frStart[k];
                if (wn > 0 && wn + fn > cap) break;
                if (tmp.length < len) tmp = new byte[len]; src.read(frOff[k], tmp, 0, len);
                final Flac.Frame F = Flac.decodeFrame(tmp, 0, len, fi, k, frStart[k], fr); if (!F.err.isEmpty() || F.n != fn) fail("FLAC core master: " + (F.err.isEmpty() ? "frame length" : F.err) + " (the file changed after it was opened?)");
                for (int c = 0; c < 2; c++) { final long[] x = F.ch[c]; final int[] d = w[c]; for (int i = 0; i < F.n; i++) d[wn + i] = (int) x[i]; }
                wn += F.n; k++; } } }
    // where each block of a layer begins (bit position), noted while the layer is decoded in this run of the player, so that a later start
    // at any frame can begin at the block that holds it. Memory only: the file holds no index (rules T-1, T-2) and nothing is saved.
    /** port of laes_play.h:LayerIdx */
    static final class LayerIdx { long[] bit = new long[16]; int bitN = 0;   // bit[k]: position of block k (known for k < size)
        // every kSparse-th position, loaded from the player's own record (File::importIndex): sp[i] = position of block i * kSparse, for the
        // spEnd blocks of the layer
        static final int kSparse = 16; long[] sp = new long[0]; long spEnd = 0;
        // port of laes_play.h:LayerIdx::note
        synchronized void note(long k, long p) { if (k == bitN) { if (bitN == bit.length) bit = java.util.Arrays.copyOf(bit, bitN * 2); bit[bitN++] = p; } }
        // the last known block at or before k (block 0 at bit 0 is always known); p[0] receives its position
        // port of laes_play.h:LayerIdx::atMost
        synchronized long atMost(long k, long[] p) { long best = 0; p[0] = 0;
            if (bitN != 0) { best = Math.min(k, bitN - 1); p[0] = bit[(int) best]; }
            if (sp.length != 0) { final long j = Math.min(k / kSparse, sp.length - 1); if (j * kSparse > best || bitN == 0) { best = j * kSparse; p[0] = sp[(int) j]; } }
            return best; }
        // port of laes_play.h:LayerIdx::known
        synchronized long known() { return Math.max(bitN, spEnd); } }

    // a layer (n values in blocks of B) read from its byte range. The blocks are decoded in order by a thread of the node's own, at most
    // `ahead` values beyond the latest request, so that the layers of all masters and channels decode at the same time (each layer is
    // sequential in itself); the values are those of decodeBlock in order. A request far ahead or behind starts the thread again at the last
    // block whose position is noted (block 0 when none is).
    /** port of laes_play.h:LayerNode */
    static final class LayerNode extends Node { final BitRS rd = new BitRS(); long B = 4096; long restarts = 0; long ahead = 1 << 19; LayerIdx ix = null;
        private Thread th = null; private final Object m = new Object(); private final ArrayDeque<long[]> q = new ArrayDeque<long[]>(); private long q0 = 0, qEnd = 0, want = 0; private boolean stop = false, started = false; private Throwable err = null;
        // port of laes_play.h:LayerNode::~LayerNode
        void close() { halt(); }
        // port of laes_play.h:LayerNode::compute
        long[] compute(long a, long b) { long[] out = new long[(int) (b - a)]; computeInto(a, b, out, 0); return out; }
        // Java (2026-11-04): writes into dst; dropped blocks of full length go to a short free list that produce() takes from
        private final ArrayDeque<long[]> free = new ArrayDeque<long[]>();
        private void drop1() { final long[] x = q.pollFirst(); if (x.length == B && free.size() < 4) free.addLast(x); }
        void computeInto(long a, long b, long[] dst, int dOff) {
            long q0s, qEnds; synchronized (m) { q0s = q0; qEnds = qEnd; }
            if (!started) start(a);
            else if (a < q0s) { halt(); restarts++; start(a); }   // behind the kept blocks
            else if (a > qEnds + ahead && ix != null) { long[] p = new long[1]; if (ix.atMost(a / B, p) * B > qEnds) { halt(); start(a); } }   // far ahead: jump
            synchronized (m) { if (b > want) { want = b; m.notifyAll(); }
                while (!(err != null || qEnd >= b)) { try { m.wait(); } catch (InterruptedException x) { Thread.currentThread().interrupt(); throw fail("interrupted"); }
                    while (!q.isEmpty() && q0 + q.peekFirst().length <= a) { q0 += q.peekFirst().length; drop1(); } }   // Java: blocks before the request are dropped while waiting (a start far into a layer without noted positions decodes from block 0: the C++ keeps all those blocks until the wait ends)
                if (err != null) throw Lift.rethrow(err);
                long at = q0; java.util.Iterator<long[]> it = q.iterator();
                for (long i = a; i < b;) { final long[] blk = it.next(); final long e = at + blk.length;
                    if (i < e) { final long f = Math.min(b, e); System.arraycopy(blk, (int) (i - at), dst, dOff + (int) (i - a), (int) (f - i)); i = f; } at = e; }
                while (!q.isEmpty() && q0 + q.peekFirst().length <= a) { q0 += q.peekFirst().length; drop1(); }   // blocks wholly before this request
                m.notifyAll(); }
            }
        // port of laes_play.h:LayerNode::start
        private void start(long a) { long[] p = {0}; final long k = ix != null ? ix.atMost(a / B, p) : 0;
            rd.reset(); rd.pos = k != 0 ? p[0] : 0;
            synchronized (m) { q.clear(); q0 = qEnd = (k != 0 ? k : 0) * B; want = q0; stop = false; err = null; started = true; }
            final long s0 = q0; th = new Thread(new Runnable() { public void run() { produce(s0); } }, "laes-layer"); th.setDaemon(true); th.start(); }
        // port of laes_play.h:LayerNode::halt
        private void halt() { synchronized (m) { stop = true; m.notifyAll(); } if (th != null) { boolean intr = false;
                for (;;) { try { th.join(); break; } catch (InterruptedException x) { intr = true; } } if (intr) Thread.currentThread().interrupt(); th = null; } }
        // port of laes_play.h:LayerNode::produce
        private void produce(long s) {
            for (; s < n;) {
                synchronized (m) { while (!(stop || qEnd - want < ahead)) { try { m.wait(); } catch (InterruptedException x) { err = new LaeException("interrupted"); m.notifyAll(); return; } } if (stop) return; }
                if (ix != null) ix.note(s / B, rd.pos);
                final int k = (int) Math.min(B, n - s); long[] blk; boolean ok = false;
                try { blk = null; if (k == B) synchronized (m) { blk = free.pollFirst(); } if (blk == null) blk = new long[k]; ok = Codec.decodeBlock(rd, blk, 0, k); } catch (Throwable x) { synchronized (m) { err = x; m.notifyAll(); } return; }   // Java: an allocation failure too
                synchronized (m) { if (!ok) { err = new LaeException("LAES layer damaged"); m.notifyAll(); return; }
                    q.addLast(blk); qEnd += k; s += k; m.notifyAll(); } } } }
    // level j of a master stored as a difference: layer + the reference of its tier (laess::refPiece)
    /** port of laes_play.h:AddRefNode */
    static final class AddRefNode extends Node { Node e = null; Stream.Sig src = null; long[] h; long wj = 0; int up = 0;
        // port of laes_play.h:AddRefNode::compute
        long[] compute(long a, long b) { long[] out = new long[(int) (b - a)]; computeInto(a, b, out, 0); return out; }
        // Java (2026-11-04): into dst, the reference from the node's workspace (the same values)
        int dc = -1000;   // port of laes_play.h:AddRefNode::dc (-1000: not looked at yet; -999: h is a filter; else the one tap's offset)
        long[] tmp = new long[0];
        // 2026-11-12 (port of laes_play.h): the layer straight from its node's decoded blocks (this node is the layer's only reader), and a
        // whole-sample reference added straight into dst (the values of refPieceW's own shortcut), without the +-127 window and the result array
        void computeInto(long a, long b, long[] dst, int off) { e.computeInto(a, b, dst, off); final int k = (int) (b - a);
            if (dc == -1000) { int c = -1, nz = 0; for (int t = 0; t < h.length; t++) if (h[t] != 0) { nz++; c = t; } dc = (nz == 1 && h[c] == (1L << 31)) ? c - 127 : -999; }
            final long s0 = a + wj + dc, s1 = b + wj + dc;
            if (dc != -999 && s0 >= 0 && s1 <= src.n) { if (tmp.length < k) tmp = new long[k]; src.get(s0, s1, tmp, 0);
                if (up >= 0) { for (int i = 0; i < k; i++) dst[off + i] += tmp[i] << up; }
                else { final int sh = -up; final long rnd = 1L << (sh - 1); for (int i = 0; i < k; i++) dst[off + i] += (tmp[i] + rnd) >> sh; }
                return; }
            final long[] r = Stream.refPieceW(src, h, wj, up, a, b, threads, ws);
            for (int i = 0; i < k; i++) dst[off + i] += r[i]; } }
    // one inverse stage: x (n values) from s (lower level) and d (octave layer)
    /** port of laes_play.h:UpNode */
    static final class UpNode extends Node { Stream.Sig s = null, d = null;
        // port of laes_play.h:UpNode::compute
        long[] compute(long a, long b) { long[] out = new long[(int) (b - a)]; computeInto(a, b, out, 0); return out; }
        // Java (2026-11-04): into dst, the stage computed in the node's workspace (the same values)
        void computeInto(long a, long b, long[] dst, int off) { final long pa = a / 2, pb = Math.min(s.n, (b + 1) / 2); final long[] x = Stream.invPieceW(s, d, n, pa, pb, threads, ws);
            System.arraycopy(x, (int) (a - 2 * pa), dst, off, (int) (b - a)); } }
    // one forward stage: the lower level s of x
    /** port of laes_play.h:DownNode */
    static final class DownNode extends Node { Stream.Sig x = null;
        // port of laes_play.h:DownNode::compute
        long[] compute(long a, long b) { return Stream.fwdPiece(x, a, b, threads)[0]; } }
    /**
     * Java only (2026-11-04, speed): the values of node `in`, computed ahead of the requests on a thread of its own, in pieces of in.chunk values
     * taken in order (in.get), at most `ahead` values beyond the latest request; the values before the latest request are kept for `keepB`
     * values (a stage re-reads a few dozen values of its input). A request behind the kept values, or far ahead, starts the thread again
     * there. Only the thread uses `in` (and the nodes below it) while it runs; the values are those of in.get.
     */
    static final class AheadNode extends Node { final Node in; final long blk, ahead, keepB = 4096; long restarts = 0;
        private Thread th = null; private final Object m = new Object(); private final ArrayDeque<long[]> q = new ArrayDeque<long[]>(), free = new ArrayDeque<long[]>();
        private long q0 = 0, qEnd = 0, want = 0; private boolean stop = false, started = false; private Throwable err = null;
        AheadNode(Node x) { in = x; n = x.n; blk = Math.max(1024, x.chunk); ahead = blk; x.keep = 4096; }   // the reads of `in` are in order and do not overlap: a short window
        long[] compute(long a, long b) { throw new IllegalStateException("AheadNode.compute"); }
        void close() { halt(); }
        public void get(long a, long b, long[] out, int off) {
            if (a < 0 || b > n || a > b) fail("internal error: signal range"); if (a == b) return;
            long q0s, qEnds; synchronized (m) { q0s = q0; qEnds = qEnd; }
            if (!started) start(a);
            else if (a < q0s) { halt(); restarts++; start(a); }   // behind the kept values
            else if (a > qEnds + ahead + blk) { halt(); start(a); }   // far ahead
            synchronized (m) { if (b > want) { want = b; m.notifyAll(); }
                while (!(err != null || qEnd >= b)) { try { m.wait(); } catch (InterruptedException x) { Thread.currentThread().interrupt(); throw fail("interrupted"); } }
                if (err != null) throw Lift.rethrow(err);
                long at = q0; java.util.Iterator<long[]> it = q.iterator();
                for (long i = a; i < b;) { final long[] t = it.next(); final long e = at + t.length;
                    if (i < e) { final long g = Math.min(b, e); System.arraycopy(t, (int) (i - at), out, off + (int) (i - a), (int) (g - i)); i = g; } at = e; }
                while (!q.isEmpty() && q0 + q.peekFirst().length <= a - keepB) { final long[] t = q.pollFirst(); q0 += t.length; if (t.length == blk && free.size() < 2) free.addLast(t); }
                m.notifyAll(); } }
        private void start(long a) {
            synchronized (m) { q.clear(); q0 = qEnd = a; want = a; stop = false; err = null; started = true; }
            final long s0 = a; th = new Thread(new Runnable() { public void run() { produce(s0); } }, "laes-ahead"); th.setDaemon(true); th.start(); }
        private void halt() { synchronized (m) { stop = true; m.notifyAll(); } if (th != null) { boolean intr = false;
                for (;;) { try { th.join(); break; } catch (InterruptedException x) { intr = true; } } if (intr) Thread.currentThread().interrupt(); th = null; } }
        private void produce(long s) {
            while (s < n) {
                long[] t = null; final long e = Math.min(n, s + blk); final int k = (int) (e - s);
                synchronized (m) { while (!(stop || qEnd - want < ahead)) { try { m.wait(); } catch (InterruptedException x) { err = new LaeException("interrupted"); m.notifyAll(); return; } }
                    if (stop) return; if (k == blk) t = free.pollFirst(); }
                try { if (t == null) t = new long[k]; in.get(s, e, t, 0); } catch (Throwable x) { synchronized (m) { err = x; m.notifyAll(); } return; }   // an allocation failure too
                synchronized (m) { if (stop) return; q.addLast(t); qEnd = e; s = e; m.notifyAll(); } } } }

    // ---- an opened LAES file
    /** port of laes_play.h:Master: layers per channel: [0] level j, [k - j] octave k */
    public static final class Master { public LaesFile.MasterInfo I = new LaesFile.MasterInfo(); public int j = 0; public long[][] layOff = new long[2][4], layLen = new long[2][4]; }
    /** port of laes_play.h:Choice: kind 0 core, 1 master, 2 rate level, 3 derived 16-bit */
    public static final class Choice { public int kind = 0; public int m = -1, level = 0, from = -1; public int rate = 0, bits = 0; public long frames = 0; public String name = "", what = "";
        public int no = 0;   // Java addition: the master's menu number (Choices.Spec.no; 0 for the core master)
        Choice copy() { Choice c = new Choice(); c.kind = kind; c.m = m; c.level = level; c.from = from; c.rate = rate; c.bits = bits; c.frames = frames; c.name = name; c.what = what; c.no = no; return c; } }
    /** a tag (key, value) */
    public static final class Tag { public final String key, value; Tag(String k, String v) { key = k; value = v; } }

    static boolean eq4(byte[] b, int o, String s) { for (int i = 0; i < 4; i++) if (b[o + i] != (byte) s.charAt(i)) return false; return true; }
    static String utf8(byte[] b, int o, int n) { try { return new String(b, o, n, "UTF-8"); } catch (UnsupportedEncodingException x) { throw new LaeException("UTF-8 not available", x); } }
    // port of laes_play.h:le32
    static long le32(byte[] p, int o) { return (p[o] & 0xFF) | (p[o + 1] & 0xFF) << 8 | (p[o + 2] & 0xFF) << 16 | (long) (p[o + 3] & 0xFF) << 24; }

    /** port of laes_play.h:File */
    public static final class File {
        /** DEC 19: bytes of ID3v2 tags in front of a FLAC file, of APEv2 / ID3v1 tags after its last frame (kept, not read) */
        public long foreignFront = 0, foreignBehind = 0;
        public boolean flac = false; public int baseRate = 0, coreBits = 0; public long coreFrames = 0;
        public byte[] fileSha = new byte[32]; public String why = "";            // why: "" when the payload passed every check; else the reason (the core still plays)
        public List<Master> ms = new ArrayList<Master>(); public List<Choice> choices = new ArrayList<Choice>(); public List<Tag> tags = new ArrayList<Tag>();
        /** the header's default master and core master flags (payload version 2; 2026-11-12) */
        public LaesFile.Head head = new LaesFile.Head();
        /** the choice a player plays when the user has chosen nothing (spec/lossless/SPEC.md, Section 10.3): the default master, else the core master (0) */
        public int defaultChoice = 0;
        private final Object idxM = new Object(); private final TreeMap<Long, LayerIdx> idx = new TreeMap<Long, LayerIdx>();
        SegSrc payload = new SegSrc(); public boolean quick = false;
        /** Java addition (tests, tools): the payload's size, and the file offset of payload byte at (-1 outside the payload) */
        public long payloadSize() { return payload.total; }
        public long payloadFileOffset(long at) { for (int s = 0; s < payload.cnt; s++) if (at >= payload.start[s] && at < payload.start[s] + payload.len[s]) return payload.off[s] + (at - payload.start[s]); return -1; }
        // block positions of the layers (by the layer's byte offset in the payload), noted while layers are decoded (memory only)
        // port of laes_play.h:File::layerIndex
        LayerIdx layerIndex(long off) { synchronized (idxM) { LayerIdx p = idx.get(off); if (p == null) { p = new LayerIdx(); idx.put(off, p); } return p; } }
        /** port of laes_play.h:File::LayerRef */
        static final class LayerRef { final long off, len, n, B; LayerRef(long o, long l, long nn, long b) { off = o; len = l; n = nn; B = b; } }

        // opens src: reads it once from start to end. Returns "" when the file is an LAES file (then why tells whether the payload is usable),
        // "not LAES" when it has no LAE2 payload (a plain WAV / FLAC, or an LAEY file: magic in laeMagic[0]), or the reason it cannot be read.
        // quickInfo: only the structure is read (for a player's track info): no CRC-32, no core, no hashes; such a File cannot be played.
        // port of laes_play.h:File::open
        public String open(final ByteSrc src, String[] laeMagic, boolean quickInfo) {
            laeMagic[0] = ""; quick = quickInfo; synchronized (idxM) { idx.clear(); } final long n = src.size(); if (n < 12) return "not LAES";
            // WAV: RIFF, or RF64 / BW64 (EBU Tech 3306, ITU-R BS.2088) for files beyond 4 GB: their 64-bit sizes are in the first chunk, "ds64"
            byte[] h = new byte[12]; src.read(0, h, 0, 12);
            foreignFront = Flac.flacStart(new Flac.Rd() { public void read(long at, byte[] buf, int k) { src.read(at, buf, 0, k); } }, n); foreignBehind = 0;   // DEC 19: ID3v2 tags in front of "fLaC"
            if (foreignFront != 0) { if (n < foreignFront + 12) return "not LAES"; src.read(foreignFront, h, 0, 12); if (!eq4(h, 0, "fLaC")) return "not LAES"; }
            flac = eq4(h, 0, "fLaC"); final boolean rf64 = (eq4(h, 0, "RF64") || eq4(h, 0, "BW64")) && eq4(h, 8, "WAVE");
            final boolean wav = (eq4(h, 0, "RIFF") && eq4(h, 8, "WAVE")) || rf64; if (!flac && !wav) return "not LAES";
            payload = new SegSrc(); payload.base = src; tags.clear();
            long audioOff = 0, audioLen = 0; final Flac.Info fi = new Flac.Info(); int fmtTag = 0, fmtCh = 0;
            if (flac) {   // metadata: STREAMINFO, the "LAE2" APPLICATION pieces, VORBIS_COMMENT tags
                long p = foreignFront + 4; boolean last = false, haveSi = false; boolean lae1 = false;
                while (!last) { if (p + 4 > n) return "FLAC metadata truncated"; byte[] bh = new byte[4]; src.read(p, bh, 0, 4); last = (bh[0] & 0x80) != 0; final int type = bh[0] & 0x7F; final long len = (long) (bh[1] & 0xFF) << 16 | (bh[2] & 0xFF) << 8 | (bh[3] & 0xFF); p += 4;
                    if (p + len > n) return "FLAC metadata truncated";
                    if (type == 0) { if (haveSi) return "two STREAMINFO blocks (FLAC allows one)"; if (len != 34) return "bad STREAMINFO"; byte[] s = new byte[34]; src.read(p, s, 0, 34); Flac.BR r = new Flac.BR(s, 0, 34); fi.minBs = (int) r.get(16); fi.maxBs = (int) r.get(16); r.get(24); r.get(24);
                        fi.rate = (int) r.get(20); fi.channels = (int) r.get(3) + 1; fi.bps = (int) r.get(5) + 1; fi.total = r.get(36); System.arraycopy(s, 18, fi.md5, 0, 16); haveSi = true; }
                    else if (type == 2 && len >= 4) { byte[] id = new byte[4]; src.read(p, id, 0, 4); if (eq4(id, 0, "LAE2")) payload.add(p + 4, len - 4); else if (eq4(id, 0, "LAE1")) lae1 = true; }
                    else if (type == 4 && len >= 8 && len < (1L << 24)) { byte[] b = new byte[(int) len]; src.read(p, b, 0, b.length); long q = 0;
                        long vl = -1, cnt = -1; if (q + 4 <= b.length) { vl = le32(b, (int) q); q += 4; }
                        if (vl >= 0 && q + vl <= b.length) { q += vl; if (q + 4 <= b.length) { cnt = le32(b, (int) q); q += 4; }
                            if (cnt >= 0) for (long i = 0; i < cnt; i++) { if (q + 4 > b.length) break; final long l = le32(b, (int) q); q += 4; if (q + l > b.length) break; final int ks = (int) q; q += l;
                                int eqp = -1; for (int t = ks; t < ks + (int) l; t++) if (b[t] == '=') { eqp = t; break; }
                                if (eqp >= 0) tags.add(new Tag(utf8(b, ks, eqp - ks), utf8(b, eqp + 1, ks + (int) l - eqp - 1))); } } }
                    p += len; }
                if (!haveSi) return "no STREAMINFO";
                if (payload.total == 0) { if (lae1) laeMagic[0] = "LAE1"; return "not LAES"; }
                if (fi.total == 0 || fi.maxBs < 16 || fi.channels != 2 || (fi.bps != 16 && fi.bps != 24 && fi.bps != 32)) return "core master is not 2-channel 16/24/32-bit FLAC with a sample count";
                foreignBehind = Flac.trailerLen(new Flac.Rd() { public void read(long at, byte[] buf, int k) { src.read(at, buf, 0, k); } }, p, n);   // DEC 19: APEv2 / ID3v1 tags at the end (checked after the frames)
                audioOff = p; audioLen = n - p; baseRate = fi.rate; coreBits = fi.bps; coreFrames = fi.total; }
            else {   // RIFF/WAVE chunks (the last 'fmt ', 'data' and 'LAE ' count; sizes clamped to the file), LIST/INFO tags
                long pos = 12, laeOff = 0, laeLen = 0; boolean haveLae = false, haveData = false;
                long dsData = 0; List<String> dsName = new ArrayList<String>(); List<Long> dsSize = new ArrayList<Long>();   // RF64: the data size and the table of other large chunks
                if (rf64) { byte[] c = new byte[8]; if (n < 20 + 28) return "RF64 without ds64"; src.read(12, c, 0, 8); if (!eq4(c, 0, "ds64") || le32(c, 4) < 28) return "RF64 without ds64";
                    final long dl = le32(c, 4); final long dn = Math.min(dl, n - 20); if (dn > Integer.MAX_VALUE - 8) throw new OutOfMemoryError(); byte[] d = new byte[(int) dn]; src.read(20, d, 0, d.length); if (d.length < 28) return "RF64 ds64 truncated";
                    dsData = le32(d, 8) | le32(d, 12) << 32; final long tn = le32(d, 24);
                    for (long i = 0; i < tn && 28 + i * 12 + 12 <= d.length; i++) { final int at = 28 + (int) i * 12; dsName.add(new String(new char[] {(char) (d[at] & 0xFF), (char) (d[at + 1] & 0xFF), (char) (d[at + 2] & 0xFF), (char) (d[at + 3] & 0xFF)}));
                        dsSize.add(le32(d, at + 4) | le32(d, at + 8) << 32); } }
                byte[] c = new byte[8];
                while (pos + 8 <= n) { src.read(pos, c, 0, 8); long len = le32(c, 4); final long body = pos + 8;
                    if (rf64 && len == 0xFFFFFFFFL) { if (eq4(c, 0, "data")) len = dsData; else for (int t = 0; t < dsName.size(); t++) if (eq4(c, 0, dsName.get(t))) { len = dsSize.get(t); break; } }   // the 64-bit size
                    if (U.gt(len, n - body)) len = n - body;
                    if (eq4(c, 0, "fmt ") && len >= 16) { byte[] f = new byte[26]; src.read(body, f, 0, len >= 26 ? 26 : 16); fmtTag = (f[0] & 0xFF) | (f[1] & 0xFF) << 8; fmtCh = (f[2] & 0xFF) | (f[3] & 0xFF) << 8; baseRate = (int) le32(f, 4); coreBits = (f[14] & 0xFF) | (f[15] & 0xFF) << 8;
                        if (fmtTag == 0xFFFE && len >= 26) fmtTag = (f[24] & 0xFF) | (f[25] & 0xFF) << 8; }
                    else if (eq4(c, 0, "data")) { audioOff = body; audioLen = len; haveData = true; }
                    else if (eq4(c, 0, "LAE ")) { laeOff = body; laeLen = len; haveLae = true; }
                    else if (eq4(c, 0, "LIST") && len >= 4 && len < (1L << 24)) { byte[] b = new byte[(int) len]; src.read(body, b, 0, b.length);
                        if (eq4(b, 0, "INFO")) for (long q = 4; q + 8 <= b.length;) { final long l = le32(b, (int) q + 4); if (q + 8 + l > b.length) break; int vn = (int) l; while (vn > 0 && b[(int) q + 8 + vn - 1] == 0) vn--;
                            tags.add(new Tag(utf8(b, (int) q, 4), utf8(b, (int) q + 8, vn))); q += 8 + l + (l & 1); } }
                    pos = body + len + (len & 1); }
                if (!haveLae || laeLen < 4) return "not LAES";
                byte[] mg = new byte[4]; src.read(laeOff, mg, 0, 4); if (!eq4(mg, 0, "LAE2")) { if (eq4(mg, 0, "LAE1")) laeMagic[0] = "LAE1"; return "not LAES"; }
                if (fmtTag != 1 || fmtCh != 2 || (coreBits != 16 && coreBits != 24 && coreBits != 32) || !haveData) return "core master is not 2-channel 16/24/32-bit integer PCM";
                payload.add(laeOff, laeLen); coreFrames = audioLen / (2 * (coreBits / 8)); }
            laeMagic[0] = "LAE2";
            if (quick) { why = payload.total < 68 ? "LAES block too short" : parsePayload(); limitMasters(); makeChoices(); return ""; }
            return openFull(src, n, audioOff, audioLen, fi);
        }

        // ---- the one pass over the whole file of open(): its SHA-256, the payload CRC-32 (all but its last 4 bytes), the core
        // Java (2026-11-04, memory bound): the core is not kept. The pass computes what the C++ computes from the kept core (the core's SHA-256
        // for parsePayload, the STREAMINFO MD5 of the FLAC form) and notes the FLAC frame positions for CoreStore; the FLAC frames are decoded
        // in order as they arrive (the C++ flacParallel, which decodes a whole kept fixed-block-size core in parallel at the end, gives the same
        // values and the same first error as the decoding in order, and is not used). Reads are clamped to the memory budget (C++: 4 MiB).
        private final Object cm = new Object(); private final ArrayDeque<byte[]> cq = new ArrayDeque<byte[]>(); private boolean cend = false; private volatile String coreErr = "";
        private byte[] fbA = new byte[0]; private int fbSize = 0, fbHead = 0; private long fbFile0 = 0; private long frameNo = 0; private AlignHash.Md5 md5; private byte[] carry = new byte[0]; private int carryN = 0;
        private Flac.Info fiC; private long coreCount = 0; private LaesFile.Sha256 coreShaD; private byte[] coreShaV = null; private byte[] shb = new byte[1 << 16]; private int shf = 0;
        CoreStore cs = null; private final Flac.Frame frO = new Flac.Frame();   // frO: Java, the frame decoded in open(), reused
        /** the memory budget in bytes for the decoding of this file (Java only; see LaesDecoder.laes_set_memory) */
        public long memBudget = 64L << 20;
        /**
         * Java only (2026-11-12; phones do not have the memory for 255 masters, so a limit applies until devices allow more): the largest number of masters a file may
         * have for this decoder to offer them; a file with more plays its core master only, with the reason in why (as a payload that failed a
         * check). Measured on a PC (Java, 64 MB budget, 44.1 kHz masters in a reference chain): about 1 MB more heap per master of the chain
         * (40 deep: 76 MB) and decoding time growing with the chain; 255 masters do not fit a phone.
         * A constant (FU26-2, user 2026-11-14: a phone must never open a file with more masters): it cannot be raised from outside, and the
         * Java LAES encoder refuses more than this many masters before it starts.
         */
        public static final int maxMasters = 16;
        private void limitMasters() { if (why.isEmpty() && ms.size() > maxMasters) { why = "this file has " + ms.size() + " masters; this player offers files with up to " + maxMasters + " (the core master plays)"; ms.clear(); } }
        // the core's SHA-256 as parsePayload computes it in the C++ (16 bit: int16 LE; 24 / 32 bit: int32 LE), fed sample by sample
        private void shaPut(int v) { if (coreBits == 16) { shb[shf++] = (byte) v; shb[shf++] = (byte) (v >>> 8); } else { shb[shf++] = (byte) v; shb[shf++] = (byte) (v >>> 8); shb[shf++] = (byte) (v >>> 16); shb[shf++] = (byte) (v >>> 24); }
            if (shf >= (1 << 16) - 8) { coreShaD.update(shb, 0, shf); shf = 0; } }
        // decodes the complete frames in fb (a frame is at most a few hundred kB; 4 MB stay ahead unless final)
        // port of laes_play.h:File::open (lambda flacFrames)
        private void flacFrames(boolean fin) {
            final int fby = (fiC.bps + 7) / 8; byte[] mb = new byte[0];
            while (coreErr.isEmpty() && fbHead < fbSize && coreCount < fiC.total && (fin || fbSize - fbHead > (4 << 20))) {   // DEC 19: no frame after the last sample
                final Flac.Frame F = Flac.decodeFrame(fbA, fbHead, fbSize - fbHead, fiC, frameNo, coreCount, frO);
                if (!F.err.isEmpty()) { coreErr = F.err; break; } cs.addFrame(fbFile0 + fbHead, coreCount); fbHead += F.used; frameNo++; cs.frEnd = fbFile0 + fbHead;
                if (mb.length < F.n * 2 * fby) mb = new byte[F.n * 2 * fby]; int mf = 0;
                for (int i = 0; i < F.n; i++) { for (int c = 0; c < 2; c++) { final long v = F.ch[c][i]; shaPut((int) v); for (int k = 0; k < fby; k++) mb[mf++] = (byte) (v >>> (8 * k)); } coreCount++; }
                md5.update(mb, 0, mf); }
            if (fbHead > (1 << 20)) { System.arraycopy(fbA, fbHead, fbA, 0, fbSize - fbHead); fbSize -= fbHead; fbFile0 += fbHead; fbHead = 0; } }
        // port of laes_play.h:File::open (lambda coreFeed)
        private void coreFeed(byte[] p, int pOff, int k) {
            if (flac) { if (fbSize + k > fbA.length) fbA = java.util.Arrays.copyOf(fbA, Math.max(fbSize + k, fbA.length + fbA.length / 2));
                System.arraycopy(p, pOff, fbA, fbSize, k); fbSize += k; flacFrames(false); return; }
            final int by = coreBits / 8;
            if (carryN + k > carry.length) carry = java.util.Arrays.copyOf(carry, carryN + k); System.arraycopy(p, pOff, carry, carryN, k); carryN += k;
            final int whole = carryN / (2 * by) * (2 * by);
            for (int q = 0; q < whole && coreCount < coreFrames; q += by * 2) { for (int c = 0; c < 2; c++) { final int s = q + c * by; int v = 0;
                    for (int t = 0; t < by; t++) v |= (carry[s + t] & 0xFF) << (8 * t); if (coreBits < 32 && ((v >>> (coreBits - 1)) & 1) != 0) v |= -1 << coreBits; shaPut(v); }
                coreCount++; }
            System.arraycopy(carry, whole, carry, 0, carryN - whole); carryN -= whole; }
        static void joinQuiet(Thread t) { boolean intr = false; for (;;) { try { t.join(); break; } catch (InterruptedException x) { intr = true; } } if (intr) Thread.currentThread().interrupt(); }
        private String openFull(final ByteSrc src, final long n, final long audioOff, final long audioLen, final Flac.Info fi) {
            final long crcEnd = payload.total >= 4 ? payload.total - 4 : 0; CRC32 crc = new CRC32();
            cs = new CoreStore(); cs.src = src; cs.flac = flac; cs.bits = coreBits; cs.audioOff = audioOff; cs.fi = fi;
            fbA = new byte[0]; fbSize = 0; fbHead = 0; fbFile0 = audioOff; frameNo = 0; coreErr = ""; md5 = new AlignHash.Md5(); carry = new byte[0]; carryN = 0; fiC = fi; cend = false; cq.clear();
            coreCount = 0; coreShaD = new LaesFile.Sha256(); shf = 0; coreShaV = null;
            final int RD = (int) Math.max(1L << 18, Math.min(4L << 20, memBudget / 64));   // bytes per read (C++ 4 MiB); at most 9 such buffers live
            // three threads, each fed in file order: this one reads and computes the CRC-32; the file SHA-256 (Hasher) and the core (FLAC frames
            // decoded and hashed, or WAV samples) run on threads of their own. The results are those of one pass in order.
            Thread ct = new Thread(new Runnable() { public void run() {
                for (;;) { byte[] b; synchronized (cm) { while (!(cend || !cq.isEmpty())) { try { cm.wait(); } catch (InterruptedException x) { coreErr = "interrupted"; cq.clear(); cm.notifyAll(); return; } }
                        if (cq.isEmpty()) return; b = cq.pollFirst(); cm.notifyAll(); }
                    try { if (coreErr.isEmpty()) coreFeed(b, 0, b.length); } catch (RuntimeException x) { coreErr = String.valueOf(x.getMessage()); }
                    catch (OutOfMemoryError x) { coreErr = "bad allocation"; } } } }, "laes-core");
            ct.start();
            Hasher hs = new Hasher(); boolean readAll = false;
            try {
                for (long a = 0; a < n; a += RD) { final int k = (int) Math.min(RD, n - a); final byte[] buf = new byte[k]; src.read(a, buf, 0, k); hs.push(buf);
                    for (int s = 0; s < payload.cnt; s++) {   // payload bytes in this read
                        final long o0 = Math.max(a, payload.off[s]), o1 = Math.min(a + k, payload.off[s] + payload.len[s]);
                        if (o0 >= o1) continue; final long p0 = payload.start[s] + (o0 - payload.off[s]); final long e1 = Math.min(o1, o0 + (crcEnd > p0 ? crcEnd - p0 : 0));
                        if (e1 > o0) crc.update(buf, (int) (o0 - a), (int) (e1 - o0)); }
                    final long c0 = Math.max(a, audioOff), c1 = Math.min(a + k, audioOff + audioLen);   // core bytes in this read
                    if (c0 < c1) { final byte[] piece = (c0 == a && c1 == a + k) ? buf : java.util.Arrays.copyOfRange(buf, (int) (c0 - a), (int) (c1 - a));
                        synchronized (cm) { while (cq.size() >= 4) { try { cm.wait(); } catch (InterruptedException x) { Thread.currentThread().interrupt(); throw fail("interrupted"); } } cq.addLast(piece); cm.notifyAll(); } } }
                readAll = true;
            } finally { synchronized (cm) { cend = true; cm.notifyAll(); } joinQuiet(ct);
                if (!readAll) hs.close(); }   // FU25-1: a read that failed half-way also ends the SHA-256 thread (it waited for ever, holding its buffers)
            hs.finish(fileSha);
            final long crcV = crc.getValue();
            if (flac) { flacFrames(true); if (!coreErr.isEmpty()) return "FLAC core master: " + coreErr;
                {   // DEC 19: after the last frame only APEv2 / ID3v1 tags (before, any byte there was refused as a frame)
                    final int rest = fbSize - fbHead;
                    if (rest != 0 && Flac.trailerLen(Flac.rdOf(fbA, fbHead), 0, rest) != rest) return "FLAC core master: bytes after the last audio frame that are no APEv2 / ID3v1 tag";
                    foreignBehind = rest; }
                if (coreCount != fi.total) return "FLAC core master: sample count differs from STREAMINFO";
                byte[] m = Flac.hexBytes(md5.hex(), 16); boolean zero = true; for (int i = 0; i < 16; i++) zero &= fi.md5[i] == 0;
                if (zero || !java.util.Arrays.equals(m, fi.md5)) return "FLAC core master: MD5 mismatch"; }
            fbA = new byte[0]; carry = new byte[0];
            coreShaD.update(shb, 0, shf); coreShaV = new byte[32]; coreShaD.finish(coreShaV);
            coreFrames = coreCount; cs.frames = coreCount;
            // the payload: CRC-32, header, master records, the layers' byte ranges
            why = ""; if (payload.total < 68) why = "LAES block too short";
            else { byte[] t = new byte[4]; payload.read(payload.total - 4, t, 0, 4); if (le32(t, 0) != crcV) why = "LAES block damaged (CRC-32)"; }
            if (why.isEmpty()) why = parsePayload();
            limitMasters();
            makeChoices(); return ""; }
        /** the core samples [a, b) of channel c (Java addition: the core is read on demand, see CoreStore) */
        public void getCore(int c, long a, long b, long[] out, int off) { if (cs == null) fail("opened for track info only"); cs.get(c, a, b, out, off); }

        // port of laes_play.h:File::parsePayload
        private String parsePayload() {
            ms.clear(); head = new LaesFile.Head(); final long end = payload.total - 4; final long[] pos = {0}; final boolean[] trunc = {false}; final byte[] t8 = new byte[8];
            class Rd { long u(int k) { if (pos[0] + k > end) { trunc[0] = true; return 0; } payload.read(pos[0], t8, 0, k); pos[0] += k; long v = 0; for (int i = 0; i < k; i++) v |= (long) (t8[i] & 0xFF) << (8 * i); return v; } }
            Rd R = new Rd();
            long magic = R.u(4), ver = R.u(2), hs = R.u(2), base = R.u(4), cbits = R.u(1), ch = R.u(1), nm = R.u(1), cmode = R.u(1), nc = R.u(8); byte[] csha = new byte[32]; if (pos[0] + 32 <= end) { payload.read(pos[0], csha, 0, 32); pos[0] += 32; } else trunc[0] = true;
            final byte[] hb = new byte[8]; if (pos[0] + 8 <= end) { payload.read(pos[0], hb, 0, 8); pos[0] += 8; } else trunc[0] = true;
            if (trunc[0] || magic != 0x3245414CL) return "not an LAES block"; if ((ver != 1 && ver != 2) || hs != 64) return "unsupported LAES version";
            if ((base != 44100 && base != 48000) || (int) base != baseRate || (int) cbits != coreBits || ch != 2 || cmode > 1 || !LaesFile.headerCountOk(ver, nm) || !LaesFile.headFrom(ver, hb, 0, nm, head)) return "invalid LAES header";
            if (nc != coreFrames) return "core master length differs from the LAES header";
            if (!quick) {   // the core's SHA-256 (16 bit: int16 LE; 24 / 32 bit: int32 LE), computed in the pass of open() (Java: the core is not kept)
                if (!java.util.Arrays.equals(coreShaV, csha)) return "core master differs from the one this LAES block was made for (SHA-256)"; }
            for (long mi = 0; mi < nm; mi++) { Master M = new Master(); LaesFile.MasterInfo I = M.I;
                I.bits = (int) R.u(1); I.stages = (int) R.u(1); I.w = (int) R.u(1); I.storage = (int) R.u(1); I.refTier = (int) R.u(1);
                final long nl = R.u(1), fl = R.u(1), mp = R.u(1); I.rate = (int) R.u(4); R.u(4); final long F64 = R.u(8);
                I.whole = R.u(8); I.frac = (int) R.u(4); R.u(4); if (pos[0] + 32 <= end) { payload.read(pos[0], I.sha, 0, 32); pos[0] += 32; } else trunc[0] = true;
                if (trunc[0]) return "LAES block truncated";
                if (ver == 2) { if (pos[0] + nl > end) return "LAES block truncated"; final byte[] nb = new byte[(int) nl]; if (nl != 0) payload.read(pos[0], nb, 0, (int) nl); pos[0] += nl;
                    if ((fl & ~(long) LaesFile.NO_DERIVE) != 0) return "invalid master record"; if (!LaesFile.validMasterName(nb, 0, nb.length)) return "invalid master name";
                    I.name = LaesFile.nameOf(nb, 0, nb.length); I.flags = (int) fl; I.menu = (int) mp; }
                if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int) mi) || (I.storage == 2 && I.refTier < 1)
                    || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 == 0 || U.gt((F64 + (1L << I.stages) - 1) >>> I.stages, nc + (1L << 24))
                    || I.whole <= -(1L << 40) || I.whole >= (1L << 40) || !LaesFile.tWithinTrack(I.whole, I.frac, nc)) return "invalid master record";   // DEC 11
                if (F64 < 0) return "master too long for this decoder"; I.frames = F64;   // F64 > INT64_MAX: as laes_play.h / laes_stream.h since 635019a
                M.j = I.storage == 2 ? Math.min(I.stages, ms.get(I.refTier - 1).I.stages) : 0;
                for (int c = 0; c < 2; c++) for (int k = M.j; k <= I.stages; k++) { final long l = R.u(4); if (trunc[0] || pos[0] + l > end) return "LAES block truncated"; M.layOff[c][k - M.j] = pos[0]; M.layLen[c][k - M.j] = l; pos[0] += l; }
                ms.add(M); }
            { List<LaesFile.MasterInfo> mi = new ArrayList<LaesFile.MasterInfo>(); for (Master M : ms) mi.add(M.I); if (!LaesFile.menuOk(mi)) return "invalid menu positions"; }
            if (pos[0] != end) return "unexpected bytes after the last master";
            return ""; }

        // port of laes_play.h:File::makeChoices  (the same list and order as `laes dec --list` (all masters offered until a run proves otherwise))
        private void makeChoices() {
            choices.clear();
            final List<LaesFile.MasterInfo> mi = new ArrayList<LaesFile.MasterInfo>(); if (why.isEmpty()) for (Master M : ms) mi.add(M.I);   // a rejected payload: the core master only
            final LaesFile.Head hd = why.isEmpty() ? head : new LaesFile.Head();
            final List<Choices.Spec> sp = Choices.build(baseRate, coreBits, coreFrames, mi, null, hd); defaultChoice = Choices.defaultIndex(sp, hd);
            for (Choices.Spec s : sp) { Choice c = new Choice(); c.kind = s.kind; c.m = s.m; c.level = s.level; c.from = s.from; c.rate = s.rate; c.bits = s.bits; c.frames = s.frames;
                c.name = s.name; c.what = s.what; c.no = s.no; choices.add(c); } }

        // every layer of every master: byte range, values, block length
        // port of laes_play.h:File::layerList
        List<LayerRef> layerList() {
            List<LayerRef> ls = new ArrayList<LayerRef>();
            for (Master M : ms) { final LaesFile.MasterInfo I = M.I;
                for (int c = 0; c < 2; c++) for (int k = M.j; k <= I.stages; k++)
                    ls.add(new LayerRef(M.layOff[c][k - M.j], M.layLen[c][k - M.j], k == M.j ? LaesFile.levLen(I.frames, I.stages - M.j) : LaesFile.levLen(I.frames, I.stages - k) / 2, 4096L << (k == M.j ? M.j : k - 1))); }
            return ls; }
        // record: "LAEI" 1, file SHA-256 (32), uint32 layers; per layer: uint64 offset, uint64 blocks, then the positions of blocks 0, 16, 32 ...
        // as differences (LEB128); uint32 CRC-32 of everything before it. Little-endian.
        // port of laes_play.h:File::exportIndex  (empty until every layer is noted in full)
        public byte[] exportIndex() {
            final List<LayerRef> ls = layerList(); java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); o.write('L'); o.write('A'); o.write('E'); o.write('I'); o.write(1); o.write(fileSha, 0, 32);
            putU(o, ls.size(), 4);
            for (LayerRef l : ls) { final long nb = (l.n + l.B - 1) / l.B; LayerIdx ix = layerIndex(l.off);
                synchronized (ix) { if (ix.bitN < nb) return new byte[0];   // not noted in full yet
                    putU(o, l.off, 8); putU(o, nb, 8); long prev = 0; for (long b = 0; b < nb; b += LayerIdx.kSparse) { final long p = ix.bit[(int) b]; long v = p - prev;
                        do { int bb = (int) (v & 0x7F); v >>>= 7; if (v != 0) bb |= 0x80; o.write(bb); } while (v != 0); prev = p; } } }
            byte[] r = o.toByteArray(); CRC32 crc = new CRC32(); crc.update(r, 0, r.length); putU(o, crc.getValue(), 4); return o.toByteArray(); }
        private static void putU(java.io.ByteArrayOutputStream o, long v, int n) { for (int i = 0; i < n; i++) o.write((int) (v >>> (8 * i)) & 0xFF); }
        // port of laes_play.h:File::importIndex
        public boolean importIndex(byte[] d, int n) {
            if (d == null || n < 5 + 32 + 4 + 4 || !(d[0] == 'L' && d[1] == 'A' && d[2] == 'E' && d[3] == 'I' && d[4] == 1)) return false;
            for (int i = 0; i < 32; i++) if (d[5 + i] != fileSha[i]) return false;
            CRC32 crc = new CRC32(); crc.update(d, 0, n - 4); if (leU(d, n - 4, 4) != crc.getValue()) return false;
            final List<LayerRef> ls = layerList(); int at = 37; final int end = n - 4;
            if (leU(d, at, 4) != ls.size()) return false; at += 4;
            List<long[]> got = new ArrayList<long[]>();
            for (int i = 0; i < ls.size(); i++) { final LayerRef l = ls.get(i); final long nb = (l.n + l.B - 1) / l.B;
                if (at + 16 > end || leU(d, at, 8) != l.off || leU(d, at + 8, 8) != nb) return false; at += 16;
                long[] g = new long[(int) ((nb + LayerIdx.kSparse - 1) / LayerIdx.kSparse)]; int gn = 0;
                long prev = 0; for (long b = 0; b < nb; b += LayerIdx.kSparse) { long v = 0; int sh = 0;
                    for (;;) { if (at >= end || sh > 63) return false; final int c = d[at++] & 0xFF; v |= (long) (c & 0x7F) << sh; sh += 7; if ((c & 0x80) == 0) break; }
                    final long p = prev + v; if ((b == 0 ? p != 0 : U.le(p, prev)) || U.ge(p, l.len * 8)) return false; g[gn++] = p; prev = p; }
                got.add(g); }
            if (at != end) return false;
            for (int i = 0; i < ls.size(); i++) { LayerIdx ix = layerIndex(ls.get(i).off); synchronized (ix) {
                ix.sp = got.get(i); ix.spEnd = (ls.get(i).n + ls.get(i).B - 1) / ls.get(i).B; } }
            return true; }
        private static long leU(byte[] d, int at, int k) { long v = 0; for (int i = 0; i < k; i++) v |= (long) (d[at + i] & 0xFF) << (8 * i); return v; }
        // decodes every layer once, entropy decoding only (seconds), noting where each block begins, so that later starts at any frame can
        // begin near it; layers run on up to `threads` threads; cancel stops it early
        // port of laes_play.h:File::indexLayers
        public void indexLayers(final AtomicBoolean cancel, int threads) {
            final List<LayerRef> ls = layerList();
            final AtomicInteger next = new AtomicInteger(0); final Throwable[] first = {null};
            Runnable work = new Runnable() { public void run() { try { for (;;) { final int i = next.getAndIncrement(); if (i >= ls.size()) return; final LayerRef l = ls.get(i); LayerIdx ix = layerIndex(l.off);
                    if (ix.known() * l.B >= l.n) continue;   // noted already
                    BitRS rd = new BitRS(); rd.src = payload; rd.off = l.off; rd.len = l.len; rd.win = 1 << 16; long[] blk = new long[(int) Math.min(l.B, l.n)];
                    for (long s = 0, kb = 0; s < l.n; s += l.B, kb++) { if (cancel != null && cancel.get()) return;
                        ix.note(kb, rd.pos); final int k = (int) Math.min(l.B, l.n - s);
                        if (!Codec.decodeBlock(rd, blk, 0, k)) break; } } }
                catch (Throwable x) { synchronized (first) { if (first[0] == null) first[0] = x; } } } };
            List<Thread> ts = new ArrayList<Thread>(); final int nt = Math.max(1, Math.min(threads, ls.size()));
            for (int t = 1; t < nt; t++) { Thread x = new Thread(work, "laes-index"); x.setDaemon(true); x.start(); ts.add(x); } work.run(); for (Thread x : ts) joinQuiet(x);
            if (first[0] != null) throw Lift.rethrow(first[0]); }
    }

    // SHA-256 on a thread of its own, fed in order (at most 4 buffers waiting), so that hashing overlaps with decoding
    /** port of laes_play.h:Hasher */
    static final class Hasher { final LaesFile.Sha256 sha = new LaesFile.Sha256(); Thread t = null; final Object m = new Object(); final ArrayDeque<Buf> q = new ArrayDeque<Buf>(); boolean end = false;
        // Java (2026-11-04): a queued buffer (n bytes of b); pooled buffers go back to a short free list once hashed (take() hands them out again)
        static final class Buf { final byte[] b; final int n; final boolean pooled; Buf(byte[] bb, int nn, boolean p) { b = bb; n = nn; pooled = p; } }
        private final ArrayDeque<byte[]> free = new ArrayDeque<byte[]>();
        // a buffer of at least n bytes for push(b, n, true)
        byte[] take(int n) { synchronized (m) { final byte[] b = free.pollFirst(); if (b != null && b.length >= n) return b; } return new byte[n]; }
        // port of laes_play.h:Hasher::push
        void push(byte[] b) { push(b, b.length, false); }
        void push(byte[] b, int n, boolean pooled) { synchronized (m) { if (t == null) { t = new Thread(new Runnable() { public void run() { loop(); } }, "laes-sha"); t.setDaemon(true); t.start(); }
                while (q.size() >= 4) { try { m.wait(); } catch (InterruptedException x) { Thread.currentThread().interrupt(); throw fail("interrupted"); } } q.addLast(new Buf(b, n, pooled)); m.notifyAll(); } }
        // port of laes_play.h:Hasher::final
        void finish(byte[] out) { synchronized (m) { end = true; m.notifyAll(); } if (t != null) File.joinQuiet(t); sha.finish(out); }
        // port of laes_play.h:Hasher::~Hasher
        void close() { synchronized (m) { end = true; q.clear(); m.notifyAll(); } if (t != null) File.joinQuiet(t); }
        // port of laes_play.h:Hasher::loop
        private void loop() { for (;;) { Buf b; synchronized (m) { while (!(end || !q.isEmpty())) { try { m.wait(); } catch (InterruptedException x) { return; } } if (q.isEmpty()) return; b = q.pollFirst(); m.notifyAll(); }
                sha.update(b.b, 0, b.n); if (b.pooled) synchronized (m) { if (free.size() < 6) free.addLast(b.b); } } } }

    // runs f(0) on a second thread and f(1) here (the two channels share no node), then rethrows the first exception
    interface Chan { void run(int c); }
    // port of laes_play.h:bothChannels
    // Java (2026-11-04): channel 0 runs on a kept pool thread (the C++ starts a thread per call; a player calls this many times per second)
    private static final java.util.concurrent.ExecutorService CH0 = java.util.concurrent.Executors.newCachedThreadPool(new java.util.concurrent.ThreadFactory() {
        public Thread newThread(Runnable r) { Thread t = new Thread(r, "laes-ch0"); t.setDaemon(true); return t; } });
    static void bothChannels(final Chan f) {
        final Throwable[] e0 = {null}; java.util.concurrent.Future<?> t = CH0.submit(new Runnable() { public void run() { try { f.run(0); } catch (Throwable x) { e0[0] = x; } } });
        Throwable e1 = null; try { f.run(1); } catch (Throwable x) { e1 = x; }
        boolean intr = false; for (;;) { try { t.get(); break; } catch (InterruptedException x) { intr = true; } catch (java.util.concurrent.ExecutionException x) { if (e0[0] == null) e0[0] = x.getCause(); break; } } if (intr) Thread.currentThread().interrupt();
        if (e0[0] != null) throw Lift.rethrow(e0[0]); if (e1 != null) throw Lift.rethrow(e1); }

    // ---- decoding one choice in time order
    /** port of laes_play.h:Player */
    public static final class Player {
        File f = null; Choice C = new Choice(); int threads = 1;
        public long pos = 0; public boolean done = false, ok = false, noHash = false;
        public String err = "";
        // the nodes: per decoded master, per channel; tiers' lower levels are shared
        /** port of laes_play.h:Player::MD */
        static final class MD { int idx = -1; List<Node> own = new ArrayList<Node>(); Node[] top = new Node[2]; Node[][] lev = new Node[2][]; Hasher sha = new Hasher(); long hashed = 0; boolean shaOk = false; boolean wide = false; }   // wide: a sample outside the word length (FU12-7)
        List<MD> chain = new ArrayList<MD>(); CoreNode[] coreSig = {new CoreNode(), new CoreNode()};
        long[] rng = {20261029L + 0x9E3779B97F4A7C15L * 1, 20261029L + 0x9E3779B97F4A7C15L * 2};
        // port of laes_play.h:Player::xs64Apply / xs64Jump (2026-11-12): the derived 16-bit dither's xorshift64 state after `steps` steps
        // (2 per sample) in O(log steps), as a 64 x 64 bit matrix power over GF(2); steps is a uint64 in a long (shifted with >>>)
        static long xs64Apply(long[] M, long s) { long r = 0; for (int j = 0; j < 64; j++) if (((s >>> j) & 1) != 0) r ^= M[j]; return r; }
        static long xs64Jump(long s, long steps) {
            long[] M = new long[64], M2 = new long[64]; for (int j = 0; j < 64; j++) { long v = 1L << j; v ^= v << 13; v ^= v >>> 7; v ^= v << 17; M[j] = v; }
            while (steps != 0) { if ((steps & 1) != 0) s = xs64Apply(M, s); for (int j = 0; j < 64; j++) M2[j] = xs64Apply(M, M[j]); System.arraycopy(M2, 0, M, 0, 64); steps >>>= 1; }
            return s; }
        // Java (2026-11-04, memory bound): unit = the values a node computes ahead per request (its chunk), the frames of one internal read or
        // hash step; tabs[m] = master m's time origin on the core's time line (base samples): T of the master plus the tabs of its tier
        long[][] px = {new long[0], new long[0]}; long[] ha = new long[0], hb = new long[0];   // Java (2026-11-04): arrays reused from step to step (readStep, hashTo)
        long unit = 1 << 17; double[] tabs = new double[0]; double tHashed = Double.NaN;   // tHashed: the time position every master is hashed to
        /** halts the layer threads and the hashers (C++: the destructors) */
        // (Java 2026-11-04: in reverse order of creation, so that the AheadNodes (added last) stop before the layers their threads may wait for)
        public void close() { for (MD d : chain) { for (int i = d.own.size() - 1; i >= 0; i--) d.own.get(i).close(); d.sha.close(); } }
        // allMasters: every master of the file is decoded and checked in this run (one pass), not only the ones the choice needs
        // startFrame > 0: output begins at that frame (the layers begin at their noted block positions, see LayerIdx); no SHA-256 is computed,
        // so this is only for a master already verified for this file content. A derived 16-bit version starts there too: its dither state
        // is moved to that frame (xs64Jump), the output is the same bit for bit as when decoded from frame 0 (2026-11-12).
        // port of laes_play.h:Player::start
        public String start(File file, int choice, int nThreads, boolean allMasters, long startFrame) {
            f = file; threads = Math.max(1, nThreads); close(); chain.clear(); pos = 0; done = false; ok = false; noHash = false; err = "";
            if (f.quick) return "opened for track info only";
            if (choice < 0 || choice >= f.choices.size()) return "no such choice";
            C = f.choices.get(choice);
            if (startFrame < 0 || startFrame > C.frames) return "start position outside the track";
            for (int c = 0; c < 2; c++) rng[c] = xs64Jump(20261029L + 0x9E3779B97F4A7C15L * (c + 1), 2 * startFrame);   // the dither state at the start frame (0: the seed)
            if (startFrame > 0) { pos = startFrame; noHash = true; }
            for (int c = 0; c < 2; c++) { coreSig[c].v = f.cs; coreSig[c].c = c; coreSig[c].n = f.coreFrames; }
            tabs = new double[f.ms.size()]; tHashed = Double.NaN;
            for (int m = 0; m < tabs.length; m++) { final LaesFile.MasterInfo I = f.ms.get(m).I; tabs[m] = (double) I.whole + (double) (I.frac & 0xFFFFFFFFL) / 4294967296.0 + (I.refTier > 0 ? tabs[I.refTier - 1] : 0.0); }
            try { final Choice S = C.kind == 3 ? f.choices.get(C.from) : C; if (S.kind != 0) need(S.m);
                if (allMasters) for (int m = 0; m < f.ms.size(); m++) need(m);   // every master decoded (and hashed) once, in this one run
                if (chain.isEmpty()) return "";
                if (S.kind == 2) for (MD d : chain) if (d.idx == S.m) for (int c = 0; c < 2; c++) levelOf(d, c, S.level);   // made now: the channels run in parallel later
                for (MD d : chain) for (Node nd : d.own) nd.threads = Math.max(1, threads / 2);   // two channels at once, each with half the threads
                // C++: nodes that other masters read at an offset (tiers) keep a window that covers the largest time offset in the chain, every
                // node computes 2^17 values ahead, a layer runs 2^19 values ahead, and the masters are hashed at the same share of their lengths.
                // Java (2026-11-04): the sizes follow the memory budget instead (the values never depend on them: a node computes any range on
                // request), and the masters are hashed at the same time position (advanceHashes), so that the readers of a shared node stay close
                // together and a short window serves them all. Per node at most about keep + 2 chunk + one request values are kept.
                // Java (2026-11-04): the stages that will run ahead (pipePlan), and room in the budget for the reused temporaries of the stages
                // (Stream.Work: about 4 chunks of an UpNode, 2 of an AddRefNode) and for the values an AheadNode holds (about 3 chunks)
                final List<UpNode> plan = pipePlan(S); final java.util.IdentityHashMap<Stream.Sig, Boolean> ahead = new java.util.IdentityHashMap<Stream.Sig, Boolean>(); for (UpNode U : plan) ahead.put(U.s, Boolean.TRUE);
                int layers = 0; double W = 0; for (MD d : chain) for (Node nd : d.own) { W += (1.0 + (nd instanceof UpNode ? 0.5 : nd instanceof AddRefNode ? 0.3 : 0.0) + (ahead.containsKey(nd) ? 0.4 : 0.0)) / (1 << nd.sh); if (nd instanceof LayerNode) layers++; }
                long u = (long) (f.memBudget * 0.4 / (64.0 * W)); u = Math.max(4096, Math.min(1 << 17, u)) & ~1023L; unit = u;   // about 8 x chunk values of 8 bytes per node at most
                final int win = (int) Math.max(1 << 14, Math.min(1 << 20, f.memBudget / 16 / Math.max(1, layers)));
                long span = 0; for (MD d : chain) { final LaesFile.MasterInfo I = f.ms.get(d.idx).I; span = Math.max(span, Math.abs(I.whole) + 2); }
                final long chainReach = (long) chain.size() * ((span << 3) + 256);   // 2026-11-12: each level of a reference chain runs at most the filter's reach (128 + T) ahead
                for (MD d : chain) for (Node nd : d.own) { nd.chunk = Math.max(1024, u >> nd.sh); nd.keep = 2 * nd.chunk + 4096 + chainReach; if (nd instanceof LayerNode) { ((LayerNode) nd).ahead = nd.chunk; ((LayerNode) nd).rd.win = win; } }
                for (MD d : chain) for (UpNode U : plan) if (d.own.contains(U)) { final AheadNode A = new AheadNode((Node) U.s); U.s = A; d.own.add(A); } }
            catch (RuntimeException x) { return String.valueOf(x.getMessage()); }
            catch (OutOfMemoryError x) { return "bad allocation"; }
            return ""; }
        // the next frames (interleaved L R, int32 in the choice's word length); returns the number written (0 at the end)
        // Java: taken in steps of at most `unit` frames at the master's top rate (the same values; the steps bound the memory a request needs)
        private long stepFrames() { final Choice S = C.kind == 3 ? f.choices.get(C.from) : C; return S.kind == 2 ? Math.max(1024, unit >> (f.ms.get(S.m).I.stages - S.level)) : unit; }
        // port of laes_play.h:Player::read
        public long read(final int[] out, long frames) {
            long got = 0;
            while (got < frames) { final long k = readStep(out, (int) (got * 2), Math.min(stepFrames(), frames - got)); if (!err.isEmpty()) return 0; if (k <= 0) break; got += k; }
            return got; }
        private long readStep(final int[] out, final int outOff, long frames) {
            if (done || !err.isEmpty()) return 0;
            try { final long k = Math.min(frames, C.frames - pos); if (k <= 0) { done = true; return 0; }
                final Choice S = C.kind == 3 ? f.choices.get(C.from) : C; final long[][] x = new long[2][];
                final long p0 = pos;
                bothChannels(new Chan() { public void run(int c) { if (px[c].length < k) px[c] = new long[(int) k]; x[c] = px[c];   // Java: the player's reused arrays
                    if (S.kind == 0) { f.cs.get(c, p0, p0 + k, x[c], 0); return; }
                    MD D = null; for (MD d : chain) if (d.idx == S.m) D = d; final LaesFile.MasterInfo I = f.ms.get(S.m).I;
                    Node src = S.kind == 1 ? D.top[c] : D.lev[c][S.level]; src.get(p0, p0 + k, x[c], 0);
                    if (S.kind == 1) { for (int i = 0; i < k; i++) x[c][i] <<= I.w; }
                    else { final long mx = (1L << (S.bits - 1)) - 1, mn = -(1L << (S.bits - 1)); for (int i = 0; i < k; i++) x[c][i] = Math.max(mn, Math.min(mx, x[c][i] << I.w)); } } });
                for (int i = 0; i < k; i++) for (int c = 0; c < 2; c++) { long v = x[c][i];
                    if (C.kind == 3) { final int sft = S.bits - 16;   // as `laes dec`
                        long st = rng[c]; st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r1 = st >>> (64 - sft); st ^= st << 13; st ^= st >>> 7; st ^= st << 17; final long r2 = st >>> (64 - sft); rng[c] = st;
                        v = Math.max(-32768L, Math.min(32767L, (v + r1 - r2 + (1L << (sft - 1))) >> sft)); }
                    out[outOff + i * 2 + c] = (int) v; }
                pos += k; if (noHash) { if (pos >= C.frames) done = true; return k; }
                advanceHashes(false);
                if (pos >= C.frames) { advanceHashes(true); ok = true; for (MD d : chain) { byte[] h = new byte[32]; d.sha.finish(h); d.shaOk = !d.wide && java.util.Arrays.equals(h, f.ms.get(d.idx).I.sha); ok &= d.shaOk; } done = true; }
                return k; }
            catch (RuntimeException e) { err = String.valueOf(e.getMessage()); return 0; }
            catch (OutOfMemoryError e) { err = "bad allocation"; return 0; } }
        // after the last sample: every master of the chain matched its SHA-256 (the core: always true)
        // port of laes_play.h:Player::verified
        public boolean verified() { return done && ok; }
        // after the last sample: master m (index in the file) matched its SHA-256 (1), did not (0), or is not part of this run (-1)
        // port of laes_play.h:Player::masterOk
        public int masterOk(int m) { if (!done) return -1; for (MD d : chain) if (d.idx == m) return d.shaOk ? 1 : 0; return -1; }

        // port of laes_play.h:Player::need  (the nodes of master m (and of its tiers, first))
        private MD need(int m) {
            for (MD d : chain) if (d.idx == m) return d;
            final Master M = f.ms.get(m); final LaesFile.MasterInfo I = M.I;
            if (I.storage != 1 && I.refTier > 0) need(I.refTier - 1);
            final MD D = new MD(); D.idx = m; final int j = M.j;
            for (int c = 0; c < 2; c++) {
                Node x = layer(D, M, c, j);
                if (I.storage != 1) { AddRefNode A = new AddRefNode(); A.e = x; A.n = x.n; A.threads = threads; A.sh = I.stages - j;
                    if (I.refTier == 0) { A.src = coreSig[c]; A.up = (I.bits - I.w) - f.coreBits; }
                    else { final LaesFile.MasterInfo T = f.ms.get(I.refTier - 1).I; A.src = tierLevel(I.refTier - 1, c, j); A.up = (I.bits - I.w) - (T.bits - T.w); }
                    A.wj = I.whole * (1L << j) + (j != 0 ? ((I.frac & 0xFFFFFFFFL) >>> (32 - j)) : 0); A.h = LaesMath.refTaps((int) ((I.frac & 0xFFFFFFFFL) << j)); x = A; D.own.add(A); }
                // levels j .. S are the steps of the reconstruction: level k of the decoded master (spec 6.6: the forward stages of its samples) equals
                // the value the inverse stages had at level k, since each lifting stage is exactly undone by its forward stage (same integer
                // operations, same symmetric extension); only levels below j need forward stages (levelOf)
                D.lev[c] = new Node[I.stages + 1]; D.lev[c][j] = x;
                for (int k = j + 1; k <= I.stages; k++) { Node d = layer(D, M, c, k); UpNode Un = new UpNode(); Un.s = x; Un.d = d; Un.n = LaesFile.levLen(I.frames, I.stages - k); Un.threads = threads; Un.sh = I.stages - k;
                    if (Un.n != x.n + d.n) fail("LAES octave layer length"); x = Un; D.own.add(Un); D.lev[c][k] = x; }
                D.top[c] = x; }
            chain.add(D); return D; }
        // port of laes_play.h:Player::need (lambda layer)
        private Node layer(MD D, Master M, int c, int k) { final int j = M.j; final LaesFile.MasterInfo I = M.I; LayerNode L = new LayerNode(); L.rd.src = f.payload; L.rd.off = M.layOff[c][k - j]; L.rd.len = M.layLen[c][k - j]; L.ix = f.layerIndex(M.layOff[c][k - j]);
            L.B = 4096L << (k == j ? j : k - 1); L.n = k == j ? LaesFile.levLen(I.frames, I.stages - j) : LaesFile.levLen(I.frames, I.stages - k) / 2; L.threads = threads; L.sh = I.stages - (k == j ? j : k - 1); D.own.add(L); return L; }
        // Java (2026-11-04, speed; no value changes): the levels below the top of a master that only the next stage reads, and whose inputs
        // are read by nothing else either (no tier level, no level a choice outputs, no forward stage), are computed ahead on a thread of their
        // own (AheadNode), so that the stages of one channel run at the same time (a pipeline) instead of one after the other on the reading
        // thread. Each node is still used by one thread at a time: the nodes below a wrapped level are reached only through it.
        private List<UpNode> pipePlan(Choice S) {
            final java.util.IdentityHashMap<Stream.Sig, Integer> refs = new java.util.IdentityHashMap<Stream.Sig, Integer>();
            for (MD d : chain) for (Node nd : d.own) {
                if (nd instanceof UpNode) { ref(refs, ((UpNode) nd).s, 1); ref(refs, ((UpNode) nd).d, 1); }
                else if (nd instanceof AddRefNode) { ref(refs, ((AddRefNode) nd).e, 1); ref(refs, ((AddRefNode) nd).src, 1); }
                else if (nd instanceof DownNode) { ref(refs, ((DownNode) nd).x, 1); } }
            for (MD d : chain) for (int c = 0; c < 2; c++) { ref(refs, d.top[c], 2); if (S.kind == 2 && d.idx == S.m) ref(refs, d.lev[c][S.level], 2); }   // read from outside
            final java.util.IdentityHashMap<Stream.Sig, Boolean> wrapped = new java.util.IdentityHashMap<Stream.Sig, Boolean>(); final List<UpNode> plan = new ArrayList<UpNode>();
            for (MD d : chain) { final LaesFile.MasterInfo I = f.ms.get(d.idx).I; final int j = f.ms.get(d.idx).j;
                for (int c = 0; c < 2; c++) for (int k = j + 1; k <= I.stages; k++) { final Node up = d.lev[c][k]; if (!(up instanceof UpNode)) continue; final UpNode U = (UpNode) up;
                    if (!(U.s instanceof Node) || U.s instanceof LayerNode || !pipeSafe(refs, wrapped, U.s)) continue;   // (a layer runs ahead by itself)
                    wrapped.put(U.s, Boolean.TRUE); plan.add(U); } }
            return plan; }
        private static void ref(java.util.IdentityHashMap<Stream.Sig, Integer> refs, Stream.Sig s, int k) { if (s == null) return; final Integer v = refs.get(s); refs.put(s, (v == null ? 0 : v) + k); }
        // x and every node below it are read by exactly one reader, and none of them reads a node shared with others (the core is safe: CoreStore
        // locks; a node already planned to run ahead (wrapped) has its own thread)
        private static boolean pipeSafe(java.util.IdentityHashMap<Stream.Sig, Integer> refs, java.util.IdentityHashMap<Stream.Sig, Boolean> wrapped, Stream.Sig x) {
            final Integer r = refs.get(x); if (r == null || r != 1) return false;
            if (wrapped.containsKey(x) || x instanceof LayerNode) return true;
            if (x instanceof AddRefNode) { final AddRefNode A = (AddRefNode) x; return A.src instanceof CoreNode && pipeSafe(refs, wrapped, A.e); }
            if (x instanceof UpNode) { final UpNode U = (UpNode) x; return pipeSafe(refs, wrapped, U.s) && pipeSafe(refs, wrapped, U.d); }
            return false; }
        // port of laes_play.h:Player::levelOf  (level k of a decoded master, from its samples by forward stages (spec 6.6))
        private Node levelOf(MD D, int c, int k) {
            final int S = f.ms.get(D.idx).I.stages; if (k >= S) return D.top[c];
            if (D.lev[c][k] == null) {   // only below the master's level j
                DownNode N = new DownNode(); N.x = levelOf(D, c, k + 1); N.n = (N.x.n + 1) / 2; N.threads = threads; N.sh = S - k; N.keep = D.top[c].keep; N.chunk = D.top[c].chunk;
                D.lev[c][k] = N; D.own.add(N); }
            return D.lev[c][k]; }
        // port of laes_play.h:Player::tierLevel
        private Stream.Sig tierLevel(int tier, int c, int k) { MD T = need(tier); return levelOf(T, c, k); }
        // port of laes_play.h:Player::advanceHashes
        // C++: every master of the chain hashed up to the same share of its length as the output. Java (2026-11-04): up to the same time
        // position as the output (each master's own time origin, see tabs), so that a tier is read by its own hashing and by the masters that
        // refer to it at nearly the same place; at the end (fin) the time runs on in steps until every master is hashed in full. Only the
        // moments at which the bytes are hashed change: each master's SHA-256 takes its samples in order, so the results are the same.
        private void advanceHashes(boolean fin) {
            final Choice S = C.kind == 3 ? f.choices.get(C.from) : C; final double base = f.baseRate;
            final double t = (S.kind == 0 ? 0.0 : tabs[S.m]) + (double) pos * base / (double) C.rate;
            double tEnd = t, rmax = base; for (MD d : chain) { final LaesFile.MasterInfo I = f.ms.get(d.idx).I; if (fin) tEnd = Math.max(tEnd, tabs[d.idx] + (double) I.frames * base / I.rate); rmax = Math.max(rmax, I.rate); }
            // in time steps of at most `unit` values at the highest rate, every master of the chain in turn (tiers first), so that no master
            // runs ahead of the others by more than a node keeps
            final double step = (double) unit * base / rmax; if (Double.isNaN(tHashed)) { tHashed = t; for (MD d : chain) tHashed = Math.min(tHashed, tabs[d.idx]); }   // from the earliest start
            for (double tt = tHashed + step; tt < tEnd; tt += step) for (MD d : chain) hashTo(d, wantAt(d, tt));
            for (MD d : chain) hashTo(d, fin ? f.ms.get(d.idx).I.frames : wantAt(d, t)); tHashed = Math.max(tHashed, t); }
        private long wantAt(MD d, double t) { final LaesFile.MasterInfo I = f.ms.get(d.idx).I; final double v = Math.floor((t - tabs[d.idx]) * (double) I.rate / (double) f.baseRate);
            return v <= 0 ? 0 : v >= (double) I.frames ? I.frames : (long) v; }
        private void hashTo(final MD d, long want) { final LaesFile.MasterInfo I = f.ms.get(d.idx).I;
            while (d.hashed < want) { final long h0 = d.hashed, e = Math.min(want, d.hashed + unit); final int na = (int) (e - h0); if (ha.length < na) { ha = new long[na]; hb = new long[na]; } final long[] a = ha, b = hb;   // Java: reused arrays
                bothChannels(new Chan() { public void run(int c) { d.top[c].get(h0, e, c != 0 ? b : a, 0); } });
                for (int i = 0; i < na; i++) if (!LaesFile.inWord(a[i] << I.w, I.bits) || !LaesFile.inWord(b[i] << I.w, I.bits)) d.wide = true;   // FU12-7 / FU13-4
                final byte[] buf = d.sha.take(na * 8); for (int i = 0; i < na; i++) { final int l = (int) (a[i] << I.w), r = (int) (b[i] << I.w);
                    for (int q = 0; q < 4; q++) { buf[i * 8 + q] = (byte) (l >>> (8 * q)); buf[i * 8 + 4 + q] = (byte) (r >>> (8 * q)); } }
                d.sha.push(buf, na * 8, true); d.hashed = e; } }
    }
}
