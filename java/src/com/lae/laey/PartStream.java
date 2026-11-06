// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.Cv;

/**
 * Port of src/part_stream.h, decoder side: the binary range decoder, the symbol walk (Walker&lt;Dec&gt;), the CRC-32 and the stream
 * header. The encoder side (Enc, Walker&lt;Enc&gt;, the token store, the tier statistics) is not part of the decoder port.
 * Range coding of partitioned layers: coder + the symbol walk shared by part_coder (verification) and lae_dec (standalone decoder).
 * <p>
 * C++ thread_local g_streamErr: "decoder: set when a decoded symbol is out of range (corrupt stream); values are clamped, memory is
 * never overrun; per thread (a thread decodes one stream at a time)". Here it is an {@link Err} object owned by one decoder and passed
 * to everything that sets it (one decoder is used by one thread at a time, as lae_api.h requires).
 */
public final class PartStream {
    private PartStream() {}

    /** the stream-error flag (C++ thread_local g_streamErr), one per decoder */
    public static final class Err { public boolean v; }

    // Binary range coder. Interval [low, low + width) on 32 bits; a symbol splits the width at split = (width >> 11) * p0, where p0 is the
    // 11-bit probability of a 0 (adapted by 1/32 of the distance to 0 or 2048). The width is renormalised to >= 2^24 one byte at a time.
    // Stream: a leading 0x00 byte, then one byte per renormalisation, then 4 bytes at the end.
    static final long RC_TOP = 1L << 24;

    // port of part_stream.h:rcAdapt (p0: uint16_t held in an int array element)
    static void rcAdapt(int[] p, int k, int b) { int p0 = p[k]; if (b != 0) p[k] = (p0 - (p0 >> 5)) & 0xFFFF; else p[k] = (p0 + ((2048 - p0) >> 5)) & 0xFFFF; }

    /** port of part_stream.h:Dec (uint32_t width and code are held as long values in [0, 2^32)) */
    static final class Dec {
        byte[] p; int off; long n, i = 0; long width = 0xFFFFFFFFL, code = 0;   // code = value - low; p[off ..] = the C++ pointer p
        // the C++ default copy (Mark keeps a copy of the decoder state)
        Dec copy() { Dec d = new Dec(); d.p = p; d.off = off; d.n = n; d.i = i; d.width = width; d.code = code; return d; }
        // port of part_stream.h:Dec::next
        int next() { return i < n ? p[off + (int) (i++)] & 0xFF : 0; }
        // port of part_stream.h:Dec::init
        void init() { for (int k = 0; k < 5; k++) code = ((code << 8) | next()) & 0xFFFFFFFFL; }
        // port of part_stream.h:Dec::normalise
        void normalise() { while (width < RC_TOP) { width = (width << 8) & 0xFFFFFFFFL; code = ((code << 8) | next()) & 0xFFFFFFFFL; } }
        // port of part_stream.h:Dec::bit
        int bit(int[] pr, int k) { long split = ((width >>> 11) * pr[k]) & 0xFFFFFFFFL; int b = code >= split ? 1 : 0;
            if (b != 0) { code -= split; width = (width - split) & 0xFFFFFFFFL; } else width = split;
            rcAdapt(pr, k, b); normalise(); return b; }
        // port of part_stream.h:Dec::direct
        int direct(int n) { int v = 0; while (n-- > 0) { width >>>= 1; int b = code >= width ? 1 : 0; if (b != 0) code -= width; v = (v << 1) | b; normalise(); } return v; }
    }

    // port of part_stream.h:blen
    static int blen(int v) { int n = 0; while (v != 0) { n++; v >>>= 1; } return n; }

    /** port of part_stream.h:UModel */
    static final class UModel { int[] tree = fill(64, 1024), mant = fill(64, 1024);
        UModel copy() { UModel m = new UModel(); m.tree = tree.clone(); m.mant = mant.clone(); return m; } }
    static int[] fill(int n, int v) { int[] a = new int[n]; java.util.Arrays.fill(a, v); return a; }

