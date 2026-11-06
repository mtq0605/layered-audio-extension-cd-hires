// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.core.AlignHash;
import com.lae.core.Cv;
import com.lae.core.LMath;
import java.util.ArrayList;
import java.util.List;

/**
 * Port of src/cd_align.cpp (namespace m_align of lae_enc.cpp). Alignment of an independent CD against the master (encoder side), design
 * D (2026-10-22): run() finds where the CD lies in the master and checks it, and changes nothing; place() / applyPlaceFile() / cdOnly()
 * then move the master by whole master samples across digital silence (the CD is never changed). The last search level and the check run
 * the master through the encoder's own CD derivation filter (m_base::decimate = {@link BaseLayer#decimate}), so no resampling is involved.
 *   search, waveform only, coarse to fine in a fixed cascade (GCC-PHAT at level 0 over two halves, then halved blocks and doubled rates
 *   around the previous peak, the last level at the master's own rate); check: three blocks of the CD's sound at that one offset.
 * Every value is printed only. Nothing is stored in the file, transmitted, or used to change the CD, T or F.
 * std::complex (MSVC 14.44 &lt;complex&gt;): only component-wise operations are used besides the written-out mulConj / absC: conj, complex /
 * double and complex * double (both components divided / multiplied), += (component-wise), norm = re re + im im. Held as two arrays.
 * The master M is the raw file through {@link BlkIo.View}; the vector-based applyPlace / advance / moveMaster (tests of the C++ only)
 * are not ported.
 */
final class CdAlign {
    private CdAlign() {}

    static final int BAND_OK = 0, BAND_CONFIRM = 1, BAND_CONFIRM_LARGE = 2, BAND_STOP = 3;
    static final int EDGE_STOP = 0, EDGE_PAD = 1, EDGE_CUT = 2;

    /** port of cd_align.cpp:Report */
    static final class Report { int verdict = 2; List<String> lines = new ArrayList<String>(); String alignLine = "", why = "", tag = "", hint = ""; long cdOffset = 0, masterOffset = 0, placeAt = 0;
        boolean placeable = false, found = false, mapDiff = false; int band = BAND_STOP; double timingDb = 99, residDb = 0; long bestNear = 0;
        double frac = 0; int chanMap = 0; List<long[]> blocks = new ArrayList<long[]>();
        Report copy() { Report r = new Report(); r.verdict = verdict; r.lines = new ArrayList<String>(lines); r.alignLine = alignLine; r.why = why; r.tag = tag; r.hint = hint; r.cdOffset = cdOffset; r.masterOffset = masterOffset; r.placeAt = placeAt;
            r.placeable = placeable; r.found = found; r.mapDiff = mapDiff; r.band = band; r.timingDb = timingDb; r.residDb = residDb; r.bestNear = bestNear; r.frac = frac; r.chanMap = chanMap; r.blocks = new ArrayList<long[]>(blocks); return r; } }

