// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Fractional delay by d = phi / U1 master samples (U1 = 2^22 >> stages, so the unit is 2^-22 of a CD sample; 0 <= d < 1).
// Encoder only (design A, 2026-10-23): m_align::measurePhase uses it to try fractions of a master sample through the encoder's own
// derivation; the master is then moved by the measured fraction (m_align::moveMaster) and the file stays v5. The decoder does not use it
// (the v6 phase number that would have delayed the decoder's CD path is withdrawn).
// The delay is one time-invariant FIR at the master rate, applied to the whole piece, sample by sample: y[n] = sum_k h[k] x[n - k],
// k = -15 .. 16. Its taps are computed once per piece from phi by a closed formula, the Lagrange interpolation coefficients
// h[k] = prod over m != k of (d - m) / (k - m), in IEEE double in this fixed order, rounded to Q28 (the centre tap h[0] takes the rounding
// remainder so that the taps sum to exactly 2^28: DC gain 1). No table of coefficient sets, no interpolation between sets or between
// filter outputs, no rate conversion. Accuracy (20 kHz, d = 0.5): about -300 dB at 352.8 kHz, -300 dB at 176.4 kHz, -133 dB at 88.2 kHz.
#pragma once
#include <cstdint>
#include <cmath>
namespace lae_fd {
static const int TAPS = 32, K0 = -15;              // taps for k = -15 .. 16
static const int64_t CD_UNITS = (int64_t)1 << 22;  // phi units per CD sample
static inline int64_t unitsPerMaster(int stages) { return CD_UNITS >> stages; }
// taps for a delay of d = phi / unitsPerMaster(stages) master samples (0 <= phi < unitsPerMaster(stages))
static inline void taps(int64_t phi, int stages, int32_t h[TAPS]) {
    const double d = (double)phi / (double)unitsPerMaster(stages); int64_t sum = 0;
    for (int i = 0; i < TAPS; i++) { const int k = K0 + i; double p = 1.0;
        for (int j = 0; j < TAPS; j++) { const int m = K0 + j; if (m != k) p = p * ((d - (double)m) / (double)(k - m)); }
        h[i] = (int32_t)std::llround(p * 268435456.0); sum += h[i]; }
    h[-K0] += (int32_t)(268435456LL - sum); }
// one output sample of the delay, from a source of int64 samples get(i) (any index; the caller mirrors at the edges). Inputs are limited
// to +-2^30 first (valid signals stay far below); with |h| summing to about 2, the 64-bit sum cannot overflow on any input
template <class Get> static inline int64_t apply(const int32_t h[TAPS], long n, Get get) {
    int64_t a = 0; for (int i = 0; i < TAPS; i++) { int64_t x = get(n - (K0 + i)); x = x > (1LL << 30) ? (1LL << 30) : (x < -(1LL << 30) ? -(1LL << 30) : x); a += (int64_t)h[i] * x; }
    return (a + ((int64_t)1 << 27)) >> 28; }
}