    // port of part_stream.h:codeU (decoder instantiation: returns v, a uint32_t value as int bits)
    static int codeU(Dec c, UModel m) {
        int node = 1;
        for (int b = 4; b >= 0; b--) { int bit = c.bit(m.tree, node); node = node * 2 + bit; }
        int len = node - 32;
        if (len <= 1) return len;
        int top = c.bit(m.mant, len);
        int rest = 0; int nr = len - 2;
        if (nr != 0) rest = c.direct(nr);
        return (1 << (len - 1)) | (top << nr) | rest; }

    // port of part_stream.h:crc32
    // CRC-32 (IEEE 802.3 polynomial, reflected, the common public-domain table algorithm) for the LAE chunk integrity check
    private static final int[] CRC_T = new int[256];
    static { for (int i = 0; i < 256; i++) { int c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; CRC_T[i] = c; } }
    public static int crc32(byte[] p, int off, int n) {
        int c = 0xFFFFFFFF; for (int i = 0; i < n; i++) c = CRC_T[(c ^ p[off + i]) & 0xFF] ^ (c >>> 8); return c ^ 0xFFFFFFFF; }

    /** port of part_stream.h:Hdr: bins k0..k1-1 of an N-bin plane; lv = mode index (part::MODES) */
    static final class Hdr { long K; int N, k0, k1, rate; long L; int cw, lv, eq; }

    /**
     * port of part_stream.h:Walker&lt;Dec&gt;.
     * Walks the trees of the roots, one row of roots (ROOTT frames) per call, so a decoder can stream: the state (adaptive models,
     * previous tier and exponent, next row) persists between calls. dec: decodes tokens into the plane.
     * The plane is written at frame index (m - frame0): frame0 = 0 for a whole-track plane, frame0 = first frame of the row for a row buffer.
     * The C++ token store s (tokens.push_back) is not kept: the decoder never reads it back.
     */
    static final class Walker {
        final Hdr h; final int nc; long M0 = 0;
        UModel[] coefM, expM; int[] tierP, zlP, expZero, expSign; int prevTier = 0, prevE = 12;
        final Err err;
        // row-local state of the C++ lambdas
        private Dec c; private int[] plane; private long frame0; private int C0; private final int[] kbOut = new int[1];

        // port of part_stream.h:Walker::Walker(const Hdr&)
        Walker(Hdr hh, Err e) { h = hh; err = e; nc = PartitionFormat.numCells(hh.k0, hh.k1);
            coefM = new UModel[3 * 13]; for (int i = 0; i < coefM.length; i++) coefM[i] = new UModel();
            expM = new UModel[4]; for (int i = 0; i < expM.length; i++) expM[i] = new UModel();
            tierP = fill(4 * 4, 1024); zlP = fill(4 * 8, 1024); expZero = fill(4, 1024); expSign = fill(4, 1024); }
        // port of part_stream.h:Walker::Walker(const Hdr&, const Walker&): the state of another walker of the same stream, bound to hh
        Walker(Hdr hh, Walker o) { h = hh; err = o.err; nc = o.nc; M0 = o.M0;
            coefM = new UModel[o.coefM.length]; for (int i = 0; i < coefM.length; i++) coefM[i] = o.coefM[i].copy();
            expM = new UModel[o.expM.length]; for (int i = 0; i < expM.length; i++) expM[i] = o.expM[i].copy();
            tierP = o.tierP.clone(); zlP = o.zlP.clone(); expZero = o.expZero.clone(); expSign = o.expSign.clone(); prevTier = o.prevTier; prevE = o.prevE; }
        // the C++ default copy constructor (keeps the other's header)
        Walker(Walker o) { this(o.h, o); }
        // port of part_stream.h:Walker::done
        boolean done() { return M0 >= h.K; }
        // port of part_stream.h:Walker::rowFrames
        int rowFrames() { return (int) Math.min(PartitionFormat.ROOTT, h.K - M0); }

