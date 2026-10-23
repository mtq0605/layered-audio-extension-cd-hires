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
#include "enc_io.h"
using namespace std;

int main(int argc, char** argv) {
    vector<int32_t> dump = encio::readAll<int32_t>(argv[1]);   // magic, K, N, k0, k1, rate, L, cw, mode, equal, tokens, then the tokens
    if (dump.size() < 11 || dump[0] != 0x50415254 || dump[10] < 0 || dump.size() != 11 + (size_t)dump[10]) { fprintf(stderr, "not a partition dump\n"); return 2; }
    const int32_t* hd = dump.data();
    Hdr h{hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7], hd[8], hd[9]}; vector<int32_t> s(dump.begin() + 11, dump.end());
    vector<int32_t> pe((size_t)h.K * h.N, 0), pd((size_t)h.K * h.N, 0);
    Enc e; run(e, h, s, true, pe); e.flush();
    vector<int32_t> s2; Dec dc; dc.p = e.out.data(); dc.n = e.out.size(); dc.init(); run(dc, h, s2, false, pd);
    bool ok = s2 == s && pd == pe;
    bool planeOk = true;
    if (argc > 2) { vector<int32_t> enc = encio::readAll<int32_t>(argv[2]); planeOk = enc == pd; }
    if (argc > 3) { int32_t ph[5] = {0x504C4E45, (int32_t)h.K, h.k0, h.rate, (int32_t)h.L}; vector<int32_t> buf(ph, ph + 5); buf.insert(buf.end(), pd.begin(), pd.end()); encio::writeVec(argv[3], buf); }
    if (getenv("PARTBYTES")) {   // container payload: the 9 header fields + byte count + range-coded bytes
        int32_t ph[10] = {(int32_t)h.K, h.N, h.k0, h.k1, h.rate, (int32_t)h.L, h.cw, h.lv, h.eq, (int32_t)e.out.size()};
        vector<uint8_t> buf((const uint8_t*)ph, (const uint8_t*)ph + sizeof ph); buf.insert(buf.end(), e.out.begin(), e.out.end()); encio::writeVec(getenv("PARTBYTES"), buf); }
    double secs = (double)h.L / h.rate;
    printf("partition mode %d (%s): %zu bytes = %.1f kb/s (mono) | symbols %s | decoded plane %s encoder plane\n", h.lv, h.eq ? "equal" : "unequal",
           e.out.size(), e.out.size() * 8.0 / secs / 1000, ok ? "BIT-EXACT" : "MISMATCH", planeOk ? "==" : "!=");
    if (getenv("TIERSTAT")) printf("tier stats: B %.0f B / %ld coef, C %.0f B / %ld coef, D %.0f B / %ld coef\n", g_tierBytes[1], g_tierCoef[1], g_tierBytes[2], g_tierCoef[2], g_tierBytes[3], g_tierCoef[3]);
    return ok && planeOk ? 0 : 1;
}
