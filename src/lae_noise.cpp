// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE Noise Marker (2026-11-04; plugins/lae_noise_marker). The noise system only MARKS positions; silencing
// them (tier A) is the LAEY encoder's job. A position is a rectangle of the encoder's own cells: the textbook MDCT of mdct_tb.h
// (N = 512, sine window, frame m = samples [mN - N, mN + N)), cells of 8 bins x 1 frame, M and S together.
// The marking file holds geometry only: the master's SHA-256, length and rate, and the rectangles in cell
// coordinates. No detector, reason or class is stored. Nothing here encodes; the page never starts processing.
// usage:
//   lae_noise mark <master.wav> <out.laenoise> <t0 s> <t1 s> <f0 Hz> <f1 Hz> [<t0> <t1> <f0> <f1> ...]
//        numeric input: each range snaps inward to whole cells (frames whose hop [mN - N/2, mN + N/2) lies inside [t0, t1),
//        columns whose 8 bins lie inside [f0, f1)); the snapped range is printed and written
//   lae_noise show <master.wav> <marks.laenoise>         checks the file against the master, lists the positions
//   lae_noise preview <master.wav> <marks.laenoise> <outdir> [context s, default and at most 1] [longest preview per position s, default 10, at most 30]
//        position by position: pos_NNN_marked.wav (the sound of exactly the marked cells), pos_NNN_silenced.wav (the master
//        with those cells silenced) and pos_NNN_original.wav (the same span unchanged, for a blind A/B), each over the position plus
//        the context; preview_check.txt: the difference master - silenced,
//        analysed again with the same MDCT, must sit in the marked cells only. Previews are 32-bit PCM (the master x 256), so the
//        rounding stays far below a 24-bit step and cannot hide or fake a change
//        preview_list.txt (2026-11-14): whose previews these are, for the panel: the master's SHA-256, length and rate, and per
//        pos_NNN its rectangle in cells as in the marking file (geometry only), with the end frame of the previewed part
//   lae_noise preview-all <master.wav> <marks.laenoise> <outdir> <t0 s> <t1 s>
//        every position silenced together over one excerpt of at most 30 s: all_original.wav, all_silenced.wav, all_marked.wav, with the same check
//   Decision 6 (2026-11-15, "we do not support export"): every preview is limited in the program, so no command writes a cleaned
//   master: preview covers at most the first 30 s of a position (10 s by default) with at most 1 s of context on each side, preview-all at most 30 s (30 s: user decision 2026-11-15);
//   a longer request is cut to the limit and the program says so (as a long position was already cut to its first part)
//   lae_noise level <file.wav> <marks.laenoise>   level of the marked cells in any file of that length (master, copy, decode)
//   lae_noise spectro <master.wav> <out.json>   display data for the panel (tools/noise_panel.html): cell levels and waveform
//   lae_noise scan <master.wav> <report.txt> [candidates.laenoise]   the scanner (src/noise_scan0c.cpp): stationary noise and fixed
//        narrow lines of 10 s or more, as a list and as a marking file of candidates; nothing is marked until the user keeps one
//   --overwrite (with any command, anywhere): decision 21 (2026-11-15): an output file that exists is replaced only after two "yes"
//        answers at a console, or with --overwrite; without a console it is refused (src/noise_replace.h)
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "mdct_tb.h"
#include "noise_marks.h"   // the marking format (Pos, Marks, masterSha, read / write / check)
#include "wavio.h"
#include "noise_replace.h"   // decision 21: an existing output is not replaced silently (--overwrite)
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
#include "lae_math.h"   // levels in dB the same on every platform (2026-11-06)
using namespace std;
using namespace nmk;
// previews are 32-bit PCM: the master's 24 bits x 256, so rounding stays 256 times below a 24-bit step and the check sees the deletion
// itself (a sub-LSB removal rounded to 24 bit would spread its own distortion into other cells)
static const double CHECKMAX = 0.05;   // outside the marked cells, in 24-bit units: rounding to 32 bit alone stays near 0.005
static const double MAXPREV = 30.0, MAXCTX = 1.0;   // decision 6: the longest preview (s) of a position or of preview-all, and its context (s) on each side
static int32_t clip32(long long v) { return (int32_t)max((long long)INT32_MIN, min((long long)INT32_MAX, v)); }
static bool write32(const char* path, int rate, const vector<int32_t>& s) {
    FILE* f = fopen(path, "wb"); if (!f) return false; const uint32_t db = (uint32_t)(s.size() * 4);
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + db); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32((uint32_t)rate); w32((uint32_t)rate * 8); w16(8); w16(32);
    fwrite("data", 1, 4, f); w32(db); const bool ok = fwrite(s.data(), 4, s.size(), f) == s.size(); return fclose(f) == 0 && ok; }
