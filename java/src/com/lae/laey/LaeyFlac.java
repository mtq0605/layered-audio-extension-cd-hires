// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.AlignHash;
import com.lae.laes.Flac;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;

/**
 * The LAEY FLAC form (2026-11-10): port of src/lae_flacmeta.h (the structure, no decoding) and src/laey_flac.h (the CD layer from the
 * FLAC audio frames, its MD5).
 *   "fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (optional) | APPLICATION blocks with ID "LAE1" (k &gt;= 1) | audio frames = the CD layer
 * The extension is the body of the WAV form's "LAE " chunk, byte for byte, split into pieces of at most 2^24 - 5 bytes; a reader joins
 * the data of every "LAE1" block in file order. The pair (CD layer, extension) is then decoded exactly as the WAV form's.
 * Java: the frames are decoded in order (the C++ decodes long streams in parts; the result is the same).
 */
public final class LaeyFlac {
    private LaeyFlac() {}

    // port of lae_flacmeta.h:Piece
    public static final class Piece { public long off, len; }
    // port of lae_flacmeta.h:Layout
    public static final class Layout {
        public int rate, channels, bps, minBs, maxBs; public long total; public byte[] md5 = new byte[16];   // STREAMINFO
        public long audioOff, audioLen;                                      // the audio frames: from the end of the metadata to the end of the file
        public final List<Piece> lae = new ArrayList<Piece>(); public long laeLen;   // the data of the "LAE1" blocks, in file order, and their total
        public boolean lae2;                                                 // an "LAE2" block was met (an LAES file)
        public long vcOff, vcLen; }                                          // the first VORBIS_COMMENT block (0: none)
    /** the largest piece: a metadata block holds 2^24 - 1 bytes, 4 of them the ID */
    public static final int kPiece = (1 << 24) - 1 - 4;

    private static boolean tag(byte[] b, int o, String t) { for (int k = 0; k < 4; k++) if (b[o + k] != t.charAt(k)) return false; return true; }

    // port of lae_flacmeta.h:layout: the metadata blocks of a FLAC file; "" or the reason. Nothing is decoded.
    public static String layout(LaeyDecoder.ReadFn rd, long size, Layout L) {
        byte[] h = new byte[4]; if (size < 8 || rd.read(0, h, 0, 4) != 0 || !tag(h, 0, "fLaC")) return "not a FLAC file";
        long p = 4; boolean last = false, haveSi = false;
        L.rate = L.channels = L.bps = L.minBs = L.maxBs = 0; L.total = 0; L.md5 = new byte[16]; L.audioOff = L.audioLen = L.laeLen = 0; L.lae.clear(); L.lae2 = false; L.vcOff = L.vcLen = 0;
        byte[] bh = new byte[4];
        while (!last) { if (size - p < 4 || rd.read(p, bh, 0, 4) != 0) return "FLAC metadata truncated";
            last = (bh[0] & 0x80) != 0; final int type = bh[0] & 0x7F; final long len = (bh[1] & 0xFFL) << 16 | (bh[2] & 0xFFL) << 8 | (bh[3] & 0xFFL); p += 4;
            if (len > size - p) return "FLAC metadata truncated";
            if (!haveSi && type != 0) return "STREAMINFO is not the first metadata block";
            if (type == 0) { byte[] s = new byte[34]; if (haveSi || len != 34 || rd.read(p, s, 0, 34) != 0) return "bad STREAMINFO";
                L.minBs = (s[0] & 0xFF) << 8 | (s[1] & 0xFF); L.maxBs = (s[2] & 0xFF) << 8 | (s[3] & 0xFF); long v = 0; for (int i = 10; i < 18; i++) v = v << 8 | (s[i] & 0xFFL);
                L.rate = (int) (v >>> 44); L.channels = (int) ((v >>> 41) & 7) + 1; L.bps = (int) ((v >>> 36) & 31) + 1; L.total = v & ((1L << 36) - 1);
                System.arraycopy(s, 18, L.md5, 0, 16); haveSi = true; }
            else if (type == 2 && len >= 4) { byte[] id = new byte[4]; if (rd.read(p, id, 0, 4) != 0) return "FLAC metadata truncated";
                if (tag(id, 0, "LAE1")) { if (len > 4) { Piece q = new Piece(); q.off = p + 4; q.len = len - 4; L.lae.add(q); L.laeLen += len - 4; } }
                else if (tag(id, 0, "LAE2")) L.lae2 = true; }
            else if (type == 4 && L.vcLen == 0) { L.vcOff = p; L.vcLen = len; }
            p += len; }
        L.audioOff = p; L.audioLen = size - p; return ""; }

