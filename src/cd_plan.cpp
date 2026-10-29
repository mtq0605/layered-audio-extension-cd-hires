// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// The CD's split markers onto the master (2026-10-22; spec L14, P8-P11).
//   lae_enc plan <master.wav> [<master2.wav> ...] <publisher.cue> <outdir> [cdalign=<one position per piece>] [cdaccept] [laes]
//   laes = LAE Lossless output (2026-10-29): the same plan and checks, but the master is not moved: slices cut from it as it is (16 / 24 / 32
//   bit, every bit), each piece with its T for laes enc; the cut piece is checked with its fraction (-60 / -54 dB bands, confirmation as cdaccept).
// Order (2026-10-23): join, check (silence, coverage) on the master as it is, align (both searches;
// pieces the CD splits from continuous master form a group with one whole offset and one fraction), the user confirms every position
// (without cdalign= nothing is written: both searches are reported), the master is moved group by group with its real neighbouring
// samples, and only then cut at the CD's split points.
// The master's own split markers are not used: several master files are joined sample by sample in the given order. Reads the
// publisher's disc (CUE + BIN or WAV files), finds where each CD piece lies in the master with the encoder's search (cd_align.cpp; each
// piece on its own, around its place on the CD's time line), cuts the master there and checks that every master sample belongs to
// exactly one piece. The rule is length matching: whichever side is longer plays first / ends later, the shorter
// side gets digital silence, nothing non-silent is cut.
//   - between two pieces, what only one side has must be digital silence: the publisher's gaps on the CD, silence the publisher left out
//     on the master; less than one CD sample of master sound between pieces stays with its neighbour (the format keeps it);
//   - a piece that is digital silence on the CD is not searched; it takes the master between its neighbours, which must be silence;
//   - master sound less than one CD sample before the CD's first sample: piece 1 gets one digital-silence frame in front of its CD layer,
//     which lies in the pregap (digital silence anyway), so session 1 stays the publisher's program sample for sample;
//   - one CD sample or more that only one side has (sound, noise or dither) stops the planner, with the time range (P10).
// Each cut piece is then checked as the encoder will check it (three blocks at offset 0); nothing is moved after the cut. Writes
// TRACKnn.master.wav (24 bit) and TRACKnn.cd.wav (the publisher's samples, INDEX 01 to the next INDEX 01) per piece. Each piece is then
// encoded with lae_enc TRACKnn.master.wav TRACKnn.v5.wav 3 cd=TRACKnn.cd.wav cdalign=0 (3 = the mode; any mode works), and the disc built with
// lae_cdx image ... layout=. Offsets go to stdout and to the plain-text report plan.align.txt (no LAE file, disc or file a program reads contains one). Compiled into lae_enc.cpp (namespace
// m_plan) after m_align.

namespace fsp = std::filesystem;
using m_align::detail::fmt;

struct Pub { cdxr::Disc d; cdxr::Layout lay; std::vector<long long> p; long long end = 0; };
static std::string readPublisher(const std::string& cue, Pub& P) {
    std::string e = cdxr::loadCue(fsp::path(cue), P.d, cdxr::diskSource(), &P.lay); if (!e.empty()) return e;
    if (!P.d.notes.empty()) return P.d.notes[0];
    if (P.d.conflicts) return fmt("%lld samples conflict between the files of the sheet", P.d.conflicts);
    if (P.lay.tracks.empty()) return "no audio track";
    for (auto& t : P.lay.tracks) {
        if (t.flags.find("PRE") != std::string::npos) return fmt("track %d has FLAGS PRE (pre-emphasis): a WAV cannot carry the flag, not supported (L11, P10)", t.num);
        for (auto& ix : t.idx) if (ix.second % cdxr::SECTOR) return fmt("track %d INDEX %02d is not on the sector grid (not a CD layout)", t.num, ix.first); }
    for (auto& kv : P.d.p) P.p.push_back(kv.second);
    P.end = P.lay.end; if (P.end % cdxr::SECTOR) return "the program does not end on a sector boundary (not a CD layout)";
    for (long long a = P.d.base; a < 0; a++) if (P.d.L(a) || P.d.R(a)) {   // track 1 INDEX 00 inside the file
        long long z = a; while (z < 0 && (P.d.L(z) || P.d.R(z) || z == a)) z++;
        return fmt("track 1 INDEX 00 (the pregap) has sound %.3f s before INDEX 01: not supported (L4, P10; hidden track)", -a / 44100.0); }
    for (long long a = 0; a < P.end; a++) if (!P.d.has(a)) return fmt("the program has an unknown sample at %.3f s", a / 44100.0);
    for (size_t k = 0; k < P.p.size(); k++) { const long long n = (k + 1 < P.p.size() ? P.p[k + 1] : P.end) - P.p[k];
        if (n < 300 * cdxr::SECTOR) return fmt("track %zu is %lld sectors; a CD track needs at least 300 (L10)", k + 1, n / cdxr::SECTOR); }
    return ""; }

