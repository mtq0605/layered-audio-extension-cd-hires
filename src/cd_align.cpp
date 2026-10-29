// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Alignment of an independent CD against the master (encoder side), design D (2026-10-22):
// run() finds where the CD lies in the master and checks it, and changes nothing; place() / applyPlace() / cdOnly() then move the
// master by whole master samples across digital silence (the CD is never changed). Compiled into lae_enc.cpp inside namespace m_align,
// after m_base: the last search level and the check run the master through the encoder's own CD derivation filter (m_base::decimate),
// so no resampling of any kind is involved.
//   search, waveform only, coarse to fine in a fixed cascade: level 0 splits the CD's sound into two halves at a low rate (one fixed
//     linear low-pass and decimation of signed samples per halving) and scans the whole allowed range with whitened cross-correlation
//     (GCC-PHAT, Knapp & Carter 1976), then always scans that range once more around its peak; each next level halves the blocks,
//     doubles the rate and searches a fixed small range around the previous peak; the last level, at the master's own rate, takes the
//     whole master sample with the least waveform residual. Each level pools its blocks and channel pairings into ONE function and
//     uses only the position of its ONE peak. A peak on the edge of a level's range is no result (the search stops).
//   check: three blocks of the CD's sound (head and tail 10 s inside, middle centred; 5 s each; thirds for short pieces), all at that
//     one offset: per block the residual after one gain per channel, and the remaining time difference (what the block loses at the
//     whole master sample against the piece's one fractional best fit near it, pooled over the blocks). Nothing is searched inside a
//     block: the check never gives a block an offset, it only judges at the piece's one offset.
// Every value is printed only. Nothing is stored in the file, transmitted, or used to change the CD, T or F.
// Design rules: no energy / power envelopes; no search repeated because of
// what it found (the second level-0 scan always runs); one pooled peak per level; one offset per piece, never interpolated, smoothed or
// replaced; no default offset for silence; from a correlation only the peak position is used; the check works on waveform differences.

// tag: the reason in one word (also in the ALIGN line). found: the CD's position in the master was found (masterOffset); placeable: it
// needs the master placed (an offset, or a length difference). band: 0 aligned (<= -40 dB, or aligning the fraction
// would gain <= 1 dB), 1 confirm (-40..-33 dB), 2 confirm, the file may grow much (beyond -33 dB), 3 stop. bestNear: around the offset
// that was checked, the whole-sample offset that fits the CD best (pooled over the blocks), relative to it - for the hints of cdalign.
enum { BAND_OK = 0, BAND_CONFIRM = 1, BAND_CONFIRM_LARGE = 2, BAND_STOP = 3 };
struct Report { int verdict = 2; std::vector<std::string> lines; std::string alignLine, why, tag, hint; long cdOffset = 0, masterOffset = 0, placeAt = 0;
                bool placeable = false, found = false, mapDiff = false; int band = BAND_STOP; double timingDb = 99, residDb = 0; long bestNear = 0;
                double frac = 0; int chanMap = 0; std::vector<std::pair<long, long>> blocks; };   // for the phase number: the pooled fraction, the channel order, the usable check blocks (CD frames)

namespace detail {
using cplx = std::complex<double>;
inline void ifft(std::vector<cplx>& a) { for (auto& v : a) v = std::conj(v); fftlib::fft(a); const double s = 1.0 / (double)a.size(); for (auto& v : a) v = std::conj(v) * s; }
inline size_t pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
inline std::string fmt(const char* f, ...) { char b[1024]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
inline double db10(double v) { return v > 0 ? 10.0 * std::log10(v) : -300.0; }
inline std::string dur(double sec) { double a = std::fabs(sec); return a < 1e-3 ? fmt("%.1f us", sec * 1e6) : a < 1.0 ? fmt("%.2f ms", sec * 1e3) : fmt("%.3f s", sec); }
inline long fdiv(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }   // floor division
// the user's limits (2026-10-21/22) and the check blocks (2026-10-22)
// minSpanSec: from 40 s of sound on, the blocks sit at their fixed places (head 10 s in, tail 10 s before the end, middle centred) and
// may reach into a neighbouring third; only their anchor (head start, middle centre, tail end)
// stays inside their own third while a quiet block moves. Shorter pieces use the middle 5 s of each third.
// singleSec: under 3 s of sound the thirds would be shorter than 1 s: one check over the whole sound (and smaller units first: from 15 s
// down to 3 s each third is a block shorter than 5 s)
struct Th { double timingErrDb = -40, timingGainDb = 1, confirmDb = -33, matchResidDb = -3, quietDbfs = -60, blockSec = 5, edgeSec = 10, minSpanSec = 40, singleSec = 3; };
// one fixed linear low-pass for each halving of the search pyramid (windowed sinc, Kaiser beta 8, 47 taps, cut-off 0.22 of the input rate)
inline const std::vector<double>& lp2() { static const std::vector<double> h = [] { std::vector<double> v; const int K = 23; const double fc = 0.22, beta = 8.0;
        auto I0 = [](double z) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (z / 2) / k; s += t * t; } return s; };
        double b0 = I0(beta), sum = 0; for (int k = -K; k <= K; k++) { double w = I0(beta * std::sqrt(1.0 - (k * (double)k) / ((double)K * K))) / b0, s = k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k); v.push_back(s * w); sum += s * w; }
        for (auto& q : v) q /= sum; return v; }(); return h; }
inline std::vector<double> dec2(const std::vector<double>& x) { const std::vector<double>& h = lp2(); const long K = ((long)h.size() - 1) / 2, n = (long)x.size(), m = n / 2; std::vector<double> y((size_t)m);
    for (long i = 0; i < m; i++) { double a = 0; const long c = 2 * i; for (long k = -K; k <= K; k++) { const long j = c - k; if (j >= 0 && j < n) a += h[(size_t)(k + K)] * x[(size_t)j]; } y[(size_t)i] = a; }
    return y; }
}