    // port of lae_flacmeta.h:check: what an LAEY reader needs from the layout; "" or the reason. notLaey[0]: not an LAEY FLAC file at all
    public static String check(Layout L, boolean[] notLaey) {
        notLaey[0] = true;
        if (L.lae.isEmpty()) return L.lae2 ? "this is an LAES file (lossless, extension LAE2): decode it with laes; this decoder reads LAEY (extension LAE1)" : "no LAE1 block: plain FLAC audio";
        if (L.channels != 2 || L.bps != 16) return "the FLAC audio is not 16-bit stereo: not the CD layer of an LAEY file";
        notLaey[0] = false;
        if (L.total == 0) return "STREAMINFO gives no sample count (the CD layer of an LAEY file always has one)";
        if (L.maxBs < 16) return "bad STREAMINFO block size";
        if (L.total > 0x7FFFFFFFL || L.laeLen > 0x7FFFFFFFL) return "the file is too large for this decoder";
        return ""; }

    // port of lae_flacmeta.h:head: the first n bytes of the extension, across pieces; false when it is shorter or cannot be read
    public static boolean head(LaeyDecoder.ReadFn rd, Layout L, byte[] out, int n) {
        int got = 0; for (int i = 0; i < L.lae.size() && got < n; i++) { final int k = (int) Math.min(L.lae.get(i).len, (long) (n - got));
            if (rd.read(L.lae.get(i).off, out, got, k) != 0) return false; got += k; }
        return got == n; }

    // port of lae_flacmeta.h:comments: the Vorbis comments ({key, value}, UTF-8; keys as written) of the first VORBIS_COMMENT block
    public static List<String[]> comments(LaeyDecoder.ReadFn rd, Layout L) {
        List<String[]> out = new ArrayList<String[]>(); if (L.vcLen < 8 || L.vcLen >= (1L << 24)) return out;
        byte[] b = new byte[(int) L.vcLen]; if (rd.read(L.vcOff, b, 0, b.length) != 0) return out; int q = 0;
        if (b.length - q < 4) return out; long vl = u32(b, q); q += 4; if (vl > b.length - q) return out; q += (int) vl;
        if (b.length - q < 4) return out; final long cnt = u32(b, q); q += 4;
        for (long i = 0; i < cnt; i++) { if (b.length - q < 4) break; final long l = u32(b, q); q += 4; if (l > b.length - q) break;
            String kv; try { kv = new String(b, q, (int) l, "UTF-8"); } catch (java.io.UnsupportedEncodingException e) { throw new IllegalStateException(e); } q += (int) l;
            final int eq = kv.indexOf('='); if (eq > 0) out.add(new String[] {kv.substring(0, eq), kv.substring(eq + 1)}); }
        return out; }
    private static long u32(byte[] q, int o) { return (q[o] & 0xFFL) | (q[o + 1] & 0xFFL) << 8 | (q[o + 2] & 0xFFL) << 16 | (q[o + 3] & 0xFFL) << 24; }

    // port of laey_flac.h:infoOf
    public static Flac.Info infoOf(Layout L) { Flac.Info si = new Flac.Info(); si.rate = L.rate; si.channels = L.channels; si.bps = L.bps; si.minBs = L.minBs; si.maxBs = L.maxBs; si.total = L.total; si.md5 = L.md5.clone(); return si; }

