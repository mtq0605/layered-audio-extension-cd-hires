// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Accuracy of src/lae_math.h against the C library over the ranges the encoder uses (2026-11-03), and a checksum of the results
// that must be the same on every platform. usage: lae_math_check   (build: cl /O2 /fp:strict /I src tools\lae_math_check.cpp)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "lae_math.h"
// FU20-2 (2026-11-14): pi written here (the value of M_PI), so that the build command above works without _USE_MATH_DEFINES
static const double PI = 3.14159265358979323846;
static double ulps(double a, double b) { if (a == b) return 0; const double d = std::fabs(a - b), u = std::fabs(std::nextafter(b, 2 * b + 1) - b); return u > 0 ? d / u : d; }
static uint64_t mix(uint64_t h, double v) { uint64_t b; std::memcpy(&b, &v, 8); return (h ^ b) * 0x100000001B3ull; }
int main() {
    uint64_t h = 0xCBF29CE484222325ull; double w; long n;
    // log2: code lengths -log2(p), p in (0, 1]; exponents log2(v + 1), v integer up to 2^31; tiers log2(C fl)
    w = 0; n = 0; for (long i = 1; i <= 1000000; i++) { const double x = i / 1000000.0; w = std::fmax(w, ulps(lmath::log2(x), std::log2(x))); h = mix(h, lmath::log2(x)); n++; }
    for (long v = 0; v < 1000000; v++) { const double x = (double)v + 1.0; w = std::fmax(w, ulps(lmath::log2(x), std::log2(x))); h = mix(h, lmath::log2(x)); }
    for (int e = 0; e < 63; e++) { const double x = std::ldexp(1.0, e); if (lmath::log2(x) != e) { printf("log2(2^%d) not exact\n", e); return 1; } }
    for (long v = 0; v < 2000000; v++) { const double x = (double)v + 1.0; if (std::ceil(lmath::log2(x)) != std::ceil(std::log2(x))) { printf("ceil(log2(%ld)) differs\n", v + 1); return 1; } }
    printf("log2:  max %.2f ulp (%ld values; exact at powers of two; ceil(log2(v + 1)) as the C library for v < 2e6)\n", w, n);
    // sinpi / cospi: windows 2 i / N, taps x k up to a few thousand half-turns, twiddles -2 k / n
    w = 0; double wc = 0; for (long i = -4000000; i <= 4000000; i++) { const double t = i / 1000.0;
        w = std::fmax(w, std::fabs(lmath::sinpi(t) - std::sin(PI * t))); wc = std::fmax(wc, std::fabs(lmath::cospi(t) - std::cos(PI * t))); h = mix(mix(h, lmath::sinpi(t)), lmath::cospi(t)); }
    printf("sinpi / cospi: max abs difference %.3g / %.3g for |t| <= 4000 (the C library also rounds pi * t)\n", w, wc);
    w = 0; for (int N = 2; N <= 65536; N *= 2) for (int i = 0; i < N; i++) w = std::fmax(w, std::fabs(lmath::cospi(2.0 * i / N) - std::cos(2 * PI * i / N)));
    printf("cospi(2 i / N), N = 2 .. 65536: max abs difference %.3g\n", w);
    // exp10 (dB thresholds), log10
    w = 0; for (long i = -2000000; i <= 2000000; i++) { const double x = i / 10000.0; w = std::fmax(w, ulps(lmath::exp10(x / 20), std::pow(10.0, x / 20))); h = mix(h, lmath::exp10(x / 20)); }
    printf("exp10: max %.2f ulp for -200 .. 200 dB\n", w);
    w = 0; for (long i = 1; i <= 2000000; i++) { const double x = i * 0.37; w = std::fmax(w, ulps(lmath::log10(x), std::log10(x))); h = mix(h, lmath::log10(x)); }
    printf("log10: max %.2f ulp\n", w);
    printf("checksum (must be the same on every platform): %016llx\n", (unsigned long long)h);
    // FU20-1 (2026-11-14): the reference value (MSVC x64 and x86, /fp:strict, 2026-11-14); another value is a failure, exit code 1
    const uint64_t REF = 0x22f04c1237795a7dull;
    if (h != REF) { printf("FAILED: the checksum differs from the reference %016llx: lae_math gives other values on this platform\n", (unsigned long long)REF); return 1; }
    return 0; }
