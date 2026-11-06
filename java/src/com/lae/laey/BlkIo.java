// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Port of src/blk_io.h (namespace blkio). Block-wise encoder (2026-10-29): checked random-access reads and streamed writes of raw files,
 * and the index range a mirrored filter window touches. Same checks as enc_io.h: a failed read or write throws encio::Fail; a failed
 * write removes the file; LAE_TESTFAIL applies.
 * C++ `long` (32-bit on the reference platform) frame counts are held in Java long; the C++ check that a file's frame count fits that
 * type is kept.
 */
final class BlkIo {
    private BlkIo() {}

    /** port of blk_io.h:Reader&lt;int32_t, C&gt;: a raw file of interleaved frames of C int32 values, read by frame ranges */
    static final class ReaderI implements java.io.Closeable {
        RandomAccessFile f; final String path; final int C; final long frames; private byte[] buf = new byte[0];
        // port of blk_io.h:Reader::Reader
        ReaderI(String p, int c) { path = p; C = c; f = EncIo.open(p, "rb"); long n = EncIo.size(f);
            if (n < 0 || n % 4 != 0) { EncIo.close(f); f = null; throw EncIo.fail("cannot read the size of " + p); }
            long fr = n / 4 / C; if (fr > Integer.MAX_VALUE) { EncIo.close(f); f = null; throw EncIo.fail("file too large for this encoder version: " + path); } frames = fr; }
        // port of blk_io.h:Reader::read: frames [a, b) (0 <= a <= b <= frames) as interleaved values (one reader may serve both channel threads)
        synchronized int[] read(long a, long b) { int[] out = new int[(int) ((b - a) * C)]; if (b <= a) return out;
            if (a < 0 || b > frames) throw EncIo.fail("seek failed: " + path);
            int nb = out.length * 4; if (buf.length < nb) buf = new byte[nb];
            try { f.seek(a * C * 4); f.readFully(buf, 0, nb); } catch (IOException e) { throw EncIo.fail("read failed: " + path); }
            EncIo.toInts(buf, 0, out, 0, out.length); return out; }
        @Override public void close() { EncIo.close(f); f = null; } }

    /** port of blk_io.h:Reader&lt;int16_t, 2&gt; */
    static final class ReaderS implements java.io.Closeable {
        RandomAccessFile f; final String path; final long frames; private byte[] buf = new byte[0];
        // port of blk_io.h:Reader::Reader
        ReaderS(String p) { path = p; f = EncIo.open(p, "rb"); long n = EncIo.size(f);
            if (n < 0 || n % 2 != 0) { EncIo.close(f); f = null; throw EncIo.fail("cannot read the size of " + p); }
            long fr = n / 2 / 2; if (fr > Integer.MAX_VALUE) { EncIo.close(f); f = null; throw EncIo.fail("file too large for this encoder version: " + path); } frames = fr; }
        // port of blk_io.h:Reader::read
        synchronized short[] read(long a, long b) { short[] out = new short[(int) ((b - a) * 2)]; if (b <= a) return out;
            if (a < 0 || b > frames) throw EncIo.fail("seek failed: " + path);
            int nb = out.length * 2; if (buf.length < nb) buf = new byte[nb];
            try { f.seek(a * 4); f.readFully(buf, 0, nb); } catch (IOException e) { throw EncIo.fail("read failed: " + path); }
            EncIo.toShorts(buf, 0, out, 0, out.length); return out; }
        @Override public void close() { EncIo.close(f); f = null; } }

