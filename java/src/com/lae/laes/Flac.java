// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import com.lae.core.AlignHash;

/**
 * Port of src/laes_flac.h (namespace laesflac): FLAC reader for the LAES FLAC form (F1 draft, 2026-10-29). Written for this project
 * from the format description in RFC 9639 (no libFLAC code). The writer produces only what LAES needs:
 *   "fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (optional, before the payload) | APPLICATION blocks (ID "LAE2", the LAES payload
 *   split into pieces, in order, nothing between them) | audio frames = the core
 *   audio frames: fixed block size 4096, two independent channels (channel assignment 0001; no inter-channel decorrelation),
 *   subframes CONSTANT / VERBATIM / FIXED 0-4 / LPC (order <= 12, precision <= 15, shift 0..15), residual with 4- or 5-bit Rice
 *   parameters (partition order <= 8).
 *   No SEEKTABLE, no CUESHEET; nothing interleaved with the audio frames (LAES rules T-1, T-2). STREAMINFO's MD5 covers the core only.
 * The reader decodes any stream written this way (and FLAC streams of the same kind from other encoders: wasted bits, every
 * fixed / LPC order, both Rice methods); streams using inter-channel decorrelation are reported, not decoded.
 */
public final class Flac {
    private Flac() {}

    // CRC-8 (x^8+x^2+x+1) and CRC-16 (x^16+x^15+x^2+1) of FLAC frames, MSB first, by byte tables (the same values as bit by bit)
    private static final int[] T8 = new int[256], T16 = new int[256];
    // port of laes_flac.h:CrcTables::CrcTables
    static { for (int i = 0; i < 256; i++) { int c = i; for (int k = 0; k < 8; k++) c = (c & 0x80) != 0 ? ((c << 1) ^ 0x07) & 0xFF : (c << 1) & 0xFF; T8[i] = c;
            int d = (i << 8) & 0xFFFF; for (int k = 0; k < 8; k++) d = (d & 0x8000) != 0 ? ((d << 1) ^ 0x8005) & 0xFFFF : (d << 1) & 0xFFFF; T16[i] = d; } }
    // port of laes_flac.h:crc8
    public static int crc8(byte[] p, int off, int n) { int c = 0; for (int i = 0; i < n; i++) c = T8[c ^ (p[off + i] & 0xFF)]; return c; }
    // port of laes_flac.h:crc16
    public static int crc16(byte[] p, int off, int n) { int c = 0; for (int i = 0; i < n; i++) c = ((c << 8) ^ T16[(c >> 8) ^ (p[off + i] & 0xFF)]) & 0xFFFF; return c; }
    // MD5 of the audio as STREAMINFO defines it: interleaved, each sample little-endian signed in ceil(bits/8) bytes
    // port of laes_flac.h:audioMd5
    public static byte[] audioMd5(long[][] ch, long n, int bits) { AlignHash.Md5 m = new AlignHash.Md5(); final int by = (bits + 7) / 8; byte[] b = new byte[1 << 16]; int fill = 0;
        for (int i = 0; i < n; i++) for (int c = 0; c < 2; c++) { long v = ch[c][i]; for (int k = 0; k < by; k++) b[fill++] = (byte) (v >>> (8 * k)); if (fill > (1 << 16) - 8) { m.update(b, 0, fill); fill = 0; } }
        m.update(b, 0, fill); return hexBytes(m.hex(), 16); }
    static byte[] hexBytes(String h, int n) { byte[] out = new byte[n]; for (int i = 0; i < n; i++) out[i] = (byte) Integer.parseInt(h.substring(i * 2, i * 2 + 2), 16); return out; }

    // port of laes_flac.h:fits32  (residuals stay inside the 32-bit signed range (excluding -2^31))
    static boolean fits32(long r) { return r > -(1L << 31) && r < (1L << 31); }

