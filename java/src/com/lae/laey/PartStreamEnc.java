// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.Cv;

/**
 * Port of src/part_stream.h, encoder side: the binary range encoder (Enc), codeU and the symbol walk Walker&lt;C&gt; with its token
 * store, for both instantiations the block-wise part coder uses: Walker&lt;Enc&gt; (reads tokens and codes them) and Walker&lt;Dec&gt;
 * (decodes tokens into a token store, for the encoder's own check). The decoder-only port ({@link PartStream}) keeps no tokens; this
 * class is the template as written, with `enc` deciding what the C++ `if constexpr (is_same&lt;C, Enc&gt;)` and the `enc` argument decide.
 * Not ported: the tier statistics g_tierBytes / g_tierCoef (printed only with TIERSTAT, never in the file).
 * <p>
 * Binary range coder. Interval [low, low + width) on 32 bits; a symbol splits the width at split = (width &gt;&gt; 11) * p0, where p0 is the
 * 11-bit probability of a 0 (adapted by 1/32 of the distance to 0 or 2048). The width is renormalised to &gt;= 2^24 one byte at a time.
 * Stream: a leading 0x00 byte, then one byte per renormalisation, then 4 bytes at the end.
 */
final class PartStreamEnc {
    private PartStreamEnc() {}

    static final long M32 = 0xFFFFFFFFL;

    /** port of part_stream.h:Enc (uint32_t low and width held as long values in [0, 2^32)) */
    static final class Enc { byte[] out = new byte[1 << 12]; int size = 1; long low = 0, width = 0xFFFFFFFFL;   // out{ 0 }
        void push(int b) { if (size == out.length) out = java.util.Arrays.copyOf(out, out.length * 2); out[size++] = (byte) b; }
        // port of part_stream.h:Enc::add
        // adding to low can overflow 2^32: the overflow is added to the bytes already written (a run of 0xFF bytes becomes 0x00)
        void add(long v) { long before = low; low = (low + v) & M32; if (low < before) { int k = size; while (out[--k] == (byte) 0xFF) out[k] = 0; out[k]++; } }
        // port of part_stream.h:Enc::normalise
        void normalise() { while (width < PartStream.RC_TOP) { push((int) (low >>> 24)); low = (low << 8) & M32; width = (width << 8) & M32; } }
        // port of part_stream.h:Enc::bit
        void bit(int[] p, int k, int b) { long split = ((width >>> 11) * p[k]) & M32;
            if (b != 0) { add(split); width = (width - split) & M32; } else width = split;
            PartStream.rcAdapt(p, k, b); normalise(); }
        // port of part_stream.h:Enc::direct (v: uint32_t bits)
        void direct(int v, int n) { while (n-- > 0) { width >>>= 1; if (((v >>> n) & 1) != 0) add(width); normalise(); } }
        // port of part_stream.h:Enc::flush
        void flush() { for (int k = 0; k < 4; k++) { push((int) (low >>> 24)); low = (low << 8) & M32; } } }

    /** the C++ token store S: operator[] (encoder: read in order) and push_back (decoder) */
    interface Tokens { int get(long i); void push(int v); }

    // port of part_stream.h:codeU (both instantiations; v a uint32_t value as int bits; returns v)
    static int codeU(boolean enc, Enc ce, PartStream.Dec cd, PartStream.UModel m, int v) {
        int len = enc ? PartStream.blen(v) : 0; int node = 1;
        for (int b = 4; b >= 0; b--) { int bit = enc ? (len >> b) & 1 : 0; if (enc) ce.bit(m.tree, node, bit); else bit = cd.bit(m.tree, node); node = node * 2 + bit; }
        if (!enc) len = node - 32;
        if (len <= 1) { if (!enc) v = len; return v; }
        int top = enc ? (v >>> (len - 2)) & 1 : 0;
        if (enc) ce.bit(m.mant, len, top); else top = cd.bit(m.mant, len);
        int rest = 0; int nr = len - 2;
        if (enc) { rest = v & ((1 << nr) - 1); if (nr != 0) ce.direct(rest, nr); } else { if (nr != 0) rest = cd.direct(nr); v = (1 << (len - 1)) | (top << nr) | rest; }
        return v; }

    /**
     * port of part_stream.h:Walker&lt;C&gt;: walks the trees of the roots, one row of roots (ROOTT frames) per call; the state (adaptive
     * models, previous tier and exponent, next row) persists between calls. enc: reads tokens from s and codes them; dec: decodes tokens into s.
     * The plane is written at frame index (m - frame0).
     */
    static final class Walker {
        final PartStream.Hdr h; final int nc; long M0 = 0; final boolean enc;
        final PartStream.UModel[] coefM, expM; final int[] tierP, zlP, expZero, expSign; long pos = 0; int prevTier = 0, prevE = 12;
        boolean streamErr = false;   // C++ g_streamErr (set when a decoded symbol is out of range)
        private Enc ce; private PartStream.Dec cd; private Tokens s; private int[] plane; private long frame0; private int C0; private final int[] kbo = new int[1];
        // port of part_stream.h:Walker::Walker(const Hdr&)
        Walker(PartStream.Hdr hh, boolean isEnc) { h = hh; enc = isEnc; nc = PartitionFormat.numCells(hh.k0, hh.k1);
            coefM = new PartStream.UModel[3 * 13]; for (int i = 0; i < coefM.length; i++) coefM[i] = new PartStream.UModel();
            expM = new PartStream.UModel[4]; for (int i = 0; i < expM.length; i++) expM[i] = new PartStream.UModel();
            tierP = PartStream.fill(4 * 4, 1024); zlP = PartStream.fill(4 * 8, 1024); expZero = PartStream.fill(4, 1024); expSign = PartStream.fill(4, 1024); }
        // port of part_stream.h:Walker::done
        boolean done() { return M0 >= h.K; }
        // port of part_stream.h:Walker::rowFrames
        int rowFrames() { return (int) Math.min(PartitionFormat.ROOTT, h.K - M0); }