// cd: the given CD (interleaved int16, ncd frames). b24: the encoder's derived 24-bit base of the master (interleaved, nb frames,
// padded by pb at both ends). master: the unpadded master (interleaved, 24-bit left-aligned int32, Lm frames).
// Convention: CD[j] ~ base[j - o] (o > 0: the CD starts later); in master samples t (CD frame j sits at master sample j * 2^stages - t).
// hintAt: where this master already sits (the encoder's re-check of a placed master), so that cdalign hints count from the original master.
// follows: appended to a result that the encoder acts on ("" for the re-check, where nothing follows).
// lagCentre / rangeCd: the search covers CD offsets lagCentre +- rangeCd (rangeCd < 0: the whole of CD and master either way).
// fixedT: no search; the check runs at this master-sample offset (cdalign=<offset>, its nearest whole sample, and the re-check of a placed master at 0).
// whiten: the search's cross-correlation is whitened (GCC-PHAT, every bin of the band at unit magnitude) or plain (the band's cross-spectrum as
// it is, normalised per window by the two windows' energies). Whitening is not always better, so the encoder runs the
// search both ways; each result is judged only by its own waveform check below, the two are never compared or chosen between here.
template <class M> inline Report run(const std::vector<int16_t>& cd, long ncd, const std::vector<int32_t>& b24, long nb, long pb,
                  const M& master, long Lm, int base, int stages, long long hintAt = 0, const char* follows = "; automatic placement follows",
                  long lagCentre = 0, long rangeCd = -1, const long long* fixedT = nullptr, bool whiten = true) {
    using namespace detail; const Th th; Report R; auto say = [&](const std::string& s) { R.lines.push_back(s); };
    const long need = nb - 2 * pb; const bool lengthOk = ncd == need; const int up = 1 << stages;
    const std::string md5 = alignhash::cdMd5(cd.data(), (size_t)ncd);
    std::vector<double> x[2], r[2];
    for (int c = 0; c < 2; c++) { x[c].resize(ncd); r[c].resize(need);
        for (long j = 0; j < ncd; j++) x[c][j] = 256.0 * cd[(size_t)j * 2 + c];
        for (long i = 0; i < need; i++) r[c][i] = (double)b24[(size_t)(pb + i) * 2 + c]; }
    say(fmt("CD %ld frames (%.3f s), master needs %ld frames at %d Hz (%s); CD MD5 %s", ncd, ncd / (double)base, need, base, lengthOk ? "same length" : "different length", md5.c_str()));
    long cdLead, cdTail, mLead, mTail; alignhash::silenceEdges(cd.data(), ncd, cdLead, cdTail); alignhash::silenceEdges(master.data(), Lm, mLead, mTail);
    auto finish = [&](int v, const char* tag, const std::string& why) { R.verdict = v; R.why = why; R.tag = tag; say("result: " + why); R.band = v == 2 ? BAND_STOP : BAND_OK;
        if (v == 2) R.hint = "not supported: this CD cannot be aligned with this master";
        R.alignLine = fmt("ALIGN verdict=%s reason=%s length=%s md5=%s", v == 0 ? "aligned" : (v == 1 ? "warn" : "stop"), tag, lengthOk ? "same" : "differs", md5.c_str()); return R; };
    if (cdLead == ncd) { if (mLead == Lm) { R.found = true; return finish(lengthOk ? 1 : 2, "both_silent", "CD and master are digital silence throughout: nothing to align"); }
        if (mLead >= Lm - (up - 1) || mTail >= Lm - (up - 1)) { R.found = true;   // the master's sound lies within one CD sample of an edge: a neighbouring piece's, coded by the extension
            return finish(lengthOk ? 1 : 2, "both_silent", fmt("the CD is digital silence throughout; the master has sound only in its %s %ld samples (less than one CD sample, a neighbouring piece's): nothing to align",
                                                               mLead >= Lm - (up - 1) ? "last" : "first", (long)(mLead >= Lm - (up - 1) ? Lm - mLead : Lm - mTail))); }
        if (alignhash::allZero(master.data(), (long)up - 1, Lm - (up - 1))) { R.found = true;   // sound only within less than one CD sample of BOTH edges: the two neighbouring pieces'
            return finish(lengthOk ? 1 : 2, "both_silent", fmt("the CD is digital silence throughout; the master has sound only in its first and last %d samples (less than one CD sample at each edge, the neighbouring pieces'): nothing to align", up - 1)); }
        return finish(2, "silent_cd", "the CD is digital silence throughout, but the master has content: this is not the CD of this master"); }
    const double quietE = std::pow(8388608.0 * std::pow(10.0, th.quietDbfs / 20), 2);   // per-sample energy of a -60 dBFS rms signal
    const long s0 = cdLead, s1 = ncd - cdTail;   // the CD's sound
    auto stopWith = [&](const char* tag, const std::string& why, const std::string& hint) { R.verdict = 2; R.band = BAND_STOP; R.tag = tag; R.why = why; R.hint = hint; say("result: " + why); };
    bool searchOk = true; long oCd = 0;   // the search's result at the CD rate (CD samples)

    // ---- 1. search (skipped when the offset is given)
    if (!fixedT) {
        const long R0 = rangeCd >= 0 ? rangeCd : std::max(ncd, need);
        // level 0 at 1/2^L0 of the CD rate: 1/16, lower only as far as one level-0 FFT must stay within 2^21 points, and at most 1/256 (172 Hz;
        // below about 1/400 the whitening band, 50 Hz to 0.45 of the rate, would be empty). Longer pieces use FFTs of up to 2^22 points at
        // 1/256; a piece that needs more is not searched (review 2026-10-22: before, it found "no sound" in a CD that has sound)
        auto fftAt = [&](int l) { return pow2((size_t)((((s1 - s0) + 2 * (R0 + 2)) >> l) + ((s1 - s0) >> (l + 1)) + 2)); };
        int L0 = 4; while (L0 < 8 && fftAt(L0) > ((size_t)1 << 21)) L0++;
        long o = 0;
        if (fftAt(L0) > ((size_t)1 << 22)) stopWith("toolong", fmt("the CD's sound (%.0f min) and the search range are too long for this version's search", (s1 - s0) / (60.0 * base)),
                                                    "give the offset by hand: cdalign=<offset> (master samples); the check then runs at that offset");
        else {
        std::vector<std::vector<double>> px[2], pr[2];   // levels 1..L0 (level 0 of the pyramid is x / r itself)
        for (int c = 0; c < 2; c++) for (int l = 1; l <= L0; l++) { px[c].push_back(dec2(l == 1 ? x[c] : px[c].back())); pr[c].push_back(dec2(l == 1 ? r[c] : pr[c].back())); }
        // pooled whitened cross-correlation at level l: CD[j] ~ base[j - o] for o in [oc - Rl, oc + Rl] (level samples); ext: each CD window
        // is its block widened by Rl on both sides, as long as the base window (equal lengths), otherwise the block alone
        auto pooled = [&](int l, const std::vector<std::pair<long, long>>& blocks, long oc, long Rl, bool ext, int& used) {
            const std::vector<double>* X[2] = {l ? &px[0][(size_t)l - 1] : &x[0], l ? &px[1][(size_t)l - 1] : &x[1]}; const std::vector<double>* Y[2] = {l ? &pr[0][(size_t)l - 1] : &r[0], l ? &pr[1][(size_t)l - 1] : &r[1]};
            const long n = (long)X[0]->size(), nr = (long)Y[0]->size(); const double fs = base / (double)(1L << l); std::vector<double> P((size_t)(2 * Rl + 1), 0.0); used = 0;
            for (auto& bl : blocks) { const long a = bl.first, b = bl.second, ca = ext ? a - Rl : a, cl = ext ? (b - a) + 2 * Rl : (b - a), ma = a - Rl - oc, ml = (b - a) + 2 * Rl;
                const size_t F = pow2((size_t)(cl + ml)); const int k0 = std::max(1, (int)std::ceil(50.0 * F / fs)), k1 = std::min((int)(F / 2) - 1, (int)std::floor(std::min(16000.0, 0.45 * fs) * F / fs));
                std::vector<cplx> Xf[2], Yf[2]; double ex = 0, ey = 0, exc[2] = {0, 0}, eyc[2] = {0, 0};
                for (int c = 0; c < 2; c++) { Xf[c].assign(F, 0); Yf[c].assign(F, 0);
                    // the CD window gets cosine ramps, 1/8 of its length at each end (found 2026-10-22). Whitened, the edges of a rectangular
                    // window correlate with any abrupt place in the other window and outweigh the music: at the refinement levels with the
                    // master window's own edges (zero shift, so no level below the first refined: offsets came out as multiples of 8 CD
                    // samples); at level 0 with a sudden start or a join in the master (the half's edge at the master's first sound won).
                    const long ramp = std::max(1L, cl / 8);
                    for (long i = 0; i < cl; i++) { const long j = ca + i; if (j >= 0 && j < n) { const long e = std::min(i, cl - 1 - i);
                        const double tw = e >= ramp ? 1.0 : 0.5 - 0.5 * std::cos(M_PI * (e + 0.5) / ramp), v = (*X[c])[(size_t)j] * tw; Xf[c][(size_t)i] = v; ex += v * v; exc[c] += v * v; } }
                    for (long i = 0; i < ml; i++) { const long j = ma + i; if (j >= 0 && j < nr) { const double v = (*Y[c])[(size_t)j]; Yf[c][(size_t)i] = v; ey += v * v; eyc[c] += v * v; } }
                    fftlib::fft(Xf[c]); fftlib::fft(Yf[c]); }
                if (ex <= 0 || ey <= 0 || k1 < k0) continue; used++;
                const int pa[4] = {0, 1, 0, 1}, pm[4] = {0, 1, 1, 0};   // channel pairings LL RR LR RL, all into the one function
                for (int p = 0; p < 4; p++) { std::vector<cplx> G(F, 0); const double en = std::sqrt(exc[pa[p]] * eyc[pm[p]]);   // plain: a correlation coefficient
                    if (!whiten && en <= 0) continue;
                    for (int k = k0; k <= k1; k++) { const cplx g = Yf[pm[p]][(size_t)k] * std::conj(Xf[pa[p]][(size_t)k]); const double mg = std::abs(g);
                        if (mg > 0) { G[(size_t)k] = whiten ? g / mg : g / en; G[F - (size_t)k] = std::conj(G[(size_t)k]); } }
                    ifft(G);
                    for (long o = oc - Rl; o <= oc + Rl; o++) { const long s = ca - o - ma; P[(size_t)(o - oc + Rl)] += std::fabs(G[(size_t)(((s % (long)F) + (long)F) % (long)F)].real()); } } }
            return P; };
        auto peak = [](const std::vector<double>& P) { size_t b = 0; for (size_t i = 1; i < P.size(); i++) if (P[i] > P[b]) b = i; return (long)b; };
        // level 0: two halves of the CD's sound, the whole range, then (always) the same range once more around that peak
        const long a0 = s0 >> L0, b0 = std::max(a0 + 2, s1 >> L0), mid = (a0 + b0) / 2; const long Rl0 = (R0 >> L0) + 2, oc0 = fdiv(lagCentre, 1L << L0);
        std::vector<std::pair<long, long>> halves{{a0, mid}, {mid, b0}}; int used = 0;
        std::vector<double> P = pooled(L0, halves, oc0, Rl0, false, used);
        if (!used) stopWith("nomatch", "no part where both the CD and the master have sound in the searched range", "not supported: this CD cannot be aligned with this master");
        o = oc0 - Rl0 + peak(P);
        if (R.tag.empty()) { const long o1 = o; P = pooled(L0, halves, o1, Rl0, false, used); const long pk = peak(P); o = o1 - Rl0 + pk;
            say(fmt("search level 0 (%s, %.0f Hz, 2 halves, %+.1f..%+.1f s): best %+.3f s; second scan around it: %+.3f s", whiten ? "whitened" : "plain", base / (double)(1L << L0), (oc0 - Rl0) * (double)(1L << L0) / base, (oc0 + Rl0) * (double)(1L << L0) / base,
                    o1 * (double)(1L << L0) / base, o * (double)(1L << L0) / base));
            if (!used || pk == 0 || pk == 2 * Rl0) stopWith("notfound", fmt("the CD's position could not be found: the best fit lies on the edge of the search (%+.1f s); not this master's CD, or further away than the search reaches", o * (double)(1L << L0) / base),
                                                            "not supported: this CD cannot be aligned with this master"); }
        // levels 1 .. L0: blocks halved, rate doubled, +-W around the previous peak (equal-length windows)
        // the first refinement level searches +-32 (level 0 at 1/16 of the CD rate can be off by one period of a low tone, about 8 of its
        // samples, when neighbouring peaks are almost as high); the others +-8. Both ranges are fixed.
        for (int l = L0 - 1; l >= 0 && R.tag.empty(); l--) { const long W = l == L0 - 1 ? 32 : 8, a = s0 >> l, b = std::max(a + 2, s1 >> l), oc = 2 * o; long nbk = 2L << (L0 - l);
            while (nbk > 1 && (b - a) / nbk < 64) nbk /= 2; nbk = std::min(nbk, 64L); std::vector<std::pair<long, long>> bl;
            for (long k = 0; k < nbk; k++) bl.push_back({a + (long)((long long)(b - a) * k / nbk), a + (long)((long long)(b - a) * (k + 1) / nbk)});   // 64-bit: long is 32 bits on Windows
            P = pooled(l, bl, oc, W, true, used); const long pk = peak(P); o = oc - W + pk;
            if (!used || pk == 0 || pk == 2 * W) stopWith("edge", fmt("the search found no clear position at %.0f Hz (the best fit lies on the edge of its range around %+ld): not this master's CD, or an edit", base / (double)(1L << l), oc),
                                                         "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"); }
        if (R.tag.empty()) say(fmt("search levels 1-%d (blocks halved, rate doubled each): %+ld CD samples (%s)", L0, o, dur(o / (double)base).c_str())); }
        oCd = o; searchOk = R.tag.empty(); }

    // ---- 2. the three check blocks (CD samples) and the master-rate curves: residual per block at whole-sample offsets t, with one gain per
    // channel, direct and swapped channel order; through the encoder's own derivation (one decimation per phase, then shifts)
    struct Blk { long a = 0, b = 0; bool use = false; double X2 = 0; std::vector<double> E[2]; std::vector<double> g[2][2]; };
    std::vector<Blk> B3(3); const char* bname[3] = {"head", "middle", "tail"}; bool singleBlock = false;
    { const long span = s1 - s0, bl = (long)(th.blockSec * base), eg = (long)(th.edgeSec * base), third = std::max(1L, span / 3);
      const bool single = span < (long)(th.singleSec * base); if (single) bname[0] = "whole"; singleBlock = single;
      for (int k = 0; k < 3; k++) { long a, b; if (single && k > 0) { B3[k].use = false; continue; }
          if (single) { a = s0; b = s1; }
          else if (span >= (long)(th.minSpanSec * base)) { a = k == 0 ? s0 + eg : (k == 1 ? s0 + span / 2 - bl / 2 : s1 - eg - bl); b = a + bl; }
          else { const long t0 = s0 + k * third, t1 = k == 2 ? s1 : t0 + third, len = std::min(bl, t1 - t0); a = t0 + (t1 - t0 - len) / 2; b = a + len; }
          const long guard = std::min(span / 8, 1024L + 4L * up);   // keep the derivation filter's reach and the +-2 CD-sample shifts inside the piece
          a = std::max(a, s0 + guard); b = std::min(b, s1 - guard);
          const long lo3 = single ? s0 : s0 + k * third, hi3 = (single || k == 2) ? s1 : s0 + (k + 1) * third, step = std::max(1L, std::min(bl, b - a));
          auto loud = [&](long p, long q) { double e = 0; for (long j = p; j < q; j++) e += x[0][j] * x[0][j] + x[1][j] * x[1][j]; return q > p && e >= 2.0 * (q - p) * quietE; };
          const bool fixedPos = span >= (long)(th.minSpanSec * base);
          auto inside = [&](long p, long q) {   // fixed places: the anchor in its third, the block in the piece's sound; thirds: the block in its third
              if (!fixedPos) return p >= std::max(0L, lo3) && q <= std::min(ncd, hi3);
              const long an = k == 0 ? p : (k == 2 ? q : (p + q) / 2); return an >= lo3 && an <= hi3 && p >= s0 && q <= s1; };
          while (inside(a, b) && !loud(a, b)) { if (k == 2) { a -= step; b -= step; } else { a += step; b += step; } }   // quiet: fixed steps, the anchor inside its third
          B3[k].a = a; B3[k].b = b; B3[k].use = inside(a, b) && b - a >= 64 && loud(a, b); } }
    const long long cM = fixedT ? *fixedT : (long long)oCd * up;   // master-sample centre
    const long long tLo = cM - 2 * up, tHi = cM + 2 * up; const int nT = (int)(tHi - tLo + 1);
    std::vector<double> diag[2]; long diagA = 0, diagN = 0;   // the middle block's prediction at the final offset, for the printed EQ / coherence
    auto curves = [&]() {
        const long margin = 2048;   // master samples around the needed span (the derivation filter's reach)
        for (int k = 0; k < 3; k++) { Blk& K = B3[k]; if (!K.use) continue; const long a = K.a, b = K.b;
            for (int m = 0; m < 2; m++) { K.E[m].assign((size_t)nT, -1.0); for (int c = 0; c < 2; c++) K.g[m][c].assign((size_t)nT, 0.0); }
            K.X2 = 0; for (int c = 0; c < 2; c++) for (long j = a; j < b; j++) K.X2 += x[c][j] * x[c][j];
            for (int ph = 0; ph < up; ph++) {   // t with (-t) mod up == ph: base_t[j] = D_ph[j - (t + ph) / up]
                long long qmin = LLONG_MAX, qmax = LLONG_MIN; for (long long t = tLo; t <= tHi; t++) if ((((-t) % up) + up) % up == ph) { const long long q = (t + ph) / up; qmin = std::min(qmin, q); qmax = std::max(qmax, q); }
                if (qmin == LLONG_MAX) continue;
                const long long uLo = a - qmax, uHi = b - qmin, sM = uLo * up + ph - margin, wM = (uHi - uLo) * up + 2 * margin;
                if (sM < 0 || sM + wM > Lm) { K.use = false; break; }
                std::vector<double> D[2];
                for (int c = 0; c < 2; c++) { std::vector<int32_t> q((size_t)wM); for (long long i = 0; i < wM; i++) q[(size_t)i] = master[(size_t)(sM + i) * 2 + c] >> 4;
                    for (int st = stages - 1; st >= 0; st--) q = m_base::decimate(q, st);
                    D[c].resize((size_t)(uHi - uLo)); for (long long u = 0; u < uHi - uLo; u++) D[c][(size_t)u] = (double)(((int64_t)q[(size_t)(margin / up + u)] + 8) >> 4); }
                for (long long t = tLo; t <= tHi; t++) { if ((((-t) % up) + up) % up != ph) continue; const long long q = (t + ph) / up; const size_t ti = (size_t)(t - tLo);
                    for (int m = 0; m < 2; m++) { double E = 0;
                        for (int c = 0; c < 2; c++) { const std::vector<double>& pv = D[m ? 1 - c : c]; double sxr = 0, srr = 0, sxx = 0;
                            for (long j = a; j < b; j++) { const double xv = x[c][j], rv = pv[(size_t)(j - q - uLo)]; sxr += xv * rv; srr += rv * rv; sxx += xv * xv; }
                            const double g = srr > 0 ? sxr / srr : 0; K.g[m][c][ti] = g; E += sxx - g * sxr; }
                        K.E[m][ti] = std::max(E, 0.0); } } } } };
    if (searchOk) curves();
    // the drift / edit check (2026-10-22): per block, the residual at exactly two FIXED positions, 2 CD samples before and after
    // t, with the gains fitted at t, sample by sample over the block's own N samples. Compared only with the residual at t, and only to stop:
    // it never picks a side, moves t or searches on. -1: not possible here (the master ends too close), 0: t holds, 1: a side fits better.
    auto sideCheck = [&](const Blk& K, long long tt, int m) -> int {
        const long margin = 2048, a = K.a, b = K.b; const int ph = (int)((((-tt) % up) + up) % up); const long long q = (tt + ph) / up;
        const long long uLo = a - q - 2, uHi = b - q + 2, sM = uLo * up + ph - margin, wM = (uHi - uLo) * up + 2 * margin;
        if (sM < 0 || sM + wM > Lm) return -1;
        const size_t ti = (size_t)(tt - tLo); double e0 = 0, eM = 0, eP = 0;
        for (int c = 0; c < 2; c++) { std::vector<int32_t> s((size_t)wM); for (long long i = 0; i < wM; i++) s[(size_t)i] = master[(size_t)(sM + i) * 2 + (m ? 1 - c : c)] >> 4;
            for (int st = stages - 1; st >= 0; st--) s = m_base::decimate(s, st);
            const double g = K.g[m][c][ti];   // base_{t + 2 CD}[j] = D[j - q - 2], base_t[j] = D[j - q], base_{t - 2 CD}[j] = D[j - q + 2]
            auto D = [&](long long u) { return (double)(((int64_t)s[(size_t)(margin / up + (u - uLo))] + 8) >> 4); };
            for (long j = a; j < b; j++) { const double xv = x[c][j], r0 = xv - g * D(j - q), rP = xv - g * D(j - q - 2), rM = xv - g * D(j - q + 2); e0 += r0 * r0; eP += rP * rP; eM += rM * rM; } }
        return (eM < e0 || eP < e0) ? 1 : 0; };
    int usable = 0; for (auto& K : B3) if (K.use) usable++;
    if (searchOk && !usable) { searchOk = false; stopWith("quiet", "too quiet to check: no check block has sound on both sides (each CD block below -60 dBFS, or outside the master)", "not supported: this CD cannot be aligned with this master"); }
    // pooled residual over the blocks for one channel order. The order is judged once, at the one offset cM, by the waveform difference
    // there (swapped only when it explains the CD clearly better, 3 dB); the master-rate level then has one function (review 2026-10-22,
    // before, it built both and kept the one that searched better)
    auto pooledE = [&](int m, size_t ti) { double E = 0, X2 = 0; for (auto& K : B3) if (K.use && K.E[m][ti] >= 0) { E += K.E[m][ti]; X2 += K.X2; } return X2 > 0 ? E / X2 : 1.0; };
    int map = 0; long long t = cM, tb = cM;
    if (searchOk) {
        const size_t ci = (size_t)(cM - tLo); map = pooledE(1, ci) < pooledE(0, ci) * 0.5 ? 1 : 0;
        // every whole master sample within two CD samples of the CD-rate result (one was too tight: under a minimum-phase EQ the CD-rate
        // result can be one CD sample off, and the true fit then sat on the edge; review 2026-10-22); the ends only feed the fraction
        const long long uA = cM - 2 * up + 1, uB = cM + 2 * up - 1;
        for (long long u = uA; u <= uB; u++) if (pooledE(map, (size_t)(u - tLo)) < pooledE(map, (size_t)(tb - tLo))) tb = u;
        if (!fixedT) { t = tb; if (t == uA || t == uB) { searchOk = false; stopWith("edge", fmt("the master-sample fit lies on the edge of its range around %+lld master samples: not this master's CD, or an edit", cM),
                                                                                    "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"); }
                     else say(fmt("search at the master rate (%d Hz): %+lld master samples (%s; %+.2f CD samples)", base * up, t, dur(t / (double)(base * up)).c_str(), t / (double)up)); }
        R.bestNear = (long)(tb - t); }
    R.masterOffset = (long)t; R.cdOffset = (long)std::lround(t / (double)up); R.placeAt = (long)t;

    // ---- 3. the check at t (never an offset per block): residual, and the remaining time difference against the best fit near t
    int band = BAND_OK; double worstT = -300, worstR = -300; int nMis = 0, nDrift = 0; std::string blocks; double gainDb[2] = {0, 0}; int pol[2] = {1, 1};
    // the reference: the piece's one fractional optimum (pooled over the blocks) near the best whole sample tb, tb + dFr. Automatic: tb = t.
    // By hand (cdalign=<n>): the loss at n includes its distance to tb (review 2026-10-22: before, n a few samples off counted as aligned)
    double dFr = 0; const size_t bi = (size_t)(tb - tLo);
    if (searchOk) { const double ea = pooledE(map, bi - 1), eb = pooledE(map, bi), ec = pooledE(map, bi + 1), den = ea - 2 * eb + ec;
        if (den > 0) dFr = std::max(-0.5, std::min(0.5, 0.5 * (ea - ec) / den)); }
    double worstAsk = -300;   // the largest remaining time difference among the blocks that ask for a confirmation (for the message)
    if (searchOk) {
        for (int k = 0; k < 3; k++) { Blk& K = B3[k]; if (!K.use) { if (singleBlock && k > 0) continue;   // under 3 s of sound there is one block only
                blocks += fmt("%s%s:n/a", blocks.empty() ? "" : ",", bname[k]); say(fmt("check %s: not usable (quiet, or outside the master)", bname[k])); continue; }
            const size_t ti = (size_t)(t - tLo); const double e = K.E[map][ti] / K.X2; const double rd = db10(e);
            // the remaining time difference: what this block loses at the whole sample t against the piece's fractional optimum t + dFr
            // (each block's own best fit drifts with its spectrum under a minimum-phase EQ; that is the CD's processing, not timing)
            const double ea = K.E[map][bi - 1] / K.X2, eb = K.E[map][bi] / K.X2, ec = K.E[map][bi + 1] / K.X2;
            const double eMin = std::max(eb + dFr * (ec - ea) / 2 + dFr * dFr * (ec - 2 * eb + ea) / 2, 0.01 * e);
            const double tE = db10(std::max(e - eMin, 1e-30)), gE = db10(e / std::max(eMin, 1e-30));
            const bool mis = rd > th.matchResidDb; const int sd = mis ? 0 : sideCheck(K, t, map); const bool drift = sd == 1; int bb;
            if (mis || drift) bb = BAND_STOP; else if (tE <= th.timingErrDb || gE <= th.timingGainDb) bb = BAND_OK; else if (tE <= th.confirmDb) bb = BAND_CONFIRM; else bb = BAND_CONFIRM_LARGE;
            if (mis) nMis++; if (drift) nDrift++; if (bb == BAND_CONFIRM || bb == BAND_CONFIRM_LARGE) worstAsk = std::max(worstAsk, tE);
            band = std::max(band, bb); worstT = std::max(worstT, std::max(tE, -99.0)); worstR = std::max(worstR, rd);
            blocks += fmt("%s%s:%.1f/%.1f", blocks.empty() ? "" : ",", bname[k], std::max(rd, -99.0), std::max(tE, -99.0));
            say(fmt("check %s (%.1f-%.1f s of the CD): residual %.1f dB re the CD, remaining time difference %.1f dB (aligning the fraction would gain %.1f dB)%s",
                    bname[k], K.a / (double)base, K.b / (double)base, rd, std::max(tE, -99.0), gE,
                    mis ? ": DOES NOT MATCH here" : (drift ? "; drift check: the music fits better 2 CD samples away than at the piece's offset (drift, an edit or another take)"
                                                        : (sd < 0 ? "; drift check: not possible here (the master ends too close)" : "; drift check (2 CD samples either side): holds"))));
            if (k == 1 || (diagN == 0 && k == 2) || (diagN == 0 && k == 0)) { for (int c = 0; c < 2; c++) { gainDb[c] = K.g[map][c][ti] != 0 ? 20 * std::log10(std::fabs(K.g[map][c][ti])) : -999; pol[c] = K.g[map][c][ti] >= 0 ? 1 : -1; } diagA = K.a; diagN = K.b - K.a; } }
        R.mapDiff = map == 1 || pol[0] < 0 || pol[1] < 0; R.timingDb = worstT; R.residDb = worstR; R.band = band;
        R.frac = (double)(tb - t) + dFr; R.chanMap = map; for (auto& K : B3) if (K.use) R.blocks.push_back({K.a, K.b});
        if (R.mapDiff) say(fmt("channels: %s; polarity L %s, R %s", map ? "SWAPPED (CD left = master right)" : "in order", pol[0] > 0 ? "normal" : "INVERTED", pol[1] > 0 ? "normal" : "INVERTED")); }
    R.found = searchOk && band != BAND_STOP;

    // ---- 4. diagnostics, printed only: level, EQ, coherence, pre-emphasis (middle block), clipping, level over time, edges
    double eqDev = 0, cohEdge = 0; bool preEmph = false; long clipRuns = 0; bool onlySilence = true; const long o = R.cdOffset;
    if (R.found && diagN >= 8192) {   // the prediction at the final offset for the diagnostic block, through the same derivation
        const long long q = (long long)fdiv((long)(t + ((((-t) % up) + up) % up)), up); const int ph = (int)((((-t) % up) + up) % up);
        const long margin = 2048; const long long sM = (diagA - q) * up + ph - margin, wM = (long long)diagN * up + 2 * margin;
        if (sM >= 0 && sM + wM <= Lm) { for (int c = 0; c < 2; c++) { std::vector<int32_t> qq((size_t)wM); for (long long i = 0; i < wM; i++) qq[(size_t)i] = master[(size_t)(sM + i) * 2 + c] >> 4;
                for (int st = stages - 1; st >= 0; st--) qq = m_base::decimate(qq, st);
                diag[c].resize((size_t)diagN); for (long k = 0; k < diagN; k++) diag[c][(size_t)k] = (double)(((int64_t)qq[(size_t)(margin / up + k)] + 8) >> 4); }
            say(fmt("level: CD relative to the master's derived base: L %+.2f dB, R %+.2f dB", gainDb[0], gainDb[1]));
            const int Ns = 8192; std::vector<double> hw(Ns); for (int k = 0; k < Ns; k++) hw[k] = 0.5 - 0.5 * std::cos(2 * M_PI * (k + 0.5) / Ns);
            std::vector<cplx> Sxr(Ns / 2, 0); std::vector<double> Sxx(Ns / 2, 0), Srr(Ns / 2, 0);
            for (long w0 = 0; w0 + Ns <= diagN; w0 += Ns / 2) for (int a = 0; a < 2; a++) { std::vector<cplx> X(Ns), Y(Ns); const std::vector<double>& rb = diag[map ? 1 - a : a];
                for (int k = 0; k < Ns; k++) { X[k] = x[a][diagA + w0 + k] * hw[k]; Y[k] = pol[a] * rb[w0 + k] * hw[k]; } fftlib::fft(X); fftlib::fft(Y);
                for (int k = 0; k < Ns / 2; k++) { Sxr[k] += X[k] * std::conj(Y[k]); Sxx[k] += std::norm(X[k]); Srr[k] += std::norm(Y[k]); } }
            const double bands[] = {31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
            const int nbd = sizeof bands / sizeof bands[0]; std::vector<double> mag(nbd, 0), coh(nbd, 0); std::vector<bool> ok(nbd, false); double ref1k = 0;
            for (int b = 0; b < nbd; b++) { int k0 = (int)std::ceil(bands[b] * std::pow(2.0, -1.0 / 6) * Ns / base), k1 = std::min((int)std::floor(bands[b] * std::pow(2.0, 1.0 / 6) * Ns / base), Ns / 2 - 1);
                cplx sr = 0; double sx = 0, sy = 0; for (int k = std::max(k0, 1); k <= k1; k++) { sr += Sxr[k]; sx += Sxx[k]; sy += Srr[k]; }
                if (sy > 0 && sx > 0) { ok[b] = true; mag[b] = 20 * std::log10(std::abs(sr) / sy + 1e-300); coh[b] = std::norm(sr) / (sx * sy); } if (bands[b] == 1000) ref1k = mag[b]; }
            std::string eq; double meanEmphErr = 0, meanFlat = 0; int ne = 0;
            auto emph = [](double f) { return 10 * std::log10((1 + std::pow(f / 3183.1, 2)) / (1 + std::pow(f / 10610.3, 2))); };
            for (int b = 0; b < nbd; b++) if (ok[b]) { double rel = mag[b] - ref1k; if (bands[b] <= 16000) eqDev = std::max(eqDev, std::fabs(rel));
                if (bands[b] >= 2000 && bands[b] <= 16000) { meanEmphErr += std::fabs(rel - (emph(bands[b]) - emph(1000))); meanFlat += std::fabs(rel); ne++; }
                if (bands[b] == 63 || bands[b] == 250 || bands[b] == 1000 || bands[b] == 4000 || bands[b] == 10000 || bands[b] == 16000) eq += fmt(" %g Hz %+.1f dB", bands[b], rel); }
            if (ne) { meanEmphErr /= ne; meanFlat /= ne; preEmph = meanEmphErr < 1.5 && meanFlat > 3.0; }
            for (int b = 0; b < nbd; b++) if (ok[b] && bands[b] >= 1000) { if (coh[b] >= 0.9) cohEdge = bands[b]; else break; }
            say(fmt("EQ relative to 1 kHz:%s; largest deviation %.1f dB (31.5 Hz - 16 kHz); coherence >= 0.9 up to %.0f Hz", eq.c_str(), eqDev, cohEdge));
            if (preEmph) say("WARNING: the CD looks pre-emphasised (50/15 us curve). A WAV cannot carry the emphasis flag, so the CD layer would play with the wrong EQ"); } }
    if (R.found) {
        for (int c = 0; c < 2; c++) { int run = 0; for (long j = 0; j < ncd; j++) { int16_t v = cd[(size_t)j * 2 + c]; if (v == 32767 || v == -32768) { if (++run == 3) clipRuns++; } else run = 0; } }
        if (clipRuns) say(fmt("clipping: %ld runs of 3 or more full-scale samples in the CD", clipRuns));
        double lvMin = 1e9, lvMax = -1e9; long a0 = std::max(0L, o), a1 = std::min(ncd, need + o);
        for (long w0 = a0; w0 + base <= a1; w0 += base) { double sx = 0, sr = 0; for (long j = w0; j < w0 + base; j++) for (int c = 0; c < 2; c++) { sx += x[c][j] * x[c][j]; sr += r[c][j - o] * r[c][j - o]; }
            if (sr < 2.0 * base * quietE || sx <= 0) continue; double d = db10(sx / sr); lvMin = std::min(lvMin, d); lvMax = std::max(lvMax, d); }
        if (lvMax >= lvMin) { say(fmt("level difference per second: %+.2f to %+.2f dB", lvMin, lvMax)); if (lvMax - lvMin > 3.0) say("note: the level difference changes over time by more than 3 dB (fade, compression or a different master)"); }
        struct Edge { const char* what; long n; bool silent; }; std::vector<Edge> edges; long endCd = ncd - o;
        if (o > 0) edges.push_back({"only the CD has (start)", o, cdLead >= o});
        if (o < 0) edges.push_back({"only the master has (start)", -o, mLead >= -o * up});
        if (endCd < need) edges.push_back({"only the master has (end)", need - endCd, Lm - endCd * up <= mTail});
        if (endCd > need) edges.push_back({"only the CD has (end)", endCd - need, cdTail >= endCd - need});
        for (auto& e : edges) { say(fmt("%s: %ld CD frames (%.3f s), %s", e.what, e.n, e.n / (double)base, e.silent ? "digital silence" : "CONTENT")); if (!e.silent) onlySilence = false; } }

    // ---- 5. decision (the CD and the master are never changed)
    auto shifted = [&](long long tt) { return fmt("the CD is shifted against the master by %lld master samples (%s; %.2f CD samples)", tt, dur(tt / (base * (double)up)).c_str(), tt / (double)up); };
    if (searchOk && R.tag.empty()) {
        R.placeable = R.found && (t != 0 || !lengthOk);
        if (band == BAND_STOP) { R.verdict = 2;
            if (nMis > 0 && nMis < usable) { R.tag = "edit"; R.why = fmt("%d of %d check blocks do not match at this offset (residual above -3 dB there): an edit, a different speed (drift) or another take", nMis, usable);
                R.hint = "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"; }
            else if (nMis == 0) { R.tag = "drift"; R.why = fmt("in %d of %d check blocks the music fits better 2 CD samples away than at the piece's one offset: a different speed (drift), an edit or another take", nDrift, usable);
                R.hint = "not supported: a different speed (drift), an edit or another take cannot be aligned by one offset"; }
            else { R.tag = "nomatch"; R.why = "the CD does not match this master at the best position (residual above -3 dB in every check block)"; R.hint = "not supported: this CD cannot be aligned with this master"; } }
        else if (R.placeable) { R.verdict = 2; R.tag = t != 0 ? "shift" : "length";
            R.why = (t != 0 ? shifted(t) : std::string("the CD is aligned but ") + (onlySilence ? "longer or shorter by digital silence only" : "its length differs and the difference contains content")) + follows;
            R.hint = fmt("set the whole offset by hand: cdalign=%+lld (master samples; a fraction can be added, as cdalign=3.5)", hintAt + t); }
        else if (band == BAND_OK) { R.verdict = R.mapDiff ? 1 : 0; R.tag = R.mapDiff ? "channels_differ" : "aligned";
            R.why = std::string("the CD is aligned with the master") + fmt(" (check blocks: remaining time difference at most %.1f dB re the CD)", worstT) + (R.mapDiff ? ", but the channel order or polarity differs" : ""); }
        else { R.verdict = 2; R.tag = band == BAND_CONFIRM ? "confirm" : "confirm_large";
            R.why = fmt("the remaining time difference changes the waveform by up to %.1f dB re the CD (%s): %s", worstAsk, band == BAND_CONFIRM ? "between -40 and -33 dB" : "beyond -33 dB",
                        band == BAND_CONFIRM ? "close, but not aligned within the limit" : "the CD may have been made differently from this master (another EQ or mastering, a different speed or an edit), and the file may get much larger");
            R.hint = R.bestNear ? fmt("still shifted by %+ld master samples: try cdalign=%+lld", R.bestNear, hintAt + t + R.bestNear) : "confirm to continue (a console asks; in scripts add cdaccept)"; }
        say("result: " + R.why); }
    if (R.verdict != 2 && R.mapDiff) say("WARNING: channel order or polarity differs from the master: the extension will code that difference (much larger file)");
    const char* vtxt = !R.found ? "stop" : (R.placeable ? "place" : (band == BAND_OK ? (R.verdict == 0 ? "aligned" : "warn") : (band == BAND_STOP ? "stop" : "confirm")));
    R.alignLine = fmt("ALIGN verdict=%s reason=%s cd_offset=%ld master_offset=%ld blocks=%s timing=%.1f resid=%.1f map=%s pol=%c%c gain=%+.2f,%+.2f eqdev=%.1f coh_edge=%.0f preemph=%d clip_runs=%ld length=%s md5=%s",
                      vtxt, R.tag.c_str(), R.cdOffset, R.masterOffset, blocks.empty() ? "n/a" : blocks.c_str(), std::max(worstT, -99.0), std::max(worstR, -99.0), map ? "swapped" : "direct",
                      pol[0] > 0 ? '+' : '-', pol[1] > 0 ? '+' : '-', gainDb[0], gainDb[1], eqDev, cohEdge, preEmph ? 1 : 0, clipRuns,
                      lengthOk ? "same" : (!R.found ? "differs" : (onlySilence ? "differs_silence" : "differs_content")), md5.c_str());   // the edges are only examined once found
    return R; }
// ---- placement on a common time line (encoder step after run()): whole master samples only, nothing is resampled. The rule (user
// 2026-10-21): alignment is length matching; whichever is longer at the start plays first, whichever is longer at the end ends later,
// and the shorter side gets digital silence there. Neither side loses a non-silent sample:
//   - the master is moved by whole master samples (zeros added in front, or its own leading digital silence dropped);
//   - when the master's sound starts less than one CD sample before the CD's, one CD frame of digital silence is added in front of the
//     CD layer (the CD's own samples are never changed); at the end the format's spare samples keep up to 2^stages - 1 master samples.
//     One CD sample or more of sound that only the master has, at either end, is not supported in this step (step 4).
// M'[i] = master[i - shift]; Lm2 is the length the (padded) CD needs: ncd2 * 2^stages ... ncd2 * 2^stages + 2^stages - 1.
// Zeros added to the master must meet digital silence on the CD (cdOnly(), once the CD layer has been derived again from M').
struct Place { bool ok = false; long long t = 0, shift = 0, Lm = 0, Lm2 = 0, dropLead = 0, dropTail = 0, padLead = 0, padTail = 0, a0 = -1, z1 = -1; long ncd2 = 0; int cdLead = 0;
               std::string why; std::vector<std::string> lines; };
template <class M> inline Place place(const M& master, long long Lm, long ncd, int stages, long long t, int rate) {
    using namespace detail; Place p; p.t = t; p.Lm = Lm; const long long up = 1LL << stages;
    auto sec = [&](long long i) { return i / (double)rate; };
    p.lines.push_back(fmt("placement: master moved by %+lld master samples (%+.2f CD samples, %s; %s)", t, t / (double)up, dur(t / (double)rate).c_str(),
                          t > 0 ? "the CD starts later" : (t < 0 ? "the CD starts earlier" : "same start")));
    auto nz = [&](long long i) { return master[(size_t)i * 2] != 0 || master[(size_t)i * 2 + 1] != 0; };
    long long a0 = -1, z1 = -1; for (long long i = 0; i < Lm && a0 < 0; i++) if (nz(i)) a0 = i; for (long long i = Lm - 1; i >= 0 && z1 < 0; i--) if (nz(i)) z1 = i;
    p.a0 = a0; p.z1 = z1; if (a0 < 0) { p.why = "the master is digital silence throughout"; return p; }
    // start: the master's first sound at t + a0 (CD frame 0 at 0)
    const long long early = -(t + a0);   // > 0: the master's sound starts this many master samples before the CD's first sample
    if (early >= up) { p.why = fmt("placement at %+lld: the master has sound %lld master samples (%s, one CD sample or more) before the CD starts, at %.6f-%.6f s of the master: content only the master has (not supported in this step)",
                                   t, early, dur(early / (double)rate).c_str(), sec(a0), sec(a0 + early)); return p; }
    if (early > 0) { p.cdLead = 1; p.lines.push_back(fmt("the master's sound starts %lld master samples (%s) before the CD's first sample: one CD frame of digital silence goes in front of the CD layer, so the master plays from its own first sound (the CD's samples are unchanged)",
                                                          early, dur(early / (double)rate).c_str())); }
    p.shift = t + p.cdLead * up;
    // end: the master's last sound at shift + z1; the CD covers positions up to ncd2 * up, and the format keeps up to 2^stages - 1 master
    // samples beyond that (a master length need not be a multiple of 2^stages), so a shorter overhang needs nothing added to the CD
    const long ncd2 = ncd + p.cdLead; const long long hi = (long long)ncd2 * up + up - 1;
    const long long late = p.shift + z1 + 1 - (long long)ncd2 * up;   // > 0: the master's sound goes on this many master samples after the CD's last sample
    if (late >= up) { p.why = fmt("placement at %+lld: the master has sound %lld master samples (%s, one CD sample or more) after the CD ends, at %.6f-%.6f s of the master: content only the master has (not supported in this step)",
                                  t, late, dur(late / (double)rate).c_str(), sec(z1 + 1 - late), sec(z1 + 1)); return p; }
    if (late > 0) p.lines.push_back(fmt("the master's sound ends %lld master samples (%s) after the CD's last sample: kept, the master plays on that much longer", late, dur(late / (double)rate).c_str()));    const long long lo = (long long)ncd2 * up; p.ncd2 = ncd2;
    p.Lm2 = p.shift + Lm <= hi ? std::max(p.shift + Lm, lo) : std::max(p.shift + z1 + 1, lo);   // past what the CD needs, the master's trailing silence is dropped
    p.dropLead = std::max(0LL, -p.shift); p.dropTail = std::max(0LL, p.shift + Lm - p.Lm2); p.padLead = std::max(0LL, p.shift); p.padTail = std::max(0LL, p.Lm2 - (p.shift + Lm));
    if (p.dropLead + p.dropTail >= Lm || p.Lm2 - p.padLead - p.padTail <= 0) { p.why = fmt("placement at %+lld leaves no overlap between the CD and the master", t); return p; }
    p.lines.push_back(fmt("master samples dropped (digital silence only): start %lld, end %lld; zeros added to the master: start %lld, end %lld; digital-silence frames added in front of the CD layer: %d",
                          p.dropLead, p.dropTail, p.padLead, p.padTail, p.cdLead));
    p.lines.push_back(fmt("placed master: %lld frames (was %lld) = %ld CD frames x %lld + %lld", p.Lm2, Lm, ncd2, up, p.Lm2 - lo));
    p.ok = true; return p; }// in place (no second copy of the master): m becomes M' with Lm2 frames
inline void applyPlace(std::vector<int32_t>& m, const Place& p) {
    const long long t = p.shift, n = p.Lm, n2 = p.Lm2, s0 = std::max(0LL, -t), s1 = std::min(n, n2 - t);   // master samples [s0, s1) are kept, at s + t
    if (n2 > n) m.resize((size_t)n2 * 2, 0);
    if (t > 0) std::copy_backward(m.begin() + 2 * s0, m.begin() + 2 * s1, m.begin() + 2 * (s1 + t)); else if (t < 0) std::copy(m.begin() + 2 * s0, m.begin() + 2 * s1, m.begin() + 2 * (s0 + t));
    std::fill(m.begin(), m.begin() + 2 * (s0 + t), 0); std::fill(m.begin() + 2 * (s1 + t), m.begin() + 2 * n2, 0); m.resize((size_t)n2 * 2); }
// CD frames whose centre sample (j * 2^stages) lies in zeros added to the master; there the CD must be digital silence wherever the base
// derived from M' is exactly zero on both channels (the derivation filter's own transition next to the master's first and last samples
// is not content of the CD alone). Returns "" or the reason, with the frame and time ranges.
inline std::string cdOnly(const std::vector<int16_t>& cd, long ncd, const std::vector<int32_t>& b24, long pb, const Place& p, int stages, int base, long& nLead, long& nTail) {
    using namespace detail; const long long up = 1LL << stages;
    const long jl = (long)std::min<long long>(ncd, (p.padLead + up - 1) / up), jt = (long)std::max<long long>(jl, std::min<long long>(ncd, (p.Lm2 - p.padTail + up - 1) / up));
    nLead = jl; nTail = ncd - jt;
    auto scan = [&](long a, long b, const char* where) { long f = -1, l = -1;
        for (long j = a; j < b; j++) if (!b24[(size_t)(pb + j) * 2] && !b24[(size_t)(pb + j) * 2 + 1] && (cd[(size_t)j * 2] || cd[(size_t)j * 2 + 1])) { if (f < 0) f = j; l = j; }
        return f < 0 ? std::string() : fmt("the CD has content where the placed master has none (%s): CD frames %ld-%ld (%.6f-%.6f s) are not digital silence: not supported in this step (only one side has content)",
                                          where, f, l, f / (double)base, (l + 1) / (double)base); };
    std::string r = scan(0, jl, "start"); if (r.empty()) r = scan(jt, ncd, "end"); return r; }
// ---- measuring the fraction: the master advanced by a (in units U = 2^22 >> stages per master sample, |a| <= U) through the encoder's own
// derivation, with the closed-form Lagrange fractional delay of frac_delay.h: z[n] = x(n + a/U), which is the delay U - a one sample later
// for a > 0 and the delay -a for a < 0. Used only to measure; the master itself is moved by moveMaster (below). Nothing about it is stored.
inline void advanceTaps(int64_t a, int stages, int32_t h[lae_fd::TAPS]) { const int64_t U = lae_fd::unitsPerMaster(stages);
    if (a > 0) lae_fd::taps(U - a, stages, h); else if (a < 0) lae_fd::taps(-a, stages, h); }
template <class Get> inline int64_t advanced(const int32_t* h, int64_t a, long n, Get get) { return a == 0 ? get(n) : lae_fd::apply(h, a > 0 ? n + 1 : n, get); }
// the whole master (24-bit left-aligned) advanced by a, computed on x >> 4 like the derivation (four guard bits, kept in the result)
inline std::vector<int32_t> advance(const std::vector<int32_t>& m, long long Lm, int64_t a, int stages) {
    if (a == 0) return m; int32_t h[lae_fd::TAPS]; advanceTaps(a, stages, h); std::vector<int32_t> z(m.size());
    for (int c = 0; c < 2; c++) for (long long i = 0; i < Lm; i++) {
        const int64_t v = advanced(h, a, (long)i, [&](long j) { j = j < 0 ? -j : (j >= (long)Lm ? (long)(2 * (Lm - 1) - j) : j); j = std::max(0L, std::min((long)Lm - 1, j)); return (int64_t)(m[(size_t)j * 2 + c] >> 4); });
        z[(size_t)i * 2 + c] = (int32_t)std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, v * 16)); }
    return z; }
