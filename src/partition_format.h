// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE partition format: everything a decoder needs to read a partitioned layer - precision rules, reconstruction,
// zero-level table, mode table and the block geometry (root blocks, binary cut positions, cells).
// The coefficient plane of one channel (frames x bins k0..k1) is divided into cells of CW bins x 1 frame, grouped into fixed root
// blocks of ROOTF cells x ROOTT frames; each root is cut recursively (one binary cut at a time, in frequency or time) into leaves.
// Each leaf carries its own tier (A/B/C/D), exponent, zero-level index and line-wise coefficients.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdlib>

namespace part {
static const int TB[4] = {0, 2, 4, 8};   // bits relative to the block exponent for A/B/C/D
// 16-bit backstop: no quantisation step is coarser than 1 LSB16 (2^8 in LSB24 units), as in the CD layer: everything above the
// 16-bit level is coded with its waveform.
static const int STEPMAX = 8;
// Precision rules (recorded per stream in the stream header field cw = CW + 256 x rule):
//   rule 1 (scheme 1, both layers; scheme 2 depth layer): step exponent s = min(E, 8) - TB  -> the finer of "block peak - 2/4/8 bit"
//          (waveform similarity) and "16-bit base + 2/4/8 bit" (absolute): loud blocks B = 18, C = 20, D = 24 bit
//   rule 2 (scheme 2 frequency layer): s = min(E - TB, 8 - {0,0,4,8}) -> loud blocks B = 16, C = 20, D = 24 bit, same relative part
//   rule 0: container v4 (s = min(8, E - TB)).
inline int shiftOfR(int E, int tier, int rule) {
    static const int TB16[4] = {0, 0, 4, 8};
    if (rule == 1) return std::max(0, std::min(E, STEPMAX) - TB[tier]);
    if (rule == 2) return std::max(0, std::min(E - TB[tier], STEPMAX - TB16[tier]));
    if (rule == 3) return std::max(0, std::min(E, STEPMAX - 2) - TB[tier]);   // keep20 depth layer: B 20 / C 22 / D 24 bit
    return std::min(STEPMAX, std::max(0, E - TB[tier])); }
static const double RECOFF = 0.20;   // reconstruction offset of non-zero values, (|q| + 0.20) x step: a format constant
inline double recOff() { return RECOFF; }
// Zero level: every leaf carries a 3-bit zero-level index. 0 = zero coefficients stay 0;
// k > 0 = each zero coefficient carries its TRUE sign (1 bit) and is rebuilt as sign x ZC[k] x step. No noise is generated anywhere.
// The encoder writes k > 0 only with the opt-in "zs" (zero sign) option; ZC is dyadic (exact in binary).
static const double ZC[8] = {0.0, 1.0 / 32, 1.0 / 16, 3.0 / 32, 1.0 / 8, 3.0 / 16, 1.0 / 4, 3.0 / 8};
inline double recon(int32_t qi, double st) { return qi ? (qi + (qi > 0 ? recOff() : -recOff())) * st : 0.0; }
static const int CW = 8;                  // cell width in bins
static const int ROOTF = 16, ROOTT = 8;   // root block: 16 cells x 8 frames (same for every mode)
struct Mode { const char* name; double dShare, cShare; };
// 6 modes per scheme; every adjacent pair is >= 5.8 % apart in size in all structured worst cases (theory).
// keep20 = keep with the 20-bit depth layer (opt-in, larger file). Index: 0 max, 1 "1", 2 "3", 3 "5", 4 keep, 5 keep20.
static const int NMODES = 6, MODE_KEEP = 4, MODE_KEEP20 = 5;
static const Mode MODES[NMODES] = {{"max", 0.10, 0.30}, {"1", 0.30, 0.50}, {"3", 0.50, 0.70}, {"5", 0.70, 0.90}, {"keep", 0.90, 1.00}, {"keep20", 0.90, 1.00}};
inline int modeIndex(const char* s) { for (int i = 0; i < NMODES; i++) { const char* a = MODES[i].name; const char* b = s; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) return i; } return -1; }

// ---- geometry shared by encoder and decoder
inline std::vector<int> cuts(int w, bool equal) {
    std::vector<int> c; if (w < 2) return c;
    if (equal) { c.push_back(w / 2); return c; }
    for (int p = 1; p < w; p++) c.push_back(p);
    return c; }
inline int posBits(size_t n) { int b = 0; while ((size_t)(1u << b) < n) b++; return b; }
struct Plane { const std::vector<int32_t>* X; long K; int N, k0, k1, nc; };   // bins k0..k1-1, nc = cells
inline int cellBins(const Plane& P, int c, int& kb) { kb = P.k0 + c * CW; return std::min(P.k1, kb + CW) - kb; }
inline int numCells(int k0, int k1) { return (k1 - k0 + CW - 1) / CW; }
}   // namespace part
