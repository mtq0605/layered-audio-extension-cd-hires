// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Check of src/mdct_tb.h: perfect reconstruction (forward -> inverse) on noise and on a near-full-scale sine, and orthonormal scale.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "../src/mdct_tb.h"
int main() {
    long L = 100000; std::vector<int32_t> x(L); uint32_t r = 1;
    for (long i = 0; i < L; i++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; x[i] = (int32_t)((i < L / 2) ? ((int32_t)(r >> 8) - 8388608) : lround(8000000 * sin(0.0123 * i))); }
    auto X = mdct::forward(x); auto y = mdct::inverse(X, L); long bad = 0; long mx = 0; double ex = 0, eX = 0;
    for (long i = 0; i < L; i++) { long d = labs((long)x[i] - y[i]); mx = d > mx ? d : mx; if (d > 2) bad++; ex += (double)x[i] * x[i]; }
    for (size_t i = 0; i < X.size(); i++) eX += (double)X[i] * X[i];
    printf("max |x - inverse(forward(x))| = %ld LSB, samples off by more than 2: %ld | energy ratio coefficients/signal %.6f\n", mx, bad, eX / ex);
    return 0; }