    // port of laey_flac.h:decodeAudio: the audio frames (all of them, n bytes of a) decoded into pcm[0] (interleaved L/R); "" or the reason
    public static String decodeAudio(byte[] a, int n, Flac.Info si, short[][] pcm) {
        if (si.total * 2 > Integer.MAX_VALUE - 8) return "the file is too large for this decoder";   // Java only: an array holds < 2^31 values
        final short[] o = new short[(int) (si.total * 2)]; pcm[0] = o; long have = 0, frameNo = 0; int p = 0; final Flac.Frame F = new Flac.Frame();
        while (p < n) { Flac.decodeFrame(a, p, n - p, si, frameNo, have, F); if (!F.err.isEmpty()) return "FLAC CD layer: " + F.err;
            final int k = F.n; if (have + k > si.total) return "FLAC CD layer: more samples than STREAMINFO gives";
            final int at = (int) have * 2; final long[] l = F.ch[0], r = F.ch[1]; for (int i = 0; i < k; i++) { o[at + i * 2] = (short) l[i]; o[at + i * 2 + 1] = (short) r[i]; }
            have += k; p += F.used; frameNo++; }
        if (have != si.total) return "FLAC CD layer: sample count differs from STREAMINFO";
        return ""; }

    // port of laey_flac.h:md5ok: STREAMINFO's MD5 of the decoded CD layer; it must be present and right (an all-zero MD5 fails too)
    public static boolean md5ok(short[] pcm, Flac.Info si) {
        boolean zero = true; for (int i = 0; i < 16; i++) zero = zero && si.md5[i] == 0; if (zero) return false;
        final String h = AlignHash.cdMd5(pcm, pcm.length / 2); final String d = "0123456789abcdef";
        for (int i = 0; i < 16; i++) if (h.charAt(i * 2) != d.charAt((si.md5[i] >> 4) & 15) || h.charAt(i * 2 + 1) != d.charAt(si.md5[i] & 15)) return false;
        return true; }

    static final String MD5_MSG = "FLAC CD layer: MD5 missing or wrong (the audio is not the one this file was written with): refusing to decode; the CD audio itself still plays";
    private static final int STEP = 4 << 20;

    // port of laey_flac.h:read: a whole file through a reader: the CD layer and the extension, checked; "" or the reason
    public static String read(LaeyDecoder.ReadFn rd, long size, int[] rate, short[][] pcm, byte[][] lae, boolean[] notLaey) {
        Layout L = new Layout(); notLaey[0] = true; String e = layout(rd, size, L); if (!e.isEmpty()) return e;
        e = check(L, notLaey); if (!e.isEmpty()) return e;
        if (L.audioLen > Integer.MAX_VALUE - 8) return "the file is too large for this decoder";
        final byte[] x = new byte[(int) L.laeLen]; { int at = 0; for (Piece q : L.lae) { for (long i = 0; i < q.len; i += STEP) { final int k = (int) Math.min(STEP, q.len - i); if (rd.read(q.off + i, x, at, k) != 0) return "cannot read the file"; at += k; } } }
        lae[0] = x;
        byte[] au = new byte[(int) L.audioLen];
        for (int at = 0; at < au.length; at += STEP) if (rd.read(L.audioOff + at, au, at, Math.min(STEP, au.length - at)) != 0) return "cannot read the file";
        final Flac.Info si = infoOf(L); e = decodeAudio(au, au.length, si, pcm); if (!e.isEmpty()) return e;
        if (!md5ok(pcm[0], si)) return MD5_MSG;
        rate[0] = L.rate; return ""; }

    // port of laey_flac.h:isFlacFile
    public static boolean isFlacFile(String path) {
        RandomAccessFile f = null; try { f = new RandomAccessFile(path, "r"); byte[] h = new byte[4]; return f.read(h) == 4 && tag(h, 0, "fLaC"); }
        catch (IOException e) { return false; } finally { if (f != null) try { f.close(); } catch (IOException e) { /* read only */ } } }

    // port of laey_flac.h:readFile
    public static String readFile(String path, int[] rate, short[][] pcm, byte[][] lae, boolean[] notLaey) {
        notLaey[0] = true; RandomAccessFile f0; try { f0 = new RandomAccessFile(path, "r"); } catch (IOException e) { return "cannot read the file"; }
        final RandomAccessFile f = f0;
        try { final long sz = f.length();
            return read(new LaeyDecoder.ReadFn() { @Override public int read(long offset, byte[] buf, int off, int len) { try { f.seek(offset); f.readFully(buf, off, len); return 0; } catch (IOException x) { return -1; } } }, sz, rate, pcm, lae, notLaey); }
        catch (IOException e) { return "cannot read the file"; }
        finally { try { f.close(); } catch (IOException e) { /* read only */ } } }
}
