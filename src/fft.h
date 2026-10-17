// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Complex FFT (forward, unnormalised) for the encoder's analysis stages and the test tools; n must be a power of two.
// A plan per size holds a bit-reversal table and one twiddle table exp(-2 pi i k / n), k < n/2.
// The transform reorders the input from the table and then runs log2(n) passes of radix-2 butterflies,
// taking every twiddle from the table with a stride. Plans are cached per thread.
#pragma once
#include <cstdint>
#include <cmath>
#include <complex>
#include <vector>
#include <map>
#include <utility>
namespace fftlib {
struct Plan { size_t n = 0; std::vector<uint32_t> rev; std::vector<std::complex<double>> tw;
    explicit Plan(size_t size) : n(size), rev(size, 0), tw(size / 2) {
        int bits = 0; while (((size_t)1 << bits) < n) bits++;
        for (size_t i = 1; i < n; i++) rev[i] = (uint32_t)((rev[i >> 1] >> 1) | ((i & 1) << (bits - 1)));
        const double PI = 3.14159265358979323846;
        for (size_t k = 0; k < n / 2; k++) { double a = -2.0 * PI * (double)k / (double)n; tw[k] = std::complex<double>(std::cos(a), std::sin(a)); } } };
inline const Plan& planFor(size_t n) { thread_local std::map<size_t, Plan> cache;
    auto it = cache.find(n); if (it == cache.end()) it = cache.emplace(n, Plan(n)).first; return it->second; }
inline void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size(); if (n < 2) return; const Plan& p = planFor(n);
    for (size_t i = 0; i < n; i++) if (i < p.rev[i]) std::swap(a[i], a[p.rev[i]]);
    for (size_t half = 1, stride = n / 2; half < n; half <<= 1, stride >>= 1)
        for (size_t blk = 0; blk < n; blk += 2 * half)
            for (size_t k = 0; k < half; k++) {
                std::complex<double>& lo = a[blk + k]; std::complex<double>& hi = a[blk + half + k];
                const std::complex<double> t = hi * p.tw[k * stride]; hi = lo - t; lo += t; } }
}
