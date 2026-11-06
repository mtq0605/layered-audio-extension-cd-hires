// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;

import com.lae.core.LaeException;
import com.lae.core.U;

/**
 * Port of src/laes_file.h (namespace laes), decoder side: LAES file format and decoder (draft v1, 2026-10-28). An LAES WAV file is an
 * LAE file whose "LAE " chunk carries an LAES payload:
 *   RIFF/WAVE: "fmt " (PCM, 2 ch, base rate, 16 bit) | "data" = the core master (can be CD-compatible: 44.1 kHz or 48 kHz, 16 bit) | "LAE " = payload + CRC-32
 *
 * Payload (little-endian):
 *   header, 64 bytes: magic "LAE2" | u16 version = 1 | u16 header size = 64 | u32 base rate | u8 core bits (16 / 24 / 32) | u8 channels = 2 |
 *                     u8 number of masters (1..16) | u8 core mode (0 = core in the WAV "data" / CD tracks, 1 = core in the FLAC audio frames) |
 *                     u64 core frames | 32-byte SHA-256 of the core (16 bit: int16 LE as in "data"; 24 / 32 bit: int32 LE) |
 *                     u64 reserved = 0
 *   per master, 72-byte record, then its layers:
 *     u8 bits (16/24/32) | u8 stages (0..3: rate = base * 2^stages) | u8 unused low bits w | u8 storage (0 = difference to a tier at the base
 *     level, 1 = full, 2 = difference to an earlier master at the highest level both have) | u8 reference tier (0 = core, k = master k;
 *     storage 0: core or any earlier master; storage 2: an earlier master) | 3 reserved | u32 rate | u32 reserved | u64 frames |
 *     i64 T whole | u32 T fraction (2^-32 base sample; T relative to the reference tier) | u32 reserved | 32-byte SHA-256 of the master
 *     (samples as int32 LE, L R L R ...)
 *     per channel (L, then R), with j = 0 for storage 0 / 1 and j = min(stages, stages of the tier) for storage 2:
 *       u32 length + the level-j difference (storage 0 / 2) or the base level (storage 1), coded with laes_codec (block 4096 * 2^j);
 *       then for each stage k = j+1..stages: u32 length + octave layer k (block 4096 * 2^(k-1))
 *   No index, no seek table, no cue table; nothing interleaved with the core (rules T-1, T-2).
 * Reconstruction of master m: level j = (storage 1 ? 0 : reference(tier's level j, T * 2^j, grid difference)) + layer; then inv() per stage
 * k = j+1..stages; samples &lt;&lt; w. A tier's level j comes from its decoded samples (&gt;&gt; w) by fwd(). Each channel only from the same channel.
 * grid difference (up) = the master's significant bits (bits - w) minus the tier's (core: its bits; master k: bits_k - w_k); it may be negative.
 * The SHA-256 is java.security.MessageDigest (the C++ SHA-NI path is not ported; both give FIPS 180-4 SHA-256).
 */
public final class LaesFile {
    private LaesFile() {}

    /** C++ LONG_MAX on the reference build (MSVC x64: long is 32-bit); see the report: masters above it are refused, as by the reference */
    /** the whole-in-memory decoder's limit as laes_file.h has it since 635019a: PTRDIFF_MAX / sizeof(int64_t) on x64 */
    public static final long WHOLE_MAX_FRAMES = Long.MAX_VALUE / 8;