    /**
     * port of blk_io.h:Writer: a file written in pieces, in order; the checks of encio::writeAll on every piece and at the end.
     * C++ destructor: "not finished: an error elsewhere; the half-written file goes" = {@link #close()} before {@link #finish()}.
     */
    static final class Writer implements java.io.Closeable {
        BufferedOutputStream f; final String path; boolean ok = true; private byte[] buf = new byte[0];
        // port of blk_io.h:Writer::Writer
        Writer(EncEnv env, String p) { path = p; try { f = new BufferedOutputStream(new FileOutputStream(p), 1 << 16); } catch (IOException e) { throw EncIo.fail("cannot open " + p); } ok = !EncIo.testFail(env, p); }
        // port of blk_io.h:Writer::put
        void put(int[] d, int off, int n) { if (ok && n > 0) { if (buf.length < n * 4) buf = new byte[n * 4]; EncIo.toBytes(d, off, n, buf); ok = write(buf, n * 4); } if (!ok) finish(); }
        void put(int[] d) { put(d, 0, d.length); }
        void put(short[] d, int off, int n) { if (ok && n > 0) { if (buf.length < n * 2) buf = new byte[n * 2]; EncIo.toBytes(d, off, n, buf); ok = write(buf, n * 2); } if (!ok) finish(); }
        void put(short[] d) { put(d, 0, d.length); }
        void putBytes(byte[] d, int off, int n) { if (ok && n > 0) { try { f.write(d, off, n); } catch (IOException e) { ok = false; } } if (!ok) finish(); }
        private boolean write(byte[] b, int n) { try { f.write(b, 0, n); return true; } catch (IOException e) { return false; } }
        // port of blk_io.h:Writer::finish
        void finish() { if (f == null) return; try { f.close(); } catch (IOException e) { ok = false; } f = null;
            if (!ok) { new File(path).delete(); throw EncIo.fail("write failed (disk full?): " + path); } }
        // port of blk_io.h:Writer::~Writer
        @Override public void close() { if (f != null) { try { f.close(); } catch (IOException e) { /* removed below */ } f = null; new File(path).delete(); } } }

    // port of blk_io.h:cover
    // smallest index range [lo, hi] that j <- mirror(j) maps [a, b] into for an array of n (j < 0: -j; j >= n: 2 (n - 1) - j), clamped to [0, n - 1]
    static long[] cover(long a, long b, long n) { long[] r = {n, -1};
        if (a < 0) { long e = Math.min(b, -1L); add(r, -e, n); add(r, -a, n); }
        if (b >= 0 && a <= n - 1) { add(r, Math.max(a, 0L), n); add(r, Math.min(b, n - 1), n); }
        if (b >= n) { long s = Math.max(a, n); add(r, 2 * (n - 1) - b, n); add(r, 2 * (n - 1) - s, n); }
        return r; }
    // port of blk_io.h:cover lambda add
    private static void add(long[] r, long x, long n) { x = Math.min(Math.max(x, 0L), n - 1); r[0] = Math.min(r[0], x); r[1] = Math.max(r[1], x); }

    /** port of blk_io.h:Win&lt;int32_t&gt;: values of one array over the index window [base, base + d.size()) */
    static final class WinI { long base = 0; int[] d = new int[0];
        // port of blk_io.h:Win::at
        int at(long j) { long k = j - base; if (k < 0 || k >= d.length) throw EncIo.fail("block-wise encoder: window index outside the block (internal error)"); return d[(int) k]; } }
    /** port of blk_io.h:Win&lt;double&gt; */
    static final class WinD { long base = 0; double[] d = new double[0];
        // port of blk_io.h:Win::at
        double at(long j) { long k = j - base; if (k < 0 || k >= d.length) throw EncIo.fail("block-wise encoder: window index outside the block (internal error)"); return d[(int) k]; } }

    /**
     * port of blk_io.h:View: read-only view of a raw interleaved stereo int32 file (a master) as if it were the vector of its samples: s[i]
     * is sample i (frame i / 2, channel i % 2); a window of frames around the last access is held in memory.
     */
    static final class View implements java.io.Closeable {
        final String path; long frames = 0; RandomAccessFile f; long wb = 0; int[] w = new int[0]; byte[] buf = new byte[0];
        static final long W = 1L << 20;   // frames per window (8 MB)
        // port of blk_io.h:View::View
        View(String p) { path = p; f = EncIo.open(p, "rb"); long n = EncIo.size(f);
            if (n < 0 || n % 8 != 0) { EncIo.close(f); f = null; throw EncIo.fail("cannot read the size of " + p); } frames = n / 8; }
        // port of blk_io.h:View::size
        long size() { return frames * 2; }
        // port of blk_io.h:View::operator[]
        int get(long i) { long fr = i / 2;
            if (fr < wb || fr >= wb + w.length / 2) { if (fr >= frames) throw EncIo.fail("internal error: master sample beyond its end: " + path);
                wb = Math.max(0L, fr - W / 2); long e = Math.min(frames, wb + W); int n = (int) (e - wb) * 2; if (w.length != n) w = new int[n]; if (buf.length < n * 4) buf = new byte[n * 4];
                try { f.seek(wb * 8); f.readFully(buf, 0, n * 4); } catch (IOException x) { throw EncIo.fail("read failed: " + path); }
                EncIo.toInts(buf, 0, w, 0, n); }
            return w[(int) (i - wb * 2)]; }
        @Override public void close() { EncIo.close(f); f = null; } }
}
