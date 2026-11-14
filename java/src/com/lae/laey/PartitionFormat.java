// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

/**
 * Port of src/partition_format.h (namespace part).
 * LAE partition format: everything a decoder needs to read a partitioned layer - precision rules, reconstruction,
 * zero-level table, mode table and the block geometry (root blocks, binary cut positions, cells).
 * The coefficient plane of one channel (frames x bins k0..k1) is divided into cells of CW bins x 1 frame, grouped into fixed root
 * blocks of ROOTF cells x ROOTT frames; each root is cut recursively (one binary cut at a time, in frequency or time) into leaves.
 * Each leaf carries its own tier (A/B/C/D), exponent, zero-level index and line-wise coefficients.
 */
public final class PartitionFormat {
    private PartitionFormat() {}

    static final int[] TB = {0, 2, 4, 8};   // bits relative to the block exponent for A/B/C/D
    // 16-bit backstop: no quantisation step is coarser than 1 LSB16 (2^8 in LSB24 units), as in the CD layer: everything above the
    // 16-bit level is coded with its waveform.
    static final int STEPMAX = 8;
    private static final int[] TB16 = {0, 0, 4, 8};

    // port of partition_format.h:shiftOfR
    // Precision rules (recorded per stream in the stream header field cw = CW + 256 x rule):
    //   rule 1 (scheme 1, both layers; scheme 2 depth layer): step exponent s = min(E, 8) - TB  -> the finer of "block peak - 2/4/8 bit"
    //          (waveform similarity) and "16-bit base + 2/4/8 bit" (absolute): loud blocks B = 18, C = 20, D = 24 bit
    //   rule 2 (scheme 2 frequency layer): s = min(E - TB, 8 - {0,0,4,8}) -> loud blocks B = 16, C = 20, D = 24 bit, same relative part
    //   rule 0: container v4 (s = min(8, E - TB)).
    static int shiftOfR(int E, int tier, int rule) {
        if (rule == 1) return Math.max(0, Math.min(E, STEPMAX) - TB[tier]);
        if (rule == 2) return Math.max(0, Math.min(E - TB[tier], STEPMAX - TB16[tier]));
        if (rule == 3) return Math.max(0, Math.min(E, STEPMAX - 2) - TB[tier]);   // keep20 depth layer: B 20 / C 22 / D 24 bit
        return Math.min(STEPMAX, Math.max(0, E - TB[tier])); }

    static final double RECOFF = 0.20;   // reconstruction offset of non-zero values, (|q| + 0.20) x step: a format constant
    // port of partition_format.h:recOff
    static double recOff() { return RECOFF; }
    // Zero level: every leaf carries a 3-bit zero-level index. 0 = zero coefficients stay 0;
    // k > 0 = each zero coefficient carries its TRUE sign (1 bit) and is rebuilt as sign x ZC[k] x step. No noise is generated anywhere.
    // The encoder writes k > 0 only with the opt-in "zs" (zero sign) option; ZC is dyadic (exact in binary).
    static final double[] ZC = {0.0, 1.0 / 32, 1.0 / 16, 3.0 / 32, 1.0 / 8, 3.0 / 16, 1.0 / 4, 3.0 / 8};
    // port of partition_format.h:recon
    static double recon(int qi, double st) { return qi != 0 ? (qi + (qi > 0 ? recOff() : -recOff())) * st : 0.0; }
    static final int CW = 8;                  // cell width in bins
    static final int ROOTF = 16, ROOTT = 8;   // root block: 16 cells x 8 frames (same for every mode)

    /** port of partition_format.h:Mode */
    public static final class Mode { public final String name; public final double dShare, cShare;
        Mode(String n, double d, double c) { name = n; dShare = d; cShare = c; } }
    // 6 modes per scheme; every adjacent pair is >= 5.8 % apart in size in all structured worst cases (theory).
    // keep20 = keep with the 20-bit depth layer (opt-in, larger file). Index: 0 max, 1 "1", 2 "3", 3 "5", 4 keep, 5 keep20.
    public static final int NMODES = 6, MODE_KEEP = 4, MODE_KEEP20 = 5;
    public static final Mode[] MODES = {new Mode("max", 0.10, 0.30), new Mode("1", 0.30, 0.50), new Mode("3", 0.50, 0.70), new Mode("5", 0.70, 0.90),
        new Mode("keep", 0.90, 1.00), new Mode("keep20", 0.90, 1.00)};
    // port of partition_format.h:modeIndex
    public static int modeIndex(String s) { for (int i = 0; i < NMODES; i++) if (MODES[i].name.equals(s)) return i; return -1; }

    // ---- geometry shared by encoder and decoder
    // port of partition_format.h:cuts (returns the cut positions; the C++ vector)
    // FU21-1 (Java only): the walkers call this twice per node; the arrays depend only on w and equal, so widths up to CUT_TAB come from a
    // table made once (shared and read-only: no caller writes into the result); wider ones (none in valid streams) are made as before
    static int[] cuts(int w, boolean equal) {
        if (w < 2) return EMPTY;
        if (w <= CUT_TAB) return CUTS[equal ? 1 : 0][w];
        return makeCuts(w, equal); }
    private static int[] makeCuts(int w, boolean equal) {
        if (w < 2) return EMPTY;
        if (equal) return new int[] {w / 2};
        int[] c = new int[w - 1]; for (int p = 1; p < w; p++) c[p - 1] = p;
        return c; }
    private static final int[] EMPTY = new int[0];
    private static final int CUT_TAB = 64;
    private static final int[][][] CUTS = new int[2][CUT_TAB + 1][];
    static { for (int e = 0; e < 2; e++) for (int w = 0; w <= CUT_TAB; w++) CUTS[e][w] = makeCuts(w, e != 0); }
    // port of partition_format.h:posBits (the C++ loop `(size_t)(1u << b) < n`: n is at most ROOTF - 1 here, so b stays far below 32)
    static int posBits(long n) { int b = 0; while ((long) (1 << b) < n) b++; return b; }
    // port of partition_format.h:cellBins (Plane P reduced to its fields k0, k1; kb returned in kb[0])
    static int cellBins(int k0, int k1, int c, int[] kb) { kb[0] = k0 + c * CW; return Math.min(k1, kb[0] + CW) - kb[0]; }
    // port of partition_format.h:numCells
    static int numCells(int k0, int k1) { return (k1 - k0 + CW - 1) / CW; }
}