        // port of part_stream.h:Walker::row
        void row(Enc e, PartStream.Dec d, Tokens st, int[] pl, long f0) {
            ce = e; cd = d; s = st; plane = pl; frame0 = f0;
            for (C0 = 0; C0 < nc; C0 += PartitionFormat.ROOTF) {
                int rf = Math.min(PartitionFormat.ROOTF, nc - C0), rt = (int) Math.min(PartitionFormat.ROOTT, h.K - M0);
                node(0, rf, 0, rt); }
            M0 += PartitionFormat.ROOTT;
            ce = null; cd = null; s = null; plane = null; }

        // port of part_stream.h:Walker::row lambda tok
        private int tok(int v) { if (enc) return s.get(pos++); s.push(v); pos++; return v; }
        private int cur() { return enc ? s.get(pos) : 0; }
        // port of part_stream.h:Walker::row lambda bits
        private int bits(int v, int n) { if (enc) { ce.direct(v, n); return v; } else return cd.direct(n); }
        // port of part_stream.h:Walker::row lambda tree (pr = &arr[base])
        private int tree(int[] arr, int base, int v, int nb) { int node = 1; for (int b = nb - 1; b >= 0; b--) { int bit = enc ? (v >> b) & 1 : 0;
                if (enc) ce.bit(arr, base + node, bit); else bit = cd.bit(arr, base + node); node = node * 2 + bit; } return node - (1 << nb); }

        // port of part_stream.h:Walker::row lambda node
        private void node(int f0, int f1, int t0, int t1) {
            int[] cf = PartitionFormat.cuts(f1 - f0, h.eq != 0), ct = PartitionFormat.cuts(t1 - t0, h.eq != 0);
            int split = 0;
            if (cf.length != 0 || ct.length != 0) split = tok(bits(cur(), 1));
            if (split != 0) { int dir = ct.length == 0 ? 0 : (cf.length == 0 ? 1 : tok(bits(cur(), 1)));
                int[] cc = dir != 0 ? ct : cf; int nb = PartitionFormat.posBits(cc.length); int pi = tok(nb != 0 ? bits(cur(), nb) : 0); if (pi >= cc.length) { streamErr = true; pi = cc.length - 1; } int p = cc[pi];
                if (dir == 0) { node(f0, f0 + p, t0, t1); node(f0 + p, f1, t0, t1); } else { node(f0, f1, t0, t0 + p); node(f0, f1, t0 + p, t1); }
                return; }
            int tier = tok(tree(tierP, prevTier * 4, cur(), 2)); prevTier = tier;
            if (tier == 0) return;
            // exponent: delta to the previous coded leaf
            int E = cur(); int d = E - prevE, z = d == 0 ? 1 : 0;
            if (enc) ce.bit(expZero, tier, z); else z = cd.bit(expZero, tier);
            if (z == 0) { int sg = enc ? (d < 0 ? 1 : 0) : 0; if (enc) ce.bit(expSign, tier, sg); else sg = cd.bit(expSign, tier);
                int a = enc ? Math.abs(d) - 1 : 0; a = codeU(enc, ce, cd, expM[tier], a); if (!enc) d = sg != 0 ? -(a + 1) : (a + 1); } else d = 0;
            if (!enc && (prevE + d < 0 || prevE + d > 34)) { streamErr = true; d = Math.max(0, Math.min(34, prevE + d)) - prevE; }   // exponents beyond 24-bit range: corrupt
            E = tok(prevE + d); prevE = E;
            int zl = tok(tree(zlP, tier * 8, cur(), 3));
            int sh = PartitionFormat.shiftOfR(E, tier, (h.cw >> 8) & 255); double stp = Cv.ldexp(1.0, sh); PartStream.UModel cm = coefM[(tier - 1) * 13 + Math.min(12, Math.max(0, E / 2))];
            for (int m = (int) (M0 + t0); m < M0 + t1; m++) for (int cc = C0 + f0; cc < C0 + f1; cc++) { int nb = PartitionFormat.cellBins(h.k0, h.k1, cc, kbo); int kb = kbo[0];
                for (int j = 0; j < nb; j++) { int q = cur(); int a = Math.abs(q); a = codeU(enc, ce, cd, cm, a);
                    if (a != 0) { int sg = enc ? (q < 0 ? 1 : 0) : 0; if (enc) ce.direct(sg, 1); else sg = cd.direct(1); if (!enc) q = sg != 0 ? -a : a; } else q = 0;
                    tok(q); int ix = (int) ((m - frame0) * h.N + kb + j);
                    int zs = 0; if (q == 0 && zl != 0) zs = tok(bits(cur(), 1));   // zero level: the true sign of a zero coefficient (bypass bit)
                    double v = q != 0 ? PartitionFormat.recon(q, stp) : (zl != 0 ? (zs != 0 ? -1.0 : 1.0) * PartitionFormat.ZC[zl] * stp : 0.0);
                    if (v > 1073741823.0 || v < -1073741823.0) { streamErr = true; v = v > 0 ? 1073741823.0 : -1073741823.0; }   // beyond any 24-bit signal: corrupt
                    plane[ix] = (int) Cv.llround(v); } } }
    }
}