// The master moved by the fraction (design A, 2026-10-23: the fraction is applied to the MASTER at alignment, the file stays v5 and the
// decoder is unchanged): Z(i) = M(i + f), the band-limited value of the placed master at i + f, |f| <= 1/2 master sample, by a Kaiser-
// windowed sinc of 128 taps (half length 64, beta 10; the taps computed once from f by the closed formula, no table), digital silence
// outside the master; rounded to 24 bit and hard-clipped (nclip counts). Encoder only: nothing about it is stored. The same kernel as
// cd_variant mfdelay (d = -f), so a test can rebuild Z independently.
inline double moveKern(double u) { const int kH = 64; const double beta = 10.0; if (std::fabs(u) >= kH) return 0;
    auto I0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (v / (2 * k)) * (v / (2 * k)); s += t; if (t < 1e-17 * s) break; } return s; };
    const double s = u == 0 ? 1.0 : std::sin(M_PI * u) / (M_PI * u), r = u / kH; return s * I0(beta * std::sqrt(1 - r * r)) / I0(beta); }
inline std::vector<int32_t> moveMaster(const std::vector<int32_t>& m, long long Lm, double f, long& nclip) {
    nclip = 0; if (f == 0) return m; const int kH = 64; const long long sh = (long long)std::floor(f); const double fr = f - (double)sh;   // i + f = (i + sh) + fr
    double k[2 * kH]; for (int q = -kH + 1; q <= kH; q++) k[q + kH - 1] = moveKern(fr - q);
    std::vector<int32_t> z(m.size(), 0);
    for (int c = 0; c < 2; c++) for (long long i = 0; i < Lm; i++) { const long long i0 = i + sh; double s = 0;
        for (int q = -kH + 1; q <= kH; q++) { const long long j = i0 + q; if (j < 0 || j >= Lm) continue; s += (double)(m[(size_t)j * 2 + c] >> 8) * k[q + kH - 1]; }
        long long v = std::llround(s); if (v > 8388607) { v = 8388607; nclip++; } if (v < -8388608) { v = -8388608; nclip++; } z[(size_t)i * 2 + c] = (int32_t)(v * 256); }
    return z; }