// FU17-6 (2026-11-14): a preview file is written only when it is not the master itself and fits a RIFF WAV (4 GB); false = the
// reason is printed and the command ends with exit code 1 (before, a failed write still printed "preview written", exit code 0)
static bool writePreview(const string& path, const char* master, int rate, const vector<int32_t>& s) {
    if (sameFile(master, path.c_str())) { printf("%s is the master itself: not written, nothing changed\n", path.c_str()); return false; }
    if ((unsigned long long)s.size() * 4 > 0xFFFFFFFFull - 36) { printf("cannot write %s: longer than a WAV file can hold (4 GB); give a shorter preview\n", path.c_str()); return false; }
    if (!write32(path.c_str(), rate, s)) { printf("cannot write %s\n", path.c_str()); return false; }
    return true; }
// DEC 21 (2026-11-15): every output a command will write is checked before anything is written: an existing file is replaced only
// after two "yes" at a console, or with --overwrite (a name that is the master itself is refused later, as before)
static bool mayWriteAll(const char* master, const vector<string>& outs) { for (const string& o : outs) if (!sameFile(master, o.c_str()) && !nrep::mayReplace(o)) return false; return true; }

// plane samples (24-bit units, as the encoder: M = r + ((l - r) >> 1), S = l - r) as doubles, for original samples [a, a + n)
static void planes(WavStream& ws, long long a, long long n, vector<double>& M, vector<double>& S, vector<long long>& li, vector<long long>& ri) {
    vector<double> L, R; ws.read(a, n, L, R); M.assign((size_t)n, 0); S.assign((size_t)n, 0); li.assign((size_t)n, 0); ri.assign((size_t)n, 0);
    for (long long i = 0; i < n; i++) { long long l = llround(L[(size_t)i] * 8388608.0), r = llround(R[(size_t)i] * 8388608.0); li[(size_t)i] = l; ri[(size_t)i] = r;
        M[(size_t)i] = (double)(r + ((l - r) >> 1)); S[(size_t)i] = (double)(l - r); } }
// textbook MDCT of frame m from plane samples x (x[0] = original sample a): the steps of mdct::forward, without the final rounding
static void fwdFrame(const vector<double>& x, long long a, long m, double* c) {
    double z[2 * NM], u[NM]; for (int n = 0; n < 2 * NM; n++) { long long g = m * (long long)NM - NM + n - a; z[n] = (g >= 0 && g < (long long)x.size() ? x[(size_t)g] : 0.0) * mdct::WIN[n]; }
    for (int n = 0; n < NM / 2; n++) { u[n] = -z[3 * NM / 2 - 1 - n] - z[3 * NM / 2 + n]; u[NM / 2 + n] = z[n] - z[NM - 1 - n]; }
    mdct::dct4(u, c); }
// textbook inverse of one frame, overlap-added into y (y[0] = original sample a): the steps of mdct::inverse
static void invFrame(const double* c, long m, long long a, vector<double>& y) {
    double cc[NM], u[NM]; memcpy(cc, c, sizeof cc); mdct::dct4(cc, u); long long s = m * (long long)NM - NM - a;   // padded index m N = original m N - N
    auto add = [&](long long i, double v) { if (i >= 0 && i < (long long)y.size()) y[(size_t)i] += v; };
    for (int n = 0; n < NM / 2; n++) { add(s + n, u[NM / 2 + n] * mdct::WIN[n]); add(s + NM / 2 + n, -u[NM - 1 - n] * mdct::WIN[NM / 2 + n]);
        add(s + NM + n, -u[NM / 2 - 1 - n] * mdct::WIN[NM + n]); add(s + 3 * NM / 2 + n, -u[n] * mdct::WIN[3 * NM / 2 + n]); } }

