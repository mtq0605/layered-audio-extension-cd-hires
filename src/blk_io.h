// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise encoder (2026-10-29): checked random-access reads and streamed writes of raw files, and the index range a mirrored filter
// window touches. Same checks as enc_io.h: a failed read or write throws encio::Fail; a failed write removes the file; LAE_TESTFAIL applies.
#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include "enc_io.h"
namespace blkio {
inline bool seek(FILE* f, long long at) {
#ifdef _WIN32
    return _fseeki64(f, at, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)at, SEEK_SET) == 0;
#endif
}
// a raw file of interleaved frames of C values of type T, read by frame ranges
template <class T, int C> struct Reader { FILE* f = nullptr; std::string path; long frames = 0; std::mutex mu;   // one reader may serve both channel threads
    explicit Reader(const char* p) : path(p) { f = encio::open(p, "rb"); long long n = encio::size(f);
        if (n < 0 || n % (long long)sizeof(T)) { fclose(f); f = nullptr; encio::fail(std::string("cannot read the size of ") + p); }
        const long long fr = n / (long long)sizeof(T) / C; if (fr > (long long)LONG_MAX) { fclose(f); f = nullptr; encio::fail("file too large for this encoder version: " + path); } frames = (long)fr; }
    ~Reader() { if (f) fclose(f); }
    Reader(const Reader&) = delete; Reader& operator=(const Reader&) = delete;
    // frames [a, b) (0 <= a <= b <= frames) as interleaved values
    void read(long a, long b, std::vector<T>& out) { out.resize((size_t)(b - a) * C); if (b <= a) return; std::lock_guard<std::mutex> g(mu);
        if (a < 0 || b > frames || !seek(f, (long long)a * C * (long long)sizeof(T))) encio::fail("seek failed: " + path);
        if (fread(out.data(), sizeof(T), out.size(), f) != out.size()) encio::fail("read failed: " + path); } };
// a file written in pieces, in order; the checks of encio::writeAll on every piece and at the end
struct Writer { FILE* f = nullptr; std::string path; bool ok = true;
    explicit Writer(const char* p) : path(p) { f = encio::open(p, "wb"); ok = !encio::testFail(p); }
    ~Writer() { if (f) { fclose(f); remove(path.c_str()); } }   // not finished: an error elsewhere; the half-written file goes
    Writer(const Writer&) = delete; Writer& operator=(const Writer&) = delete;
    template <class T> void put(const T* d, size_t n) { if (ok && n) ok = fwrite(d, sizeof(T), n, f) == n; if (!ok) finish(); }
    template <class T> void put(const std::vector<T>& v) { put(v.data(), v.size()); }
    void finish() { if (!f) return; ok = (fclose(f) == 0) && ok; f = nullptr; if (!ok) { remove(path.c_str()); encio::fail("write failed (disk full?): " + path); } } };
// smallest index range [lo, hi] that j <- mirror(j) maps [a, b] into for an array of n (j < 0: -j; j >= n: 2 (n - 1) - j), clamped to [0, n - 1]
inline void cover(long a, long b, long n, long& lo, long& hi) { lo = n; hi = -1;
    auto add = [&](long x) { x = std::min(std::max(x, 0L), n - 1); lo = std::min(lo, x); hi = std::max(hi, x); };
    if (a < 0) { long e = std::min(b, -1L); add(-e); add(-a); }
    if (b >= 0 && a <= n - 1) { add(std::max(a, 0L)); add(std::min(b, n - 1)); }
    if (b >= n) { long s = std::max(a, n); add(2 * (n - 1) - b); add(2 * (n - 1) - s); } }
// values of one array over the index window [base, base + d.size())
template <class T> struct Win { long base = 0; std::vector<T> d;
    T at(long j) const { const long k = j - base; if (k < 0 || k >= (long)d.size()) encio::fail("block-wise encoder: window index outside the block (internal error)"); return d[(size_t)k]; } };
// read-only view of a raw interleaved stereo int32 file (a master) as if it were the vector of its samples: s[i] is sample i (frame i / 2,
// channel i % 2); a window of frames around the last access is held in memory. data() gives the view itself, for code written for vectors.
struct View { std::string path; long long frames = 0; mutable FILE* f = nullptr; mutable long long wb = 0; mutable std::vector<int32_t> w;
    static const long long W = 1LL << 20;   // frames per window (8 MB)
    explicit View(const std::string& p) : path(p) { f = encio::open(p.c_str(), "rb"); long long n = encio::size(f);
        if (n < 0 || n % 8) { fclose(f); f = nullptr; encio::fail("cannot read the size of " + p); } frames = n / 8; }
    ~View() { if (f) fclose(f); }
    View(const View&) = delete; View& operator=(const View&) = delete;
    size_t size() const { return (size_t)frames * 2; }
    const View& data() const { return *this; }
    int32_t operator[](size_t i) const { const long long fr = (long long)(i / 2);
        if (fr < wb || fr >= wb + (long long)w.size() / 2) { if (fr >= frames) encio::fail("internal error: master sample beyond its end: " + path);
            wb = std::max(0LL, fr - W / 2); const long long e = std::min(frames, wb + W); w.resize((size_t)(e - wb) * 2);
            if (!seek(f, wb * 8) || fread(w.data(), 8, (size_t)(e - wb), f) != (size_t)(e - wb)) encio::fail("read failed: " + path); }
        return w[i - (size_t)wb * 2]; } };
}