// The same two steps on a master held in a raw file (the encoder, 2026-10-29): read through blkio::View, written piece by piece to out;
// every output sample is computed exactly as by applyPlace() / moveMaster() above.
inline void applyPlaceFile(const blkio::View& m, const Place& p, const std::string& out) {
    const long long t = p.shift, n = p.Lm, n2 = p.Lm2, s0 = std::max(0LL, -t), s1 = std::min(n, n2 - t);   // master samples [s0, s1) are kept, at s + t
    blkio::Writer w(out.c_str()); std::vector<int32_t> buf; const long long BS = 1LL << 18;
    for (long long a = 0; a < n2; a += BS) { const long long b = std::min(n2, a + BS); buf.assign((size_t)(b - a) * 2, 0);
        for (long long i = a; i < b; i++) { const long long s = i - t; if (s >= s0 && s < s1) { buf[(size_t)(i - a) * 2] = m[(size_t)s * 2]; buf[(size_t)(i - a) * 2 + 1] = m[(size_t)s * 2 + 1]; } }
        w.put(buf); }
    w.finish(); }
inline void moveMasterFile(const blkio::View& m, long long Lm, double f, const std::string& out, long& nclip) {
    nclip = 0; const int kH = 64; const long long sh = (long long)std::floor(f); const double fr = f - (double)sh;
    double k[2 * kH]; for (int q = -kH + 1; q <= kH; q++) k[q + kH - 1] = moveKern(fr - q);
    blkio::Writer w(out.c_str()); std::vector<int32_t> z, x; const long long BS = 1LL << 18;
    for (long long a = 0; a < Lm; a += BS) { const long long b = std::min(Lm, a + BS); z.assign((size_t)(b - a) * 2, 0);
        const long long xa = std::max(0LL, a + sh - kH + 1), xb = std::min(Lm, b + sh + kH);   // the input samples this piece reads (inside the master)
        x.assign((size_t)std::max(0LL, xb - xa) * 2, 0); for (long long j = xa; j < xb; j++) { x[(size_t)(j - xa) * 2] = m[(size_t)j * 2]; x[(size_t)(j - xa) * 2 + 1] = m[(size_t)j * 2 + 1]; }
        for (int c = 0; c < 2; c++) for (long long i = a; i < b; i++) { const long long i0 = i + sh; double s = 0;
            for (int q = -kH + 1; q <= kH; q++) { const long long j = i0 + q; if (j < 0 || j >= Lm) continue; s += (double)(x[(size_t)(j - xa) * 2 + c] >> 8) * k[q + kH - 1]; }
            long long v = std::llround(s); if (v > 8388607) { v = 8388607; nclip++; } if (v < -8388608) { v = -8388608; nclip++; } z[(size_t)(i - a) * 2 + c] = (int32_t)(v * 256); }
        w.put(z); }
    w.finish(); }