static int doMark(int argc, char** argv) {
    if (argc < 8 || (argc - 4) % 4) { printf("usage: lae_noise mark <master.wav> <out.laenoise> <t0 s> <t1 s> <f0 Hz> <f1 Hz> [...]\n"); return 2; }
    if (!mayWriteAll(argv[2], { argv[3] })) return 1;   // DEC 21
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    Marks mk; mk.sha = masterSha(ws); mk.frames = ws.frames; mk.rate = ws.rate; long K = mdct::frames((long)ws.frames);
    for (int i = 4; i + 3 < argc; i += 4) { double t0 = atof(argv[i]), t1 = atof(argv[i + 1]), f0 = atof(argv[i + 2]), f1 = atof(argv[i + 3]);
        long m0, m1; int c0, c1; snapTime(t0, t1, ws.rate, K, m0, m1); snapFreq(f0, f1, ws.rate, c0, c1);
        if (m0 >= m1 || c0 >= c1) { printf("range %s-%s s, %s-%s Hz holds no whole cell: skipped\n", argv[i], argv[i + 1], argv[i + 2], argv[i + 3]); continue; }
        mk.pos.push_back({m0, m1, c0, c1});
        printf("position %zu: %.3f-%.3f s, %.1f-%.1f Hz (frames %ld-%ld, columns %d-%d)\n", mk.pos.size(), frameT0(m0, ws.rate), frameT0(m1, ws.rate), colHz(c0, ws.rate), colHz(c1, ws.rate), m0, m1 - 1, c0, c1 - 1); }
    if (mk.pos.empty()) { printf("no position: nothing written\n"); return 1; }
    if (sameFile(argv[2], argv[3])) { printf("%s is the master itself: nothing written\n", argv[3]); return 2; }   // FU17-6
    if (!writeMarks(argv[3], mk)) { printf("cannot write %s\n", argv[3]); return 1; }
    printf("written: %s\n", argv[3]); return 0; }

static int doShow(int argc, char** argv) {
    if (argc < 4) { printf("usage: lae_noise show <master.wav> <marks.laenoise>\n"); return 2; }
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    Marks mk; std::string why; if (!readMarks(argv[3], mk, &why)) { printf("cannot read the marking file %s (%s)\n", argv[3], why.c_str()); return 1; }
    if (!checkMarks(ws, mk)) return 1;
    printf("master OK (SHA-256, %lld frames, %.0f Hz); %zu positions\n", mk.frames, mk.rate, mk.pos.size());
    for (size_t i = 0; i < mk.pos.size(); i++) { const Pos& p = mk.pos[i];
        printf("%4zu  %9.3f - %9.3f s  %9.1f - %9.1f Hz  (%ld frames x %d columns)\n", i + 1, frameT0(p.m0, mk.rate), frameT0(p.m1, mk.rate), colHz(p.c0, mk.rate), colHz(p.c1, mk.rate), p.m1 - p.m0, p.c1 - p.c0); }
    return 0; }

