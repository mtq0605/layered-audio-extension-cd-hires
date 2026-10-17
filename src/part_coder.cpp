// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Range coding of a partitioned layer (dump written by lossy_v2 PART=<level> DUMP=...).
// Encodes the cut tree and the leaves, decodes them again, rebuilds the coefficient plane exactly as a decoder would,
// and compares it with the encoder's plane. Reports actual bytes.
// usage: part_coder <dump> [encoder plane to compare] [decoded plane out]
// Tree symbols: split / direction / cut position as plain bits. Leaves: tier (2-bit tree, context = previous leaf's tier),
// exponent delta to the previous coded leaf, zero-level index (3-bit tree, context = tier), coefficients (bit-length tree +
// mantissa, context = tier x exponent bucket: side information only).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <type_traits>
#include "part_stream.h"
using namespace std;

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); int32_t hd[11]; fread(hd, 4, 11, f);   // magic, K, N, k0, k1, rate, L, cw, mode, equal, tokens
    if (hd[0] != 0x50415254) { fprintf(stderr, "not a partition dump\n"); return 2; }
    Hdr h{hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7], hd[8], hd[9]}; vector<int32_t> s(hd[10]); fread(s.data(), 4, s.size(), f); fclose(f);
    vector<int32_t> pe((size_t)h.K * h.N, 0), pd((size_t)h.K * h.N, 0);
    Enc e; run(e, h, s, true, pe); e.flush();
    vector<int32_t> s2; Dec dc; dc.p = e.out.data(); dc.n = e.out.size(); dc.init(); run(dc, h, s2, false, pd);
    bool ok = s2 == s && pd == pe;
    bool planeOk = true;
    if (argc > 2) { FILE* g = fopen(argv[2], "rb"); vector<int32_t> enc((size_t)h.K * h.N); fread(enc.data(), 4, enc.size(), g); fclose(g); planeOk = enc == pd; }
    if (argc > 3) { FILE* g = fopen(argv[3], "wb"); int32_t ph[5] = {0x504C4E45, (int32_t)h.K, h.k0, h.rate, (int32_t)h.L}; fwrite(ph, 4, 5, g); fwrite(pd.data(), 4, pd.size(), g); fclose(g); }
    if (getenv("PARTBYTES")) {   // container payload: the 9 header fields + byte count + range-coded bytes
        FILE* g = fopen(getenv("PARTBYTES"), "wb"); int32_t ph[10] = {(int32_t)h.K, h.N, h.k0, h.k1, h.rate, (int32_t)h.L, h.cw, h.lv, h.eq, (int32_t)e.out.size()};
        fwrite(ph, 4, 10, g); fwrite(e.out.data(), 1, e.out.size(), g); fclose(g); }
    double secs = (double)h.L / h.rate;
    printf("partition mode %d (%s): %zu bytes = %.1f kb/s (mono) | symbols %s | decoded plane %s encoder plane\n", h.lv, h.eq ? "equal" : "unequal",
           e.out.size(), e.out.size() * 8.0 / secs / 1000, ok ? "BIT-EXACT" : "MISMATCH", planeOk ? "==" : "!=");
    if (getenv("TIERSTAT")) printf("tier stats: B %.0f B / %ld coef, C %.0f B / %ld coef, D %.0f B / %ld coef\n", g_tierBytes[1], g_tierCoef[1], g_tierBytes[2], g_tierCoef[2], g_tierBytes[3], g_tierCoef[3]);
    return ok && planeOk ? 0 : 1;
}