        // port of part_stream.h:Walker::row (decoder: enc = false)
        void row(Dec cc, int[] pl, long f0) {
            c = cc; plane = pl; frame0 = f0;
            for (C0 = 0; C0 < nc; C0 += PartitionFormat.ROOTF) {
                int rf = Math.min(PartitionFormat.ROOTF, nc - C0), rt = (int) Math.min(PartitionFormat.ROOTT, h.K - M0);
                node(0, rf, 0, rt); }
            M0 += PartitionFormat.ROOTT;
            c = null; plane = null; }

        // port of part_stream.h:Walker::row lambda tree (decoder)
        private int tree(int[] pr, int base, int nb) { int node = 1; for (int b = nb - 1; b >= 0; b--) { int bit = c.bit(pr, base + node); node = node * 2 + bit; } return node - (1 << nb); }

        // port of part_stream.h:Walker::row lambda node (decoder)
        private void node(int f0, int f1, int t0, int t1) {
            int[] cf = PartitionFormat.cuts(f1 - f0, h.eq != 0), ct = PartitionFormat.cuts(t1 - t0, h.eq != 0);
            int split = 0;
            if (cf.length != 0 || ct.length != 0) split = c.direct(1);
            if (split != 0) { int dir = ct.length == 0 ? 0 : (cf.length == 0 ? 1 : c.direct(1));
                int[] cc = dir != 0 ? ct : cf; int nb = PartitionFormat.posBits(cc.length); int pi = nb != 0 ? c.direct(nb) : 0; if (pi >= cc.length) { err.v = true; pi = cc.length - 1; } int p = cc[pi];
                if (dir == 0) { node(f0, f0 + p, t0, t1); node(f0 + p, f1, t0, t1); } else { node(f0, f1, t0, t0 + p); node(f0, f1, t0 + p, t1); }
                return; }
            int tier = tree(tierP, prevTier * 4, 2); prevTier = tier;
            if (tier == 0) return;
            // exponent: delta to the previous coded leaf
            int d;   // C++ (decoding): E = 0, d = E - prevE, then z and d are decoded
            int z = c.bit(expZero, tier);
            if (z == 0) { int sg = c.bit(expSign, tier);
                int a = codeU(c, expM[tier]); d = sg != 0 ? -(a + 1) : (a + 1); } else d = 0;
            if (prevE + d < 0 || prevE + d > 34) { err.v = true; d = Math.max(0, Math.min(34, prevE + d)) - prevE; }   // exponents beyond 24-bit range: corrupt
            int E = prevE + d; prevE = E;
            int zl = tree(zlP, tier * 8, 3);
            int sh = PartitionFormat.shiftOfR(E, tier, (h.cw >> 8) & 255); double st = Cv.ldexp(1.0, sh); UModel cm = coefM[(tier - 1) * 13 + Math.min(12, Math.max(0, E / 2))];
            for (int m = (int) (M0 + t0); m < M0 + t1; m++) for (int cc = C0 + f0; cc < C0 + f1; cc++) { int nb = PartitionFormat.cellBins(h.k0, h.k1, cc, kbOut); int kb = kbOut[0];
                for (int j = 0; j < nb; j++) { int q; int a = codeU(c, cm);
                    if (a != 0) { int sg = c.direct(1); q = sg != 0 ? -a : a; } else q = 0;
                    int ix = (int) ((m - frame0) * h.N + kb + j);
                    int zs = 0; if (q == 0 && zl != 0) zs = c.direct(1);   // zero level: the true sign of a zero coefficient (bypass bit)
                    double v = q != 0 ? PartitionFormat.recon(q, st) : (zl != 0 ? (zs != 0 ? -1.0 : 1.0) * PartitionFormat.ZC[zl] * st : 0.0);
                    if (v > 1073741823.0 || v < -1073741823.0) { err.v = true; v = v > 0 ? 1073741823.0 : -1073741823.0; }   // beyond any 24-bit signal: corrupt
                    plane[ix] = (int) Cv.llround(v); } } }
    }
}
