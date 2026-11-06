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

inline bool writeMarks(const char* path, const Marks& mk) {
    FILE* f = fopen(path, "wb"); if (!f) return false;   // "wb": LF line ends on every platform, so the file is the same everywhere
    fprintf(f, "LAENOISE 1\nmaster sha256 %s frames %lld rate %.0f\n", mk.sha.c_str(), mk.frames, mk.rate);
    fprintf(f, "grid mdct %d cell %d   # frame m = samples [mN-N, mN+N), its hop [mN-N/2, mN+N/2); column c = bins 8c..8c+7\n", NM, CW);
    for (size_t i = 0; i < mk.pos.size(); i++) { const Pos& p = mk.pos[i];
        fprintf(f, "pos %ld %ld %d %d   # %zu: %.3f-%.3f s (%.2f s), %.1f-%.1f Hz%s%s\n", p.m0, p.m1, p.c0, p.c1, i + 1, frameT0(p.m0, mk.rate), frameT0(p.m1, mk.rate),
                frameT0(p.m1, mk.rate) - frameT0(p.m0, mk.rate), colHz(p.c0, mk.rate), colHz(p.c1, mk.rate), i < mk.note.size() ? ", " : "", i < mk.note.size() ? mk.note[i].c_str() : ""); }
    return fclose(f) == 0; }
inline bool readMarks(const char* path, Marks& mk) {
    FILE* f = fopen(path, "r"); if (!f) return false; char line[1024]; bool magic = false;
    while (fgets(line, sizeof line, f)) { char* h = strchr(line, '#'); if (h) *h = 0;
        char sha[80]; long long fr; double rt; long m0, m1; int c0, c1, n, cw;
        if (!strncmp(line, "LAENOISE 1", 10)) magic = true;
        else if (sscanf(line, "master sha256 %79s frames %lld rate %lf", sha, &fr, &rt) == 3) { mk.sha = sha; mk.frames = fr; mk.rate = rt; }
        else if (sscanf(line, "grid mdct %d cell %d", &n, &cw) == 2) { if (n != NM || cw != CW) { fclose(f); fprintf(stderr, "unsupported grid %d / %d\n", n, cw); return false; } }
        else if (sscanf(line, "pos %ld %ld %d %d", &m0, &m1, &c0, &c1) == 4) mk.pos.push_back({m0, m1, c0, c1}); }
    fclose(f); return magic && !mk.sha.empty(); }
// the marking file must belong to this master, and every position must lie inside its grid
inline bool checkMarks(WavStream& ws, const Marks& mk) {
    if (mk.frames != ws.frames || fabs(mk.rate - ws.rate) > 0.5) { printf("the marking file is for another master (length or rate differ): nothing done\n"); return false; }
    if (masterSha(ws) != mk.sha) { printf("the marking file is for another master (SHA-256 differs): nothing done\n"); return false; }
    long K = mdct::frames((long)ws.frames);
    for (size_t i = 0; i < mk.pos.size(); i++) { const Pos& p = mk.pos[i];
        if (p.m0 < 0 || p.m1 > K || p.m0 >= p.m1 || p.c0 < 0 || p.c1 > NCOL || p.c0 >= p.c1) { printf("position %zu lies outside the grid: nothing done\n", i + 1); return false; } }
    return true; }
}  // namespace nmk