    // ---- reader
    /** port of laes_flac.h:BR: MSB-first bit reader over a byte range; pos in bits */
    public static final class BR {
        final byte[] p; final int pOff; final long n; long pos = 0; boolean fail = false;
        public BR(byte[] p, int off, long n) { this.p = p; this.pOff = off; this.n = n; }
        // up to 8 bits per step (the same value as bit by bit)
        // port of laes_flac.h:BR::get
        public long get(int w) { if (fail || w < 0 || w > 64 || pos + w > n * 8) { fail = true; return 0; }
            if (w >= 1 && w <= 56 && (pos >>> 3) + 8 <= n) {   // Java fast form (2026-11-04): from one 64-bit word, the same bits
                final int q = pOff + (int) (pos >>> 3);
                final long wd = ((p[q] & 0xFFL) << 56 | (p[q + 1] & 0xFFL) << 48 | (p[q + 2] & 0xFFL) << 40 | (p[q + 3] & 0xFFL) << 32
                    | (p[q + 4] & 0xFFL) << 24 | (p[q + 5] & 0xFFL) << 16 | (p[q + 6] & 0xFFL) << 8 | (p[q + 7] & 0xFFL)) << (int) (pos & 7);
                pos += w; return wd >>> (64 - w); }
            long v = 0;
            while (w > 0) { final int off = (int) (pos & 7), avail = 8 - off, take = w < avail ? w : avail; v = (v << take) | (long) (((p[pOff + (int) (pos >>> 3)] & 0xFF) >>> (avail - take)) & ((1 << take) - 1)); pos += take; w -= take; }
            return v; }
        // port of laes_flac.h:BR::getS
        public long getS(int w) { if (w == 0) return 0; long v = get(w); if (w < 64 && ((v >>> (w - 1)) & 1) != 0) v |= -1L << w; return v; }
        // zero bits before the next one bit, a byte at a time (as bit by bit: past the end or beyond 2^33 zeros sets fail)
        // port of laes_flac.h:BR::unary
        long unary() { long q = 0;
            while (!fail) { if (pos >= n * 8) { fail = true; break; } final int off = (int) (pos & 7); final int b = ((p[pOff + (int) (pos >>> 3)] & 0xFF) << off) & 0xFF;
                if (b == 0) { final int z = 8 - off; if (q + z > (1L << 33)) { pos += (1L << 33) - q + 1; q = (1L << 33) + 1; fail = true; break; } q += z; pos += z; continue; }
                int lz = 0; while ((b & (0x80 >>> lz)) == 0) lz++; q += lz; pos += lz + 1;
                if (q > (1L << 33)) fail = true; break; }
            return q; }
    }
    /** port of laes_flac.h:Info */
    public static final class Info { public int rate = 0, channels = 0, bps = 0, minBs = 0, maxBs = 0; public long total = 0; public byte[] md5 = new byte[16]; }
    // port of laes_flac.h:readResidual
    static boolean readResidual(BR r, long[] out, int outOff, long blk, int order) {
        final int method = (int) r.get(2); if (method > 1) return false; final int pb = method != 0 ? 5 : 4, esc = method != 0 ? 31 : 15; final int po = (int) r.get(4);
        if (r.fail || blk % (1L << po) != 0 || (blk >> po) <= order) return false; final long plen = blk >> po; long a = 0;   // the first partition is not empty
        for (long q = 0; q < (1L << po); q++) { final long len = q != 0 ? plen : plen - order; final int k = (int) r.get(pb);
            if (k == esc) { final int rw = (int) r.get(5); for (long i = 0; i < len; i++) out[outOff + (int) (a + i)] = r.getS(rw); }
            else for (long i = 0; i < len && !r.fail; i++) {
                // Java fast form (2026-11-04): the quotient's zeros, its one and the k low bits all inside the next 57 bits of the data:
                // read from one 64-bit word (the same value and position as unary() then get(k); the quotient is then below 57, so the
                // check (qq >>> (63 - k)) != 0 below cannot hold: k <= 30)
                final long bt = r.pos >>> 3;
                if (bt + 8 <= r.n) { final int q0 = r.pOff + (int) bt; final byte[] pb_ = r.p;
                    final long wd = ((pb_[q0] & 0xFFL) << 56 | (pb_[q0 + 1] & 0xFFL) << 48 | (pb_[q0 + 2] & 0xFFL) << 40 | (pb_[q0 + 3] & 0xFFL) << 32
                        | (pb_[q0 + 4] & 0xFFL) << 24 | (pb_[q0 + 5] & 0xFFL) << 16 | (pb_[q0 + 6] & 0xFFL) << 8 | (pb_[q0 + 7] & 0xFFL)) << (int) (r.pos & 7);
                    final int lz = Long.numberOfLeadingZeros(wd);
                    if (lz + 1 + k <= 57) { final long lo = k != 0 ? (wd << (lz + 1)) >>> (64 - k) : 0; r.pos += lz + 1 + k;
                        final long v = Codec.unzz(((long) lz << k) | lo); if (!fits32(v)) return false; out[outOff + (int) (a + i)] = v; continue; } }
                final long qq = r.unary(); if ((qq >>> (63 - k)) != 0) return false;   // the two reads in order (quotient, then low bits)
                final long lo = k != 0 ? r.get(k) : 0; final long v = Codec.unzz((qq << k) | lo); if (!fits32(v)) return false; out[outOff + (int) (a + i)] = v; }   // residuals inside +-(2^31 - 1)
            if (r.fail) return false; a += len; } return true; }
    // x[off + i] = res[i] + Codec.fixedPred(x, off, i, o) for i in [o, n), each checked against [-lim, lim) as it is made (false at the
    // first value outside); Java 2026-11-04: one loop per order instead of the switch per value (the same expressions)
    static boolean fixedLoop(long[] x, int off, long[] res, int o, int n, long lim) {
        switch (o) {
            case 0: for (int i = o; i < n; i++) { final long v = res[i]; x[off + i] = v; if (v < -lim || v >= lim) return false; } return true;
            case 1: for (int i = o; i < n; i++) { final int p = off + i; final long v = res[i] + x[p - 1]; x[p] = v; if (v < -lim || v >= lim) return false; } return true;
            case 2: for (int i = o; i < n; i++) { final int p = off + i; final long v = res[i] + (2 * x[p - 1] - x[p - 2]); x[p] = v; if (v < -lim || v >= lim) return false; } return true;
            case 3: for (int i = o; i < n; i++) { final int p = off + i; final long v = res[i] + (3 * x[p - 1] - 3 * x[p - 2] + x[p - 3]); x[p] = v; if (v < -lim || v >= lim) return false; } return true;
            default: for (int i = o; i < n; i++) { final int p = off + i; final long v = res[i] + (4 * x[p - 1] - 6 * x[p - 2] + 4 * x[p - 3] - x[p - 4]); x[p] = v; if (v < -lim || v >= lim) return false; } return true; } }
    // port of laes_flac.h:readSubframe  (x[off .. off + n))
    static boolean readSubframe(BR r, long[] x, int off, int n, int bps) { return readSubframe(r, x, off, n, bps, null); }
    // Java (2026-11-04): F (may be null) lends its residual array F.res (grown when short; values [order, n) are written before use)
    static boolean readSubframe(BR r, long[] x, int off, int n, int bps, Frame F) {
        if (r.get(1) != 0) return false; final int type = (int) r.get(6); int wasted = 0; if (r.get(1) != 0) { wasted = 1; while (!r.fail && r.get(1) == 0) wasted++; }
        final int b = bps - wasted; if (r.fail || b < 1) return false;
        if (type == 0) { long v = r.getS(b); for (int i = 0; i < n; i++) x[off + i] = v; }
        else if (type == 1) { for (int i = 0; i < n; i++) x[off + i] = r.getS(b); }
        else if (type >= 8 && type <= 12) { final int o = type - 8; if (o > n) return false; for (int i = 0; i < o; i++) x[off + i] = r.getS(b);
            long[] res = resOf(F, n); if (!readResidual(r, res, o, n, o)) return false; final long lim = 1L << (b - 1);
            if (!fixedLoop(x, off, res, o, n, lim)) return false; }   // checked as it goes: the sums stay inside int64
        else if (type >= 32) { final int o = type - 31; if (o > n) return false; for (int i = 0; i < o; i++) x[off + i] = r.getS(b);
            final int prec = (int) r.get(4) + 1; final long sh = r.getS(5); if (prec == 16 || sh < 0) return false; long[] c = new long[o]; for (int j = 0; j < o; j++) c[j] = r.getS(prec);
            long[] res = resOf(F, n); if (!readResidual(r, res, o, n, o)) return false;
            // Java (2026-11-04): the sum in 64 bits. It is the C++ 128-bit sum exactly: b <= 32 (bps <= 32), every x used is inside
            // [-2^(b-1), 2^(b-1)) (warm-up read in b bits, later values checked below before they are used), |c| <= 2^14 (prec <= 15), order
            // <= 32, so |sum| <= 2^50; and the C++ floor (subtract 2^(sh-1), then shr's round) of an exact sum is the arithmetic shift by sh.
            final int shi = (int) sh; final long lim = 1L << (b - 1);
            for (int i = o; i < n; i++) { final int p = off + i, q = p - 1; long acc = 0, acc2 = 0; int j = 0;   // two parts, added at the end
                for (; j + 1 < o; j += 2) { acc += c[j] * x[q - j]; acc2 += c[j + 1] * x[q - j - 1]; } if (j < o) acc += c[j] * x[q - j];
                final long v = res[i] + ((acc + acc2) >> shi); x[p] = v; if (v < -lim || v >= lim) return false; } }
        else return false;
        if (r.fail) return false;
        final long lim = 1L << (b - 1); for (int i = 0; i < n; i++) if (x[off + i] < -lim || x[off + i] >= lim) return false;   // every sample inside the subframe's bit depth
        if (wasted != 0) for (int i = 0; i < n; i++) x[off + i] = x[off + i] << wasted; return true; }