// The piece's phase, measured once at the whole offset t that run() found or was given: the advance a (phi units) with the least pooled
// residual over run()'s usable check blocks, in run()'s channel order, through the encoder's own derivation, one least-squares gain per
// channel (as the check). A fixed descent: start at the check's pooled fraction, then steps of U/8, U/16, ... 1 unit, each comparing
// a - step, a and a + step; candidates beyond half a master sample either way are not taken (the whole offset is run()'s, or the user's).
// Only the position is used; nothing is stored here. The caller turns a into the placement (t, or t + 1 for a < 0) and phi.
template <class M> inline int64_t measurePhase(const std::vector<int16_t>& cd, const M& master, long long Lm, int stages, int rate, long long t, const Report& R, std::string& line) {
    using namespace detail; const long long up = 1LL << stages; const int64_t U = lae_fd::unitsPerMaster(stages); const long long margin = 2048;
    if (R.blocks.empty()) { line = "phase: not measured (no check block with sound on both sides): phase number 0"; return 0; }   // digital silence (both_silent)
    const int ph = (int)((((-t) % up) + up) % up); const long long q = (t + ph) / up; const int map = R.chanMap; int used = 0;
    auto resid = [&](int64_t a) {   // base_t[j] = D_ph[j - q] as in run(), D_ph from the master advanced by a
        int32_t h[lae_fd::TAPS]; advanceTaps(a, stages, h); double E = 0, X2 = 0; used = 0;
        for (auto& bl : R.blocks) { const long long ja = bl.first, jb = bl.second, uLo = ja - q, sM = uLo * up + ph - margin, wM = (jb - ja) * up + 2 * margin;
            if (sM < 0 || sM + wM > Lm) continue;   // as run(): the window inside the master (the delay's 16 samples beyond it are mirrored; they
            used++;                                  // reach only the margin, never the block; review 2026-10-22: requiring them inside lost short pieces' only block at 88.2/96 kHz)
            auto mir = [&](long j) { j = j < 0 ? -j : (j >= (long)Lm ? (long)(2 * (Lm - 1) - j) : j); return std::max(0L, std::min((long)Lm - 1, j)); };
            for (int c = 0; c < 2; c++) { const int mc = map ? 1 - c : c; std::vector<int32_t> s((size_t)wM);
                for (long long i = 0; i < wM; i++) s[(size_t)i] = (int32_t)advanced(h, a, (long)(sM + i), [&](long j) { return (int64_t)(master[(size_t)mir(j) * 2 + mc] >> 4); });
                for (int st = stages - 1; st >= 0; st--) s = m_base::decimate(s, st);
                double sxr = 0, srr = 0, sxx = 0;   // the derived base kept at its guard bits (1/16 LSB): the fit needs no rounding
                for (long long j = ja; j < jb; j++) { const double xv = 256.0 * cd[(size_t)j * 2 + c], rv = s[(size_t)(margin / up + (j - ja))] / 16.0; sxr += xv * rv; srr += rv * rv; sxx += xv * xv; }
                const double g = srr > 0 ? sxr / srr : 0; E += sxx - g * sxr; X2 += sxx; } }
        return X2 > 0 ? E / X2 : 1.0; };
    int64_t a = std::max(-U / 2, std::min(U / 2, (int64_t)std::llround(-R.frac * (double)U))); const int64_t a0 = a; double ea = resid(a); int steps = 1;
    for (int64_t step = U / 8; step >= 1; step /= 2, steps++) {
        const bool lo = a - step >= -U / 2, hi = a + step <= U / 2; const double em = lo ? resid(a - step) : 2.0, ep = hi ? resid(a + step) : 2.0;
        if (em < ea && em <= ep) { a -= step; ea = em; } else if (ep < ea) { a += step; ea = ep; } }
    const double sec = (double)a / (double)U / rate; const std::string st = std::fabs(sec) < 1e-9 ? fmt("%+.1f ps", sec * 1e12) : (std::fabs(sec) < 1e-6 ? fmt("%+.2f ns", sec * 1e9) : fmt("%+.3f us", sec * 1e6));
    line = fmt("phase: at the whole offset %+lld, the master advanced by %+.7f master samples (%s) fits the CD best: pooled residual %.2f dB re the CD over %d check block(s) (start %+.7f; %d fixed steps down to 1/%lld master sample)",
               t, (double)a / (double)U, st.c_str(), db10(ea), used, (double)a0 / (double)U, steps, (long long)U);
    return a; }
