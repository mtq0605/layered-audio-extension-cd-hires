// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * Port of src/wav_in.h (namespace wavin). Block-wise encoder (2026-10-29): the master read piece by piece instead of whole. RIFF/WAVE
 * (as wavio::read), RF64 (ds64 sizes) and Sony Wave64; integer PCM 16 / 24 / 32 bit (PCM or WAVE_FORMAT_EXTENSIBLE). The samples become
 * the raw stereo int32 file every module reads (24-bit value left-aligned, 32-bit input: the top 24 bits), exactly the values wavio::read
 * gives. While reading, the MD5 of the data bytes is computed once (RM-1) and, for 32-bit input, the samples with a non-zero lowest byte
 * are counted.
 */
final class WavIn {
    private WavIn() {}

    /** port of wav_in.h:Info */
    static final class Info { int rate = 0, ch = 0, bits = 0, fmt = 0; long dataOff = -1, dataBytes = 0; String kind = "";
        boolean badLen = false;     // the header gives a data length of 2^63 bytes or more (RF64 / W64): no file has that; probe() refuses it
        long hdrLen = 0; }   // not 0: the header gives this data length (below 2^63), more than the file holds; dataBytes = what is there, and the caller says so (2026-11-14)

    // port of wav_in.h:le32 (uint32 as a non-negative long)
    static long le32(byte[] b, int o) { return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 | (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24; }
    // port of wav_in.h:le64 (uint64)
    static long le64(byte[] b, int o) { return le32(b, o) | le32(b, o + 4) << 32; }
    private static boolean eq(byte[] b, int o, String s) { for (int i = 0; i < s.length(); i++) if (b[o + i] != (byte) s.charAt(i)) return false; return true; }
    private static boolean eq(byte[] b, int o, int[] s) { for (int i = 0; i < s.length; i++) if ((b[o + i] & 0xFF) != s[i]) return false; return true; }

    // port of wav_in.h:probe lambda at
    private static boolean at(RandomAccessFile f, long sz, long p, byte[] d, int n) {
        if (!(p >= 0 && p + n <= sz)) return false; try { f.seek(p); f.readFully(d, 0, n); return true; } catch (IOException e) { return false; } }
    // port of wav_in.h:probe lambda fmtChunk (n: uint64)
    private static boolean fmtChunk(RandomAccessFile f, long sz, long body, long n, Info in) { byte[] b = new byte[26];
        if (U64.lt(n, 16) || !at(f, sz, body, b, U64.ge(n, 26) ? 26 : 16)) return false;
        in.fmt = (b[0] & 0xFF) | (b[1] & 0xFF) << 8; in.ch = (b[2] & 0xFF) | (b[3] & 0xFF) << 8; in.rate = (int) le32(b, 4); in.bits = (b[14] & 0xFF) | (b[15] & 0xFF) << 8;
        if (in.fmt == 0xFFFE && U64.ge(n, 26)) in.fmt = (b[24] & 0xFF) | (b[25] & 0xFF) << 8; return true; }

    private static final int[] RIFF64 = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
    private static final int[] TAIL64 = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

    // port of wav_in.h:probe
    // reads the header chunks; false if the file is not a WAV / RF64 / W64 with a fmt and a data chunk
    static boolean probe(String path, Info in) {
        RandomAccessFile f; try { f = new RandomAccessFile(path, "r"); } catch (IOException e) { return false; }
        try { final long sz = EncIo.size(f); boolean ok = false; byte[] h = new byte[40];
            if (sz >= 12 && at(f, sz, 0, h, 12) && (eq(h, 0, "RIFF") || eq(h, 0, "RF64")) && eq(h, 8, "WAVE")) {
                final boolean rf64 = eq(h, 0, "RF64"); in.kind = rf64 ? "RF64" : "WAV"; long ds64Data = 0; boolean haveFmt = false; long p = 12;
                while (p + 8 <= sz) { byte[] c = new byte[8]; if (!at(f, sz, p, c, 8)) break; long n = le32(c, 4); final long body = p + 8;
                    if (rf64 && eq(c, 0, "ds64")) { byte[] d = new byte[24]; if (n >= 24 && at(f, sz, body, d, 24)) ds64Data = le64(d, 8); }
                    if (rf64 && eq(c, 0, "data") && n == 0xFFFFFFFFL) n = ds64Data;
                    if (eq(c, 0, "data") && n < 0) { in.badLen = true; break; }   // 2^63 or more: as a signed length it would be negative (2026-11-14)
                    if (com.lae.core.U.gt(n, sz - body)) { if (eq(c, 0, "data")) in.hdrLen = n; n = sz - body; }   // a truncated last chunk is tolerated (as wavio::read); compared unsigned, so p only moves forward
                    if (eq(c, 0, "fmt ")) { if (!fmtChunk(f, sz, body, n, in)) break; haveFmt = true; }
                    else if (eq(c, 0, "data")) { in.dataOff = body; in.dataBytes = n; }
                    p = body + n + (n & 1); }
                ok = haveFmt && in.dataOff >= 0 && !in.badLen; }
            else if (sz >= 40 && at(f, sz, 0, h, 40)) {   // Wave64: GUID chunks, 64-bit sizes that include the 24-byte chunk header, 8-byte alignment
                if (eq(h, 0, RIFF64) && eq(h, 24, "wave") && eq(h, 28, TAIL64)) { in.kind = "W64"; boolean haveFmt = false; long p = 40;
                    while (p + 24 <= sz) { byte[] c = new byte[24]; if (!at(f, sz, p, c, 24)) break; long n = le64(c, 16); if (U64.lt(n, 24)) break; final long body = p + 24; long len = n - 24;
                        if (eq(c, 4, TAIL64) && eq(c, 0, "data") && len < 0) { in.badLen = true; break; }
                        if (com.lae.core.U.gt(len, sz - body)) { if (eq(c, 4, TAIL64) && eq(c, 0, "data")) in.hdrLen = len; len = sz - body; }
                        if (eq(c, 4, TAIL64)) { if (eq(c, 0, "fmt ")) { if (!fmtChunk(f, sz, body, len, in)) break; haveFmt = true; } else if (eq(c, 0, "data")) { in.dataOff = body; in.dataBytes = len; } }
                        p = body + ((len + 7) & ~7L); }
                    ok = haveFmt && in.dataOff >= 0 && !in.badLen; } }
            return ok && in.fmt == 1 && (in.bits == 16 || in.bits == 24 || in.bits == 32); }
        finally { EncIo.close(f); } }

    /** unsigned 64-bit comparisons (the C++ uint64_t chunk sizes) */
    static final class U64 { static boolean lt(long a, long b) { return com.lae.core.U.lt(a, b); } static boolean ge(long a, long b) { return com.lae.core.U.ge(a, b); } }

    // port of wav_in.h:toRaw
    // writes the raw stereo int32 file; frames = whole stereo frames; low8 = 32-bit samples with a non-zero lowest byte; md5 of the data bytes
    // (out[0] = frames, out[1] = low8; the MD5 text is returned)
    static String toRaw(EncEnv env, String path, Info in, String out, long[] res) {
        if (in.dataBytes < 0 || in.dataOff < 0) throw EncIo.fail("internal error: data length of " + path);   // never sized from a negative length
        RandomAccessFile f = EncIo.open(path, "rb"); BlkIo.Writer w = null;
        try { final int bps = in.bits / 8; final long n = in.dataBytes / bps; res[0] = n / 2; res[1] = 0; MessageDigest m = md5();
            w = new BlkIo.Writer(env, out); byte[] b = new byte[0]; int[] s = new int[0]; final long BS = 1L << 18;   // frames per piece
            EncIo.seek(f, in.dataOff, path);
            for (long a = 0; a < n; a += 2 * BS) { env.checkCancel(); final long cnt = Math.min(2 * BS, n - a); int nb = (int) (cnt * bps); if (b.length != nb) b = new byte[nb];
                try { f.readFully(b, 0, nb); } catch (IOException e) { throw EncIo.fail("read failed: " + path); }
                m.update(b, 0, nb); if (s.length != (int) cnt) s = new int[(int) cnt];
                for (int i = 0; i < cnt; i++) { int q = i * bps; int v;
                    if (bps == 2) v = (short) ((b[q] & 0xFF) | (b[q + 1] & 0xFF) << 8) << 16;
                    else if (bps == 3) v = (b[q] & 0xFF) << 8 | (b[q + 1] & 0xFF) << 16 | (b[q + 2] & 0xFF) << 24;
                    else { if (b[q] != 0) res[1]++; v = ((b[q] & 0xFF) | (b[q + 1] & 0xFF) << 8 | (b[q + 2] & 0xFF) << 16 | (b[q + 3] & 0xFF) << 24) & ~0xFF; }
                    s[i] = v; }
                final long whole = (a + cnt) / 2 * 2 - a; w.put(s, 0, (int) Math.max(0L, whole)); }   // an odd last sample is not a frame
            if (in.dataBytes % bps != 0) { int k = (int) (in.dataBytes % bps); byte[] r = new byte[4]; try { f.readFully(r, 0, k); m.update(r, 0, k); } catch (IOException e) { /* as the C++: the MD5 then omits them */ } }
            w.finish(); w = null;
            return hex(m.digest()) + ":" + in.rate + ":" + in.bits; }
        finally { if (w != null) w.close(); EncIo.close(f); } }

    // port of wav_in.h:low8
    // samples of a 32-bit file whose lowest byte is not zero (what reducing it to 24 bit drops), read piece by piece; -1 if unreadable
    static long low8(String path, Info in) {
        RandomAccessFile f; try { f = new RandomAccessFile(path, "r"); } catch (IOException e) { return -1; }
        try { long cnt = 0; byte[] b = new byte[1 << 20]; long left = in.dataBytes / 4 * 4;
            try { f.seek(in.dataOff); } catch (IOException e) { return -1; }
            while (left > 0) { final int k = (int) Math.min(left, (long) b.length); try { f.readFully(b, 0, k); } catch (IOException e) { return -1; }
                for (int i = 0; i < k; i += 4) if (b[i] != 0) cnt++; left -= k; }
            return cnt; }
        finally { EncIo.close(f); } }

    static MessageDigest md5() { try { return MessageDigest.getInstance("MD5"); } catch (NoSuchAlgorithmException e) { throw new com.lae.core.LaeException("MD5 not available", e); } }
    static String hex(byte[] d) { StringBuilder s = new StringBuilder(); for (byte x : d) { s.append(Character.forDigit((x >> 4) & 15, 16)); s.append(Character.forDigit(x & 15, 16)); } return s.toString(); }
}