    /** port of laes_file.h:Sha256 (FIPS 180-4), by java.security.MessageDigest */
    public static final class Sha256 {
        private final MessageDigest md;
        public Sha256() { try { md = MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new LaeException("SHA-256 not available", e); } }
        // port of laes_file.h:Sha256::update
        public void update(byte[] p, int off, int k) { md.update(p, off, k); }
        // port of laes_file.h:Sha256::final
        public void finish(byte[] out) { byte[] d = md.digest(); System.arraycopy(d, 0, out, 0, 32); }
    }
    // ---- types
    /** port of laes_file.h:Pcm */
    public static final class Pcm { public int rate = 0, bits = 0; public long[][] c = {new long[0], new long[0]}; public long frames() { return c[0].length; } }
    // port of laes_file.h:hashPcm
    public static void hashPcm(Pcm p, byte[] out) { Sha256 s = new Sha256(); byte[] b = new byte[1 << 16]; int fill = 0;
        for (int i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { final int v = (int) p.c[c][i]; b[fill++] = (byte) v; b[fill++] = (byte) (v >>> 8); b[fill++] = (byte) (v >>> 16); b[fill++] = (byte) (v >>> 24); if (fill >= (1 << 16)) { s.update(b, 0, fill); fill = 0; } }
        s.update(b, 0, fill); s.finish(out); }
    // port of laes_file.h:hashCore  (int16 LE, as in the "data" chunk)
    public static void hashCore(Pcm p, byte[] out) { Sha256 s = new Sha256(); byte[] b = new byte[1 << 16]; int fill = 0;
        for (int i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { final int v = (int) p.c[c][i]; b[fill++] = (byte) v; b[fill++] = (byte) (v >>> 8); if (fill >= (1 << 16)) { s.update(b, 0, fill); fill = 0; } }
        s.update(b, 0, fill); s.finish(out); }
    /** port of laes_file.h:MasterInfo (frac: uint32 in an int) */
    public static final class MasterInfo { public int bits = 0, stages = 0, w = 0, storage = 0, refTier = 0, rate = 0; public long frames = 0; public long whole = 0; public int frac = 0; public byte[] sha = new byte[32]; public double layerBits = 0; }
    // port of laes_file.h:levLen
    public static long levLen(long n, int k) { while (k-- > 0) n = (n + 1) / 2; return n; }
    // port of laes_file.h:unusedBits
    public static int unusedBits(Pcm p) { long o = 0; for (int c = 0; c < 2; c++) for (long v : p.c[c]) o |= v; if (o == 0) return 0; int w = 0; while (w < 31 && ((o >> w) & 1) == 0) w++; return w; }
    // the reference of a tier on the master's base grid, nOut values: r[i] = round(sum_q h[q] src[m(i + whole + q)] / 2^(31 - up)), h = refTaps(frac).
    // Normative form: laes::reference (laes_math.h). This is its fast form (same values; laesfast technique, 2026-10-29): direct indexing away from
    // the ends, 64-bit accumulation when 2 x max|src| x sum|h| + rounding stays below 2^63 (then the 64-bit sum equals the 128-bit one),
    // otherwise 128-bit; indices stay int64 as in the normative form; the output is split across threads.
    // port of laes_file.h:refLen
    public static long[] refLen(final long[] src, long nOut, final long whole, int frac, int up) {
        final long[] h = LaesMath.refTaps(frac); final long n = src.length; final long[] r = new long[(int) nOut]; final int s = 31 - up;
        long shv = 0; for (long v : h) shv += v < 0 ? -v : v;
        final boolean wide = !((double) Lift.maxAbs(src) * (double) shv + (s > 0 ? (double) (1L << (s - 1)) : 0.0) < 9.0e18);
        Lift.parallelFor(nOut, Lift.defaultThreads(), new Lift.Range() { public void run(long a, long b) { for (long i = a; i < b; i++) { final long j0 = i + whole - 127, j1 = i + whole + 128;
            if (!wide) { long acc = 0;
                if (j0 >= 0 && j1 < n) { final int c = (int) j0; for (int q = 0; q < 256; q++) acc += h[q] * src[c + q]; }
                else for (int q = -127; q <= 128; q++) acc += h[q + 127] * src[(int) LaesMath.mir(i + whole + q, n)];
                r[(int) i] = s > 0 ? (acc + (1L << (s - 1))) >> s : acc << (-s); }
            else { LaesMath.Acc A = new LaesMath.Acc();
                if (j0 >= 0 && j1 < n) { final int c = (int) j0; for (int q = 0; q < 256; q++) A.mac(h[q], src[c + q]); }
                else for (int q = -127; q <= 128; q++) A.mac(h[q + 127], src[(int) LaesMath.mir(i + whole + q, n)]);
                r[(int) i] = s > 0 ? A.shr(s) : A.lo << (-s); } } } });
        return r;
    }
    /** port of laes_file.h:BRd2 (little-endian fields over a byte range) */
    static final class BRd2 { final byte[] p; final long n; long pos = 0; boolean fail = false;
        BRd2(byte[] p, long n) { this.p = p; this.n = n; }
        // port of laes_file.h:BRd2::u
        long u(int k) { if (pos + k > n) { fail = true; return 0; } long v = 0; for (int i = 0; i < k; i++) v |= (long) (p[(int) pos + i] & 0xFF) << (8 * i); pos += k; return v; }
        // port of laes_file.h:BRd2::take  (returns the offset, or -1)
        int take(long k) { if (pos + k > n) { fail = true; return -1; } int r = (int) pos; pos += k; return r; } }
    // port of laes_file.h:hashAnyCore
    public static void hashAnyCore(Pcm p, byte[] out) { if (p.bits == 16) hashCore(p, out); else hashPcm(p, out); }
    // ---- decoder: "LAE " chunk body + core -> masters. Every field and layer is checked; returns "" or the reason.
    // port of laes_file.h:isLaes
    public static boolean isLaes(byte[] body) { return body.length >= 8 && body[0] == 'L' && body[1] == 'A' && body[2] == 'E' && body[3] == '2'; }
    static boolean eq32(byte[] a, int ao, byte[] b, int bo) { for (int i = 0; i < 32; i++) if (a[ao + i] != b[bo + i]) return false; return true; }
    /** port of laes_file.h:decodeLaes (whole signals in memory) */
    public static String decodeLaes(byte[] body, Pcm core, int baseRate, List<Pcm> out, List<MasterInfo> info, List<Boolean> shaOk) {
        out.clear(); info.clear(); shaOk.clear(); if (body.length < 64 + 4) return "LAES block too short";
        int crc = 0xFFFFFFFF; for (int i = 0; i + 4 < body.length; i++) { crc ^= body[i] & 0xFF; for (int k = 0; k < 8; k++) crc = (crc & 1) != 0 ? 0xEDB88320 ^ (crc >>> 1) : crc >>> 1; } crc ^= 0xFFFFFFFF;
        final int L = body.length; int stored = (body[L - 4] & 0xFF) | (body[L - 3] & 0xFF) << 8 | (body[L - 2] & 0xFF) << 16 | (body[L - 1] & 0xFF) << 24; if (crc != stored) return "LAES block damaged (CRC-32)";
        BRd2 r = new BRd2(body, body.length - 4);
        long magic = r.u(4), ver = r.u(2), hs = r.u(2), base = r.u(4), cbits = r.u(1), ch = r.u(1), nm = r.u(1), cmode = r.u(1), nc = r.u(8); final int csha = r.take(32); r.u(8);
        if (r.fail || magic != 0x3245414CL) return "not an LAES block"; if (ver != 1 || hs != 64) return "unsupported LAES version";
        if ((base != 44100 && base != 48000) || (int) base != baseRate || (int) cbits != core.bits || ch != 2 || cmode > 1 || nm < 1 || nm > 16) return "invalid LAES header";
        if (nc != core.frames()) return "core master length differs from the LAES header";   // compared as uint64: nothing truncated
        { byte[] h = new byte[32]; hashAnyCore(core, h); if (!eq32(h, 0, body, csha)) return "core master differs from the one this LAES block was made for (SHA-256)"; }
        List<long[][][]> levs = new ArrayList<long[][][]>();   // levs[m][c][k]: level k of decoded master m (samples >> w)
        for (long mi = 0; mi < nm; mi++) { MasterInfo I = new MasterInfo();
            I.bits = (int) r.u(1); I.stages = (int) r.u(1); I.w = (int) r.u(1); I.storage = (int) r.u(1); I.refTier = (int) r.u(1); r.u(3); I.rate = (int) r.u(4); r.u(4); final long F64 = r.u(8);
            I.whole = r.u(8); I.frac = (int) r.u(4); r.u(4); final int sha = r.take(32);
            if (r.fail) return "LAES block truncated";
            if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int) mi) || (I.storage == 2 && I.refTier < 1)
                || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 == 0 || U.gt((F64 + (1L << I.stages) - 1) >>> I.stages, nc + (1L << 24))   // all in uint64
                || I.whole <= -(1L << 40) || I.whole >= (1L << 40)) return "invalid master record";
            if (U.gt(F64, WHOLE_MAX_FRAMES)) return "master too long for this decoder"; I.frames = F64;
            System.arraycopy(body, sha, I.sha, 0, 32); final int j = I.storage == 2 ? Math.min(I.stages, info.get(I.refTier - 1).stages) : 0; final long nj = levLen(I.frames, I.stages - j);
            Pcm m = new Pcm(); m.rate = I.rate; m.bits = I.bits; long[][] top = new long[2][];
            for (int c = 0; c < 2; c++) {
                long eb = r.u(4); final int ep = r.take(eb); if (r.fail) return "LAES block truncated";
                boolean[] ok = new boolean[1]; long[] x = Codec.decodeLayer(body, ep, (int) eb, nj, 4096L << j, ok); if (!ok[0]) return "damaged base layer";
                if (I.storage != 1) { final long[] src; int up; if (I.refTier == 0) { src = core.c[c]; up = (I.bits - I.w) - core.bits; } else { src = levs.get(I.refTier - 1)[c][j]; up = (I.bits - I.w) - (info.get(I.refTier - 1).bits - info.get(I.refTier - 1).w); }
                    final long wj = I.whole * (1L << j) + (j != 0 ? ((I.frac & 0xFFFFFFFFL) >>> (32 - j)) : 0); final int fj = (int) ((I.frac & 0xFFFFFFFFL) << j);   // T in level-j samples
                    long[] rr = refLen(src, nj, wj, fj, up); for (int i = 0; i < nj; i++) x[i] += rr[i]; }
                for (int k = j + 1; k <= I.stages; k++) { long nh = levLen(I.frames, I.stages - k); long db = r.u(4); final int dp = r.take(db); if (r.fail) return "LAES block truncated";
                    long[] d = Codec.decodeLayer(body, dp, (int) db, nh / 2, 4096L << (k - 1), ok); if (!ok[0]) return "damaged octave layer"; x = Lift.inv(x, d, (int) nh); }
                top[c] = x; m.c[c] = new long[(int) I.frames]; for (int i = 0; i < I.frames; i++) m.c[c][i] = x[i] << I.w; }
            long[][][] lv = new long[2][I.stages + 1][];   // every level of this master, for later masters that refer to it
            for (int c = 0; c < 2; c++) { lv[c][I.stages] = top[c]; for (int k = I.stages; k > 0; k--) { lv[c][k - 1] = Lift.fwd(lv[c][k])[0]; } }
            byte[] h = new byte[32]; hashPcm(m, h); shaOk.add(eq32(h, 0, I.sha, 0)); levs.add(lv); info.add(I); out.add(m); }
        if (r.pos != r.n) return "unexpected bytes after the last master";
        return "";
    }
}
