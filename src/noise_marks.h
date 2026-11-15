// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// The LAEY noise marking format, shared by lae_noise (mark / show / preview) and the scanner (noise_scan0c): positions are
// rectangles of the encoder's own cells (textbook MDCT of mdct_tb.h, N = 512; a cell = 8 bins x 1 frame; M and S together).
// Geometry only: the master's SHA-256, length and rate, and the rectangles; no detector, reason, class or
// type. A scanner's candidate file and a user's marking file have the same format (the user's is a subset).
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include "mdct_tb.h"
#include "noise_io.h"
#include "laes_file.h"   // laes::Sha256
namespace nmk {
static const int NM = mdct::N, CW = 8, NCOL = NM / CW;
struct Pos { long m0, m1; int c0, c1; };   // frames [m0, m1), columns [c0, c1)
struct Marks { std::string sha; long long frames = 0; double rate = 0; std::vector<Pos> pos; std::vector<std::string> note; };   // note: comment text only, never read back

inline std::string masterSha(WavStream& ws) {
    laes::Sha256 h; const long long total = ws.frames * 2 * ws.bps; std::vector<uint8_t> b;
    for (long long off = 0; off < total; off += (1 << 22)) { size_t n = (size_t)std::min<long long>(1 << 22, total - off); if (!ws.raw(off, n, b)) { fprintf(stderr, "read failed\n"); exit(1); } h.update(b.data(), n); }
    uint8_t d[32]; h.final(d); char s[65]; for (int i = 0; i < 32; i++) snprintf(s + 2 * i, 3, "%02x", d[i]); return std::string(s, 64); }
inline double frameT0(long m, double rate) { return (m * (double)NM - NM / 2.0) / rate; }   // start of frame m's hop [mN - N/2, mN + N/2)
inline double colHz(int c, double rate) { return c * CW * rate / (2.0 * NM); }
// time range -> whole frames inside it (hop inside [t0, t1)); frequency range -> whole columns inside it
inline void snapTime(double t0, double t1, double rate, long K, long& m0, long& m1) {
    m0 = std::max(0L, (long)ceil((t0 * rate + NM / 2.0) / NM)); m1 = std::min(K, (long)floor((t1 * rate - NM / 2.0) / NM) + 1); }
inline void snapFreq(double f0, double f1, double rate, int& c0, int& c1) {
    c0 = std::max(0, (int)ceil(f0 / colHz(1, rate) - 1e-9)); c1 = std::min(NCOL, (int)floor(f1 / colHz(1, rate) + 1e-9)); }

// 2026-11-14: true when path b names the same existing file as path a (any spelling): an output that is the master
// itself is refused before it is opened for writing ("wb" would empty the master while it is being read)
inline bool sameFile(const char* a, const char* b) { std::error_code ec; const bool eq = std::filesystem::equivalent(std::filesystem::path(a), std::filesystem::path(b), ec); return eq && !ec; }
inline bool writeMarks(const char* path, const Marks& mk) {
    FILE* f = fopen(path, "wb"); if (!f) return false;   // "wb": LF line ends on every platform, so the file is the same everywhere
    fprintf(f, "LAENOISE 1\nmaster sha256 %s frames %lld rate %.0f\n", mk.sha.c_str(), mk.frames, mk.rate);
    fprintf(f, "grid mdct %d cell %d   # frame m = samples [mN-N, mN+N), its hop [mN-N/2, mN+N/2); column c = bins 8c..8c+7\n", NM, CW);
    for (size_t i = 0; i < mk.pos.size(); i++) { const Pos& p = mk.pos[i];
        fprintf(f, "pos %ld %ld %d %d   # %zu: %.3f-%.3f s (%.2f s), %.1f-%.1f Hz%s%s\n", p.m0, p.m1, p.c0, p.c1, i + 1, frameT0(p.m0, mk.rate), frameT0(p.m1, mk.rate),
                frameT0(p.m1, mk.rate) - frameT0(p.m0, mk.rate), colHz(p.c0, mk.rate), colHz(p.c1, mk.rate), i < mk.note.size() ? ", " : "", i < mk.note.size() ? mk.note[i].c_str() : ""); }
    const bool ok = !ferror(f); return fclose(f) == 0 && ok; }   // a write error (disk full) counts, not only fclose
// Reading rules (SPEC_LAENOISE.md Section 4; strict since 2026-11-07, the same in NoiseMarks.java): a comment (# to the end of the
// line), CR and blank lines are skipped. The first remaining line is exactly "LAENOISE 1". Then exactly one master line and exactly one
// grid line, both before the first pos line. A line that begins with LAENOISE, master, grid or pos must have exactly its fields (a
// second LAENOISE line is an error); any other line is ignored. why: the reason when the file is refused.
inline bool readMarks(const char* path, Marks& mk, std::string* why = nullptr) {
    auto bad = [&](long ln, const std::string& w) { if (why) *why = ln ? "line " + std::to_string(ln) + ": " + w : w; return false; };
    FILE* f = fopen(path, "rb"); if (!f) return bad(0, "cannot open the file");
    std::string all; { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) all.append(b, n); } fclose(f);
    auto digits = [](const std::string& s, int maxLen) { if (s.empty() || (int)s.size() > maxLen) return false; for (char c : s) if (c < '0' || c > '9') return false; return true; };
    bool magic = false, haveMaster = false, haveGrid = false; long ln = 0;
    for (size_t p = 0; p < all.size();) { size_t e = all.find('\n', p); if (e == std::string::npos) e = all.size();
        std::string line = all.substr(p, e - p); p = e + 1; ln++;
        const size_t h = line.find('#'); if (h != std::string::npos) line.resize(h);
        std::vector<std::string> t; for (size_t q = 0; q < line.size();) { while (q < line.size() && (line[q] == ' ' || line[q] == '\t' || line[q] == '\r')) q++;
            size_t r = q; while (r < line.size() && line[r] != ' ' && line[r] != '\t' && line[r] != '\r') r++; if (r > q) t.push_back(line.substr(q, r - q)); q = r; }
        if (t.empty()) continue;
        if (!magic) { if (t.size() == 2 && t[0] == "LAENOISE" && t[1] == "1") { magic = true; continue; } return bad(ln, "the first line must be \"LAENOISE 1\" (not a marking file of version 1)"); }
        if (t[0] == "LAENOISE") return bad(ln, "a second LAENOISE line");
        if (t[0] == "master") {
            if (haveMaster) return bad(ln, "a second master line");
            if (!mk.pos.empty()) return bad(ln, "the master line must come before the pos lines");
            if (t.size() != 7 || t[1] != "sha256" || t[3] != "frames" || t[5] != "rate") return bad(ln, "expected \"master sha256 <64 hex digits> frames <L> rate <fs>\"");
            if (t[2].size() != 64) return bad(ln, "the SHA-256 must have 64 hex digits"); for (char c : t[2]) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return bad(ln, "the SHA-256 must be lower-case hex digits");
            if (!digits(t[4], 18) || !digits(t[6], 9)) return bad(ln, "frames and rate must be whole numbers");
            mk.sha = t[2]; mk.frames = atoll(t[4].c_str()); mk.rate = (double)atoll(t[6].c_str()); haveMaster = true; }
        else if (t[0] == "grid") {
            if (haveGrid) return bad(ln, "a second grid line");
            if (!mk.pos.empty()) return bad(ln, "the grid line must come before the pos lines");
            if (t.size() != 5 || t[1] != "mdct" || t[3] != "cell" || !digits(t[2], 9) || !digits(t[4], 9)) return bad(ln, "expected \"grid mdct 512 cell 8\"");
            if (atoi(t[2].c_str()) != NM || atoi(t[4].c_str()) != CW) return bad(ln, "unsupported grid mdct " + t[2] + " cell " + t[4] + " (version 1 has only mdct 512 cell 8)");
            haveGrid = true; }
        else if (t[0] == "pos") {
            if (!haveMaster || !haveGrid) return bad(ln, "a pos line before the master and grid lines");
            if (t.size() != 5 || !digits(t[1], 18) || !digits(t[2], 18) || !digits(t[3], 9) || !digits(t[4], 9)) return bad(ln, "expected \"pos <m0> <m1> <c0> <c1>\" with whole numbers");
            // 2026-11-14: a frame number above 2^31 - 1 lies outside every grid; it is kept as 2^31 - 1, so that it fails the grid
            // check on every platform (a 32-bit long, Windows, cut 4294967297 to 1 and accepted it; Linux and Java refuse it)
            auto frameNo = [](const std::string& s) { return (long)std::min<long long>(atoll(s.c_str()), 2147483647LL); };
            mk.pos.push_back({frameNo(t[1]), frameNo(t[2]), atoi(t[3].c_str()), atoi(t[4].c_str())}); } }
    if (!magic) return bad(0, "not a marking file (no \"LAENOISE 1\" line)");
    if (!haveMaster) return bad(0, "no master line");
    if (!haveGrid) return bad(0, "no grid line");
    return true; }
// every position must lie inside the grid of a master of this length (2026-11-14: a function of its own, so that
// lae_noise level, which compares length and rate but no SHA-256, checks it too)
inline bool checkGrid(long long frames, const Marks& mk) {
    long K = mdct::frames((long)frames);
    for (size_t i = 0; i < mk.pos.size(); i++) { const Pos& p = mk.pos[i];
        if (p.m0 < 0 || p.m1 > K || p.m0 >= p.m1 || p.c0 < 0 || p.c1 > NCOL || p.c0 >= p.c1) { printf("position %zu lies outside the grid: nothing done\n", i + 1); return false; } }
    return true; }
// the marking file must belong to this master, and every position must lie inside its grid
inline bool checkMarks(WavStream& ws, const Marks& mk) {
    if (mk.frames != ws.frames || fabs(mk.rate - ws.rate) > 0.5) { printf("the marking file is for another master (length or rate differ): nothing done\n"); return false; }
    if (masterSha(ws) != mk.sha) { printf("the marking file is for another master (SHA-256 differs): nothing done\n"); return false; }
    return checkGrid(ws.frames, mk); }
}  // namespace nmk