static int planMain(int argc, char** argv) {
    // lae_enc plan <master.wav> [<master2.wav> ...] <publisher.cue> <outdir> [cdaccept]
    clearModuleEnv();   // e.g. an inherited DITHERFLAGS would make the base module write a file outside outdir
    std::vector<std::string> args; bool accept = false, laes = false; std::string alignArg;
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "cdaccept")) accept = true; else if (!strcmp(argv[i], "laes")) laes = true; else if (!strncmp(argv[i], "cdalign=", 8)) alignArg = argv[i] + 8; else args.push_back(argv[i]); }
    if (args.size() < 3) { printf("usage: lae_enc plan <master.wav> [<master2.wav> ...] <publisher.cue> <outdir> [cdalign=<one position per piece>] [cdaccept] [laes]\n"); return 1; }
    const std::string cueArg = args[args.size() - 2], outArg = args.back(); const std::vector<std::string> mfiles(args.begin(), args.end() - 2);
    const fsp::path out(outArg); std::error_code ec;
    if (fsp::exists(out, ec) && !fsp::is_empty(out, ec)) { printf("%s is not empty: the slices go into a new or empty folder; nothing written\n", outArg.c_str()); return 1; }
    // the master: one file, or several joined sample by sample in the given order (their own split points are not used)
    // laes: LAE Lossless output. The master is never moved: the slices are cut from it as it is (16 / 24 / 32 bit, every bit kept)
    // and each piece's position goes into T (base samples) for laes enc, instead of moving the master by the fraction.
    wavio::Wav w; std::vector<long long> fileStart; std::vector<int32_t> full; int fullBits = 0;   // full: the joined master as plain integers, all bits
    for (size_t f = 0; f < mfiles.size(); f++) { wavio::Wav part; if (!wavio::read(mfiles[f].c_str(), part) || part.ch != 2 || (part.bits != 16 && part.bits != 24 && (!laes || part.bits != 32))) { printf("%s: the master must be a 16- or 24-bit%s stereo PCM WAV\n", mfiles[f].c_str(), laes ? " (or 32-bit)" : ""); return 1; }
        if (f == 0) w.rate = part.rate; else if (part.rate != w.rate) { printf("%s is at %d Hz, %s at %d Hz: the master files must have one rate\n", mfiles[f].c_str(), part.rate, mfiles[0].c_str(), w.rate); return 1; }
        if (f == 0) fullBits = part.bits; else if (laes && part.bits != fullBits) { printf("%s is %d-bit, %s %d-bit: for laes the master files must have one bit depth\n", mfiles[f].c_str(), part.bits, mfiles[0].c_str(), fullBits); return 1; }
        if (part.s.size() & 1) { part.s.pop_back(); if (!part.raw32.empty()) part.raw32.pop_back(); printf("plan: %s ends with half a sample frame (a cut-off data chunk): dropped\n", mfiles[f].c_str()); }   // else every later file's channels would swap
        fileStart.push_back((long long)w.s.size() / 2); w.s.insert(w.s.end(), part.s.begin(), part.s.end());
        for (size_t i = 0; i < part.s.size(); i++) full.push_back(part.bits == 32 ? part.raw32[i] : part.s[i] >> (32 - part.bits)); }
    w.ch = 2; w.bits = 24;
    int stages = 0; while ((44100 << stages) < w.rate) stages++;
    if ((44100 << stages) != w.rate || stages < 1 || stages > 3) { printf("master at %d Hz: a CD-Extra needs 88.2, 176.4 or 352.8 kHz (44.1 kHz family)\n", w.rate); return 1; }
    const long long up = 1LL << stages, Lm = (long long)w.s.size() / 2; const int P = 4096; const long pb = P >> stages;
    Pub pub; { std::string e = readPublisher(cueArg, pub); if (!e.empty()) { printf("publisher disc: %s; nothing written\n", e.c_str()); return 1; } }
    const size_t n = pub.p.size(); std::vector<long long> nk(n);
    for (size_t k = 0; k < n; k++) nk[k] = (k + 1 < n ? pub.p[k + 1] : pub.end) - pub.p[k];
    printf("plan: publisher disc %s: %zu pieces, %lld sectors; master %s%s: %.3f s at %d Hz\n", cueArg.c_str(), n, pub.end / cdxr::SECTOR, mfiles[0].c_str(),
           mfiles.size() > 1 ? fmt(" + %zu more files, joined in this order", mfiles.size() - 1).c_str() : "", Lm / (double)w.rate, w.rate);
    // the master's derived 24-bit base, once (the encoder's own derivation; also what the search compares the CD with)
    const fsp::path tmp = out / "plan.tmp"; fsp::create_directories(tmp, ec); if (ec) { printf("cannot create %s\n", tmp.u8string().c_str()); return 2; }
    struct Rm { fsp::path p; ~Rm() { std::error_code e2; fsp::remove_all(p, e2); } } rm{tmp};
    encrun::tmp = tmp.string(); encrun::tmpMine = true;   // also removed on Ctrl+C, exit() inside a module or terminate (the destructor does not run then)
    auto T = [&](const char* nm) { return (tmp / nm).string(); };
    encio::writeVec(T("m0.raw").c_str(), w.s);
    if (call(m_padblk::main, {"pad", T("m0.raw"), T("m.raw"), std::to_string(P)})) { printf("padding failed\n"); return 2; }
    env("BASE24", T("b24.s32")); int rc = call(m_baseblk::main, {"base", T("m.raw"), std::to_string(w.rate), "44100", T("b16.s16")}); env("BASE24", "");
    if (rc) { printf("base derivation failed\n"); return 2; }
    const std::vector<int32_t> b24 = encio::readAll<int32_t>(T("b24.s32").c_str()); const long nbAll = (long)b24.size() / 2;
    auto cdOf = [&](size_t k) { std::vector<int16_t> c((size_t)nk[k] * 2); for (long long j = 0; j < nk[k]; j++) { c[(size_t)j * 2] = pub.d.L(pub.p[k] + j); c[(size_t)j * 2 + 1] = pub.d.R(pub.p[k] + j); } return c; };
    auto zero = [](const std::vector<int16_t>& c, long long a, long long b) { for (long long j = std::max(0LL, a); j < std::min<long long>((long long)c.size() / 2, b); j++) if (c[(size_t)j * 2] || c[(size_t)j * 2 + 1]) return j; return -1LL; };
    auto sec = [&](long long m) { return m / (double)w.rate; };
    auto nzM = [&](long long i) { return i >= 0 && i < Lm && (full[(size_t)i * 2] || full[(size_t)i * 2 + 1]); };   // all bits (a 32-bit master's low byte counts)
    long long a0 = -1, z1 = -1; for (long long i = 0; i < Lm && a0 < 0; i++) if (nzM(i)) a0 = i; for (long long i = Lm - 1; i >= 0 && z1 < 0; i--) if (nzM(i)) z1 = i;
    if (a0 < 0) { printf("the master is digital silence throughout; nothing written\n"); return 1; }
    // ---- 1. where each CD piece lies in the master: the encoder's search (cd_align.cpp) on a window around the piece's place on the CD's
    // own time line, +- max(60 s, the piece's length); no piece's result is used for another piece; a piece that is digital silence on
    // the CD is not searched (there is nothing to find): it takes the master between its neighbours, which must be digital silence
    std::vector<long long> A(n, LLONG_MIN); std::vector<std::vector<int16_t>> cds(n); std::vector<bool> silentCd(n, false);
    for (size_t k = 0; k < n; k++) { cds[k] = cdOf(k);
        if (zero(cds[k], 0, nk[k]) < 0) { silentCd[k] = true; printf("plan: piece %02zu: digital silence on the CD (every frame scanned): not searched; it takes the master between its neighbours\n", k + 1); } }
    // both searches, always and in the same way; the program does not compare them or choose between them
    auto searchAll = [&](bool wh) -> int { const char* nm = wh ? "whitened" : "plain"; A.assign(n, LLONG_MIN);
        for (size_t k = 0; k < n; k++) { if (silentCd[k]) continue; const long long len = nk[k] * up;
            const long long reach = std::max<long long>(60LL * 44100, nk[k]), expect = pub.p[k] * up;
            const long long f0 = std::max(0LL, (expect - reach * up) / up), f1 = std::min<long long>(nbAll - 2 * pb, (expect + len + reach * up + up - 1) / up);
            if (f1 - f0 < 1) { printf("plan (%s): piece %02zu lies outside the master\n", nm, k + 1); return 1; }
            const long nwin = (long)(f1 - f0); const long long w0 = f0 * up;
            std::vector<int32_t> bw(b24.begin() + (ptrdiff_t)f0 * 2, b24.begin() + (ptrdiff_t)(f0 + nwin + 2 * pb) * 2);
            std::vector<int32_t> mw((size_t)nwin * up * 2, 0); for (long long i = 0; i < (long long)nwin * up; i++) if (w0 + i < Lm) { mw[(size_t)i * 2] = w.s[(size_t)(w0 + i) * 2]; mw[(size_t)i * 2 + 1] = w.s[(size_t)(w0 + i) * 2 + 1]; }
            m_align::Report rep = m_align::run(cds[k], (long)nk[k], bw, nwin + 2 * pb, pb, mw, (long)nwin * up, 44100, stages, 0, "", (long)(-(expect - w0) / up), (long)reach, nullptr, wh);
            for (auto& l : rep.lines) if (l.rfind("result: ", 0) != 0 && l.rfind("only the ", 0) != 0)   // those two count from the search window's start
                printf("plan (%s): piece %02zu: %s\n", nm, k + 1, l.c_str());
            if (!rep.found && rep.tag == "edit")   // some check blocks do not match: step 2 may name the cause more precisely (sound in a gap the
                // publisher added, content only one side has); the cut piece is checked again in step 4 and stops there if nothing else stops first
                printf("plan (%s): piece %02zu: %s; the cut piece is checked again after step 2\n", nm, k + 1, rep.why.c_str());
            else if (!rep.found) { printf("plan (%s): piece %02zu: %s%s\n", nm, k + 1, rep.why.c_str(), rep.hint.empty() ? "" : ("; " + rep.hint).c_str()); return 1; }
            A[k] = w0 - rep.masterOffset;
            printf("plan (%s): piece %02zu at %.6f s of the master (CD frame 0 = master sample %lld)\n", nm, k + 1, sec(A[k]), A[k]); }
        return 0; };
    const std::vector<int32_t>* src = &w.s;   // where the slices take their samples from: the master as it is, or as moved (the checks always see it as it is)
    // ---- 2. coverage: every master sample in exactly one piece; what only one side has is digital silence (between pieces, no
    // exception). 3. the slices (in memory). CD frames within R of the master's first or last sound in a piece are not decided by a
    // scan of the CD alone: they are checked in step 4 against the slice's own derived base, the encoder's rule (cdOnly).
    const long long R = 64;   // CD frames next to the master's sound that may carry the derivation filter's transition (it reaches about 45)
    struct Slice { long long S = 0, len = 0; int lead = 0; std::vector<int32_t> m; std::vector<int16_t> c; std::vector<std::pair<long long, long long>> near; };
    std::vector<long long> cutAt(n, LLONG_MIN);   // where each piece's own master range starts (for the notice about the files' split points)
    auto build = [&](std::vector<Slice>& sl, bool say) -> int {   // 0 = built, 1 = stop (reason printed)
        std::vector<long long> lo(n, LLONG_MIN), hi(n, LLONG_MAX), spare(n, 0);   // the master range each piece owns
        std::vector<size_t> sn; for (size_t k = 0; k < n; k++) if (!silentCd[k]) sn.push_back(k);
        for (size_t q = 0; q + 1 < sn.size(); q++) { const size_t k = sn[q], j = sn[q + 1]; const long long E = A[k] + nk[k] * up, B = A[j];
            if (B < A[k] + up) { printf("plan: piece %02zu starts before piece %02zu in the master (the order differs from the master): not supported; nothing written\n", j + 1, k + 1); return 1; }
            const bool between = j > k + 1;   // digital-silence pieces of the CD lie between k and j
            if (B > E || (between && B == E)) {   // master samples no sounding CD frame covers: digital silence, except less than one CD sample of
                // sound right after piece k (it stays with k) and, with silent pieces between, right before piece j (it stays with the last of them)
                long long hl = -1; for (long long i = E; i < std::min(B, E + up); i++) if (nzM(i)) hl = i;
                long long tf = -1; if (between) for (long long i = std::max(E, B - up); i < B; i++) if (nzM(i)) { tf = i; break; }
                const long long ma = hl >= 0 ? hl + 1 : E, mb = tf >= 0 ? tf : B; long long f = -1, l = -1;
                for (long long i = ma; i < mb && f < 0; i++) if (nzM(i)) f = i;
                if (f >= 0) { for (long long i = mb - 1; i >= f; i--) if (nzM(i)) { l = i; break; }
                    printf("plan: the master has sound between piece %02zu and %02zu that is not on the CD: %.6f-%.6f s of the master (%.6f s): not supported (the CD lacks content); nothing written\n", k + 1, j + 1, sec(f), sec(l + 1), sec(l + 1 - f)); return 1; }
                if (hl >= 0) { spare[k] = hl + 1 - E; if (say) printf("plan: %lld master samples (%s) between piece %02zu and %02zu stay with piece %02zu (less than one CD sample)\n", spare[k], m_align::detail::dur(sec(spare[k])).c_str(), k + 1, j + 1, k + 1); }
                hi[k] = between ? E + spare[k] : B; lo[j] = B;
                if (between) { for (size_t s = k + 1; s + 1 < j; s++) { lo[s] = E + spare[k]; hi[s] = E + spare[k]; }   // the earlier silent pieces of the run own nothing
                               lo[j - 1] = E + spare[k]; hi[j - 1] = B; } }   // the last one owns the gap (its sound, if any, is less than one CD sample before piece j)
            else if (B < E) {   // the CD covers these master samples twice: one of the two CD pieces must be digital silence there
                const long long fT = (B - A[k] + up - 1) / up, nH = (E - B) / up;   // piece k frames from fT on; piece j frames before nH
                const long long exT = zero(cds[k], fT, nk[k]), exH = zero(cds[j], 0, nH);
                const long long badT = exT < 0 ? -1 : zero(cds[k], std::min(nk[k], fT + R), nk[k]), badH = exH < 0 ? -1 : zero(cds[j], 0, std::max(0LL, nH - R));
                const bool tailSilent = exT < 0 || (nk[k] - fT > R && badT < 0), headSilent = exH < 0 || (nH > R && badH < 0);
                if (tailSilent) { hi[k] = B; lo[j] = B; if (say && E - B >= up) printf("plan: piece %02zu ends with %s of digital silence on the CD (a gap the publisher added): the master gets digital silence there\n", k + 1, m_align::detail::dur(sec(E - B)).c_str()); }
                else if (headSilent) { hi[k] = E; lo[j] = E; if (say && E - B >= up) printf("plan: piece %02zu starts with %s of digital silence on the CD (added by the publisher): the master gets digital silence there\n", j + 1, m_align::detail::dur(sec(E - B)).c_str()); }
                else { printf("plan: pieces %02zu and %02zu both have sound for the same %.6f-%.6f s of the master (CD frame %lld of piece %02zu, %lld of piece %02zu are not silent): not supported; nothing written\n",
                              k + 1, j + 1, sec(B), sec(E), exT, k + 1, exH, j + 1); return 1; }
                if (between) for (size_t s = k + 1; s < j; s++) { lo[s] = lo[j]; hi[s] = lo[j]; } }   // silent pieces between own nothing: the master has no room for them
            else { hi[k] = B; lo[j] = B; } }
        if (sn.empty()) { printf("plan: every piece is digital silence on the CD; nothing written\n"); return 1; }
        int L = 0; { const size_t k = sn.front(); const long long early = A[k] - a0;   // the master sounds before the first sounding CD piece starts
            if (k > 0) { lo[k - 1] = 0; hi[k - 1] = A[k]; lo[k] = A[k]; for (size_t s = 0; s + 1 < k; s++) { lo[s] = 0; hi[s] = 0; }   // leading silent pieces: the last of them owns what lies before
                if (early >= up) { printf("plan: the master has sound %.6f s before the CD's first sounding piece starts, at %.6f-%.6f s of the master: content only the master has (not supported); nothing written\n", sec(early), sec(a0), sec(A[k])); return 1; } }
            else { if (early >= up) { printf("plan: the master has sound %.6f s (one CD sample or more) before the CD starts, at %.6f-%.6f s of the master: content only the master has (not supported); nothing written\n", sec(early), sec(a0), sec(A[0])); return 1; }
                   if (early > 0) { L = 1; if (say) printf("plan: the master's sound starts %lld master samples (%s) before piece 01's first sample: one digital-silence frame goes in front of piece 01's CD layer, in the pregap (session 1 unchanged)\n", early, m_align::detail::dur(sec(early)).c_str()); } } }
        { const size_t k = sn.back(); const long long late = z1 + 1 - (A[k] + nk[k] * up);
            if (k + 1 < n) { if (late >= up) { printf("plan: the master has sound %.6f s after the CD's last sounding piece ends, at %.6f-%.6f s of the master: content only the master has (not supported); nothing written\n", sec(late), sec(A[k] + nk[k] * up), sec(z1 + 1)); return 1; }
                if (late > 0) spare[k] = late; lo[k + 1] = A[k] + nk[k] * up + spare[k]; hi[k + 1] = Lm; hi[k] = lo[k + 1]; for (size_t s = k + 2; s < n; s++) { lo[s] = Lm; hi[s] = Lm; } }   // trailing silent pieces: the first of them owns what follows
            else { if (late >= up) { printf("plan: the master has sound %.6f s (one CD sample or more) after the CD ends, at %.6f-%.6f s of the master: content only the master has (not supported); nothing written\n", sec(late), sec(A[k] + nk[k] * up), sec(z1 + 1)); return 1; }
                   if (late > 0) spare[k] = late; } }
        sl.assign(n, Slice());
        for (size_t k = 0; k < n; k++) { Slice& s = sl[k];
            if (silentCd[k]) {   // no position of its own: its slice is digital silence of its CD length; the master range it owns must be digital silence
                s.len = nk[k] * up; s.m.assign((size_t)s.len * 2, 0); s.c = cds[k]; cutAt[k] = lo[k]; long long carried = 0;
                const long long glo = std::max(0LL, lo[k] == LLONG_MIN ? 0 : lo[k]), ghi = std::min(Lm, hi[k] == LLONG_MAX ? Lm : hi[k]);   // never the sentinels
                if (glo < ghi) { const bool lastOfRun = k + 1 < n && !silentCd[k + 1];   // a neighbour's sound of less than one CD sample just before the next piece lands at this slice's end
                    for (long long g = glo; g < ghi; g++) if (nzM(g)) { const long long d = hi[k] - g;
                        if (!(lastOfRun && d < up) && !(g - glo < up && k > 0 && !silentCd[k - 1])) { printf("plan: piece %02zu is digital silence on the CD, but the master has sound at %.6f s of the master (only the master has it): not supported; nothing written\n", k + 1, sec(g)); return 1; }
                        if (lastOfRun && d < up) { s.m[(size_t)(s.len - d) * 2] = (*src)[(size_t)g * 2]; s.m[(size_t)(s.len - d) * 2 + 1] = (*src)[(size_t)g * 2 + 1]; carried++; } } }
                if (say) { if (carried) printf("plan: piece %02zu: digital silence on the CD; its master slice is digital silence except its last %lld samples, the first sound of piece %02zu (less than one CD sample)\n", k + 1, carried, k + 2);
                           else printf("plan: piece %02zu: digital silence on the CD and in the master (scanned sample by sample)\n", k + 1); }
                continue; }
            s.lead = k == 0 ? L : 0; s.S = A[k] - s.lead * up; s.len = (nk[k] + s.lead) * up + spare[k]; s.m.assign((size_t)s.len * 2, 0); cutAt[k] = std::max(lo[k], s.S);
            long long mf = -1, ml = -1;   // the piece's own master sound: first and last non-zero sample it owns
            for (long long i = 0; i < s.len; i++) { const long long g = s.S + i; if (g >= 0 && g < Lm && g >= lo[k] && g < hi[k]) { s.m[(size_t)i * 2] = (*src)[(size_t)g * 2]; s.m[(size_t)i * 2 + 1] = (*src)[(size_t)g * 2 + 1];
                    if (s.m[(size_t)i * 2] || s.m[(size_t)i * 2 + 1]) { if (mf < 0) mf = g; ml = g; } } }
            for (long long g = std::max(0LL, lo[k] == LLONG_MIN ? 0 : lo[k]); g < std::min(Lm, hi[k] == LLONG_MAX ? Lm : hi[k]); g++) if ((g < s.S || g >= s.S + s.len) && nzM(g)) {   // owned but outside the slice: only digital silence may be dropped
                printf("plan: piece %02zu: the master has sound at %.6f s that does not fit the CD piece (only the master has it): not supported; nothing written\n", k + 1, sec(g)); return 1; }
            s.c.assign((size_t)s.lead * 2, 0); s.c.insert(s.c.end(), cds[k].begin(), cds[k].end());
            if (mf < 0) { printf("plan: piece %02zu has sound on the CD, but its range of the master is digital silence: content only the CD has (not supported in this step); nothing written\n", k + 1); return 1; }
            // CD frames clearly before the piece's first master sound or after its last must be digital silence (gaps, the disc ends; P9, P10)
            const long long jh = (mf - A[k]) / up - R, jt = (ml + 1 - A[k] + up - 1) / up + R;
            const long long bh = jh > 0 ? zero(cds[k], 0, jh) : -1, bt = zero(cds[k], std::max(0LL, jt), nk[k]);
            if (bh >= 0 || bt >= 0) { const long long j = bh >= 0 ? bh : bt;
                printf("plan: piece %02zu has sound on the CD at %.6f s of the piece where the master has none (%s): content only the CD has (a gap must be digital silence; not supported in this step); nothing written\n",
                       k + 1, j / 44100.0, bh >= 0 ? "before the piece's first master sound" : "after the piece's last master sound"); return 1; }
            s.near.push_back({std::max(0LL, jh) + s.lead, std::max(0LL, (mf - A[k]) / up + 1) + s.lead});   // the R frames next to the sound: step 4
            s.near.push_back({std::max(0LL, (ml - A[k]) / up) + s.lead, std::min(nk[k], std::max(0LL, jt)) + s.lead}); }
        return 0; };
    auto deriveB24 = [&](const std::vector<int32_t>& m) {   // the encoder's own derivation of a slice (mirror padding at its edges)
        encio::writeVec(T("s0.raw").c_str(), m);
        if (call(m_padblk::main, {"pad", T("s0.raw"), T("s.raw"), std::to_string(P)})) return std::vector<int32_t>();
        env("BASE24", T("sb24.s32")); const int r = call(m_baseblk::main, {"base", T("s.raw"), std::to_string(w.rate), "44100", T("sb16.s16")}); env("BASE24", "");
        return r ? std::vector<int32_t>() : encio::readAll<int32_t>(T("sb24.s32").c_str()); };
    // groups: consecutive sounding pieces whose master ranges touch at the CD's split point (no master sample, no silent piece between):
    // the CD splits continuous master material there, so the group has one whole offset and one fraction
    auto groupsOf = [&]() { std::vector<std::vector<size_t>> g; for (size_t k = 0; k < n; k++) { if (silentCd[k]) continue;
            if (!g.empty() && g.back().back() + 1 == k && A[k] == A[g.back().back()] + nk[g.back().back()] * up) g.back().push_back(k); else g.push_back({k}); } return g; };
    const double U1 = (double)lae_fd::unitsPerMaster(stages);
    auto measureGroup = [&](const std::vector<Slice>& s0, const std::vector<size_t>& g, double& frac, m_align::Report& r) -> bool {   // the group as one piece, measured as the encoder measures one
        std::vector<int32_t> m; std::vector<int16_t> c; for (size_t k : g) { m.insert(m.end(), s0[k].m.begin(), s0[k].m.end()); c.insert(c.end(), s0[k].c.begin(), s0[k].c.end()); }
        const long long len = (long long)m.size() / 2; const std::vector<int32_t> bk = deriveB24(m); if (bk.empty()) return false; const long long zero0 = 0; frac = 0;
        r = m_align::run(c, (long)c.size() / 2, bk, (long)bk.size() / 2, pb, m, (long)len, 44100, stages, 0, "", 0, -1, &zero0);
        if (r.found && r.band != m_align::BAND_STOP) { std::string pl; frac = -(double)m_align::measurePhase(c, m, len, stages, w.rate, 0, r, pl) / U1; }
        return true; };
    auto gname = [&](const std::vector<size_t>& g) { return g.size() == 1 ? fmt("piece %02zu", g[0] + 1) : fmt("pieces %02zu-%02zu", g.front() + 1, g.back() + 1); };
    if (alignArg.empty()) {   // positions only: both searches, each through the whole plan; nothing is written; the user confirms every position
        std::string st[2], li[2];
        for (int v = 0; v < 2; v++) { const bool wh = v == 0; const char* nm = wh ? "whitened" : "plain"; st[v] = "failed"; li[v] = "-";
            std::vector<Slice> s0;
            if (searchAll(wh) || build(s0, v == 0)) { printf("plan: %s search: alignment failed (above): confirm the positions by hand\n", nm); continue; }
            std::vector<std::string> pos(n, "-"); bool fail = false, ask = false;
            for (auto& g : groupsOf()) { double f = 0; m_align::Report r; if (!measureGroup(s0, g, f, r)) { printf("base derivation failed\n"); return 2; }
                if (!r.found || r.band == m_align::BAND_STOP) fail = true; else if (r.band != m_align::BAND_OK) ask = true;
                printf("plan (%s): %s%s: fraction %+.7f master samples; check: %s\n", nm, gname(g).c_str(), g.size() > 1 ? " (one group: the CD splits continuous master there)" : "", f, r.alignLine.c_str());
                for (size_t k : g) pos[k] = fmt("%+.7f", (double)A[k] - f); }   // where CD frame 0 lies after the move: whole minus the fraction moved
            li[v].clear(); for (size_t k = 0; k < n; k++) li[v] += (k ? "," : "") + pos[k];
            st[v] = fail ? "failed" : (ask ? "not_aligned" : "aligned");
            printf("plan: %s search: %s: cdalign=%s\n", nm, fail ? "alignment failed" : (ask ? "not aligned within the limit" : "aligned"), li[v].c_str()); }
        printf("PLAN_SUGGEST whitened=%s:%s plain=%s:%s\n", st[0].c_str(), li[0].c_str(), st[1].c_str(), li[1].c_str());
        printf("plan: nothing written. You confirm every position: run again with cdalign=<one value per piece, as above; - for a piece that is digital silence on the CD> (a value given by hand always wins)\n");
        return 1; }
    // ---- the positions you confirmed: one per piece, the master sample of its CD frame 0 (a fraction allowed)
    std::vector<double> fr(n, 0.0);
    { std::vector<std::string> v; size_t a = 0; for (;;) { const size_t b = alignArg.find(',', a); v.push_back(alignArg.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      if (v.size() != n) { printf("cdalign= gives %zu values; the disc has %zu pieces (one value per piece, - for a piece that is digital silence on the CD); nothing written\n", v.size(), n); return 1; }
      for (size_t k = 0; k < n; k++) { if (silentCd[k]) { if (v[k] != "-") { printf("piece %02zu is digital silence on the CD: give - for it; nothing written\n", k + 1); return 1; } continue; }
          char* e = nullptr; const double x = strtod(v[k].c_str(), &e);
          if (v[k].empty() || v[k] == "-" || !e || *e || !std::isfinite(x)) { printf("piece %02zu: '%s' is not a position (master samples; a fraction is written with a point); nothing written\n", k + 1, v[k].c_str()); return 1; }
          A[k] = (long long)std::llround(x); fr[k] = (double)A[k] - x;   // the slice at the whole position, the master moved by -fr = x - A: slice sample j = master(x + j)
          printf("plan: piece %02zu: confirmed CD frame 0 = master sample %+.7f (whole %lld, fraction %+.7f)\n", k + 1, x, A[k], fr[k]); } }
    std::vector<Slice> sl; if (build(sl, true)) return 1;   // the silence and coverage checks on the master as it is, before anything is moved
    const auto G = groupsOf();
    for (auto& g : G) for (size_t k : g) if (std::fabs(fr[k] - fr[g[0]]) > 5e-8) {
        printf("plan: %s join without a gap at the CD's split points but were given different fractions (%+.7f, %+.7f): pieces the CD splits from continuous master share one; nothing written\n", gname(g).c_str(), fr[g[0]], fr[k]); return 1; }
    // ---- the move, before cutting: each group by its own fraction, from the joined master with its real neighbouring samples; outside the
    // groups the master is digital silence (checked above) or less than one CD sample of a neighbour's sound, which stays as it is
    std::vector<std::string> spill; std::vector<int32_t> Z;
    if (!laes) {   // laes: no move; the fraction becomes T
    Z = w.s; long long lostS = 0, lostE = 0; long nclipAll = 0; const long long Wd = 72;
    auto gRange = [&](const std::vector<size_t>& g, long long& a, long long& b) { a = std::max(0LL, sl[g.front()].S); b = std::min(Lm, sl[g.back()].S + sl[g.back()].len); };
    for (auto& g : G) { long long a, b; gRange(g, a, b); if (fr[g[0]] != 0 && b > a) std::fill(Z.begin() + (ptrdiff_t)a * 2, Z.begin() + (ptrdiff_t)b * 2, 0); }
    for (auto& g : G) { long long a, b; gRange(g, a, b); const double mvv = -fr[g[0]]; if (mvv == 0 || b <= a) continue;
        std::vector<int32_t> win((size_t)(b - a + 2 * Wd) * 2, 0); std::copy(w.s.begin() + (ptrdiff_t)a * 2, w.s.begin() + (ptrdiff_t)b * 2, win.begin() + (ptrdiff_t)Wd * 2);
        long nc = 0; const std::vector<int32_t> mz = m_align::moveMaster(win, b - a + 2 * Wd, mvv, nc); nclipAll += nc;
        for (long long i = 0; i < b - a + 2 * Wd; i++) for (int ch = 0; ch < 2; ch++) { const int32_t vv = mz[(size_t)i * 2 + ch]; if (!vv) continue; const long long gi = a - Wd + i;
            if (gi < 0) lostS++; else if (gi >= Lm) lostE++; else Z[(size_t)gi * 2 + ch] += vv; }
        printf("plan: %s: the master moved by %+.7f master samples (its waveform taken that much later), from the joined master with the real neighbouring samples\n", gname(g).c_str(), mvv); }
    if (nclipAll) printf("plan: WARNING: %ld samples of the moved master exceed 24-bit full scale and are hard-clipped\n", nclipAll);
    src = &Z; if (build(sl, false)) return 1;   // the slices again, now from the moved master (the checks above saw the master as it is)
    {   // what the move carries to where no piece takes master samples (the master's very start or end, a gap only the master has): the user
        // decides whether to add digital silence there to both the master and the CD, the later split points moving with it (
        // silence is not content). 1 ms (44 CD frames) holds the spill at every rate: it reaches 64 master samples
        auto count = [&](long long x, long long y) { long long c = 0; for (long long i = std::max(0LL, x); i < std::min(Lm, y); i++) for (int ch = 0; ch < 2; ch++) if (Z[(size_t)i * 2 + ch] && !w.s[(size_t)i * 2 + ch]) c++; return c; };
        const std::string add = fmt("1 ms of digital silence (44 CD frames = %lld master samples) to both the master and the CD", 44 * up);
        long long prev = 0; size_t pk = n;
        for (size_t k = 0; k < n; k++) if (!silentCd[k]) { const long long c = count(prev, sl[k].S);
            if (pk == n) lostS += c; else if (c) spill.push_back(fmt("%lld sample values (left and right counted separately) to the gap between piece %02zu and %02zu (%.6f s of the master), which the CD does not have: to keep them, add at least %s there and move the later split points by the same amount", c, pk + 1, k + 1, sec(sl[k].S), add.c_str()));
            prev = std::max(prev, sl[k].S + sl[k].len); pk = k; }
        lostE += count(prev, Lm);
        if (lostS) spill.insert(spill.begin(), fmt("%lld sample values (left and right counted separately) before the first piece: to keep them, add at least %s at the start and move every split point by the same amount", lostS, add.c_str()));
        if (lostE) spill.push_back(fmt("%lld sample values (left and right counted separately) after the last piece: to keep them, add at least %s at the end", lostE, add.c_str()));
        for (auto& s : spill) printf("plan: NOTE: the move carries %s; then run the planner again (or leave it: these samples are not in any slice)\n", s.c_str()); }
    }   // !laes
    // 4. the encoder's own checks on every exact slice, as the encoder will see it: the CD frames next to the master's sound (cdOnly's
    // rule: non-zero only where the derived base is non-zero) and the three-block check at offset 0. The slices are already moved: a slice
    // that does not pass stops the planner (or, between -40 and beyond -33 dB, needs the confirmation, as in the encoder)
    std::vector<bool> needsAccept(n, false); bool anyAsk = false, anyLarge = false;
    for (size_t k = 0; k < n; k++) { if (silentCd[k]) continue; const std::vector<int32_t> bk = deriveB24(sl[k].m); if (bk.empty()) { printf("base derivation failed\n"); return 2; }
        const long nc = (long)sl[k].c.size() / 2;
        for (auto& wv : sl[k].near) for (long long j = std::max(0LL, wv.first); j < std::min<long long>(nc, wv.second); j++)
            if ((sl[k].c[(size_t)j * 2] || sl[k].c[(size_t)j * 2 + 1]) && !bk[(size_t)(pb + j) * 2] && !bk[(size_t)(pb + j) * 2 + 1]) {
                printf("plan: piece %02zu: CD frame %lld (%.6f s of the piece) has sound where the master has none and its derived base is zero (not the derivation filter's transition): content only the CD has (not supported in this step); nothing written\n",
                       k + 1, j - sl[k].lead, (j - sl[k].lead) / 44100.0); return 1; }
        const long long zero0 = 0; m_align::Report r = m_align::run(sl[k].c, nc, bk, (long)bk.size() / 2, pb, sl[k].m, (long)sl[k].len, 44100, stages, 0, "", 0, -1, &zero0);
        if (laes) {   // the cut piece is not moved: the check is the fraction measured on it and the error left after it (-60 / -54 dB bands)
            if (!r.found) { printf("plan: piece %02zu: the check of the cut piece finds no position: %s; nothing written\n", k + 1, r.why.c_str()); return 1; }
            std::string pl; const double meas = -(double)m_align::measurePhase(sl[k].c, sl[k].m, sl[k].len, stages, w.rate, 0, r, pl) / U1; const size_t pp = pl.find("pooled residual ");
            const double e = pp == std::string::npos ? 0.0 : atof(pl.c_str() + pp + 16);
            printf("plan: piece %02zu: check of the cut piece (not moved): fraction measured %+.7f master samples, confirmed %+.7f; alignment error after the fraction %.1f dB\n", k + 1, meas, fr[k], e);
            if (pp == std::string::npos || e > -60) { needsAccept[k] = true; anyAsk = true; if (pp == std::string::npos || e > -54) anyLarge = true;
                printf("plan: piece %02zu needs your confirmation: alignment error %.1f dB (%s -54 dB)\n", k + 1, e, e <= -54 ? "between -60 and" : "worse than"); }
            continue; }
        printf("plan: piece %02zu: check of the cut piece: %s\n", k + 1, r.alignLine.c_str());
        if (!r.found || r.band == m_align::BAND_STOP) { printf("plan: piece %02zu: the encoder's check of the cut piece does not pass: %s%s; nothing written\n", k + 1, r.why.c_str(), r.hint.empty() ? "" : ("; " + r.hint).c_str()); return 1; }
        if (r.band != m_align::BAND_OK || r.mapDiff) { needsAccept[k] = true; anyAsk = true; if (r.band == m_align::BAND_CONFIRM_LARGE) anyLarge = true; printf("plan: piece %02zu needs your confirmation: %s\n", k + 1, r.why.c_str()); } }
    if (anyAsk && !accept) {
        if (anyLarge) printf("  note: continuing is possible (the extension codes the whole difference and the precision stays the same), but the files may get much larger\n");
        if (!interactive()) { printf("  nothing written. If this is the right CD and it cannot be aligned better, add the option cdaccept.\n"); return 1; }
        printf("  1 = end the task (nothing is written)\n  2 = continue%s\n  choose 1 or 2: ", anyLarge ? " (the files may get much larger)" : ""); fflush(stdout);
        char line[32] = {0}; if (!fgets(line, sizeof line, stdin) || line[0] != '2') { printf("task ended: nothing written\n"); return 1; } }
    // the files' own split points against the CD's (only when the master came as several files)
    if (mfiles.size() > 1) { bool moved = false;   // each file boundary against the nearest place where a piece's master range starts (piece 01 included)
        for (size_t f = 1; f < mfiles.size(); f++) { const long long fb = fileStart[f]; long long best = LLONG_MAX; size_t bk = 0;
            for (size_t k = 0; k < n; k++) if (cutAt[k] != LLONG_MIN && std::llabs(cutAt[k] - fb) < std::llabs(best)) { best = cutAt[k] - fb; bk = k; }
            if (best != 0) { moved = true; if (best == LLONG_MAX) printf("plan: NOTE: the master is re-split at the CD's positions: the split between %s and %s (%.6f s of the joined master) is not used\n", mfiles[f - 1].c_str(), mfiles[f].c_str(), sec(fb));
                else printf("plan: NOTE: the master is re-split at the CD's positions: the split between %s and %s (%.6f s of the joined master) is not used; the nearest CD position, the start of piece %02zu, lies %+lld master samples (%s) from it\n",
                            mfiles[f - 1].c_str(), mfiles[f].c_str(), sec(fb), bk + 1, best, m_align::detail::dur(sec(best)).c_str()); } }
        if (!moved) printf("plan: the master files' own split points are the CD's positions\n"); }
    // 5. the slices on disk
    if (laes) {   // the same cuts from the master as it is, every bit; T = where CD frame 0 lies relative to the slice, in base samples:
        // slice sample i = master(A + i - lead*up), CD frame j (lead frame included) = master time x + (j - lead)*up, so base(u) = core(u + T) with T = (A - x) / up
        std::vector<Slice> sf; src = &full; if (build(sf, false)) return 1;
        std::string t = "LAE plan alignment report (LAES: the master is not moved)\npublisher disc: " + cueArg + "\nmaster:"; for (auto& f : mfiles) t += " " + f; t += "\n";
        std::string cmds;
        for (size_t k = 0; k < n; k++) { const Slice& s = sf[k]; const double Tk = silentCd[k] ? 0.0 : fr[k] / (double)up;
            char nm[64]; snprintf(nm, sizeof nm, "TRACK%02zu.master.wav", k + 1); const fsp::path pm = out / nm; snprintf(nm, sizeof nm, "TRACK%02zu.cd.wav", k + 1); const fsp::path pc = out / nm;
            const int bps = fullBits / 8; const size_t db = s.m.size() * bps; std::vector<uint8_t> o; wavio::hdr(o, w.rate, 2, fullBits, db, 0);
            for (int32_t v : s.m) for (int b = 0; b < bps; b++) o.push_back((uint8_t)((uint32_t)v >> (8 * b))); if (db & 1) o.push_back(0);
            FILE* f = fopen(pm.string().c_str(), "wb"); const bool ok = f && fwrite(o.data(), 1, o.size(), f) == o.size(); if (f && fclose(f)) { printf("cannot write %s\n", pm.u8string().c_str()); return 2; }
            if (!ok || !wavio::writeCD(pc.string().c_str(), 44100, s.c, {})) { printf("cannot write the slices of piece %02zu\n", k + 1); return 2; }
            char tx[64]; snprintf(tx, sizeof tx, "%.10f", Tk);
            printf("plan: piece %02zu: master %s, %lld samples (%d bit, not moved), CD %lld frames%s; T = %s base samples%s\n", k + 1, silentCd[k] ? "digital silence" : fmt("%.6f-%.6f s", sec(s.S), sec(s.S + s.len)).c_str(), s.len, fullBits, nk[k] + s.lead,
                   s.lead ? " (one digital-silence frame in front, in the pregap)" : "", tx, needsAccept[k] ? " (confirmed by you)" : "");
            t += fmt("piece %02zu: CD frame 0 = master sample %+.7f; slice from master sample %lld; T = %s base samples\n", k + 1, silentCd[k] ? 0.0 : (double)A[k] - fr[k], s.S, tx);
            cmds += fmt("  laes enc TRACK%02zu.laes.wav --core=TRACK%02zu.cd.wav TRACK%02zu.master.wav@%s\n", k + 1, k + 1, k + 1, tx); }
        const fsp::path pr = out / "plan.align.txt"; FILE* f = fopen(pr.string().c_str(), "wb"); const bool ok = f && fwrite(t.data(), 1, t.size(), f) == t.size(); if (f) fclose(f);
        printf(ok ? "plan: alignment report %s\n" : "plan: WARNING: the alignment report %s could not be written\n", pr.u8string().c_str());
        printf("plan OK (laes): %zu pieces written to %s, the master not moved. Encode each (other masters of the piece: plan them the same way and add them with their own @T):\n%s"
               "then lae_cdx image <dir> TRACK*.laes.wav layout=%s\n", n, outArg.c_str(), cmds.c_str(), cueArg.c_str());
        return 0; }
    for (size_t k = 0; k < n; k++) { const Slice& s = sl[k]; std::vector<int32_t> m24(s.m.size()); long long nz = 0;
        for (size_t i = 0; i < s.m.size(); i++) m24[i] = s.m[i] >> 8; for (long long i = 0; i < s.len; i++) if (s.m[(size_t)i * 2] || s.m[(size_t)i * 2 + 1]) nz++;
        char nm[64]; snprintf(nm, sizeof nm, "TRACK%02zu.master.wav", k + 1); const fsp::path pm = out / nm; snprintf(nm, sizeof nm, "TRACK%02zu.cd.wav", k + 1); const fsp::path pc = out / nm;
        if (!wavio::write24(pm.string().c_str(), w.rate, m24) || !wavio::writeCD(pc.string().c_str(), 44100, s.c, {})) { printf("cannot write the slices of piece %02zu\n", k + 1); return 2; }
        printf("plan: piece %02zu: master %s, %lld samples (%lld silent), CD %lld frames%s; encode it with cdalign=0%s\n", k + 1, silentCd[k] ? (nz ? "digital silence but its last samples" : "digital silence") : fmt("%.6f-%.6f s", sec(s.S), sec(s.S + s.len)).c_str(), s.len, s.len - nz, nk[k] + s.lead,
               s.lead ? " (one digital-silence frame in front, in the pregap)" : "", needsAccept[k] ? " cdaccept" : ""); }
    {   // the movement report (a separate text file for the user's records; plain text, read by no program)
        std::string t = "LAE plan alignment report\npublisher disc: " + cueArg + "\nmaster:"; for (auto& f : mfiles) t += " " + f; t += "\n";
        for (auto& g : G) t += fmt("%s%s: CD frame 0 of piece %02zu = master sample %+.7f; master moved by %+.7f master samples\n", gname(g).c_str(), g.size() > 1 ? " (one group: the CD splits continuous master there)" : "",
                                   g[0] + 1, (double)A[g[0]] - fr[g[0]], -fr[g[0]]);
        for (auto& s : spill) t += "the move carries " + s + " (not in any slice)\n";
        t += "each slice is already moved: encode it with cdalign=0\n";
        const fsp::path pr = out / "plan.align.txt"; FILE* f = fopen(pr.string().c_str(), "wb"); const bool ok = f && fwrite(t.data(), 1, t.size(), f) == t.size(); if (f) fclose(f);
        printf(ok ? "plan: movement report %s\n" : "plan: WARNING: the movement report %s could not be written\n", pr.u8string().c_str()); }
    // the position of every piece was confirmed by the user (2026-10-23); the slices are moved, so each piece is encoded at offset 0
    printf("plan OK: %zu pieces written to %s. Encode each with: lae_enc TRACKnn.master.wav TRACKnn.v5.wav 3 cd=TRACKnn.cd.wav cdalign=0 (3 = the mode; any mode works%s); "
           "then lae_cdx image <dir> TRACK*.v5.wav layout=%s\n",
           n, outArg.c_str(), anyAsk ? "; add cdaccept for the pieces marked above" : "", cueArg.c_str());
    return 0; }