static int doPreview(int argc, char** argv) {
    if (argc < 5) { printf("usage: lae_noise preview <master.wav> <marks.laenoise> <outdir> [context s, at most 1] [longest preview per position s, default 10, at most 30]\n"); return 2; }
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    Marks mk; std::string why; if (!readMarks(argv[3], mk, &why)) { printf("cannot read the marking file %s (%s)\n", argv[3], why.c_str()); return 1; }
    if (!checkMarks(ws, mk)) return 1;
    const string dir = argv[4]; std::error_code ec; std::filesystem::create_directories(dir, ec);
    double ctx = argc > 5 ? atof(argv[5]) : 1.0; const long long L = ws.frames; const long K = mdct::frames((long)L);
    double maxSec = argc > 6 ? atof(argv[6]) : 10.0;
    if (ctx > MAXCTX) { printf("context %s s: cut to %.0f s (previews are limited; lae_noise does not export)\n", argv[5], MAXCTX); ctx = MAXCTX; }   // decision 6
    if (maxSec > MAXPREV) { printf("longest preview per position %s s: cut to %.0f s (previews are limited; lae_noise does not export)\n", argv[6], MAXPREV); maxSec = MAXPREV; }
    const long maxFr = max(1L, (long)llround(maxSec * ws.rate / NM));   // a long position is previewed from its start
    { vector<string> outs{ dir + "/preview_check.txt", dir + "/preview_list.txt" }; char pn[64];   // DEC 21
      for (size_t pi = 0; pi < mk.pos.size(); pi++) for (const char* kind : { "marked", "silenced", "original" }) { snprintf(pn, sizeof pn, "/pos_%03zu_%s.wav", pi + 1, kind); outs.push_back(dir + pn); }
      if (!mayWriteAll(argv[2], outs)) return 1; }
    FILE* rep = fopen((dir + "/preview_check.txt").c_str(), "w"); if (!rep) { printf("cannot write in %s\n", dir.c_str()); return 1; }
    fprintf(rep, "master %s\nmarking file %s\nposition | time s | Hz | marked level dBFS | outside the marked cells: max |coefficient| and rms (24-bit units; previews are 32-bit, rounding alone gives about 0.001 rms; fails above %.2f)\n", argv[2], argv[3], CHECKMAX);
    // FU17-8: the list the panel matches by (the master by SHA-256, each preview by its rectangle); "wb": LF line ends everywhere
    FILE* lst = fopen((dir + "/preview_list.txt").c_str(), "wb"); if (!lst) { fclose(rep); printf("cannot write in %s\n", dir.c_str()); return 1; }
    fprintf(lst, "LAEPREVIEW 1\nmaster sha256 %s frames %lld rate %.0f\ngrid mdct %d cell %d\n", mk.sha.c_str(), mk.frames, mk.rate, NM, CW);
    fprintf(lst, "# pos_NNN <m0> <m1> <c0> <c1> <e>: the files pos_NNN_*.wav are the preview of the position pos <m0> <m1> <c0> <c1> of the marking file; frames [m0, e) are previewed (e < m1: the first part of a long position)\n");
    const double fsPow = 0.5 * 70368744177664.0; int bad = 0;
    for (size_t pi = 0; pi < mk.pos.size(); pi++) { Pos p = mk.pos[pi]; const bool cut = p.m1 - p.m0 > maxFr; if (cut) p.m1 = p.m0 + maxFr;
        const long long sup0 = p.m0 * (long long)NM - NM, sup1 = p.m1 * (long long)NM;   // the samples the marked frames touch
        const long long a = max(0LL, sup0 - (long long)llround(ctx * ws.rate)), b = min(L, sup1 + (long long)llround(ctx * ws.rate));
        const long long r0 = min(a, sup0 - NM), r1 = max(b, sup1 + NM);   // also the frames next to the position, for the check
        vector<double> M, S; vector<long long> li, ri; planes(ws, r0, r1 - r0, M, S, li, ri);
        vector<double> DM((size_t)(r1 - r0), 0.0), DS((size_t)(r1 - r0), 0.0); double cm[NM], cs[NM], eMarked = 0;
        const int k0 = p.c0 * CW, k1 = p.c1 * CW;
        for (long m = p.m0; m < p.m1; m++) { fwdFrame(M, r0, m, cm); fwdFrame(S, r0, m, cs);
            for (int k = 0; k < NM; k++) if (k < k0 || k >= k1) { cm[k] = 0; cs[k] = 0; } else eMarked += cm[k] * cm[k] + cs[k] * cs[k] / 4;
            invFrame(cm, m, r0, DM); invFrame(cs, m, r0, DS); }
        // channels: l = M + S/2, r = M - S/2 (M = r + (l - r)/2, S = l - r); silenced = master - marked, written as 32-bit PCM (the
        // master's 24 bits x 256): rounding a sub-LSB removal to 24 bit would itself spread distortion into other cells
        vector<int32_t> outSil, outMark, outOrig; vector<double> dM((size_t)(r1 - r0)), dS((size_t)(r1 - r0));
        for (long long i = r0; i < r1; i++) { size_t j = (size_t)(i - r0); double dl = DM[j] + DS[j] / 2, dr = DM[j] - DS[j] / 2; const bool inSup = i >= sup0 && i < sup1;
            long long ol = li[j] * 256, orr = ri[j] * 256, sl = inSup ? llround(((double)li[j] - dl) * 256) : ol, sr = inSup ? llround(((double)ri[j] - dr) * 256) : orr;
            long long xl = ol - sl, xr = orr - sr;   // what was taken out, in 32-bit units
            dM[j] = (xl + xr) / 512.0; dS[j] = (xl - xr) / 256.0;   // back in 24-bit units; exact halves (an integer >> 1 would add an offset)
            if (i >= a && i < b) { outSil.push_back(clip32(sl)); outSil.push_back(clip32(sr)); outMark.push_back(clip32(xl)); outMark.push_back(clip32(xr)); outOrig.push_back(clip32(ol)); outOrig.push_back(clip32(orr)); } }
        // check: the difference, analysed with the same MDCT, must sit in the marked cells only
        double mx = 0, ss = 0; long cnt = 0;
        for (long m = max(1L, p.m0 - 1); m <= min(K - 2, p.m1); m++) { fwdFrame(dM, r0, m, cm); fwdFrame(dS, r0, m, cs);
            for (int k = 0; k < NM; k++) { bool in = m >= p.m0 && m < p.m1 && k >= k0 && k < k1; if (in) continue;
                double v1 = fabs(cm[k]), v2 = fabs(cs[k]) / 2;
                mx = max(mx, max(v1, v2)); ss += cm[k] * cm[k] + cs[k] * cs[k] / 4; cnt += 2; } }
        double rms = cnt ? sqrt(ss / cnt) : 0; if (mx > CHECKMAX) bad++;
        char nm[3][64]; snprintf(nm[0], sizeof nm[0], "/pos_%03zu_marked.wav", pi + 1); snprintf(nm[1], sizeof nm[1], "/pos_%03zu_silenced.wav", pi + 1);
        snprintf(nm[2], sizeof nm[2], "/pos_%03zu_original.wav", pi + 1);   // original: the same span unchanged, for a blind A/B
        if (!writePreview(dir + nm[0], argv[2], (int)ws.rate, outMark) || !writePreview(dir + nm[1], argv[2], (int)ws.rate, outSil) ||
            !writePreview(dir + nm[2], argv[2], (int)ws.rate, outOrig)) { fclose(rep); fclose(lst); return 1; }
        fprintf(lst, "pos_%03zu %ld %ld %d %d %ld\n", pi + 1, p.m0, mk.pos[pi].m1, p.c0, p.c1, p.m1);
        double lv = 10 * lmath::log10(max(eMarked / (double)(sup1 - sup0) / fsPow, 1e-30));
        fprintf(rep, "%4zu  %9.3f-%9.3f  %9.1f-%9.1f  %8.1f   max %.4f  rms %.5f%s\n", pi + 1, frameT0(p.m0, mk.rate), frameT0(p.m1, mk.rate), colHz(p.c0, mk.rate), colHz(p.c1, mk.rate), lv, mx, rms, mx > CHECKMAX ? "  CHECK FAILED" : "");
        printf("position %zu: preview written (%.2f s with context%s), outside the marked cells max %.4f rms %.5f\n", pi + 1, (b - a) / ws.rate, cut ? ", first part of a longer position" : "", mx, rms); }
    fprintf(rep, "%s\n", bad ? "RESULT: some positions changed cells outside the marking" : "RESULT: every position changed only its marked cells (outside: rounding to 32 bit only)");
    { const bool werr = ferror(rep) != 0; if (fclose(rep) != 0 || werr) { fclose(lst); printf("cannot write %s/preview_check.txt\n", dir.c_str()); return 1; } }   // FU17-6
    if (fclose(lst) != 0) { printf("cannot write %s/preview_list.txt\n", dir.c_str()); return 1; }
    return bad ? 3 : 0; }