    /** the result of decodeFrame: err ("" = ok), the two channels of the frame, its size in bytes */
    public static final class Frame { public String err = ""; public long[][] ch = new long[2][]; public int n; public int used; long[] res = null; }
    static long[] resOf(Frame F, int n) { if (F == null) return new long[n]; if (F.res == null || F.res.length < n) F.res = new long[n]; return F.res; }
    // one audio frame at fp (avail bytes left in the stream): its samples (ch[0] / ch[1]) and its size in bytes (used).
    // have = samples of the stream decoded before this frame; frameNo = its index. err "" or the reason.
    // port of laes_flac.h:decodeFrame
    public static Frame decodeFrame(byte[] buf, int fp, long avail, Info in, long frameNo, long have) {
        Frame F = new Frame(); F.err = decodeFrameImpl(buf, fp, avail, in, frameNo, have, F, false); return F; }
    // Java (2026-11-04): the same, decoded into F (reused: its channel arrays are kept when long enough, so F.ch[c] may be longer than F.n)
    public static Frame decodeFrame(byte[] buf, int fp, long avail, Info in, long frameNo, long have, Frame F) {
        F.err = ""; F.n = 0; F.used = 0; F.err = decodeFrameImpl(buf, fp, avail, in, frameNo, have, F, true); return F; }
    private static String decodeFrameImpl(byte[] buf, int fp, long avail, Info in, long frameNo, long have, Frame F, boolean reuse) {
        BR r = new BR(buf, fp, avail);
        if (r.get(15) != 0x7FFC) return "lost frame sync"; final int strategy = (int) r.get(1); final int bsc = (int) r.get(4), src = (int) r.get(4), chc = (int) r.get(4), ssc = (int) r.get(3);
        if (r.get(1) != 0) return "frame header reserved bit set";
        if (chc != 1) return chc <= 7 ? "core master frame without 2 channels" : "core master frame uses inter-channel decorrelation (not supported by this reader)";
        long num = r.get(8); { int extra = 0; if (num == 0xFF) return "bad frame number"; if (num >= 0xFE) { if (strategy == 0) return "bad frame number"; extra = 6; } else if (num >= 0xFC) extra = 5; else if (num >= 0xF8) extra = 4; else if (num >= 0xF0) extra = 3; else if (num >= 0xE0) extra = 2; else if (num >= 0xC0) extra = 1; else if (num >= 0x80) return "bad frame number";
            if (extra != 0) { num &= (0x3F >> extra); for (int i = 0; i < extra; i++) { final long b = r.get(8); if ((b & 0xC0) != 0x80) return "bad frame number"; num = num << 6 | (b & 0x3F); } } }
        long n; if (bsc == 1) n = 192; else if (bsc >= 2 && bsc <= 5) n = 576L << (bsc - 2); else if (bsc == 6) n = r.get(8) + 1; else if (bsc == 7) { n = r.get(16) + 1; if (n == 65536) return "bad block size"; } else if (bsc >= 8) n = 256L << (bsc - 8); else return "bad block size";
        if (n > in.maxBs || (in.total != 0 && com.lae.core.U.gt(have + n, in.total))) return "block larger than STREAMINFO allows";   // also caps memory to what the header declares
        if (src == 12) r.get(8); else if (src == 13 || src == 14) r.get(16); else if (src == 15) return "bad sample rate code";
        final int bps = ssc == 0 ? in.bps : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 4 ? 16 : ssc == 5 ? 20 : ssc == 6 ? 24 : ssc == 7 ? 32 : 0; if (bps != in.bps) return "frame bit depth differs from STREAMINFO";
        if (r.fail || (r.pos & 7) != 0) return "frame header truncated"; final int hb = (int) (r.pos / 8); final int c8 = (int) r.get(8); if (r.fail || c8 != crc8(buf, fp, hb)) return "frame header CRC-8 mismatch";
        if (strategy == 0 && num != frameNo) return "frame number out of order";
        if (strategy == 1 && num != have) return "frame sample number out of order";   // variable block size: the first sample's number (FU12-6, as laes_flac.h)
        F.n = (int) n; for (int c = 0; c < 2; c++) { if (!reuse || F.ch[c] == null || F.ch[c].length < n) F.ch[c] = new long[(int) n]; if (!readSubframe(r, F.ch[c], 0, (int) n, bps, F)) return "damaged subframe"; }
        r.pos = (r.pos + 7) & ~7L; final long fb = r.pos / 8; if (fb + 2 > r.n) return "frame truncated";
        if (crc16(buf, fp, (int) fb) != (((buf[fp + (int) fb] & 0xFF) << 8) | (buf[fp + (int) fb + 1] & 0xFF))) return "frame CRC-16 mismatch";
        F.used = (int) (fb + 2);
        return ""; }
    // the frame number of a FLAC frame header at fp (fixed block size, 2 channels, the stream's bit depth, header CRC-8 matching), for
    // splitting a core into parts that decode in parallel; false when fp does not start such a header (num[0] receives the number)
    // port of laes_flac.h:frameHeaderAt
    public static boolean frameHeaderAt(byte[] buf, int fp, long avail, Info in, long[] numOut) {
        BR r = new BR(buf, fp, avail); if (r.get(15) != 0x7FFC || r.get(1) != 0) return false; final int bsc = (int) r.get(4), src = (int) r.get(4), chc = (int) r.get(4), ssc = (int) r.get(3);
        if (r.get(1) != 0 || chc != 1) return false;
        long num = r.get(8); { int extra = 0; if (num >= 0xFE) return false; if (num >= 0xFC) extra = 5; else if (num >= 0xF8) extra = 4; else if (num >= 0xF0) extra = 3; else if (num >= 0xE0) extra = 2; else if (num >= 0xC0) extra = 1; else if (num >= 0x80) return false;
            if (extra != 0) { num &= (0x3F >> extra); for (int i = 0; i < extra; i++) { final long b = r.get(8); if ((b & 0xC0) != 0x80) return false; num = num << 6 | (b & 0x3F); } } }
        numOut[0] = num;
        if (bsc == 0) return false; if (bsc == 6) r.get(8); else if (bsc == 7) r.get(16);
        if (src == 12) r.get(8); else if (src == 13 || src == 14) r.get(16); else if (src == 15) return false;
        final int bps = ssc == 0 ? in.bps : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 4 ? 16 : ssc == 5 ? 20 : ssc == 6 ? 24 : ssc == 7 ? 32 : 0; if (bps != in.bps) return false;
        if (r.fail || (r.pos & 7) != 0) return false; final int hb = (int) (r.pos / 8); final int c8 = (int) r.get(8); return !r.fail && c8 == crc8(buf, fp, hb); }

