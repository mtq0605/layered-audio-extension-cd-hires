// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Checked file I/O for the encoder modules: 64-bit file sizes, every read and write verified.
// A failure throws encio::Fail; the encoder driver (lae_enc.cpp) catches it, writes nothing and removes its temporary folder.
// Test hook: LAE_TESTFAIL=<text> makes every write whose path contains <text> fail as if the disk were full.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <climits>
#include <stdexcept>
#include <string>
#include <vector>
namespace encio {
struct Fail : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] inline void fail(const std::string& m) { throw Fail(m); }
inline bool testFail(const char* what) { const char* t = getenv("LAE_TESTFAIL"); return t && *t && what && strstr(what, t); }
inline long long size(FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return -1; long long n = _ftelli64(f); if (_fseeki64(f, 0, SEEK_SET)) return -1;
#else
    if (fseeko(f, 0, SEEK_END)) return -1; long long n = (long long)ftello(f); if (fseeko(f, 0, SEEK_SET)) return -1;
#endif
    return n; }
inline FILE* open(const char* p, const char* mode) { FILE* f = p ? fopen(p, mode) : nullptr; if (!f) fail(std::string("cannot open ") + (p ? p : "(no path)")); return f; }
// whole file as elements of T; the file size must be a multiple of sizeof(T)
template <class T> std::vector<T> readAll(const char* p) {
    FILE* f = open(p, "rb"); long long n = size(f);
    if (n < 0 || n % (long long)sizeof(T)) { fclose(f); fail(std::string("cannot read the size of ") + p); }
    std::vector<T> v; try { v.resize((size_t)(n / (long long)sizeof(T))); } catch (...) { fclose(f); throw; }   // never leave the file open: Windows could not delete it
    if (!v.empty() && fread(v.data(), sizeof(T), v.size(), f) != v.size()) { fclose(f); fail(std::string("read failed: ") + p); }
    fclose(f); return v; }
inline void writeAll(const char* p, const void* d, size_t elem, size_t n) {
    FILE* f = open(p, "wb"); bool ok = !testFail(p) && (n == 0 || fwrite(d, elem, n, f) == n); ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(p); fail(std::string("write failed (disk full?): ") + p); } }
template <class T> void writeVec(const char* p, const std::vector<T>& v) { writeAll(p, v.data(), sizeof(T), v.size()); }
// a coefficient plane held for bins [k0, k0 + bw) only (q[m * bw + k]), written as the full K x N plane with zeros outside, frame by frame
inline void writeBandPlane(const char* p, const int32_t* q, long K, int N, int k0, int bw) {
    FILE* f = open(p, "wb"); bool ok = !testFail(p); std::vector<int32_t> row((size_t)N, 0);
    for (long m = 0; ok && m < K; m++) { if (bw > 0) memcpy(&row[(size_t)k0], q + (size_t)m * bw, (size_t)bw * 4); ok = fwrite(row.data(), 4, (size_t)N, f) == (size_t)N; }
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(p); fail(std::string("write failed (disk full?): ") + p); } }
inline void writeText(const char* p, const std::string& s) { writeAll(p, s.data(), 1, s.size()); }
inline std::string readText(const char* p) { std::vector<char> v = readAll<char>(p); return std::string(v.begin(), v.end()); }
// element count as the modules' index type; refuses counts the index type cannot hold
inline long count(size_t n) { if (n > (size_t)LONG_MAX) fail("file too large for this encoder version"); return (long)n; }
}
