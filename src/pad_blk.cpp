// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise pad_mirror.cpp (2026-10-29): the same file, written piece by piece from pieces of the input.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "blk_io.h"
int main(int argc, char** argv) {
    blkio::Reader<int32_t, 2> in(argv[1]); const long n = in.frames, P = atol(argv[3]), tot = n + 2 * P, BS = 1L << 20;
    blkio::Writer out(argv[2]); std::vector<int32_t> x, y;
    for (long a = 0; a < tot; a += BS) { const long b = std::min(tot, a + BS);
        auto mir = [&](long i) { long j = i - P; if (j < 0) j = -j; if (j >= n) j = 2 * (n - 1) - j; return j; };
        long lo = n, hi = -1; for (long i : {a, b - 1, std::min(std::max(a, P), b - 1), std::min(std::max(a, P + n - 1), b - 1)}) { long j = mir(i); lo = std::min(lo, j); hi = std::max(hi, j); }
        if (a <= P && P < b) { lo = std::min(lo, 0L); } if (a <= P + n - 1 && P + n - 1 < b) hi = std::max(hi, n - 1);
        lo = std::max(lo, 0L); hi = std::min(hi, n - 1); in.read(lo, hi + 1, x); y.resize((size_t)(b - a) * 2);
        for (long i = a; i < b; i++) { long j = mir(i); if (j < lo || j > hi) encio::fail("block-wise padding: index outside the block (internal error)"); y[(size_t)(i - a) * 2] = x[(size_t)(j - lo) * 2]; y[(size_t)(i - a) * 2 + 1] = x[(size_t)(j - lo) * 2 + 1]; }
        out.put(y); }
    out.finish(); return 0; }
