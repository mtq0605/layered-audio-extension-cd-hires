// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// The encoder's elementary functions, identical on every platform (2026-11-03). The C library's log2 / sin / cos / pow / log10 may
// differ in the last bit between platforms (x86 and x64 already did: log2 changed the choice between two partitions of equal cost), and the
// encoder's decisions (code lengths, detector thresholds, filter taps) follow from them. These versions use only IEEE-754 binary64
// +, -, *, / and the exactly rounded or exact operations sqrt, floor, frexp, ldexp, in the fixed order written below, the way
// laes_math.h does for LAES. Compile with /fp:strict (MSVC) or -ffp-contract=off (GCC / Clang); on 32-bit x86 with SSE2.
// Accuracy: a few units in the last place (tools/lae_math_check.cpp measures it against the C library).
#pragma once
#include <cmath>
namespace lmath {
inline double pi() { return 3.141592653589793115997963468544185161590576171875; }   // the binary64 nearest to pi
// sin(pi y) and cos(pi y) for |y| <= 1/4: Taylor series to y^29 / y^28, Horner, fixed order (as laes_math.h sinSeries)
inline double sinPiSmall(double y) { const double x = pi() * y, x2 = x * x; double s = 1.0; for (int k = 14; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k) * (2.0 * k + 1.0)); return x * s; }
inline double cosPiSmall(double y) { const double x = pi() * y, x2 = x * x; double s = 1.0; for (int k = 14; k >= 1; k--) s = 1.0 - s * x2 / ((2.0 * k - 1.0) * (2.0 * k)); return s; }
// the argument in half-turns: sinpi(t) = sin(pi t). t - n (n the nearest integer) is exact for |t| < 2^52, so no reduction by an
// inexact pi is needed; every angle the encoder uses is a multiple of pi (2 pi i / N, pi x k, twiddles)
inline void reduce(double t, double& r, bool& odd) { const double n = std::floor(t + 0.5); r = t - n; odd = std::fmod(n, 2.0) != 0; }
inline double sinpi(double t) { double r; bool odd; reduce(t, r, odd); const double a = r < 0 ? -r : r;
    double v = a <= 0.25 ? sinPiSmall(a) : cosPiSmall(0.5 - a);   // 0.5 - a is exact (Sterbenz)
    if (r < 0) v = -v; return odd ? -v : v; }
inline double cospi(double t) { double r; bool odd; reduce(t, r, odd); const double a = r < 0 ? -r : r;
    const double v = a <= 0.25 ? cosPiSmall(a) : sinPiSmall(0.5 - a); return odd ? -v : v; }
// log2 x for x > 0: x = m 2^e exactly (frexp), m in [sqrt(1/2), sqrt(2)), log2 m = (2 / ln 2) atanh(s), s = (m - 1) / (m + 1),
// |s| <= 0.1716, series to s^25. Exact for powers of two (s = 0).
inline double log2(double x) {
    if (!(x > 0)) return x == 0 ? -HUGE_VAL : NAN; if (x == HUGE_VAL) return x;
    int e; double m = std::frexp(x, &e); if (m < 0.70710678118654752440) { m = m * 2.0; e = e - 1; }
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s; double p = 1.0 / 25.0;
    for (int k = 23; k >= 1; k -= 2) p = 1.0 / (double)k + s2 * p;
    return (double)e + 2.8853900817779268147 * (s * p); }   // 2 / ln 2
inline double log10(double x) { return log2(x) * 0.30102999566398119521; }   // log10 2
// 2^y: y = n + f exactly (n = floor y, 0 <= f < 1), 2^f = e^(f ln 2) by the Taylor series to z^22, then ldexp (exact)
inline double exp2(double y) { const double n = std::floor(y), z = (y - n) * 0.69314718055994530942; double s = 1.0;
    for (int k = 22; k >= 1; k--) s = 1.0 + s * z / (double)k; return std::ldexp(s, (int)n); }
inline double exp10(double x) { return exp2(x * 3.3219280948873623479); }   // log2 10
}