// every position of the file silenced together, over one excerpt [t0, t1): original / silenced / marked, and the same check
static int doPreviewAll(int argc, char** argv) {
    if (argc < 7) { printf("usage: lae_noise preview-all <master.wav> <marks.laenoise> <outdir> <t0 s> <t1 s>   (an excerpt of at most 30 s)\n"); return 2; }
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    Marks mk; std::string why; if (!readMarks(argv[3], mk, &why)) { printf("cannot read the marking file %s (%s)\n", argv[3], why.c_str()); return 1; }
    if (!checkMarks(ws, mk)) return 1;
    const string dir = argv[4]; std::error_code ec; std::filesystem::create_directories(dir, ec);
    const long long L = ws.frames; const long K = mdct::frames((long)L);
    const long long a = max(0LL, (long long)llround(atof(argv[5]) * ws.rate)); long long b = min(L, (long long)llround(atof(argv[6]) * ws.rate)); if (b <= a) { printf("empty excerpt\n"); return 2; }
    const long long maxLen = llround(MAXPREV * ws.rate);   // decision 6: one excerpt of at most 30 s
    if (b - a > maxLen) { b = a + maxLen; printf("excerpt %s-%s s is longer than %.0f s: cut to %.3f-%.3f s (previews are limited; lae_noise does not export)\n", argv[5], argv[6], MAXPREV, a / ws.rate, b / ws.rate); }
    if (!mayWriteAll(argv[2], { dir + "/all_original.wav", dir + "/all_silenced.wav", dir + "/all_marked.wav" })) return 1;   // DEC 21
    const long fA = max(0L, (long)(a / NM)), fB = min(K, (long)(b / NM) + 2);   // frames that touch the excerpt
    const long long r0 = (long long)fA * NM - 2 * NM, r1 = (long long)fB * NM + 2 * NM;
    vector<double> M, S; vector<long long> li, ri; planes(ws, r0, r1 - r0, M, S, li, ri);
    vector<double> DM((size_t)(r1 - r0), 0.0), DS((size_t)(r1 - r0), 0.0); double cm[NM], cs[NM];
    vector<vector<char>> sel(fB - fA, vector<char>(NM, 0)); long used = 0;   // marked bins per frame (union of all positions)
    for (const Pos& p : mk.pos) for (long m = max(p.m0, fA); m < min(p.m1, fB); m++) { used++; for (int k = p.c0 * CW; k < p.c1 * CW; k++) sel[m - fA][k] = 1; }
    for (long m = fA; m < fB; m++) { bool any = false; for (int k = 0; k < NM; k++) any |= sel[m - fA][k] != 0; if (!any) continue;
        fwdFrame(M, r0, m, cm); fwdFrame(S, r0, m, cs); for (int k = 0; k < NM; k++) if (!sel[m - fA][k]) { cm[k] = 0; cs[k] = 0; }
        invFrame(cm, m, r0, DM); invFrame(cs, m, r0, DS); }
    vector<int32_t> outSil, outMark, outOrig; vector<double> dM((size_t)(r1 - r0)), dS((size_t)(r1 - r0));
    for (long long i = r0; i < r1; i++) { size_t j = (size_t)(i - r0); double dl = DM[j] + DS[j] / 2, dr = DM[j] - DS[j] / 2;
        long long ol = li[j] * 256, orr = ri[j] * 256, sl = llround(((double)li[j] - dl) * 256), sr = llround(((double)ri[j] - dr) * 256), xl = ol - sl, xr = orr - sr;
        dM[j] = (xl + xr) / 512.0; dS[j] = (xl - xr) / 256.0;
        if (i >= a && i < b) { outSil.push_back(clip32(sl)); outSil.push_back(clip32(sr)); outMark.push_back(clip32(xl)); outMark.push_back(clip32(xr)); outOrig.push_back(clip32(ol)); outOrig.push_back(clip32(orr)); } }
    double mx = 0, ss = 0; long cnt = 0;
    for (long m = max(1L, fA); m < min(K - 1, fB); m++) { fwdFrame(dM, r0, m, cm); fwdFrame(dS, r0, m, cs);
        for (int k = 0; k < NM; k++) { if (sel[m - fA][k]) continue; mx = max(mx, max(fabs(cm[k]), fabs(cs[k]) / 2)); ss += cm[k] * cm[k] + cs[k] * cs[k] / 4; cnt += 2; } }
    if (!writePreview(dir + "/all_original.wav", argv[2], (int)ws.rate, outOrig) || !writePreview(dir + "/all_silenced.wav", argv[2], (int)ws.rate, outSil) ||
        !writePreview(dir + "/all_marked.wav", argv[2], (int)ws.rate, outMark)) return 1;   // FU17-6
    printf("excerpt %.2f-%.2f s: %zu positions, %ld marked frame-columns touched; outside the marked cells max %.4f rms %.5f%s\n", a / ws.rate, b / ws.rate, mk.pos.size(), used,
           mx, cnt ? sqrt(ss / cnt) : 0.0, mx > CHECKMAX ? "  CHECK FAILED" : "");
    return mx > CHECKMAX ? 3 : 0; }

