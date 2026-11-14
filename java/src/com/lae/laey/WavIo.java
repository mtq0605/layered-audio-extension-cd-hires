// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Port of src/wavio.h (namespace wavio), the parts the encoder uses: the WAV header, the 4 GB check and readCD (the encoder's own
 * decode of the written file). WAV reader/writer (RIFF/WAVE, PCM and WAVE_FORMAT_EXTENSIBLE integer PCM).
 */
final class WavIo {
    private WavIo() {}

    /** port of wavio.h:Wav (the fields readCD fills) */
    static final class Wav { int rate = 0, ch = 0, bits = 0; short[] pcm16 = new short[0]; byte[] lae = new byte[0]; long dataOff = -1, dataValues = 0; }   // dataOff / dataValues: Java only, where the 16-bit data lies (readCD with loadPcm = false)

    // port of wavio.h:fits32: RIFF sizes are 32-bit
    static boolean fits32(long dataBytes, long extra) { return 4 + 24 + 8 + dataBytes + 1 + extra <= 0xFFFFFFFFL; }

    // port of wavio.h:hdr (dataBytes, extra: size_t; the RIFF fields are their low 32 bits, as the C++ uint32_t casts)
    static byte[] hdr(int rate, int ch, int bits, long dataBytes, long extra) {
        byte[] o = new byte[44]; int[] p = {0};
        put4(o, p, "RIFF"); p32(o, p, (int) (4 + 24 + 8 + dataBytes + (dataBytes & 1) + extra)); put4(o, p, "WAVE");
        put4(o, p, "fmt "); p32(o, p, 16); p16(o, p, 1); p16(o, p, ch); p32(o, p, rate); p32(o, p, rate * ch * bits / 8); p16(o, p, ch * bits / 8); p16(o, p, bits);
        put4(o, p, "data"); p32(o, p, (int) dataBytes); return o; }
    private static void put4(byte[] o, int[] p, String s) { for (int i = 0; i < 4; i++) o[p[0]++] = (byte) s.charAt(i); }
    private static void p32(byte[] o, int[] p, int v) { for (int i = 0; i < 4; i++) o[p[0]++] = (byte) (v >>> (8 * i)); }
    private static void p16(byte[] o, int[] p, int v) { o[p[0]++] = (byte) v; o[p[0]++] = (byte) (v >> 8); }

    // port of wavio.h:readCD
    // a 16-bit file read for decoding (2026-10-29): pcm16, lae, rate, ch and bits. Other sample sizes: the C++ falls back to read(); the
    // encoder then refuses the file ("not a 16-bit stereo WAV"), so only rate, ch and bits are returned for them.
    static boolean readCD(String path, Wav w) { return readCD(path, w, true); }
    // (Java only: loadPcm = false leaves pcm16 empty and gives the data chunk's place, for a decoder that reads it on demand)
    static boolean readCD(String path, Wav w, boolean loadPcm) {
        RandomAccessFile f; try { f = new RandomAccessFile(path, "r"); } catch (IOException e) { return false; }
        try { long sz = EncIo.size(f);
            byte[] h = new byte[12]; if (sz < 12 || !at(f, sz, 0, h, 12) || !eq(h, 0, "RIFF") || !eq(h, 8, "WAVE")) return false;
            long p = 12, dpos = -1; long dlen = 0; int fmt = 0;
            while (p + 8 <= sz) { byte[] c = new byte[8]; if (!at(f, sz, p, c, 8)) break; long n = WavIn.le32(c, 4); final long body = p + 8;
                if (n > sz - body) n = sz - body;
                if (eq(c, 0, "fmt ")) { byte[] b = new byte[26]; if (n < 16 || !at(f, sz, body, b, n >= 26 ? 26 : 16)) return false;
                    fmt = (b[0] & 0xFF) | (b[1] & 0xFF) << 8; w.ch = (b[2] & 0xFF) | (b[3] & 0xFF) << 8; w.rate = (int) WavIn.le32(b, 4); w.bits = (b[14] & 0xFF) | (b[15] & 0xFF) << 8; if (fmt == 0xFFFE && n >= 26) fmt = (b[24] & 0xFF) | (b[25] & 0xFF) << 8; }
                else if (eq(c, 0, "data")) { dpos = body; dlen = n; }
                else if (eq(c, 0, "LAE ")) { if (n > Integer.MAX_VALUE - 8) throw tooLarge(path); w.lae = new byte[(int) n]; if (n != 0 && !at(f, sz, body, w.lae, (int) n)) return false; }
                p = body + n + (n & 1); }
            if (w.bits != 16 || fmt != 1 || dpos < 0) return fmt == 1 && dpos >= 0 && (w.bits == 24 || w.bits == 32);   // read(): the encoder refuses it afterwards
            w.dataOff = dpos; w.dataValues = dlen / 2; if (!loadPcm) return true;
            if (dlen / 2 * 2 > Integer.MAX_VALUE - 8) throw tooLarge(path);
            byte[] d = new byte[(int) (dlen / 2 * 2)]; boolean ok = d.length == 0 || at(f, sz, dpos, d, d.length);
            w.pcm16 = new short[d.length / 2]; EncIo.toShorts(d, 0, w.pcm16, 0, w.pcm16.length);
            return ok; }
        finally { EncIo.close(f); } }
    // FU21-6 (Java only): a body of 2 GiB or more does not fit a Java array (the C++ reads it); said instead of a NegativeArraySizeException
    private static RuntimeException tooLarge(String path) { return EncIo.fail(path + ": the file is too large for the Java port (a chunk of 2 GiB or more)"); }
    private static boolean at(RandomAccessFile f, long sz, long p, byte[] d, int n) {
        if (!(p >= 0 && p + n <= sz)) return false; try { f.seek(p); f.readFully(d, 0, n); return true; } catch (IOException e) { return false; } }
    private static boolean eq(byte[] b, int o, String s) { for (int i = 0; i < s.length(); i++) if (b[o + i] != (byte) s.charAt(i)) return false; return true; }
}
