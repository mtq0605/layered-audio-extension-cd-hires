// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Port of src/lae_enc.cpp encode(), the independent-CD block (lines 308-492; part 2 of the port), installed as {@link LaeEncoder.CdHook}.
 * Independent CD: it becomes the CD layer bit for bit; the extension codes master - CD. No dither of ours to regenerate. The position is
 * always confirmed by the user (2026-10-23): without cdalign= the encoder searches twice (whitened and plain), measures each result's
 * fraction, tries each like a hand-given offset and reports both, then stops: nothing is encoded. With cdalign=&lt;offset&gt; the master is
 * placed on the CD's time line by whole master samples, moved by the fraction, checked again, and the remaining time difference decides.
 * The C++ console questions (cdedge, cdaccept) are never asked here (no console): the run stops with the C++ text for scripts, and the
 * app passes the user's choice back as the option (cdedge=pad / cdedge=cut / cdaccept). The report lines are the printed text.
 */
final class LaeCd implements LaeEncoder.CdHook {
    /** port of lae_enc.cpp:encode struct Att: kind 0 aligned, 1 asks, 2 stops (the reason in why), 3 fatal (derivation, disk) */
    static final class Att { int kind = 2; String why = ""; }

    // the C++ locals of the cd= block the lambdas capture
    LaeEncoder.Ctx x; EncEnv env; short[] cd; long have, need, nb0, pb0; byte[] b; int[] b24; String mFile, mNew;
    CdAlign.Report rep = new CdAlign.Report(); String placed = ""; long padP = 0, cutL = 0, cutT = 0; long rpT = 0; double rpMv = 0;
    CdAlign.Place edgeQ = new CdAlign.Place(); boolean edgeAsk = false; int base, stages, rate; long U1;