// level of the marked cells in any file of the same length and rate (the master, a cleaned copy, a decode): for checking only. The
// file's own SHA-256 is not compared, only length and rate (a decode is not bit-identical to the master).
static int doLevel(int argc, char** argv) {
    if (argc < 4) { printf("usage: lae_noise level <file.wav> <marks.laenoise> [reference.wav: measure file - reference]\n"); return 2; }
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    Marks mk; std::string why; if (!readMarks(argv[3], mk, &why)) { printf("cannot read the marking file %s (%s)\n", argv[3], why.c_str()); return 1; }
    if (mk.frames != ws.frames || fabs(mk.rate - ws.rate) > 0.5) { printf("length or rate differ from the marking file\n"); return 1; }
    if (!checkGrid(ws.frames, mk)) return 1;   // FU17-4: a position outside the grid was read past the 512 bins (or looped for ever)
    WavStream wr; const bool ref = argc > 4; if (ref && (!wr.open(argv[4]) || wr.frames != ws.frames)) { printf("cannot read the reference %s, or its length differs\n", argv[4]); return 1; }
    const double fsPow = 0.5 * 70368744177664.0; double eAll = 0; long long nAll = 0; double cm[NM], cs[NM];
    for (size_t pi = 0; pi < mk.pos.size(); pi++) { const Pos& p = mk.pos[pi]; double e = 0; const long CH = 2048;
        for (long c0 = p.m0; c0 < p.m1; c0 += CH) { long c1 = min(p.m1, c0 + CH); long long r0 = (long long)c0 * NM - NM, r1 = (long long)c1 * NM + NM;
            // planes as exact doubles (24-bit units, no rounding: a 32-bit file keeps its detail), optionally minus the reference
            vector<double> L, R, M((size_t)(r1 - r0)), S((size_t)(r1 - r0)); ws.read(r0, r1 - r0, L, R);
            vector<double> L2, R2; if (ref) wr.read(r0, r1 - r0, L2, R2);
            for (size_t i = 0; i < M.size(); i++) { double l = (L[i] - (ref ? L2[i] : 0)) * 8388608.0, r = (R[i] - (ref ? R2[i] : 0)) * 8388608.0; M[i] = (l + r) / 2; S[i] = l - r; }
            for (long m = c0; m < c1; m++) { fwdFrame(M, r0, m, cm); fwdFrame(S, r0, m, cs); for (int k = p.c0 * CW; k < p.c1 * CW; k++) e += cm[k] * cm[k] + cs[k] * cs[k] / 4; } }
        long long ns = (long long)(p.m1 - p.m0) * NM; eAll += e; nAll += ns;
        printf("%4zu  %9.3f-%9.3f s  %9.1f-%9.1f Hz  %8.1f dBFS\n", pi + 1, frameT0(p.m0, mk.rate), frameT0(p.m1, mk.rate), colHz(p.c0, mk.rate), colHz(p.c1, mk.rate), 10 * lmath::log10(max(e / ns / fsPow, 1e-30))); }
    printf("all marked cells: %.1f dBFS\n", 10 * lmath::log10(max(eAll / max(1LL, nAll) / fsPow, 1e-30))); return 0; }

