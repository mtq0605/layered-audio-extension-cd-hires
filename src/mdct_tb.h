// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Textbook MDCT / IMDCT for the lossy tier: sine window, 50 % overlap, N = 512 coefficients/frame.
//   forward: multiply by the window -> fold the 2N windowed samples into N (additions only: [-c_r - d, a - b_r]) -> DCT-IV
//   inverse: DCT-IV -> unfold (N -> 2N) -> multiply by the window -> overlap-add
// The DCT-IV is the classic algorithm via an N/2-point complex FFT (pre-twiddle, radix-2 FFT, post-twiddle); both twiddles are
// exp(-i 2pi (n + 1/8) / 2N).
// Determinism: only IEEE-754 double +, -, x in a fixed order with constants from mdct_tables.h (exact literals), final rounding by
// llround; build without fused multiply-add contraction (MSVC /fp:precise default; GCC/Clang -ffp-contract=off) so every platform
// produces bit-identical integers. Frame layout: frame m covers padded samples [m N, m N + 2N) of
// [N zeros | signal | zeros], K = ceil(L / N) + 1 frames.
#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "mdct_tables.h"
namespace mdct {
typedef int32_t i32;
static const int N = 512, M = N / 2;
inline long frames(long L) { return (L + N - 1) / N + 1; }
// in-place radix-2 complex FFT of size M (decimation in time); the input is reordered by the bit-reversal table BITREV
inline void fft(double* re, double* im) {
    for (int i = 0; i < M; i++) { int j = BITREV[i]; if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (int len = 2; len <= M; len <<= 1) { int step = M / len;
        for (int i = 0; i < M; i += len) for (int k = 0; k < len / 2; k++) { double wr = FFT_C[k * step], wi = FFT_S[k * step];
            double xr = re[i + k + len / 2], xi = im[i + k + len / 2]; double tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
            re[i + k + len / 2] = re[i + k] - tr; im[i + k + len / 2] = im[i + k] - ti; re[i + k] += tr; im[i + k] += ti; } } }
// orthonormal DCT-IV (self-inverse): X[k] = sqrt(2/N) sum u[n] cos(pi/N (n+1/2)(k+1/2))
inline void dct4(const double* u, double* X) {
    double re[M], im[M];
    for (int n = 0; n < M; n++) { double a = u[2 * n], b = u[N - 1 - 2 * n]; re[n] = a * PRE_C[n] - b * PRE_S[n]; im[n] = a * PRE_S[n] + b * PRE_C[n]; }
    fft(re, im);
    for (int k = 0; k < M; k++) { double yr = (re[k] * POST_C[k] - im[k] * POST_S[k]) * SCALE, yi = (re[k] * POST_S[k] + im[k] * POST_C[k]) * SCALE; X[2 * k] = yr; X[N - 1 - 2 * k] = -yi; } }
// startup self-test: a fixed coefficient pattern through the IMDCT; the FNV-1a-style hash of the integer output must equal the reference
// value. A mismatch means this build does not follow IEEE-754 double arithmetic in the specified order (x87 extended precision,
// fused multiply-add contraction, fast-math...) and its output would differ from other platforms.
inline std::vector<i32> inverse(const std::vector<i32>& X, long L);
static const uint64_t SELFTEST_REF = 0x729b547fbdbfe203ull;   // reference value; verified builds are listed in docs/BUILD.md
inline uint64_t selftestHash() {
    const long L = 6 * N; std::vector<i32> X((size_t)(frames(L)) * N);
    for (size_t i = 0; i < X.size(); i++) X[i] = (i32)((((uint32_t)i * 2654435761u) >> 7) % 2000001u) - 1000000;   // deterministic pattern up to +-1e6 (32-bit arithmetic on every platform)
    std::vector<i32> y = inverse(X, L); uint64_t h = 1469598103934665603ull;
    for (i32 v : y) { uint32_t u = (uint32_t)v; for (int b = 0; b < 4; b++) { h ^= (u >> (8 * b)) & 0xFF; h *= 1099511628211ull; } }
    return h; }
inline bool selftestOK() { return SELFTEST_REF == 0 || selftestHash() == SELFTEST_REF; }
inline std::vector<i32> forward(const std::vector<i32>& x) {
    long L = (long)x.size(), K = frames(L); std::vector<i32> X((size_t)K * N); double z[2 * N], u[N], c[N];
    auto at = [&](long p) -> double { long i = p - N; return (i >= 0 && i < L) ? (double)x[i] : 0.0; };   // padded: N zeros first
    for (long m = 0; m < K; m++) { long s = m * N;
        for (int n = 0; n < 2 * N; n++) z[n] = at(s + n) * WIN[n];                                             // window
        for (int n = 0; n < N / 2; n++) { u[n] = -z[3 * N / 2 - 1 - n] - z[3 * N / 2 + n]; u[N / 2 + n] = z[n] - z[N - 1 - n]; }   // fold: [-c_r - d, a - b_r]
        dct4(u, c); for (int k = 0; k < N; k++) X[(size_t)m * N + k] = (i32)std::llround(c[k]); }
    return X; }
inline std::vector<i32> inverse(const std::vector<i32>& X, long L) {
    long K = frames(L), LP = (K + 1) * N; std::vector<double> y(LP, 0.0); double c[N], u[N];
    for (long m = 0; m < K && (size_t)(m + 1) * N <= X.size(); m++) {
        for (int k = 0; k < N; k++) c[k] = X[(size_t)m * N + k];
        dct4(c, u); long s = m * N;
        for (int n = 0; n < N / 2; n++) {                                                                          // unfold [u2, -u2_r, -u1_r, -u1], window, overlap-add
            y[s + n] += u[N / 2 + n] * WIN[n];
            y[s + N / 2 + n] += -u[N - 1 - n] * WIN[N / 2 + n];
            y[s + N + n] += -u[N / 2 - 1 - n] * WIN[N + n];
            y[s + 3 * N / 2 + n] += -u[n] * WIN[3 * N / 2 + n]; } }
    std::vector<i32> out(L); for (long i = 0; i < L; i++) out[i] = (i32)std::llround(y[N + i]); return out; }
}