    // port of lae_enc.cpp:encode, the cd= block
    @Override public int run(LaeEncoder.Ctx xx) {
        x = xx; env = x.env; base = x.base; stages = x.stages; rate = x.rate; U1 = FracDelay.unitsPerMaster(stages); final int P = x.P;
        WavIo.Wav c = new WavIo.Wav(); if (!WavIo.readCD(x.cdPath, c) || c.bits != 16 || c.ch != 2 || c.rate != base) { env.printf("independent CD must be a 16-bit stereo WAV at %d Hz\n", base); x.oldNote(); return 1; }
        cd = c.pcm16; b = LaeEncoder.slurp(x.T("b16.s16")); nb0 = b.length / 4; pb0 = P >> stages; need = nb0 - 2 * pb0; have = cd.length / 2;
        {   // alignment (design D and A, 2026-10-22 / 10-23)
            b24 = EncIo.readAllI32(x.T("b24.s32"));
            // the master is m0.raw in the work folder, read through blkio::View windows; every step that changes it writes m0.new piece by piece, which then replaces m0.raw
            mFile = x.T("m0.raw"); mNew = x.T("m0.new");
            if (!x.manual) {
                copy(mFile, x.T("m0.orig"));   // the given master, for the second search
                final long Lm0 = x.Lm; final short[] cd0 = cd.clone(); final long have0 = have;
                String[] res = new String[2], off = new String[2]; final String[] nm = {"whitened", "plain"}; boolean edgeAny = false;
                for (int k = 0; k < 2; k++) {
                    if (k != 0) { copy(x.T("m0.orig"), mFile);
                        x.Lm = Lm0; cd = cd0.clone(); have = have0; if (!x.derive()) return 2;   // the second search starts from the given master again
                        b = LaeEncoder.slurp(x.T("b16.s16")); nb0 = b.length / 4; need = nb0 - 2 * pb0; b24 = EncIo.readAllI32(x.T("b24.s32")); }
                    CdAlign.Report r; { BlkIo.View m = new BlkIo.View(mFile); try { r = CdAlign.run(cd, have, b24, nb0, pb0, m, x.Lm, base, stages, 0, "", 0, -1, null, k == 0); } finally { m.close(); } }
                    for (String l : r.lines) env.printf("align (%s): %s\n", nm[k], l); env.printf("%s search=%s\n", r.alignLine, nm[k]);
                    long tP = r.placeAt; double mvv = 0;   // the whole offset, and the fraction the master is moved by (|mvv| <= 1/2)
                    if (r.found) { String[] phLine = {""}; long adv; { BlkIo.View m = new BlkIo.View(mFile); try { adv = CdAlign.measurePhase(cd, m, x.Lm, stages, rate, r.placeAt, r, phLine); } finally { m.close(); } }
                        env.printf("align (%s): %s\n", nm[k], phLine[0]); mvv = (double) adv / (double) U1; }
                    final Att A = attempt(r, nm[k], tP, mvv, x.cdEdge >= 0 ? x.cdEdge : CdAlign.EDGE_PAD); if (A.kind == 3) return 2;   // without cdedge= the search goes on as with pad, and says that a choice is needed
                    off[k] = r.found ? offText(tP, mvv) : "-";
                    res[k] = A.kind == 0 ? CdAlign.fmt("aligned at cdalign=%s%s", off[k], placed)
                           : A.kind == 1 ? CdAlign.fmt("not aligned within the limit at cdalign=%s: %s", off[k], A.why)
                           : r.found ? CdAlign.fmt("alignment failed at cdalign=%s: %s", off[k], A.why) : CdAlign.fmt("alignment failed: %s", A.why);
                    if (edgeAsk && x.cdEdge < 0) { res[k] += "; and it needs your choice: cdedge=pad or cdedge=cut"; edgeAny = true; }
                    env.printf("align: %s search: %s\n", nm[k], res[k]); }
                env.printf("ALIGN_SUGGEST whitened=%s:%s plain=%s:%s%s\n", st(res[0]), off[0], st(res[1]), off[1], edgeAny ? " edge=needed" : "");
                if (edgeAny) env.printf("independent CD: needs your choice: %s\n", edgeText());
                env.printf("independent CD: %s; give the position by hand: cdalign=<offset> (whitened search: %s; plain search: %s)\n",
                           x.cdCheck ? "cdcheck: both searches checked, nothing encoded" : "nothing encoded", res[0], res[1]);
                if (x.cdCheck) return 0; x.oldNote(); return 1; }
            // by hand
            {   CdAlign.Report r; { BlkIo.View m = new BlkIo.View(mFile); try { r = CdAlign.run(cd, have, b24, nb0, pb0, m, x.Lm, base, stages, 0, "; the placement given by hand follows", 0, -1, x.manualT, true); } finally { m.close(); } }
                for (String l : r.lines) env.printf("align: %s\n", l); env.printf("%s\n", r.alignLine);
                env.printf("align: offset given by hand: cdalign=%s (checked first at the nearest whole offset %+lld)\n", x.manualText, x.manualT);
                env.printf("align: fraction given by hand: cdalign=%s is the whole offset %+lld with the master moved by %+.7f master samples; not measured\n",
                           x.manualText, x.manualT, x.manualMove);
                Att A = attempt(r, "", x.manualT, x.manualMove, x.cdEdge >= 0 ? x.cdEdge : CdAlign.EDGE_STOP); if (A.kind == 3) return 2;
                if (A.kind == 2 && edgeAsk && x.cdEdge < 0) {   // the master has sound beyond the CD: tell, then the user chooses (2026-11-03)
                    env.printf("independent CD: needs your choice: %s\n", edgeText());
                    if (x.cdCheck) { env.printf("cdcheck: it would ask: cdedge=pad or cdedge=cut; nothing encoded\n"); return 0; }
                    env.printf("  nothing encoded. Add cdedge=pad or cdedge=cut to choose.\n"); x.oldNote(); return 1; }   // C++: a console asks; the app asks and passes the option
                if (A.kind == 2) { env.printf("independent CD rejected: %s%s; nothing encoded\n", A.why, r.found ? "" : CdAlign.fmt(" (at the offset given by hand, cdalign=%s: check the value, or run without cdalign= to see both searches' positions)", x.manualText));
                    if (x.cdCheck) return 0; x.oldNote(); return 1; } }
            // the user's bands (2026-10-22): <= -40 dB on, -40..-33 dB and beyond -33 dB only after a confirmation, no fit at all never
            final boolean large = rep.band == CdAlign.BAND_CONFIRM_LARGE, ask = rep.band == CdAlign.BAND_CONFIRM || large || rep.mapDiff;   // a channel swap or inverted polarity also needs your confirmation (2026-10-24)
            if (x.cdCheck) { if (ask) env.printf("cdcheck: it would ask: %s%s\n", rep.why, rep.hint.startsWith("still shifted") ? "; " + rep.hint : "");   // the hint helps stepping by hand
                env.printf("cdcheck: alignment checked%s, nothing encoded (%s)\n", placed, rep.band == CdAlign.BAND_OK ? "would encode" : (ask ? "would ask for confirmation" : "would not encode")); return 0; }
            if (rep.band == CdAlign.BAND_STOP) { env.printf("independent CD rejected: %s%s; nothing encoded\n", rep.why, rep.hint.isEmpty() ? "" : "; " + rep.hint); x.oldNote(); return 1; }   // lambda rejected
            if (ask) {
                env.printf("independent CD: needs your confirmation: %s%s\n", rep.why, placed);
                if (rep.mapDiff) env.printf("  note: the channel order or polarity also differs from the master: the extension codes that difference too\n");
                if (large) env.printf("  note: continuing is possible (the extension codes the whole difference and the precision stays the same), but the file may get much larger\n");
                if (!x.cdAccept) { env.printf("  nothing encoded. If this is the right CD and it cannot be aligned better%s, add the option cdaccept.\n", rep.hint.startsWith("still shifted") ? " (" + rep.hint + ")" : ""); x.oldNote(); return 1; }   // C++: a console asks
                else env.printf("  continuing: cdaccept given%s\n", large ? " (the file may get much larger)" : "");
                x.alignNote = "independent CD: CONTINUED AFTER CONFIRMATION, " + rep.why + (rep.mapDiff ? ", and the channel order or polarity differs" : "") + placed; }
            else x.alignNote = (rep.verdict == 1 ? "independent CD: WARNING, " + rep.why : "independent CD: alignment confirmed") + placed;
            {   // the movement report (a separate text file; plain text for the user's records, read by no program)
                final double off = (double) rpT - rpMv, us = 1e6 / (double) rate;
                x.placeT = rpT;   // noise= needs 0: the marking was made on the aligned master
                x.alignReport = CdAlign.fmt("LAE alignment report (independent CD)\nmaster: %s\nCD: %s\noutput: %s\n", x.argv[1], x.cdPath, x.outPath)
                    + CdAlign.fmt("whole offset: %+lld master samples\n", rpT)
                    + (rpMv != 0 ? CdAlign.fmt("fraction moved: %+.7f master samples (%+.4f microseconds): the master's waveform taken that much later\n", rpMv, rpMv * us) : "fraction moved: none\n")
                    + CdAlign.fmt("total offset (cdalign): %s master samples (%+.3f microseconds): the CD starts %s than the master\n", offText(rpT, rpMv), off * us,
                            off > 0 ? "later" : (off < 0 ? "earlier" : "at the same time"))
                    + (padP != 0 ? CdAlign.fmt("digital silence added before the move: %ld CD frames at each end; removed after the move where still digital silence: start %ld, end %ld; kept: start %ld, end %ld CD frames\n",
                                         padP, cutL, cutT, padP - cutL, padP - cutT) : "")
                    + (placed.isEmpty() ? "" : "placement:" + placed + "\n")
                    + (edgeAsk ? (x.cdEdge == CdAlign.EDGE_CUT
                        ? "sound only the master has: CUT (cdedge=cut): " + (edgeQ.cutLead != 0 ? CdAlign.dur(edgeQ.cutLead / (double) rate) + " at the start" : "") + (edgeQ.cutLead != 0 && edgeQ.cutTail != 0 ? " and " : "")
                          + (edgeQ.cutTail != 0 ? CdAlign.dur(edgeQ.cutTail / (double) rate) + " at the end" : "") + " of the master's sound are not in this file\n"
                        : CdAlign.fmt("sound only the master has: PADDED (cdedge=pad): the CD layer has %d CD frames of digital silence before and %ld after the given CD; the master is whole\n", edgeQ.cdLead, edgeQ.cdTail)) : "")
                    + "check after the move: " + rep.why + "\n" + rep.alignLine + "\n"; } }
        if (have != need) { env.printf("independent CD has %ld samples, the master needs %ld at %d Hz: the lengths do not match; nothing encoded\n", have, need, base); x.oldNote(); return 1; }   // lambda lengthMsg
        for (long i = 0; i < need * 2; i++) { int o = (int) ((pb0 * 2 + i) * 2); short v = cd[(int) i]; b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }
        EncIo.writeVec(env, x.T("b16.s16"), b);
        byte[] df = LaeEncoder.slurp(x.T("dither.flags")); java.util.Arrays.fill(df, (byte) 0); EncIo.writeVec(env, x.T("dither.flags"), df);
        env.printf("independent CD: %s (%ld samples) used as the CD layer\n", x.cdPath, have);
        return -1; }