// display data for the panel (tools/noise_panel.html): cell levels per time step (whole frames, about 20 ms) and cell column (M + S,
// dBFS per kHz, integer tenths), and the waveform's min / max per step. Only for display: nothing here decides anything.
static int doSpectro(int argc, char** argv) {
    if (argc < 4) { printf("usage: lae_noise spectro <master.wav> <out.json>\n"); return 2; }
    WavStream ws; if (!ws.open(argv[2])) { printf("cannot read %s (%s)\n", argv[2], ws.why.c_str()); return 1; }
    const long long L = ws.frames; const long K = mdct::frames((long)L); const int G = max(1, (int)llround(0.02 * ws.rate / NM));
    const long nst = (K - 1) / G; const double fsPow = 0.5 * 70368744177664.0, perKHz = 10 * lmath::log10(CW * ws.rate / (2.0 * NM) / 1000.0);
    if (sameFile(argv[2], argv[3])) { printf("%s is the master itself: nothing written\n", argv[3]); return 2; }   // FU17-6
    if (!nrep::mayReplace(argv[3])) return 1;   // DEC 21
    FILE* f = fopen(argv[3], "w"); if (!f) { printf("cannot write %s\n", argv[3]); return 1; }
    std::string name; for (unsigned char c : std::filesystem::path(argv[2]).filename().string()) {   // UTF-8 (win_utf8.h), JSON-escaped
        if (c == '"' || c == '\\') { name += '\\'; name += (char)c; } else if (c < 0x20) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); name += u; } else name += (char)c; }
    fprintf(f, "{\"master\":\"%s\",\"sha256\":\"%s\",\"rate\":%.0f,\"frames\":%lld,\"N\":%d,\"cw\":%d,\"ncol\":%d,\"step\":%d,\"steps\":%ld,\"lv\":[",
            name.c_str(), masterSha(ws).c_str(), ws.rate, L, NM, CW, NCOL, G, nst);
    vector<int> wmin, wmax; double cm[NM], cs[NM]; const long CH = 256;   // steps per chunk
    for (long s0 = 0; s0 < nst; s0 += CH) { long s1 = min(nst, s0 + CH); long m0 = 1 + s0 * G, m1 = 1 + s1 * G;
        long long r0 = (long long)m0 * NM - NM, r1 = (long long)m1 * NM + NM; vector<double> M, S; vector<long long> li, ri; planes(ws, r0, r1 - r0, M, S, li, ri);
        for (long st = s0; st < s1; st++) { vector<double> e(NCOL, 0.0);
            for (long m = 1 + st * G; m < 1 + (st + 1) * G; m++) { fwdFrame(M, r0, m, cm); fwdFrame(S, r0, m, cs);
                for (int k = 0; k < NM; k++) e[k / CW] += cm[k] * cm[k] + cs[k] * cs[k] / 4; }
            fprintf(f, "%s[", st ? "," : "");
            for (int c = 0; c < NCOL; c++) { double lv = 10 * lmath::log10(max(e[c] / ((double)G * NM) / fsPow, 1e-30)) - perKHz; fprintf(f, "%s%d", c ? "," : "", (int)llround(max(lv, -200.0) * 10)); }
            fprintf(f, "]");
            // waveform over the step's hops: (l + r) / 2, as a fraction of full scale x 1000
            long long a = (1 + st * G) * (long long)NM - NM / 2, b = a + (long long)G * NM; double lo = 0, hi = 0;
            for (long long i = max(a, r0); i < min(b, r1); i++) { double v = (li[(size_t)(i - r0)] + ri[(size_t)(i - r0)]) / 2.0 / 8388608.0; lo = min(lo, v); hi = max(hi, v); }
            wmin.push_back((int)llround(lo * 1000)); wmax.push_back((int)llround(hi * 1000)); } }
    fprintf(f, "],\"wmin\":["); for (size_t i = 0; i < wmin.size(); i++) fprintf(f, "%s%d", i ? "," : "", wmin[i]);
    fprintf(f, "],\"wmax\":["); for (size_t i = 0; i < wmax.size(); i++) fprintf(f, "%s%d", i ? "," : "", wmax[i]); fprintf(f, "]}\n");
    { const bool werr = ferror(f) != 0; if (fclose(f) != 0 || werr) { printf("cannot write %s (the file is incomplete)\n", argv[3]); return 1; } }   // FU17-6: disk full
    printf("written: %s (%ld steps of %d frames x %d columns)\n", argv[3], nst, G, NCOL); return 0; }