    /** the result of readFlac */
    public static final class Whole { public String err = ""; public Info in = new Info(); public long[][] ch = new long[2][]; public byte[] app = new byte[0]; public boolean md5ok = false; }
    // reads a whole FLAC file: STREAMINFO, the concatenated "LAE2" APPLICATION data, and the audio (two independent channels).
    // err "" or the reason. md5ok: the decoded audio matches STREAMINFO's MD5 (an all-zero MD5 counts as "not given").
    // port of laes_flac.h:readFlac
    public static Whole readFlac(byte[] f) {
        Whole W = new Whole(); W.err = readFlacImpl(f, W); return W; }
    private static String readFlacImpl(byte[] f, Whole W) {
        final Info in = W.in; java.io.ByteArrayOutputStream app = new java.io.ByteArrayOutputStream();
        if (f.length < 8 || !(f[0] == 'f' && f[1] == 'L' && f[2] == 'a' && f[3] == 'C')) return "not a FLAC file";
        long p = 4; boolean last = false, haveSi = false;
        while (!last) { if (p + 4 > f.length) return "metadata truncated"; last = (f[(int) p] & 0x80) != 0; final int type = f[(int) p] & 0x7F; final long len = (long) (f[(int) p + 1] & 0xFF) << 16 | (f[(int) p + 2] & 0xFF) << 8 | (f[(int) p + 3] & 0xFF); p += 4;
            if (p + len > f.length) return "metadata truncated"; if (!haveSi && type != 0) return "STREAMINFO is not the first metadata block";
            if (type == 0) { if (haveSi) return "two STREAMINFO blocks (FLAC allows one)"; if (len != 34) return "bad STREAMINFO"; BR r = new BR(f, (int) p, 34); in.minBs = (int) r.get(16); in.maxBs = (int) r.get(16); r.get(24); r.get(24); in.rate = (int) r.get(20); in.channels = (int) r.get(3) + 1; in.bps = (int) r.get(5) + 1; in.total = r.get(36);
                System.arraycopy(f, (int) p + 18, in.md5, 0, 16); haveSi = true; }
            else if (type == 2 && len >= 4 && f[(int) p] == 'L' && f[(int) p + 1] == 'A' && f[(int) p + 2] == 'E' && f[(int) p + 3] == '2') app.write(f, (int) p + 4, (int) len - 4);
            p += len; }
        W.app = app.toByteArray();
        if (!haveSi) return "no STREAMINFO"; if (in.total == 0) return "STREAMINFO gives no sample count (an LAES core master always has one)"; if (in.maxBs < 16) return "bad STREAMINFO block size"; if (in.channels != 2) return "LAES core master must have 2 channels"; if (in.bps != 16 && in.bps != 24 && in.bps != 32) return "core master must be 16, 24 or 32 bit";
        long[][] ch = {new long[1024], new long[1024]}; int size = 0; long frameNo = 0;
        while (p < f.length) { Frame F = decodeFrame(f, (int) p, f.length - p, in, frameNo, size); if (!F.err.isEmpty()) return F.err;
            if (size + F.n > ch[0].length) for (int c = 0; c < 2; c++) ch[c] = java.util.Arrays.copyOf(ch[c], Math.max(size + F.n, ch[c].length * 2));
            for (int c = 0; c < 2; c++) System.arraycopy(F.ch[c], 0, ch[c], size, F.n); size += F.n; p += F.used; frameNo++; }
        for (int c = 0; c < 2; c++) W.ch[c] = java.util.Arrays.copyOf(ch[c], size);
        if (in.total != 0 && in.total != size) return "sample count differs from STREAMINFO";
        byte[] m = audioMd5(W.ch, size, in.bps); boolean zero = true; for (int i = 0; i < 16; i++) zero &= in.md5[i] == 0; W.md5ok = !zero && java.util.Arrays.equals(m, in.md5);
        return ""; }
}