    // port of lae_enc.cpp:encode lambda st
    static String st(String r) { return r.startsWith("aligned") ? "aligned" : (r.startsWith("not aligned") ? "not_aligned" : "failed"); }
    // port of lae_enc.cpp:encode lambda offText: o = whole offset - the master's move (7 decimals: finer than 1/524288)
    static String offText(long tp, double mv) { return CdAlign.fmt("%+.7f", (double) tp - mv); }
    // std::filesystem::copy_file(from, to, overwrite_existing)
    void copy(String from, String to) {
        try { FileInputStream in = new FileInputStream(from); try { FileOutputStream out = new FileOutputStream(to); try { byte[] buf = new byte[1 << 20]; for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n); } finally { out.close(); } } finally { in.close(); } }
        catch (IOException e) { throw EncIo.fail("cannot copy " + from + ": " + e.getMessage()); } }
    // port of lae_enc.cpp:encode lambda masterReplace
    void masterReplace() { if (!LaeEncoder.rename(mNew, mFile)) throw EncIo.fail("cannot replace " + mFile + ": rename failed"); }
    // port of lae_enc.cpp:encode lambda masterRange: the new master: padA zero frames, frames [a, b) of the master, padB zero frames
    void masterRange(long a, long bb, long padA, long padB) {
        { BlkIo.View m = new BlkIo.View(mFile); BlkIo.Writer wr = new BlkIo.Writer(env, mNew); try { int[] z = new int[1 << 19]; final long BS = 1L << 18;
              for (long k = 0; k < padA; k += BS) wr.put(z, 0, (int) (Math.min(BS, padA - k) * 2));
              for (long i = a; i < bb; i += BS) { final long e = Math.min(bb, i + BS); int[] xx = new int[(int) (e - i) * 2]; for (long j = i; j < e; j++) { xx[(int) (j - i) * 2] = m.get(j * 2); xx[(int) (j - i) * 2 + 1] = m.get(j * 2 + 1); } wr.put(xx); }
              for (long k = 0; k < padB; k += BS) wr.put(z, 0, (int) (Math.min(BS, padB - k) * 2));
              wr.finish(); wr = null; } finally { if (wr != null) wr.close(); m.close(); } }
        masterReplace(); }
    // inserts n zero frames at the start (front) or the end of the CD
    void cdInsert(boolean front, long n) { short[] o = new short[(int) (cd.length + 2 * n)]; System.arraycopy(cd, 0, o, front ? (int) (2 * n) : 0, cd.length); cd = o; have = cd.length / 2; }
    // port of lae_enc.cpp:encode lambda edgeText: the two choices, said plainly (the user, 2026-11-03: both must be told clearly)
    String edgeText() {
        final CdAlign.Place q = edgeQ; final double r = (double) rate; String s = "the master has sound that the CD does not have:";
        if (q.onlyLead != 0) s += CdAlign.fmt(" %s before the CD starts;", CdAlign.dur(q.onlyLead / r));
        if (q.onlyTail != 0) s += CdAlign.fmt(" %s after the CD ends;", CdAlign.dur(q.onlyTail / r));
        return s + "\n    cdedge=pad: digital silence is added to the CD layer there, so the master's sound is kept whole; the CD's own samples are unchanged,"
                 + " but the CD layer in the LAE file (and on a disc made from it) is that much longer than the given CD"
                 + "\n    cdedge=cut: the master is cut where the CD starts and ends; its sound beyond the CD is NOT kept in the LAE file (where it still"
                 + " sounds there, the high-resolution playback ends abruptly); the CD layer is exactly the given CD"; }

    // port of lae_enc.cpp:encode lambda attempt: one attempt at one position (the checks of the hand-given path). sch: "" by hand, else the
    // search's name (output prefix, nothing kept). rep / placed: the final check
    Att attempt(CdAlign.Report rep1, String sch, long tPlace, double mv, int edge) {
        Att A = new Att(); final String pre = sch.isEmpty() ? "align: " : "align (" + sch + "): ", key = sch.isEmpty() ? "" : " search=" + sch;
        rep = rep1; placed = ""; padP = cutL = cutT = 0; rpT = tPlace; rpMv = mv; edgeQ = new CdAlign.Place(); edgeAsk = false;
        if (!rep1.found) { A.why = rep1.why + (rep1.hint.isEmpty() ? "" : "; " + rep1.hint); return A; }
        env.printf("PHASE t=%lld phi=0 move=%+.7f offset=%s%s\n", tPlace, mv, offText(tPlace, mv), key);
        if (mv != 0) env.printf("%sthe master is placed at %+lld and moved by %+.7f master samples (its band-limited waveform taken %+.7f samples later: interpolated once, at alignment; the file stays v5)\n",
                                pre, tPlace, mv, mv);
        else env.printf("%sthe CD lies on a whole master sample: the master is not moved by a fraction\n", pre);
        final boolean placeNeeded = tPlace != 0 || have != need;
        if (placeNeeded || mv != 0) {   // the CD's own samples are never changed: the master moves by whole master samples (a silent CD frame may go in front)
            final long t = tPlace; long nLead = 0, nTail = 0;
            CdAlign.Place q; { BlkIo.View m = new BlkIo.View(mFile); try { q = CdAlign.place(m, x.Lm, have, stages, t, rate, edge); } finally { m.close(); } } edgeQ = q; edgeAsk = q.onlyLead != 0 || q.onlyTail != 0;
            final boolean tentative = edgeAsk && x.cdEdge < 0;   // the search goes on as with pad, but nothing is chosen yet
            for (String l : q.lines) env.printf("%s%s\n", pre, (tentative && l.startsWith("cdedge=pad: ") ? "with cdedge=pad (not chosen yet): " + l.substring(12) : l));
            String bad = q.ok ? "" : q.why; final boolean moved = q.ok && (q.shift != 0 || q.Lm2 != x.Lm);
            if (bad.isEmpty() && q.cdLead != 0) cdInsert(true, q.cdLead);
            if (bad.isEmpty() && q.cdTail != 0) cdInsert(false, q.cdTail);   // cdedge=pad at the end
            if (bad.isEmpty() && moved) {   // cheap stops first, before anything is resized, written or derived
                final String far = CdAlign.cdOnlyFar(cd, have, q, stages, base, pb0);
                if (!far.isEmpty()) bad = far; else if (!x.fitsLength(q.Lm2)) bad = CdAlign.fmt("the placed master (%lld frames) is outside this encoder's length limits", q.Lm2); }
            if (!bad.isEmpty()) { placeLine(q, sch, nLead, nTail, "stop", key); A.why = bad; return A; }   // not possible here; no other offset is tried in its place
            if (moved || mv != 0) {
                long reuse = 0; for (String f : new String[] {"m0.raw", "m.raw", "b16.s16", "b24.s32", "dither.flags"}) { long s = LaeEncoder.fileSize(x.T(f)); if (s > 0) reuse += s; }   // derive() overwrites these
                if (moved) { { BlkIo.View m = new BlkIo.View(mFile); try { CdAlign.applyPlaceFile(env, m, q, mNew); } finally { m.close(); } } masterReplace(); x.Lm = q.Lm2; }   // M' replaces the master; the CD layer, base and dither flags are derived again from it
                if (mv != 0) {   // the fraction (2026-10-23): digital silence at both ends first, so that what the move carries past the master's first / last
                    // sample is kept; afterwards only what is still digital silence is removed again: at most the added frames, never a frame of the given CD
                    final long P = 128; final long up1 = 1L << stages, Pm = P * up1;
                    cdInsert(true, P); cdInsert(false, P);
                    masterRange(0, x.Lm, Pm, Pm); x.Lm += 2 * Pm;
                    long nclip; { BlkIo.View m = new BlkIo.View(mFile); try { nclip = CdAlign.moveMasterFile(env, m, x.Lm, mv, mNew); } finally { m.close(); } } masterReplace();   // then the fraction: the placed master's waveform taken mv later
                    if (nclip != 0) env.printf("%sWARNING: %ld samples of the moved master exceed 24-bit full scale and are hard-clipped\n", pre, nclip);
                    long kL = 0, kT = 0;
                    { BlkIo.View m = new BlkIo.View(mFile); try { while (kL < P && silent(m, kL * up1, up1)) kL++; while (kT < P && silent(m, x.Lm - (kT + 1) * up1, up1)) kT++; } finally { m.close(); } }
                    { short[] o = new short[(int) (cd.length - 2 * kT - 2 * kL)]; System.arraycopy(cd, (int) (2 * kL), o, 0, o.length); cd = o; have = cd.length / 2; }
                    if (kL != 0 || kT != 0) masterRange(kL * up1, x.Lm - kT * up1, 0, 0); x.Lm -= (kL + kT) * up1;
                    padP = P; cutL = kL; cutT = kT;
                    env.printf("%sdigital silence: %ld CD frames added at each end before the move; removed again where still digital silence: start %ld, end %ld (kept: start %ld, end %ld)\n",
                               pre, P, kL, kT, P - kL, P - kT); }
                if (!x.spaceOk(reuse) || !x.derive()) { A.kind = 3; return A; }
                b = LaeEncoder.slurp(x.T("b16.s16")); nb0 = b.length / 4; need = nb0 - 2 * pb0; b24 = EncIo.readAllI32(x.T("b24.s32")); }
            CdAlign.Place qc = q.copy();   // the layout cdOnly sees: the frames of added silence that the move filled are zeros added to the master too
            if (padP != 0) { final long up1 = 1L << stages; qc.padLead += (padP - cutL) * up1; qc.padTail += (padP - cutT) * up1; qc.Lm2 = x.Lm; }
            long[] nl = new long[2]; final String co = CdAlign.cdOnly(cd, have, b24, pb0, qc, stages, base, nl); nLead = nl[0]; nTail = nl[1];
            if (nLead != 0 || nTail != 0) env.printf("%sCD frames that meet only zeros added to the master: start %ld, end %ld (%s)\n", pre, nLead, nTail, co.isEmpty() ? "digital silence where the derived base is zero" : "NOT digital silence");
            if (placeNeeded || !co.isEmpty()) placeLine(q, sch, nLead, nTail, co.isEmpty() ? "placed" : "stop", key);
            if (!co.isEmpty()) { A.why = co; return A; }
            if (have != need) { A.why = CdAlign.fmt("the CD has %ld samples, the placed master needs %ld at %d Hz: the lengths do not match", have, need, base); return A; }
            final long zero = 0;   // the placed (and moved) master, checked again at offset 0 (the same three blocks, through its own derivation)
            CdAlign.Report rep2; { BlkIo.View m = new BlkIo.View(mFile); try { rep2 = CdAlign.run(cd, have, b24, nb0, pb0, m, x.Lm, base, stages, t, "", 0, -1, zero, true); } finally { m.close(); } }
            for (String l : rep2.lines) env.printf("%sre-check: %s\n", pre, l);
            env.printf("ALIGN_RECHECK%s%s\n", rep2.alignLine.substring(5), key);
            if (mv != 0 && rep2.hint.startsWith("still shifted"))   // the suggestion keeps the fraction
                rep2.hint = CdAlign.fmt("still shifted by %+ld master samples: try cdalign=%s", rep2.bestNear, offText(t + rep2.bestNear, mv));
            if (!rep2.found) { A.why = CdAlign.fmt("the placement at %+lld master samples%s does not align: %s", t, mv != 0 ? CdAlign.fmt(" with the master moved by %+.7f", mv) : "", rep2.why) + (rep2.hint.isEmpty() ? "" : "; " + rep2.hint); return A; }
            final String phs = mv != 0 ? CdAlign.fmt("moved by %+.7f master samples", mv) : "";
            String layer = !(q.cdLead != 0 || q.cdTail != 0) ? "" : (q.cdLead > 1 || q.cdTail != 0
                ? CdAlign.fmt("; CD layer = %d frame(s) of digital silence + the given CD + %ld frame(s) of digital silence", q.cdLead, q.cdTail) : "; CD layer = one digital-silence frame + the given CD");
            if (q.cutLead != 0 || q.cutTail != 0) layer += "; master CUT to the CD: " + (q.cutLead != 0 ? CdAlign.dur(q.cutLead / (double) rate) + " of its sound at the start" : "")
                                                     + (q.cutLead != 0 && q.cutTail != 0 ? " and " : "") + (q.cutTail != 0 ? CdAlign.dur(q.cutTail / (double) rate) + " at the end" : "") + " not kept";
            placed = placeNeeded ? CdAlign.fmt(" (master placed %s at %+lld master samples%s, then checked again%s)", sch.isEmpty() ? "by hand" : "by the " + sch + " search", t, mv != 0 ? ", " + phs : "", layer)
                                 : CdAlign.fmt(" (master %s, then checked again)", phs);
            rep = rep2; }
        if (rep.band == CdAlign.BAND_STOP) { A.why = rep.why + (rep.hint.isEmpty() ? "" : "; " + rep.hint); return A; }
        A.kind = rep.band == CdAlign.BAND_OK ? 0 : 1; A.why = rep.why; return A; }
    // port of lae_enc.cpp:encode lambda placeLine
    void placeLine(CdAlign.Place q, String sch, long nLead, long nTail, String result, String key) {
        env.printf("PLACE mode=%s t=%lld drop_lead=%lld drop_tail=%lld pad_lead=%lld pad_tail=%lld cd_zero_lead=%d cd_only_lead=%ld cd_only_tail=%ld lm=%lld lm2=%lld result=%s%s\n",
                   sch.isEmpty() ? "manual" : "auto", q.t, q.dropLead, q.dropTail, q.padLead, q.padTail, q.cdLead, nLead, nTail, q.Lm, q.Lm2, result, key); }
    // port of lae_enc.cpp:encode lambda silent (one CD frame of the master = up1 master samples, both channels zero)
    static boolean silent(BlkIo.View m, long a, long up1) { for (long i = a; i < a + up1; i++) if (m.get(i * 2) != 0 || m.get(i * 2 + 1) != 0) return false; return true; }
}
