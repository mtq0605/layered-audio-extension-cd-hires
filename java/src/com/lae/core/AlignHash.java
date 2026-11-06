// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/**
 * Port of src/align_hash.h (namespace alignhash): MD5 over a CD layer as FLAC defines its audio MD5 (interleaved little-endian 16-bit
 * samples), and the digital-silence edges. The C++ implements RFC 1321 itself; java.security's "MD5" gives the same digest
 * (md5SelfTest checks the RFC 1321 test suite, as the C++ does).
 */
public final class AlignHash {
    private AlignHash() {}

    /** port of alignhash::Md5 (update / hex); hex is lower-case, as the C++ */
    public static final class Md5 {
        private final MessageDigest md;
        public Md5() {
            try { md = MessageDigest.getInstance("MD5"); } catch (NoSuchAlgorithmException e) { throw new LaeException("MD5 not available", e); }
        }
        public void update(byte[] b, int off, int len) { md.update(b, off, len); }
        /** finalises; call once */
        public String hex() { return toHex(md.digest()); }
    }

    static String toHex(byte[] d) {
        final char[] hx = "0123456789abcdef".toCharArray();
        StringBuilder s = new StringBuilder(d.length * 2);
        for (byte b : d) { s.append(hx[(b >> 4) & 15]); s.append(hx[b & 15]); }
        return s.toString();
    }

    /** port of alignhash::cdMd5: MD5 of a CD layer given as interleaved stereo int16 (frames x 2 values in s) */
    public static String cdMd5(short[] s, long frames) {
        Md5 m = new Md5();
        byte[] buf = new byte[16384];
        int fill = 0;
        for (long i = 0; i < frames * 2; i++) {
            short v = s[(int) i];
            buf[fill++] = (byte) v; buf[fill++] = (byte) (v >> 8);
            if (fill == buf.length) { m.update(buf, 0, fill); fill = 0; }
        }
        m.update(buf, 0, fill);
        return m.hex();
    }

    /** port of alignhash::md5SelfTest: the 7 values of the RFC 1321 test suite */
    public static boolean md5SelfTest() {
        String[][] t = {{"", "d41d8cd98f00b204e9800998ecf8427e"}, {"a", "0cc175b9c0f1b6a831c399e269772661"}, {"abc", "900150983cd24fb0d6963f7d28e17f72"},
            {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"}, {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
            {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
            {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"}};
        for (String[] p : t) {
            Md5 m = new Md5();
            byte[] b;
            try { b = p[0].getBytes("US-ASCII"); } catch (java.io.UnsupportedEncodingException e) { return false; }
            m.update(b, 0, b.length);
            if (!m.hex().equals(p[1])) return false;
        }
        return true;
    }

    /** sample access for silenceEdges / allZero (the C++ template parameter S: interleaved samples, operator[]) */
    public interface Samples { long get(long i); }

    /** port of alignhash::silenceEdges: frames at the start / end whose two samples are exactly zero; returns {lead, tail} */
    public static long[] silenceEdges(Samples s, long frames) {
        long lead = 0;
        while (lead < frames && s.get(lead * 2) == 0 && s.get(lead * 2 + 1) == 0) lead++;
        long tail = 0;
        while (tail < frames - lead && s.get((frames - 1 - tail) * 2) == 0 && s.get((frames - 1 - tail) * 2 + 1) == 0) tail++;
        return new long[] {lead, tail};
    }

    /** port of alignhash::allZero over frames [a, b) */
    public static boolean allZero(Samples s, long a, long b) {
        for (long i = a; i < b; i++) if (s.get(i * 2) != 0 || s.get(i * 2 + 1) != 0) return false;
        return true;
    }
}