// The same rule as cdOnly(), on the given CD alone and before anything is moved or derived: CD frames more than pb CD frames (P master
// samples) away from the placed master's first and last sound meet only added zeros, and the base derived there is exactly zero (the
// derivation filter reaches about 360 master samples at 8x). A subset of what cdOnly() flags, so it only stops what cdOnly() would stop.
inline std::string cdOnlyFar(const std::vector<int16_t>& cd, long ncd, const Place& p, int stages, int base, long pb) {
    using namespace detail; const long long up = 1LL << stages;
    // far from the master's sound AND over zeros added to the master (cdOnly's frames): CD content over the master's own retained digital
    // silence is not stopped here, as cdOnly does not stop it either (review 2026-10-22: acceptance must not depend on whether a move happens)
    const long long jl = std::min<long long>(ncd, (p.padLead + up - 1) / up), jt = std::max<long long>(jl, std::min<long long>(ncd, (p.Lm2 - p.padTail + up - 1) / up));
    const long la = (long)std::max<long long>(0, std::min<long long>(jl, std::min<long long>(ncd, (p.shift + p.a0) / up - pb)));
    const long tb = (long)std::max<long long>(la, std::max<long long>(jt, std::min<long long>(ncd, (p.shift + p.z1) / up + 1 + pb)));
    auto scan = [&](long a, long b, const char* where) { long f = -1, l = -1; for (long j = a; j < b; j++) if (cd[(size_t)j * 2] || cd[(size_t)j * 2 + 1]) { if (f < 0) f = j; l = j; }
        return f < 0 ? std::string() : fmt("the CD has content where the placed master has none (%s): CD frames %ld-%ld (%.6f-%.6f s) are not digital silence: not supported in this step (only one side has content)",
                                          where, f, l, f / (double)base, (l + 1) / (double)base); };
    std::string r = scan(0, la, "start"); if (r.empty()) r = scan(tb, ncd, "end"); return r; }