#define NOISE_SCAN_NO_MAIN
#include "noise_scan0c.cpp"   // lae_noise scan (2026-11-06: one program for the LAE Noise Marker)

int main(int argc, char** argv) {
    lae_u8::init(argc, argv);   // UTF-8 arguments and file names on Windows (win_utf8.h)
    nrep::takeOverwrite(argc, argv);   // DEC 21: --overwrite anywhere
    if (argc < 2) { printf("usage: lae_noise scan | mark | show | preview | preview-all | level | spectro ... [--overwrite] (see the head of src/lae_noise.cpp)\n"
                           "  --overwrite: replace existing output files without asking (on a console it is asked twice; otherwise it is refused)\n"
                           "  %s\n", u8"--overwrite：不问就替换已有的输出文件（在控制台上会问两次；不在控制台上就拒绝）"); return 2; }
    if (!strcmp(argv[1], "scan")) return noiseScanMain(argc - 1, argv + 1);
    if (!strcmp(argv[1], "spectro")) return doSpectro(argc, argv);
    if (!strcmp(argv[1], "level")) return doLevel(argc, argv);
    if (!strcmp(argv[1], "mark")) return doMark(argc, argv);
    if (!strcmp(argv[1], "show")) return doShow(argc, argv);
    if (!strcmp(argv[1], "preview")) return doPreview(argc, argv);
    if (!strcmp(argv[1], "preview-all")) return doPreviewAll(argc, argv);
    printf("unknown command %s (scan, mark, show, preview, preview-all, level, spectro)\n", argv[1]); return 2; }
