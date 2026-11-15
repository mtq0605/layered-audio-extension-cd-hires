// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.LaeException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Port of src/enc_io.h (namespace encio): checked file I/O for the encoder modules: 64-bit file sizes, every read and write verified.
 * A failure throws encio::Fail (here {@link LaeException}); the encoder driver catches it, writes nothing and removes its temporary folder.
 * Test hook: LAE_TESTFAIL=&lt;text&gt; makes every write whose path contains &lt;text&gt; fail as if the disk were full.
 * The raw files hold the values in little-endian order, as the C++ writes them on every supported (little-endian) platform.
 */
final class EncIo {
    private EncIo() {}

    // port of enc_io.h:fail
    static LaeException fail(String m) { throw new LaeException(m); }
    // port of enc_io.h:testFail
    static boolean testFail(EncEnv env, String what) { String t = env.get("LAE_TESTFAIL"); return t != null && !t.isEmpty() && what != null && what.contains(t); }
    // port of enc_io.h:size (of an open file)
    static long size(RandomAccessFile f) { try { return f.length(); } catch (IOException e) { return -1; } }
    // port of enc_io.h:open
    static RandomAccessFile open(String p, String mode) {
        if (p == null) throw fail("cannot open (no path)");   // was a NullPointerException
        try { if (mode.startsWith("w")) { new FileOutputStream(p).close(); }   // "wb" / "w+b": created empty
              return new RandomAccessFile(p, mode.startsWith("r") && !mode.contains("+") ? "r" : "rw"); }
        catch (IOException e) { throw fail("cannot open " + p); } }
    static void close(RandomAccessFile f) { if (f != null) try { f.close(); } catch (IOException e) { /* the C++ fclose result of a reader is not checked */ } }
    // port of enc_io.h:readAll<uint8_t> (whole file as bytes)
    static byte[] readAll(String p) {
        RandomAccessFile f = open(p, "rb");
        try { long n = size(f); if (n < 0) { throw fail("cannot read the size of " + p); }
              if (n > Integer.MAX_VALUE - 8) throw fail(p + ": the file is too large for the Java port (a work file of 2 GiB or more is read whole)");   // said as such (the C++ has no such limit)
              byte[] v = new byte[(int) n]; try { f.readFully(v); } catch (IOException e) { throw fail("read failed: " + p); } return v; }
        finally { close(f); } }
    // port of enc_io.h:readAll<int32_t> / <int16_t> / <char> (size must be a multiple of the element size)
    static int[] readAllI32(String p) { byte[] b = readAll(p); if (b.length % 4 != 0) throw fail("cannot read the size of " + p); return toInts(b, 0, b.length / 4); }
    // port of enc_io.h:writeAll
    static void writeAll(EncEnv env, String p, byte[] d, int off, int n) {
        FileOutputStream f; try { f = new FileOutputStream(p); } catch (IOException e) { throw fail("cannot open " + p); }
        boolean ok = !testFail(env, p); if (ok && n > 0) try { f.write(d, off, n); } catch (IOException e) { ok = false; }
        try { f.close(); } catch (IOException e) { ok = false; }
        if (!ok) { new File(p).delete(); throw fail("write failed (disk full?): " + p); } }
    static void writeVec(EncEnv env, String p, byte[] d) { writeAll(env, p, d, 0, d.length); }
    static void writeVec(EncEnv env, String p, int[] v) { byte[] b = toBytes(v, 0, v.length); writeAll(env, p, b, 0, b.length); }
    // port of enc_io.h:writeText (UTF-8, as the C++ writes the bytes of its strings; the work-folder notes are read only by this encoder)
    static void writeText(EncEnv env, String p, String s) { byte[] b = s.getBytes(java.nio.charset.StandardCharsets.UTF_8); writeAll(env, p, b, 0, b.length); }
    // port of enc_io.h:readText (UTF-8, the counterpart of writeText)
    static String readText(String p) { return new String(readAll(p), java.nio.charset.StandardCharsets.UTF_8); }
    // port of enc_io.h:count (element count as the modules' index type: C++ long, 32-bit on the reference platform)
    static long count(long n) { if (n > Integer.MAX_VALUE) throw fail("file too large for this encoder version"); return n; }

    static byte[] latin1(String s) { byte[] b = new byte[s.length()]; for (int i = 0; i < b.length; i++) b[i] = (byte) s.charAt(i); return b; }
    static int[] toInts(byte[] b, int off, int n) { int[] v = new int[n]; ByteBuffer.wrap(b, off, n * 4).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(v); return v; }
    static void toInts(byte[] b, int off, int[] v, int vo, int n) { ByteBuffer.wrap(b, off, n * 4).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(v, vo, n); }
    static byte[] toBytes(int[] v, int off, int n) { byte[] b = new byte[n * 4]; ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().put(v, off, n); return b; }
    static void toBytes(int[] v, int off, int n, byte[] b) { ByteBuffer.wrap(b, 0, n * 4).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().put(v, off, n); }
    static void toShorts(byte[] b, int off, short[] v, int vo, int n) { ByteBuffer.wrap(b, off, n * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(v, vo, n); }
    static void toBytes(short[] v, int off, int n, byte[] b) { ByteBuffer.wrap(b, 0, n * 2).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(v, off, n); }
    static void seek(RandomAccessFile f, long at, String path) { try { f.seek(at); } catch (IOException e) { throw fail("seek failed: " + path); } }
}