    // ---- namespace detail
    // port of cd_align.cpp:detail::fmt (the C++ formats into a 1024-byte buffer: longer text is cut at 1023 characters)
    static String fmt(String f, Object... a) { String s = Fmt.c(f, a); return s.length() > 1023 ? s.substring(0, 1023) : s; }
    // port of cd_align.cpp:detail::ifft (conj, fft, conj * (1 / n))
    static void ifft(double[] re, double[] im, int n) { for (int i = 0; i < n; i++) im[i] = -im[i]; Fft.fft(re, im, n); final double s = 1.0 / (double) n; for (int i = 0; i < n; i++) { re[i] = re[i] * s; im[i] = -im[i] * s; } }
    // port of cd_align.cpp:detail::pow2
    static long pow2(long n) { long p = 1; while (p < n) p <<= 1; return p; }
    // port of cd_align.cpp:detail::db10
    static double db10(double v) { return v > 0 ? 10.0 * LMath.log10(v) : -300.0; }
    // port of cd_align.cpp:detail::dur
    static String dur(double sec) { double a = Math.abs(sec); return a < 1e-3 ? fmt("%.1f us", sec * 1e6) : a < 1.0 ? fmt("%.2f ms", sec * 1e3) : fmt("%.3f s", sec); }
    // port of cd_align.cpp:detail::fdiv (floor division)
    static long fdiv(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
    // port of cd_align.cpp:detail::Th: the user's limits (2026-10-21/22) and the check blocks (2026-10-22)
    static final double timingErrDb = -40, timingGainDb = 1, confirmDb = -33, matchResidDb = -3, quietDbfs = -60, blockSec = 5, edgeSec = 10, minSpanSec = 40, singleSec = 3;
    // port of cd_align.cpp:detail::lp2: one fixed linear low-pass for each halving of the search pyramid (windowed sinc, Kaiser beta 8, 47 taps, cut-off 0.22)
    private static double[] LP2;
    static synchronized double[] lp2() { if (LP2 != null) return LP2; final int K = 23; final double fc = 0.22, beta = 8.0; double[] v = new double[2 * K + 1];
        double b0 = I0(beta), sum = 0; for (int k = -K; k <= K; k++) { double w = I0(beta * Math.sqrt(1.0 - (k * (double) k) / ((double) K * K))) / b0, s = k == 0 ? 2 * fc : LMath.sinpi(2 * fc * k) / (Math.PI * k); v[k + K] = s * w; sum += s * w; }
        for (int i = 0; i < v.length; i++) v[i] /= sum; LP2 = v; return v; }
    // port of cd_align.cpp:detail::lp2 lambda I0
    private static double I0(double z) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (z / 2) / k; s += t * t; } return s; }
    // port of cd_align.cpp:detail::dec2
    static double[] dec2(double[] x) { final double[] h = lp2(); final long K = (h.length - 1) / 2, n = x.length, m = n / 2; double[] y = new double[(int) m];
        for (long i = 0; i < m; i++) { double a = 0; final long c = 2 * i; for (long k = -K; k <= K; k++) { final long j = c - k; if (j >= 0 && j < n) a += h[(int) (k + K)] * x[(int) j]; } y[(int) i] = a; }
        return y; }

    /** the master's samples as AlignHash.Samples (sample i = frame i / 2, channel i % 2) */
    static AlignHash.Samples samples(final BlkIo.View m) { return new AlignHash.Samples() { @Override public long get(long i) { return m.get(i); } }; }
    static AlignHash.Samples samples(final short[] s) { return new AlignHash.Samples() { @Override public long get(long i) { return s[(int) i]; } }; }

    /** the check block of run() (C++ struct Blk) */
    static final class Blk { long a = 0, b = 0; boolean use = false; double X2 = 0; double[][] E = new double[2][]; double[][][] g = new double[2][2][]; }

    // port of cd_align.cpp:run
    // cd: the given CD (interleaved int16, ncd frames). b24: the encoder's derived 24-bit base of the master (interleaved, nb frames, padded by
    // pb at both ends). master: the unpadded master (interleaved, 24-bit left-aligned int32, Lm frames). Convention: CD[j] ~ base[j - o]
    // (o > 0: the CD starts later); in master samples t. hintAt: where this master already sits (the re-check of a placed master). follows:
    // appended to a result the encoder acts on. lagCentre / rangeCd: the search covers CD offsets lagCentre +- rangeCd (< 0: everything).
    // fixedT: no search; the check runs at this master-sample offset. whiten: GCC-PHAT, or the plain normalised cross-spectrum.
    static Report run(final short[] cd, final long ncd, final int[] b24, long nb, long pb, final BlkIo.View master, final long Lm, final int base, final int stages,
                      long hintAt, String follows, long lagCentre, long rangeCd, Long fixedT, boolean whiten) {
        final Report R = new Report();
        final long need = nb - 2 * pb; final boolean lengthOk = ncd == need; final int up = 1 << stages;
        final String md5 = AlignHash.cdMd5(cd, ncd);
        final double[][] x = new double[2][], r = new double[2][];
        for (int c = 0; c < 2; c++) { x[c] = new double[(int) ncd]; r[c] = new double[(int) need];
            for (long j = 0; j < ncd; j++) x[c][(int) j] = 256.0 * cd[(int) (j * 2 + c)];
            for (long i = 0; i < need; i++) r[c][(int) i] = (double) b24[(int) ((pb + i) * 2 + c)]; }
        R.lines.add(fmt("CD %ld frames (%.3f s), master needs %ld frames at %d Hz (%s); CD MD5 %s", ncd, ncd / (double) base, need, base, lengthOk ? "same length" : "different length", md5));
        long[] ce = AlignHash.silenceEdges(samples(cd), ncd), me = AlignHash.silenceEdges(samples(master), Lm); final long cdLead = ce[0], cdTail = ce[1], mLead = me[0], mTail = me[1];
        if (cdLead == ncd) { if (mLead == Lm) { R.found = true; return finish(R, lengthOk ? 1 : 2, "both_silent", "CD and master are digital silence throughout: nothing to align", lengthOk, md5); }
            if (mLead >= Lm - (up - 1) || mTail >= Lm - (up - 1)) { R.found = true;   // the master's sound lies within one CD sample of an edge: a neighbouring piece's, coded by the extension
                return finish(R, lengthOk ? 1 : 2, "both_silent", fmt("the CD is digital silence throughout; the master has sound only in its %s %ld samples (less than one CD sample, a neighbouring piece's): nothing to align",
                                                                     mLead >= Lm - (up - 1) ? "last" : "first", (mLead >= Lm - (up - 1) ? Lm - mLead : Lm - mTail)), lengthOk, md5); }
            if (AlignHash.allZero(samples(master), (long) up - 1, Lm - (up - 1))) { R.found = true;   // sound only within less than one CD sample of BOTH edges: the two neighbouring pieces'
                return finish(R, lengthOk ? 1 : 2, "both_silent", fmt("the CD is digital silence throughout; the master has sound only in its first and last %d samples (less than one CD sample at each edge, the neighbouring pieces'): nothing to align", up - 1), lengthOk, md5); }
            return finish(R, 2, "silent_cd", "the CD is digital silence throughout, but the master has content: this is not the CD of this master", lengthOk, md5); }
        final double qa = 8388608.0 * LMath.exp10(quietDbfs / 20), quietE = qa * qa;   // per-sample energy of a -60 dBFS rms signal
        final long s0 = cdLead, s1 = ncd - cdTail;   // the CD's sound
        boolean searchOk = true; long oCd = 0;   // the search's result at the CD rate (CD samples)

        // ---- 1. search (skipped when the offset is given)
        if (fixedT == null) {
            final long R0 = rangeCd >= 0 ? rangeCd : Math.max(ncd, need);
            int L0 = 4; while (L0 < 8 && fftAt(L0, s0, s1, R0) > (1L << 21)) L0++;
            long o = 0;
            if (fftAt(L0, s0, s1, R0) > (1L << 22)) stopWith(R, "toolong", fmt("the CD's sound (%.0f min) and the search range are too long for this version's search", (s1 - s0) / (60.0 * base)),
                                                             "give the offset by hand: cdalign=<offset> (master samples); the check then runs at that offset");
            else {
            final double[][][] px = new double[2][L0][], pr = new double[2][L0][];   // levels 1..L0 (level 0 of the pyramid is x / r itself)
            for (int c = 0; c < 2; c++) for (int l = 1; l <= L0; l++) { px[c][l - 1] = dec2(l == 1 ? x[c] : px[c][l - 2]); pr[c][l - 1] = dec2(l == 1 ? r[c] : pr[c][l - 2]); }
            final Search S = new Search(); S.x = x; S.r = r; S.px = px; S.pr = pr; S.base = base; S.whiten = whiten;
            // level 0: two halves of the CD's sound, the whole range, then (always) the same range once more around that peak
            final long a0 = s0 >> L0, b0 = Math.max(a0 + 2, s1 >> L0), mid = (a0 + b0) / 2; final long Rl0 = (R0 >> L0) + 2, oc0 = fdiv(lagCentre, 1L << L0);
            List<long[]> halves = new ArrayList<long[]>(); halves.add(new long[] {a0, mid}); halves.add(new long[] {mid, b0});
            double[] P = S.pooled(L0, halves, oc0, Rl0, false);
            if (S.used == 0) stopWith(R, "nomatch", "no part where both the CD and the master have sound in the searched range", "not supported: this CD cannot be aligned with this master");
            o = oc0 - Rl0 + peak(P);
            if (R.tag.isEmpty()) { final long o1 = o; P = S.pooled(L0, halves, o1, Rl0, false); final long pk = peak(P); o = o1 - Rl0 + pk;
                R.lines.add(fmt("search level 0 (%s, %.0f Hz, 2 halves, %+.1f..%+.1f s): best %+.3f s; second scan around it: %+.3f s", whiten ? "whitened" : "plain", base / (double) (1L << L0), (oc0 - Rl0) * (double) (1L << L0) / base, (oc0 + Rl0) * (double) (1L << L0) / base,
                        o1 * (double) (1L << L0) / base, o * (double) (1L << L0) / base));
                if (S.used == 0 || pk == 0 || pk == 2 * Rl0) stopWith(R, "notfound", fmt("the CD's position could not be found: the best fit lies on the edge of the search (%+.1f s); not this master's CD, or further away than the search reaches", o * (double) (1L << L0) / base),
                                                                "not supported: this CD cannot be aligned with this master"); }
            // levels 1 .. L0: blocks halved, rate doubled, +-W around the previous peak (equal-length windows); the first refinement level searches +-32, the others +-8
            for (int l = L0 - 1; l >= 0 && R.tag.isEmpty(); l--) { final long W = l == L0 - 1 ? 32 : 8, a = s0 >> l, b = Math.max(a + 2, s1 >> l), oc = 2 * o; long nbk = 2L << (L0 - l);
                while (nbk > 1 && (b - a) / nbk < 64) nbk /= 2; nbk = Math.min(nbk, 64L); List<long[]> bl = new ArrayList<long[]>();
                for (long k = 0; k < nbk; k++) bl.add(new long[] {a + (b - a) * k / nbk, a + (b - a) * (k + 1) / nbk});
                P = S.pooled(l, bl, oc, W, true); final long pk = peak(P); o = oc - W + pk;
                if (S.used == 0 || pk == 0 || pk == 2 * W) stopWith(R, "edge", fmt("the search found no clear position at %.0f Hz (the best fit lies on the edge of its range around %+ld): not this master's CD, or an edit", base / (double) (1L << l), oc),
                                                             "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"); }
            if (R.tag.isEmpty()) R.lines.add(fmt("search levels 1-%d (blocks halved, rate doubled each): %+ld CD samples (%s)", L0, o, dur(o / (double) base))); }
            oCd = o; searchOk = R.tag.isEmpty(); }

        // ---- 2. the three check blocks (CD samples) and the master-rate curves: residual per block at whole-sample offsets t, with one gain per
        // channel, direct and swapped channel order; through the encoder's own derivation (one decimation per phase, then shifts)
        final Blk[] B3 = {new Blk(), new Blk(), new Blk()}; final String[] bname = {"head", "middle", "tail"}; boolean singleBlock = false;
        { final long span = s1 - s0, bl = Cv.d2i(blockSec * base), eg = Cv.d2i(edgeSec * base), third = Math.max(1L, span / 3);
          final boolean single = span < Cv.d2i(singleSec * base); if (single) bname[0] = "whole"; singleBlock = single;
          for (int k = 0; k < 3; k++) { long a, b; if (single && k > 0) { B3[k].use = false; continue; }
              if (single) { a = s0; b = s1; }
              else if (span >= Cv.d2i(minSpanSec * base)) { a = k == 0 ? s0 + eg : (k == 1 ? s0 + span / 2 - bl / 2 : s1 - eg - bl); b = a + bl; }
              else { final long t0 = s0 + k * third, t1 = k == 2 ? s1 : t0 + third, len = Math.min(bl, t1 - t0); a = t0 + (t1 - t0 - len) / 2; b = a + len; }
              final long guard = Math.min(span / 8, 1024L + 4L * up);   // keep the derivation filter's reach and the +-2 CD-sample shifts inside the piece
              a = Math.max(a, s0 + guard); b = Math.min(b, s1 - guard);
              final long lo3 = single ? s0 : s0 + k * third, hi3 = (single || k == 2) ? s1 : s0 + (k + 1) * third, step = Math.max(1L, Math.min(bl, b - a));
              final boolean fixedPos = span >= Cv.d2i(minSpanSec * base);
              while (inside(fixedPos, k, a, b, lo3, hi3, s0, s1, ncd) && !loud(x, a, b, quietE)) { if (k == 2) { a -= step; b -= step; } else { a += step; b += step; } }   // quiet: fixed steps, the anchor inside its third
              B3[k].a = a; B3[k].b = b; B3[k].use = inside(fixedPos, k, a, b, lo3, hi3, s0, s1, ncd) && b - a >= 64 && loud(x, a, b, quietE); } }
        final long cM = fixedT != null ? fixedT : oCd * up;   // master-sample centre
        final long tLo = cM - 2 * up, tHi = cM + 2 * up; final int nT = (int) (tHi - tLo + 1);
        if (searchOk) curves(B3, x, master, Lm, stages, up, tLo, tHi, nT);
        int usable = 0; for (Blk K : B3) if (K.use) usable++;
        if (searchOk && usable == 0) { searchOk = false; stopWith(R, "quiet", "too quiet to check: no check block has sound on both sides (each CD block below -60 dBFS, or outside the master)", "not supported: this CD cannot be aligned with this master"); }
        // pooled residual over the blocks for one channel order, judged once at cM (swapped only when it explains the CD clearly better, 3 dB)
        int map = 0; long t = cM, tb = cM;
        if (searchOk) {
            final int ci = (int) (cM - tLo); map = pooledE(B3, 1, ci) < pooledE(B3, 0, ci) * 0.5 ? 1 : 0;
            // every whole master sample within two CD samples of the CD-rate result; the ends only feed the fraction
            final long uA = cM - 2 * up + 1, uB = cM + 2 * up - 1;
            for (long u = uA; u <= uB; u++) if (pooledE(B3, map, (int) (u - tLo)) < pooledE(B3, map, (int) (tb - tLo))) tb = u;
            if (fixedT == null) { t = tb; if (t == uA || t == uB) { searchOk = false; stopWith(R, "edge", fmt("the master-sample fit lies on the edge of its range around %+lld master samples: not this master's CD, or an edit", cM),
                                                                                         "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"); }
                         else R.lines.add(fmt("search at the master rate (%d Hz): %+lld master samples (%s; %+.2f CD samples)", base * up, t, dur(t / (double) (base * up)), t / (double) up)); }
            R.bestNear = tb - t; }
        R.masterOffset = t; R.cdOffset = Cv.lround32(t / (double) up); R.placeAt = t;

        // ---- 3. the check at t (never an offset per block): residual, and the remaining time difference against the best fit near t
        int band = BAND_OK; double worstT = -300, worstR = -300; int nMis = 0, nDrift = 0; String blocks = ""; double[] gainDb = {0, 0}; int[] pol = {1, 1};
        double dFr = 0; final int bi = (int) (tb - tLo);
        if (searchOk) { final double ea = pooledE(B3, map, bi - 1), eb = pooledE(B3, map, bi), ec = pooledE(B3, map, bi + 1), den = ea - 2 * eb + ec;
            if (den > 0) dFr = Cv.max(-0.5, Cv.min(0.5, 0.5 * (ea - ec) / den)); }
        double worstAsk = -300;   // the largest remaining time difference among the blocks that ask for a confirmation (for the message)
        long diagA = 0, diagN = 0;
        if (searchOk) {
            for (int k = 0; k < 3; k++) { Blk K = B3[k]; if (!K.use) { if (singleBlock && k > 0) continue;   // under 3 s of sound there is one block only
                    blocks += fmt("%s%s:n/a", blocks.isEmpty() ? "" : ",", bname[k]); R.lines.add(fmt("check %s: not usable (quiet, or outside the master)", bname[k])); continue; }
                final int ti = (int) (t - tLo); final double e = K.E[map][ti] / K.X2; final double rd = db10(e);
                final double ea = K.E[map][bi - 1] / K.X2, eb = K.E[map][bi] / K.X2, ec = K.E[map][bi + 1] / K.X2;
                final double eMin = Cv.max(eb + dFr * (ec - ea) / 2 + dFr * dFr * (ec - 2 * eb + ea) / 2, 0.01 * e);
                final double tE = db10(Cv.max(e - eMin, 1e-30)), gE = db10(e / Cv.max(eMin, 1e-30));
                final boolean mis = rd > matchResidDb; final int sd = mis ? 0 : sideCheck(K, t, map, x, master, Lm, stages, up, tLo); final boolean drift = sd == 1; int bb;
                if (mis || drift) bb = BAND_STOP; else if (tE <= timingErrDb || gE <= timingGainDb) bb = BAND_OK; else if (tE <= confirmDb) bb = BAND_CONFIRM; else bb = BAND_CONFIRM_LARGE;
                if (mis) nMis++; if (drift) nDrift++; if (bb == BAND_CONFIRM || bb == BAND_CONFIRM_LARGE) worstAsk = Cv.max(worstAsk, tE);
                band = Math.max(band, bb); worstT = Cv.max(worstT, Cv.max(tE, -99.0)); worstR = Cv.max(worstR, rd);
                blocks += fmt("%s%s:%.1f/%.1f", blocks.isEmpty() ? "" : ",", bname[k], Cv.max(rd, -99.0), Cv.max(tE, -99.0));
                R.lines.add(fmt("check %s (%.1f-%.1f s of the CD): residual %.1f dB re the CD, remaining time difference %.1f dB (aligning the fraction would gain %.1f dB)%s",
                        bname[k], K.a / (double) base, K.b / (double) base, rd, Cv.max(tE, -99.0), gE,
                        mis ? ": DOES NOT MATCH here" : (drift ? "; drift check: the music fits better 2 CD samples away than at the piece's offset (drift, an edit or another take)"
                                                            : (sd < 0 ? "; drift check: not possible here (the master ends too close)" : "; drift check (2 CD samples either side): holds"))));
                if (k == 1 || (diagN == 0 && k == 2) || (diagN == 0 && k == 0)) { for (int c = 0; c < 2; c++) { gainDb[c] = K.g[map][c][ti] != 0 ? 20 * LMath.log10(Math.abs(K.g[map][c][ti])) : -999; pol[c] = K.g[map][c][ti] >= 0 ? 1 : -1; } diagA = K.a; diagN = K.b - K.a; } }
            R.mapDiff = map == 1 || pol[0] < 0 || pol[1] < 0; R.timingDb = worstT; R.residDb = worstR; R.band = band;
            R.frac = (double) (tb - t) + dFr; R.chanMap = map; for (Blk K : B3) if (K.use) R.blocks.add(new long[] {K.a, K.b});
            if (R.mapDiff) R.lines.add(fmt("channels: %s; polarity L %s, R %s", map != 0 ? "SWAPPED (CD left = master right)" : "in order", pol[0] > 0 ? "normal" : "INVERTED", pol[1] > 0 ? "normal" : "INVERTED")); }
        R.found = searchOk && band != BAND_STOP;

        // ---- 4. diagnostics, printed only: level, EQ, coherence, pre-emphasis (middle block), clipping, level over time, edges
        double eqDev = 0, cohEdge = 0; boolean preEmph = false; long clipRuns = 0; boolean onlySilence = true; final long o = R.cdOffset;
        if (R.found && diagN >= 8192) {   // the prediction at the final offset for the diagnostic block, through the same derivation
            final long q = fdiv(t + ((((-t) % up) + up) % up), up); final int ph = (int) ((((-t) % up) + up) % up);
            final long margin = 2048; final long sM = (diagA - q) * up + ph - margin, wM = diagN * up + 2 * margin;
            if (sM >= 0 && sM + wM <= Lm) { double[][] diag = new double[2][];
                for (int c = 0; c < 2; c++) { int[] qq = new int[(int) wM]; for (long i = 0; i < wM; i++) qq[(int) i] = master.get((sM + i) * 2 + c) >> 4;
                    for (int st = stages - 1; st >= 0; st--) qq = BaseLayer.decimate(qq, st);
                    diag[c] = new double[(int) diagN]; for (long k = 0; k < diagN; k++) diag[c][(int) k] = (double) (((long) qq[(int) (margin / up + k)] + 8) >> 4); }
                R.lines.add(fmt("level: CD relative to the master's derived base: L %+.2f dB, R %+.2f dB", gainDb[0], gainDb[1]));
                final int Ns = 8192; double[] hw = new double[Ns]; for (int k = 0; k < Ns; k++) hw[k] = 0.5 - 0.5 * LMath.cospi(2 * (k + 0.5) / Ns);
                double[] Sxr = new double[Ns / 2], Sxi = new double[Ns / 2], Sxx = new double[Ns / 2], Srr = new double[Ns / 2];
                double[] Xr = new double[Ns], Xi = new double[Ns], Yr = new double[Ns], Yi = new double[Ns];
                for (long w0 = 0; w0 + Ns <= diagN; w0 += Ns / 2) for (int a = 0; a < 2; a++) { final double[] rb = diag[map != 0 ? 1 - a : a];
                    for (int k = 0; k < Ns; k++) { Xr[k] = x[a][(int) (diagA + w0 + k)] * hw[k]; Xi[k] = 0; Yr[k] = pol[a] * rb[(int) (w0 + k)] * hw[k]; Yi[k] = 0; } Fft.fft(Xr, Xi, Ns); Fft.fft(Yr, Yi, Ns);
                    for (int k = 0; k < Ns / 2; k++) { Sxr[k] += Xr[k] * Yr[k] + Xi[k] * Yi[k]; Sxi[k] += Xi[k] * Yr[k] - Xr[k] * Yi[k]; Sxx[k] += Xr[k] * Xr[k] + Xi[k] * Xi[k]; Srr[k] += Yr[k] * Yr[k] + Yi[k] * Yi[k]; } }   // mulConj(X, Y), norm
                final double[] bands = {31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
                final int nbd = bands.length; double[] mag = new double[nbd], coh = new double[nbd]; boolean[] ok = new boolean[nbd]; double ref1k = 0;
                for (int b = 0; b < nbd; b++) { int k0 = Cv.d2i(Math.ceil(bands[b] * LMath.exp2(-1.0 / 6) * Ns / base)), k1 = Math.min(Cv.d2i(Math.floor(bands[b] * LMath.exp2(1.0 / 6) * Ns / base)), Ns / 2 - 1);
                    double srr = 0, sri = 0, sx = 0, sy = 0; for (int k = Math.max(k0, 1); k <= k1; k++) { srr += Sxr[k]; sri += Sxi[k]; sx += Sxx[k]; sy += Srr[k]; }
                    if (sy > 0 && sx > 0) { ok[b] = true; mag[b] = 20 * LMath.log10(Math.sqrt(srr * srr + sri * sri) / sy + 1e-300); coh[b] = (srr * srr + sri * sri) / (sx * sy); } if (bands[b] == 1000) ref1k = mag[b]; }
                String eq = ""; double meanEmphErr = 0, meanFlat = 0; int ne = 0;
                for (int b = 0; b < nbd; b++) if (ok[b]) { double rel = mag[b] - ref1k; if (bands[b] <= 16000) eqDev = Cv.max(eqDev, Math.abs(rel));
                    if (bands[b] >= 2000 && bands[b] <= 16000) { meanEmphErr += Math.abs(rel - (emph(bands[b]) - emph(1000))); meanFlat += Math.abs(rel); ne++; }
                    if (bands[b] == 63 || bands[b] == 250 || bands[b] == 1000 || bands[b] == 4000 || bands[b] == 10000 || bands[b] == 16000) eq += fmt(" %g Hz %+.1f dB", bands[b], rel); }
                if (ne != 0) { meanEmphErr /= ne; meanFlat /= ne; preEmph = meanEmphErr < 1.5 && meanFlat > 3.0; }
                for (int b = 0; b < nbd; b++) if (ok[b] && bands[b] >= 1000) { if (coh[b] >= 0.9) cohEdge = bands[b]; else break; }
                R.lines.add(fmt("EQ relative to 1 kHz:%s; largest deviation %.1f dB (31.5 Hz - 16 kHz); coherence >= 0.9 up to %.0f Hz", eq, eqDev, cohEdge));
                if (preEmph) R.lines.add("WARNING: the CD looks pre-emphasised (50/15 us curve). A WAV cannot carry the emphasis flag, so the CD layer would play with the wrong EQ"); } }
        if (R.found) {
            for (int c = 0; c < 2; c++) { int run = 0; for (long j = 0; j < ncd; j++) { short v = cd[(int) (j * 2 + c)]; if (v == 32767 || v == -32768) { if (++run == 3) clipRuns++; } else run = 0; } }
            if (clipRuns != 0) R.lines.add(fmt("clipping: %ld runs of 3 or more full-scale samples in the CD", clipRuns));
            double lvMin = 1e9, lvMax = -1e9; long a0 = Math.max(0L, o), a1 = Math.min(ncd, need + o);
            for (long w0 = a0; w0 + base <= a1; w0 += base) { double sx = 0, sr = 0; for (long j = w0; j < w0 + base; j++) for (int c = 0; c < 2; c++) { sx += x[c][(int) j] * x[c][(int) j]; sr += r[c][(int) (j - o)] * r[c][(int) (j - o)]; }
                if (sr < 2.0 * base * quietE || sx <= 0) continue; double d = db10(sx / sr); lvMin = Cv.min(lvMin, d); lvMax = Cv.max(lvMax, d); }
            if (lvMax >= lvMin) { R.lines.add(fmt("level difference per second: %+.2f to %+.2f dB", lvMin, lvMax)); if (lvMax - lvMin > 3.0) R.lines.add("note: the level difference changes over time by more than 3 dB (fade, compression or a different master)"); }
            final long endCd = ncd - o; List<Object[]> edges = new ArrayList<Object[]>();
            if (o > 0) edges.add(new Object[] {"only the CD has (start)", o, cdLead >= o});
            if (o < 0) edges.add(new Object[] {"only the master has (start)", -o, mLead >= -o * up});
            if (endCd < need) edges.add(new Object[] {"only the master has (end)", need - endCd, Lm - endCd * up <= mTail});
            if (endCd > need) edges.add(new Object[] {"only the CD has (end)", endCd - need, cdTail >= endCd - need});
            for (Object[] e : edges) { long n = (Long) e[1]; boolean silent = (Boolean) e[2]; R.lines.add(fmt("%s: %ld CD frames (%.3f s), %s", e[0], n, n / (double) base, silent ? "digital silence" : "CONTENT")); if (!silent) onlySilence = false; } }

        // ---- 5. decision (the CD and the master are never changed)
        if (searchOk && R.tag.isEmpty()) {
            R.placeable = R.found && (t != 0 || !lengthOk);
            if (band == BAND_STOP) { R.verdict = 2;
                if (nMis > 0 && nMis < usable) { R.tag = "edit"; R.why = fmt("%d of %d check blocks do not match at this offset (residual above -3 dB there): an edit, a different speed (drift) or another take", nMis, usable);
                    R.hint = "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"; }
                else if (nMis == 0) { R.tag = "drift"; R.why = fmt("in %d of %d check blocks the music fits better 2 CD samples away than at the piece's one offset: a different speed (drift), an edit or another take", nDrift, usable);
                    R.hint = "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"; }
                else { R.tag = "nomatch"; R.why = "the CD does not match this master at the best position (residual above -3 dB in every check block)"; R.hint = "not supported: this CD cannot be aligned with this master"; } }
            else if (R.placeable) { R.verdict = 2; R.tag = t != 0 ? "shift" : "length";
                R.why = (t != 0 ? fmt("the CD is shifted against the master by %lld master samples (%s; %.2f CD samples)", t, dur(t / (base * (double) up)), t / (double) up) : "the CD is aligned but " + (onlySilence ? "longer or shorter by digital silence only" : "its length differs and the difference contains content")) + follows;
                R.hint = fmt("set the whole offset by hand: cdalign=%+lld (master samples; a fraction can be added, as cdalign=3.5)", hintAt + t); }
            else if (band == BAND_OK) { R.verdict = R.mapDiff ? 1 : 0; R.tag = R.mapDiff ? "channels_differ" : "aligned";
                R.why = "the CD is aligned with the master" + fmt(" (check blocks: remaining time difference at most %.1f dB re the CD)", worstT) + (R.mapDiff ? ", but the channel order or polarity differs" : ""); }
            else { R.verdict = 2; R.tag = band == BAND_CONFIRM ? "confirm" : "confirm_large";
                R.why = fmt("the remaining time difference changes the waveform by up to %.1f dB re the CD (%s): %s", worstAsk, band == BAND_CONFIRM ? "between -40 and -33 dB" : "beyond -33 dB",
                            band == BAND_CONFIRM ? "close, but not aligned within the limit" : "the CD may have been made differently from this master (another EQ or mastering, a different speed or an edit), and the file may get much larger");
                R.hint = R.bestNear != 0 ? fmt("still shifted by %+ld master samples: try cdalign=%+lld", R.bestNear, hintAt + t + R.bestNear) : "confirm to continue (a console asks; in scripts add cdaccept)"; }
            R.lines.add("result: " + R.why); }
        if (R.verdict != 2 && R.mapDiff) R.lines.add("WARNING: channel order or polarity differs from the master: the extension will code that difference (much larger file)");
        final String vtxt = !R.found ? "stop" : (R.placeable ? "place" : (band == BAND_OK ? (R.verdict == 0 ? "aligned" : "warn") : (band == BAND_STOP ? "stop" : "confirm")));
        R.alignLine = fmt("ALIGN verdict=%s reason=%s cd_offset=%ld master_offset=%ld blocks=%s timing=%.1f resid=%.1f map=%s pol=%c%c gain=%+.2f,%+.2f eqdev=%.1f coh_edge=%.0f preemph=%d clip_runs=%ld length=%s md5=%s",
                          vtxt, R.tag, R.cdOffset, R.masterOffset, blocks.isEmpty() ? "n/a" : blocks, Cv.max(worstT, -99.0), Cv.max(worstR, -99.0), map != 0 ? "swapped" : "direct",
                          pol[0] > 0 ? '+' : '-', pol[1] > 0 ? '+' : '-', gainDb[0], gainDb[1], eqDev, cohEdge, preEmph ? 1 : 0, clipRuns,
                          lengthOk ? "same" : (!R.found ? "differs" : (onlySilence ? "differs_silence" : "differs_content")), md5);   // the edges are only examined once found
        return R; }

    // port of cd_align.cpp:run lambda finish
    private static Report finish(Report R, int v, String tag, String why, boolean lengthOk, String md5) { R.verdict = v; R.why = why; R.tag = tag; R.lines.add("result: " + why); R.band = v == 2 ? BAND_STOP : BAND_OK;
        if (v == 2) R.hint = "not supported: this CD cannot be aligned with this master";
        R.alignLine = fmt("ALIGN verdict=%s reason=%s length=%s md5=%s", v == 0 ? "aligned" : (v == 1 ? "warn" : "stop"), tag, lengthOk ? "same" : "differs", md5); return R; }
    // port of cd_align.cpp:run lambda stopWith
    private static void stopWith(Report R, String tag, String why, String hint) { R.verdict = 2; R.band = BAND_STOP; R.tag = tag; R.why = why; R.hint = hint; R.lines.add("result: " + why); }
    // port of cd_align.cpp:run lambda fftAt
    private static long fftAt(int l, long s0, long s1, long R0) { return pow2((((s1 - s0) + 2 * (R0 + 2)) >> l) + ((s1 - s0) >> (l + 1)) + 2); }
    // port of cd_align.cpp:run lambda peak (the first highest)
    private static long peak(double[] P) { int b = 0; for (int i = 1; i < P.length; i++) if (P[i] > P[b]) b = i; return b; }
    // port of cd_align.cpp:run lambda loud
    private static boolean loud(double[][] x, long p, long q, double quietE) { double e = 0; for (long j = p; j < q; j++) e += x[0][(int) j] * x[0][(int) j] + x[1][(int) j] * x[1][(int) j]; return q > p && e >= 2.0 * (q - p) * quietE; }
    // port of cd_align.cpp:run lambda inside: fixed places: the anchor in its third, the block in the piece's sound; thirds: the block in its third
    private static boolean inside(boolean fixedPos, int k, long p, long q, long lo3, long hi3, long s0, long s1, long ncd) {
        if (!fixedPos) return p >= Math.max(0L, lo3) && q <= Math.min(ncd, hi3);
        final long an = k == 0 ? p : (k == 2 ? q : (p + q) / 2); return an >= lo3 && an <= hi3 && p >= s0 && q <= s1; }
    // port of cd_align.cpp:run lambda emph
    private static double emph(double f) { final double a = f / 3183.1, b = f / 10610.3; return 10 * LMath.log10((1 + a * a) / (1 + b * b)); }
    // port of cd_align.cpp:run lambda pooledE
    private static double pooledE(Blk[] B3, int m, int ti) { double E = 0, X2 = 0; for (Blk K : B3) if (K.use && K.E[m][ti] >= 0) { E += K.E[m][ti]; X2 += K.X2; } return X2 > 0 ? E / X2 : 1.0; }

    // port of cd_align.cpp:run lambda curves
    private static void curves(Blk[] B3, double[][] x, BlkIo.View master, long Lm, int stages, int up, long tLo, long tHi, int nT) {
        final long margin = 2048;   // master samples around the needed span (the derivation filter's reach)
        for (int k = 0; k < 3; k++) { Blk K = B3[k]; if (!K.use) continue; final long a = K.a, b = K.b;
            for (int m = 0; m < 2; m++) { K.E[m] = new double[nT]; java.util.Arrays.fill(K.E[m], -1.0); for (int c = 0; c < 2; c++) K.g[m][c] = new double[nT]; }
            K.X2 = 0; for (int c = 0; c < 2; c++) for (long j = a; j < b; j++) K.X2 += x[c][(int) j] * x[c][(int) j];
            for (int ph = 0; ph < up; ph++) {   // t with (-t) mod up == ph: base_t[j] = D_ph[j - (t + ph) / up]
                long qmin = Long.MAX_VALUE, qmax = Long.MIN_VALUE; for (long t = tLo; t <= tHi; t++) if ((((-t) % up) + up) % up == ph) { final long q = (t + ph) / up; qmin = Math.min(qmin, q); qmax = Math.max(qmax, q); }
                if (qmin == Long.MAX_VALUE) continue;
                final long uLo = a - qmax, uHi = b - qmin, sM = uLo * up + ph - margin, wM = (uHi - uLo) * up + 2 * margin;
                if (sM < 0 || sM + wM > Lm) { K.use = false; break; }
                double[][] D = new double[2][];
                for (int c = 0; c < 2; c++) { int[] q = new int[(int) wM]; for (long i = 0; i < wM; i++) q[(int) i] = master.get((sM + i) * 2 + c) >> 4;
                    for (int st = stages - 1; st >= 0; st--) q = BaseLayer.decimate(q, st);
                    D[c] = new double[(int) (uHi - uLo)]; for (long u = 0; u < uHi - uLo; u++) D[c][(int) u] = (double) (((long) q[(int) (margin / up + u)] + 8) >> 4); }
                for (long t = tLo; t <= tHi; t++) { if ((((-t) % up) + up) % up != ph) continue; final long q = (t + ph) / up; final int ti = (int) (t - tLo);
                    for (int m = 0; m < 2; m++) { double E = 0;
                        for (int c = 0; c < 2; c++) { final double[] pv = D[m != 0 ? 1 - c : c]; double sxr = 0, srr = 0, sxx = 0;
                            for (long j = a; j < b; j++) { final double xv = x[c][(int) j], rv = pv[(int) (j - q - uLo)]; sxr += xv * rv; srr += rv * rv; sxx += xv * xv; }
                            final double g = srr > 0 ? sxr / srr : 0; K.g[m][c][ti] = g; E += sxx - g * sxr; }
                        K.E[m][ti] = Cv.max(E, 0.0); } } } } }

    // port of cd_align.cpp:run lambda sideCheck: the drift / edit check: per block, the residual 2 CD samples before and after t with the
    // gains fitted at t. -1: not possible here (the master ends too close), 0: t holds, 1: a side fits better
    private static int sideCheck(Blk K, long tt, int m, double[][] x, BlkIo.View master, long Lm, int stages, int up, long tLo) {
        final long margin = 2048, a = K.a, b = K.b; final int ph = (int) ((((-tt) % up) + up) % up); final long q = (tt + ph) / up;
        final long uLo = a - q - 2, uHi = b - q + 2, sM = uLo * up + ph - margin, wM = (uHi - uLo) * up + 2 * margin;
        if (sM < 0 || sM + wM > Lm) return -1;
        final int ti = (int) (tt - tLo); double e0 = 0, eM = 0, eP = 0;
        for (int c = 0; c < 2; c++) { int[] s = new int[(int) wM]; for (long i = 0; i < wM; i++) s[(int) i] = master.get((sM + i) * 2 + (m != 0 ? 1 - c : c)) >> 4;
            for (int st = stages - 1; st >= 0; st--) s = BaseLayer.decimate(s, st);
            final double g = K.g[m][c][ti];   // base_{t + 2 CD}[j] = D[j - q - 2], base_t[j] = D[j - q], base_{t - 2 CD}[j] = D[j - q + 2]
            for (long j = a; j < b; j++) { final double xv = x[c][(int) j], r0 = xv - g * Dv(s, margin, up, uLo, j - q), rP = xv - g * Dv(s, margin, up, uLo, j - q - 2), rM = xv - g * Dv(s, margin, up, uLo, j - q + 2); e0 += r0 * r0; eP += rP * rP; eM += rM * rM; } }
        return (eM < e0 || eP < e0) ? 1 : 0; }
    // port of cd_align.cpp:sideCheck lambda D
    private static double Dv(int[] s, long margin, int up, long uLo, long u) { return (double) (((long) s[(int) (margin / up + (u - uLo))] + 8) >> 4); }

    /** the search state of run() (the C++ lambda pooled captures) */
    private static final class Search { double[][] x, r; double[][][] px, pr; int base; boolean whiten; int used;
        // port of cd_align.cpp:run lambda pooled: pooled whitened cross-correlation at level l: CD[j] ~ base[j - o] for o in [oc - Rl, oc + Rl]
        // (level samples); ext: each CD window is its block widened by Rl on both sides, as long as the base window, otherwise the block alone
        double[] pooled(int l, List<long[]> blocks, long oc, long Rl, boolean ext) {
            final double[][] X = {l != 0 ? px[0][l - 1] : x[0], l != 0 ? px[1][l - 1] : x[1]}, Y = {l != 0 ? pr[0][l - 1] : r[0], l != 0 ? pr[1][l - 1] : r[1]};
            final long n = X[0].length, nr = Y[0].length; final double fs = base / (double) (1L << l); double[] P = new double[(int) (2 * Rl + 1)]; used = 0;
            for (long[] bl : blocks) { final long a = bl[0], b = bl[1], ca = ext ? a - Rl : a, cl = ext ? (b - a) + 2 * Rl : (b - a), ma = a - Rl - oc, ml = (b - a) + 2 * Rl;
                final int F = (int) pow2(cl + ml); final int k0 = Math.max(1, Cv.d2i(Math.ceil(50.0 * F / fs))), k1 = Math.min(F / 2 - 1, Cv.d2i(Math.floor(Cv.min(16000.0, 0.45 * fs) * F / fs)));
                double[][] Xr = new double[2][F], Xi = new double[2][F], Yr = new double[2][F], Yi = new double[2][F]; double ex = 0, ey = 0; double[] exc = {0, 0}, eyc = {0, 0};
                for (int c = 0; c < 2; c++) {
                    // the CD window gets cosine ramps, 1/8 of its length at each end (found 2026-10-22)
                    final long ramp = Math.max(1L, cl / 8);
                    for (long i = 0; i < cl; i++) { final long j = ca + i; if (j >= 0 && j < n) { final long e = Math.min(i, cl - 1 - i);
                        final double tw = e >= ramp ? 1.0 : 0.5 - 0.5 * LMath.cospi((e + 0.5) / ramp), v = X[c][(int) j] * tw; Xr[c][(int) i] = v; ex += v * v; exc[c] += v * v; } }
                    for (long i = 0; i < ml; i++) { final long j = ma + i; if (j >= 0 && j < nr) { final double v = Y[c][(int) j]; Yr[c][(int) i] = v; ey += v * v; eyc[c] += v * v; } }
                    Fft.fft(Xr[c], Xi[c], F); Fft.fft(Yr[c], Yi[c], F); }
                if (ex <= 0 || ey <= 0 || k1 < k0) continue; used++;
                final int[] pa = {0, 1, 0, 1}, pm = {0, 1, 1, 0};   // channel pairings LL RR LR RL, all into the one function
                double[] Gr = new double[F], Gi = new double[F];
                for (int p = 0; p < 4; p++) { java.util.Arrays.fill(Gr, 0); java.util.Arrays.fill(Gi, 0); final double en = Math.sqrt(exc[pa[p]] * eyc[pm[p]]);   // plain: a correlation coefficient
                    if (!whiten && en <= 0) continue;
                    final double[] ar = Yr[pm[p]], ai = Yi[pm[p]], br = Xr[pa[p]], bi = Xi[pa[p]];
                    for (int k = k0; k <= k1; k++) { final double gr = ar[k] * br[k] + ai[k] * bi[k], gi = ai[k] * br[k] - ar[k] * bi[k]; final double mg = Math.sqrt(gr * gr + gi * gi);   // mulConj, absC
                        if (mg > 0) { final double d = whiten ? mg : en; Gr[k] = gr / d; Gi[k] = gi / d; Gr[F - k] = Gr[k]; Gi[F - k] = -Gi[k]; } }
                    ifft(Gr, Gi, F);
                    for (long o = oc - Rl; o <= oc + Rl; o++) { final long s = ca - o - ma; P[(int) (o - oc + Rl)] += Math.abs(Gr[(int) (((s % (long) F) + (long) F) % (long) F)]); } } }
            return P; } }

    /** port of cd_align.cpp:Place */
    static final class Place { boolean ok = false; long t = 0, shift = 0, Lm = 0, Lm2 = 0, dropLead = 0, dropTail = 0, padLead = 0, padTail = 0, a0 = -1, z1 = -1; long ncd2 = 0; int cdLead = 0;
        long cdTail = 0; long onlyLead = 0, onlyTail = 0, cutLead = 0, cutTail = 0; String why = ""; List<String> lines = new ArrayList<String>();
        Place copy() { Place p = new Place(); p.ok = ok; p.t = t; p.shift = shift; p.Lm = Lm; p.Lm2 = Lm2; p.dropLead = dropLead; p.dropTail = dropTail; p.padLead = padLead; p.padTail = padTail; p.a0 = a0; p.z1 = z1; p.ncd2 = ncd2; p.cdLead = cdLead;
            p.cdTail = cdTail; p.onlyLead = onlyLead; p.onlyTail = onlyTail; p.cutLead = cutLead; p.cutTail = cutTail; p.why = why; p.lines = new ArrayList<String>(lines); return p; } }

    // port of cd_align.cpp:place
    // placement on a common time line (encoder step after run()): whole master samples only, nothing is resampled; whichever is longer at the
    // start plays first, whichever is longer at the end ends later, and the shorter side gets digital silence there. One CD sample or more of
    // sound that only the master has: edge = EDGE_STOP stops and reports it; EDGE_PAD pads the CD layer with digital silence; EDGE_CUT cuts the master.
    static Place place(BlkIo.View master, long Lm, long ncd, int stages, long t, int rate, int edge) {
        Place p = new Place(); p.t = t; p.Lm = Lm; final long up = 1L << stages;
        p.lines.add(fmt("placement: master moved by %+lld master samples (%+.2f CD samples, %s; %s)", t, t / (double) up, dur(t / (double) rate),
                        t > 0 ? "the CD starts later" : (t < 0 ? "the CD starts earlier" : "same start")));
        long a0 = -1, z1 = -1; for (long i = 0; i < Lm && a0 < 0; i++) if (master.get(i * 2) != 0 || master.get(i * 2 + 1) != 0) a0 = i; for (long i = Lm - 1; i >= 0 && z1 < 0; i--) if (master.get(i * 2) != 0 || master.get(i * 2 + 1) != 0) z1 = i;
        p.a0 = a0; p.z1 = z1; if (a0 < 0) { p.why = "the master is digital silence throughout"; return p; }
        final long early = -(t + a0);   // > 0: the master's sound starts this many master samples before the CD's first sample
        final long late0 = t + z1 + 1 - ncd * up;   // > 0: the master's sound goes on this many master samples after the CD's last sample (before any frame is added)
        if (early >= up) p.onlyLead = early; if (late0 >= up) p.onlyTail = late0;
        final String leadTxt = p.onlyLead != 0 ? fmt("the master has sound %lld master samples (%s, one CD sample or more) before the CD starts, at %.6f-%.6f s of the master", early, dur(early / (double) rate), a0 / (double) rate, (a0 + early) / (double) rate) : "",
                     tailTxt = p.onlyTail != 0 ? fmt("the master has sound %lld master samples (%s, one CD sample or more) after the CD ends, at %.6f-%.6f s of the master", late0, dur(late0 / (double) rate), (z1 + 1 - late0) / (double) rate, (z1 + 1) / (double) rate) : "";
        if (edge == EDGE_STOP && (p.onlyLead != 0 || p.onlyTail != 0)) {
            p.why = fmt("placement at %+lld: ", t) + (p.onlyLead != 0 ? leadTxt : tailTxt) + (p.onlyLead != 0 && p.onlyTail != 0 ? "; and " + tailTxt : "")
                  + ": content only the master has (needs your choice: cdedge=pad adds digital silence to the CD layer, cdedge=cut cuts the master)"; return p; }
        if (early >= up && edge == EDGE_CUT) {   // the master is cut where the CD begins: its sound before that is not kept
            p.cutLead = early; p.lines.add(fmt("cdedge=cut: %s: CUT, the master starts where the CD starts (%s of the master's sound not kept; the CD's samples are unchanged)", leadTxt, dur(early / (double) rate))); }
        else if (early > 0) { p.cdLead = (int) ((early + up - 1) / up);
            if (early < up) p.lines.add(fmt("the master's sound starts %lld master samples (%s) before the CD's first sample: one CD frame of digital silence goes in front of the CD layer, so the master plays from its own first sound (the CD's samples are unchanged)",
                                            early, dur(early / (double) rate)));
            else p.lines.add(fmt("cdedge=pad: %s: %d CD frames (%s) of digital silence go in front of the CD layer, so the master plays from its own first sound (the CD's samples are unchanged; the CD layer is that much longer than the given CD)",
                                 leadTxt, p.cdLead, dur(p.cdLead / (double) (rate / up)))); }
        p.shift = t + p.cdLead * up;
        long late = p.shift + z1 + 1 - (ncd + p.cdLead) * up;   // > 0: the master's sound goes on this many master samples after the CD's last sample
        if (late >= up && edge == EDGE_PAD) { p.cdTail = late / up;
            p.lines.add(fmt("cdedge=pad: %s: %ld CD frames (%s) of digital silence go after the CD layer, so the master plays to its own last sound (the CD's samples are unchanged; the CD layer is that much longer than the given CD)",
                            tailTxt, p.cdTail, dur(p.cdTail / (double) (rate / up))));
            late -= p.cdTail * up; }
        final long ncd2 = ncd + p.cdLead + p.cdTail; final long hi = ncd2 * up + up - 1;
        if (late >= up) { p.cutTail = late - (up - 1);   // EDGE_CUT: the master is cut where the CD (with the format's spare samples) ends
            p.lines.add(fmt("cdedge=cut: %s: CUT, the master ends where the CD ends (%s of the master's sound not kept; the CD's samples are unchanged)", tailTxt, dur(p.cutTail / (double) rate))); }
        else if (late > 0) p.lines.add(fmt("the master's sound ends %lld master samples (%s) after the CD's last sample: kept, the master plays on that much longer", late, dur(late / (double) rate)));
        final long lo = ncd2 * up; p.ncd2 = ncd2;
        p.Lm2 = p.shift + Lm <= hi ? Math.max(p.shift + Lm, lo) : (p.cutTail != 0 ? hi : Math.max(p.shift + z1 + 1, lo));   // past what the CD needs, the master's trailing silence is dropped (cdedge=cut: its sound too)
        p.dropLead = Math.max(0L, -p.shift); p.dropTail = Math.max(0L, p.shift + Lm - p.Lm2); p.padLead = Math.max(0L, p.shift); p.padTail = Math.max(0L, p.Lm2 - (p.shift + Lm));
        if (p.dropLead + p.dropTail >= Lm || p.Lm2 - p.padLead - p.padTail <= 0) { p.why = fmt("placement at %+lld leaves no overlap between the CD and the master", t); return p; }
        if (p.cutLead == 0 && p.cutTail == 0 && p.cdTail == 0)   // the former line, unchanged where nothing beyond the CD was cut or padded
            p.lines.add(fmt("master samples dropped (digital silence only): start %lld, end %lld; zeros added to the master: start %lld, end %lld; digital-silence frames added in front of the CD layer: %d",
                            p.dropLead, p.dropTail, p.padLead, p.padTail, p.cdLead));
        else p.lines.add(fmt("master samples dropped: start %lld (of them %lld from the master's first sound on, cut), end %lld (of them %lld up to the master's last sound, cut); zeros added to the master: start %lld, end %lld; digital-silence frames added to the CD layer: start %d, end %ld",
                             p.dropLead, p.cutLead, p.dropTail, p.cutTail, p.padLead, p.padTail, p.cdLead, p.cdTail));
        p.lines.add(fmt("placed master: %lld frames (was %lld) = %ld CD frames x %lld + %lld", p.Lm2, Lm, ncd2, up, p.Lm2 - lo));
        p.ok = true; return p; }

    // port of cd_align.cpp:cdOnly: CD frames whose centre sample lies in zeros added to the master must be digital silence wherever the base
    // derived from M' is exactly zero on both channels. Returns "" or the reason; nLead / nTail returned in nl[0], nl[1]
    static String cdOnly(short[] cd, long ncd, int[] b24, long pb, Place p, int stages, int base, long[] nl) {
        final long up = 1L << stages;
        final long jl = Math.min(ncd, (p.padLead + up - 1) / up), jt = Math.max(jl, Math.min(ncd, (p.Lm2 - p.padTail + up - 1) / up));
        nl[0] = jl; nl[1] = ncd - jt;
        String r = scan(cd, b24, pb, 0, jl, "start", base); if (r.isEmpty()) r = scan(cd, b24, pb, jt, ncd, "end", base); return r; }
    // port of cd_align.cpp:cdOnly lambda scan (b24 == null: cdOnlyFar's scan, the CD alone)
    private static String scan(short[] cd, int[] b24, long pb, long a, long b, String where, int base) { long f = -1, l = -1;
        for (long j = a; j < b; j++) if ((b24 == null || (b24[(int) ((pb + j) * 2)] == 0 && b24[(int) ((pb + j) * 2 + 1)] == 0)) && (cd[(int) (j * 2)] != 0 || cd[(int) (j * 2 + 1)] != 0)) { if (f < 0) f = j; l = j; }
        return f < 0 ? "" : fmt("the CD has content where the placed master has none (%s): CD frames %ld-%ld (%.6f-%.6f s) are not digital silence: not supported in this step (only one side has content)",
                                where, f, l, f / (double) base, (l + 1) / (double) base); }

    // port of cd_align.cpp:advanceTaps: the master advanced by a (units U = 2^22 >> stages per master sample, |a| <= U)
    static void advanceTaps(long a, int stages, int[] h) { final long U = FracDelay.unitsPerMaster(stages);
        if (a > 0) FracDelay.taps(U - a, stages, h); else if (a < 0) FracDelay.taps(-a, stages, h); }
    // port of cd_align.cpp:advanced
    static long advanced(int[] h, long a, long n, FracDelay.Get get) { return a == 0 ? get.get(n) : FracDelay.apply(h, a > 0 ? n + 1 : n, get); }

    // port of cd_align.cpp:applyPlaceFile: M'[i] = master[i - shift], written piece by piece
    static void applyPlaceFile(EncEnv env, BlkIo.View m, Place p, String out) {
        final long t = p.shift, n = p.Lm, n2 = p.Lm2, s0 = Math.max(0L, -t), s1 = Math.min(n, n2 - t);   // master samples [s0, s1) are kept, at s + t
        BlkIo.Writer w = new BlkIo.Writer(env, out); try { final long BS = 1L << 18;
            for (long a = 0; a < n2; a += BS) { final long b = Math.min(n2, a + BS); int[] buf = new int[(int) (b - a) * 2];
                for (long i = a; i < b; i++) { final long s = i - t; if (s >= s0 && s < s1) { buf[(int) (i - a) * 2] = m.get(s * 2); buf[(int) (i - a) * 2 + 1] = m.get(s * 2 + 1); } }
                w.put(buf); }
            w.finish(); w = null; } finally { if (w != null) w.close(); } }
    // port of cd_align.cpp:moveKern: Kaiser-windowed sinc of 128 taps (half length 64, beta 10)
    static double moveKern(double u) { final int kH = 64; final double beta = 10.0; if (Math.abs(u) >= kH) return 0;
        final double s = u == 0 ? 1.0 : LMath.sinpi(u) / (Math.PI * u), r = u / kH; return s * I0k(beta * Math.sqrt(1 - r * r)) / I0k(beta); }
    // port of cd_align.cpp:moveKern lambda I0
    private static double I0k(double v) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; if (t < 1e-17 * s) break; } return s; }
    // port of cd_align.cpp:moveMasterFile: Z(i) = M(i + f), |f| <= 1/2, digital silence outside the master; rounded to 24 bit, hard-clipped (returns nclip)
    static long moveMasterFile(EncEnv env, BlkIo.View m, long Lm, double f, String out) {
        long nclip = 0; final int kH = 64; final long sh = Cv.d2l(Math.floor(f)); final double fr = f - (double) sh;
        double[] k = new double[2 * kH]; for (int q = -kH + 1; q <= kH; q++) k[q + kH - 1] = moveKern(fr - q);
        BlkIo.Writer w = new BlkIo.Writer(env, out); try { final long BS = 1L << 18;
            for (long a = 0; a < Lm; a += BS) { env.checkCancel(); final long b = Math.min(Lm, a + BS); int[] z = new int[(int) (b - a) * 2];
                final long xa = Math.max(0L, a + sh - kH + 1), xb = Math.min(Lm, b + sh + kH);   // the input samples this piece reads (inside the master)
                int[] x = new int[(int) Math.max(0L, xb - xa) * 2]; for (long j = xa; j < xb; j++) { x[(int) (j - xa) * 2] = m.get(j * 2); x[(int) (j - xa) * 2 + 1] = m.get(j * 2 + 1); }
                for (int c = 0; c < 2; c++) for (long i = a; i < b; i++) { final long i0 = i + sh; double s = 0;
                    for (int q = -kH + 1; q <= kH; q++) { final long j = i0 + q; if (j < 0 || j >= Lm) continue; s += (double) (x[(int) (j - xa) * 2 + c] >> 8) * k[q + kH - 1]; }
                    long v = Cv.llround(s); if (v > 8388607) { v = 8388607; nclip++; } if (v < -8388608) { v = -8388608; nclip++; } z[(int) (i - a) * 2 + c] = (int) (v * 256); }
                w.put(z); }
            w.finish(); w = null; } finally { if (w != null) w.close(); }
        return nclip; }

    // port of cd_align.cpp:measurePhase: the piece's phase at the whole offset t: the advance a (phi units) with the least pooled residual
    // over run()'s usable check blocks, by a fixed descent (start at the check's pooled fraction, then steps of U/8, U/16, ... 1 unit). line[0]: the report line
    static long measurePhase(final short[] cd, final BlkIo.View master, final long Lm, final int stages, int rate, long t, Report R, String[] line) {
        final long up = 1L << stages; final long U = FracDelay.unitsPerMaster(stages); final long margin = 2048;
        if (R.blocks.isEmpty()) { line[0] = "phase: not measured (no check block with sound on both sides): phase number 0"; return 0; }   // digital silence (both_silent)
        final int ph = (int) ((((-t) % up) + up) % up); final long q = (t + ph) / up; final int map = R.chanMap; final int[] used = {0};
        Phase P = new Phase(); P.cd = cd; P.master = master; P.Lm = Lm; P.stages = stages; P.up = up; P.margin = margin; P.ph = ph; P.q = q; P.map = map; P.R = R;
        long a = Math.max(-U / 2, Math.min(U / 2, Cv.llround(-R.frac * (double) U))); final long a0 = a; double ea = P.resid(a, used); int steps = 1;
        for (long step = U / 8; step >= 1; step /= 2, steps++) {
            final boolean lo = a - step >= -U / 2, hi = a + step <= U / 2; final double em = lo ? P.resid(a - step, used) : 2.0, ep = hi ? P.resid(a + step, used) : 2.0;
            if (em < ea && em <= ep) { a -= step; ea = em; } else if (ep < ea) { a += step; ea = ep; } }
        final double sec = (double) a / (double) U / rate; final String st = Math.abs(sec) < 1e-9 ? fmt("%+.1f ps", sec * 1e12) : (Math.abs(sec) < 1e-6 ? fmt("%+.2f ns", sec * 1e9) : fmt("%+.3f us", sec * 1e6));
        line[0] = fmt("phase: at the whole offset %+lld, the master advanced by %+.7f master samples (%s) fits the CD best: pooled residual %.2f dB re the CD over %d check block(s) (start %+.7f; %d fixed steps down to 1/%lld master sample)",
                      t, (double) a / (double) U, st, db10(ea), used[0], (double) a0 / (double) U, steps, U);
        return a; }
    /** the state of measurePhase's lambda resid */
    private static final class Phase { short[] cd; BlkIo.View master; long Lm, up, margin, q; int stages, ph, map; Report R;
        // port of cd_align.cpp:measurePhase lambda mir
        long mir(long j) { j = j < 0 ? -j : (j >= Lm ? 2 * (Lm - 1) - j : j); return Math.max(0L, Math.min(Lm - 1, j)); }
        // port of cd_align.cpp:measurePhase lambda resid: base_t[j] = D_ph[j - q] as in run(), D_ph from the master advanced by a
        double resid(long a, int[] used) {
            int[] h = new int[FracDelay.TAPS]; advanceTaps(a, stages, h); double E = 0, X2 = 0; used[0] = 0;
            for (long[] bl : R.blocks) { final long ja = bl[0], jb = bl[1], uLo = ja - q, sM = uLo * up + ph - margin, wM = (jb - ja) * up + 2 * margin;
                if (sM < 0 || sM + wM > Lm) continue;
                used[0]++;
                for (int c = 0; c < 2; c++) { final int mc = map != 0 ? 1 - c : c; int[] s = new int[(int) wM];
                    FracDelay.Get get = new FracDelay.Get() { @Override public long get(long j) { return master.get(mir(j) * 2 + mc) >> 4; } };
                    for (long i = 0; i < wM; i++) s[(int) i] = (int) advanced(h, a, sM + i, get);
                    for (int st = stages - 1; st >= 0; st--) s = BaseLayer.decimate(s, st);
                    double sxr = 0, srr = 0, sxx = 0;   // the derived base kept at its guard bits (1/16 LSB): the fit needs no rounding
                    for (long j = ja; j < jb; j++) { final double xv = 256.0 * cd[(int) (j * 2 + c)], rv = s[(int) (margin / up + (j - ja))] / 16.0; sxr += xv * rv; srr += rv * rv; sxx += xv * xv; }
                    final double g = srr > 0 ? sxr / srr : 0; E += sxx - g * sxr; X2 += sxx; } }
            return X2 > 0 ? E / X2 : 1.0; } }

    // port of cd_align.cpp:cdOnlyFar: the same rule as cdOnly(), on the given CD alone and before anything is moved or derived
    static String cdOnlyFar(short[] cd, long ncd, Place p, int stages, int base, long pb) {
        final long up = 1L << stages;
        final long jl = Math.min(ncd, (p.padLead + up - 1) / up), jt = Math.max(jl, Math.min(ncd, (p.Lm2 - p.padTail + up - 1) / up));
        final long la = Math.max(0L, Math.min(jl, Math.min(ncd, (p.shift + p.a0) / up - pb)));
        final long tb = Math.max(la, Math.max(jt, Math.min(ncd, (p.shift + p.z1) / up + 1 + pb)));
        String r = scan(cd, null, 0, 0, la, "start", base); if (r.isEmpty()) r = scan(cd, null, 0, tb, ncd, "end", base); return r; }
}
