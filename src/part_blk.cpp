// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// Block-wise part_coder.cpp (2026-10-29): the same range-coded stream and the same checks, one row of roots at a time. The tokens are read
// from the dump in order; the coded plane of each row goes to a temporary file; then the stream is decoded row by row and its tokens and
// plane are compared with the dump, the coded plane and the encoder's plane. Only the coded bytes (the stream itself) stay in memory.
// usage and environment as part_coder.cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <functional>
#include <type_traits>
#include <string>
#include <memory>
#include "part_stream.h"
#include "enc_io.h"
#include "blk_io.h"
using namespace std;
namespace {
// tokens of the dump, read in order through a window (token i = int32 number 11 + i of the file)
struct TokIn { blkio::Reader<int32_t, 1>* r = nullptr; size_t n = 0, wb = 0; vector<int32_t> w;
    int32_t operator[](size_t i) { if (i >= n) encio::fail("part coder: the stream asks for more tokens than the dump holds");
        if (i < wb || i >= wb + w.size()) { wb = i; const size_t e = std::min(n, i + ((size_t)1 << 20)); r->read((long)(11 + i), (long)(11 + e), w); } return w[i - wb]; }
    void push_back(int32_t) { encio::fail("internal error: token input written"); } };
// decoded tokens, compared with the dump as they come
struct TokCheck { TokIn* ref = nullptr; size_t cnt = 0; bool same = true;
    int32_t operator[](size_t) const { return 0; }
    void push_back(int32_t v) { if (cnt >= ref->n || (*ref)[cnt] != v) same = false; cnt++; } };
}
int main(int argc, char** argv) {
    for (int t = 0; t < 4; t++) { g_tierBytes[t] = 0; g_tierCoef[t] = 0; }
    blkio::Reader<int32_t, 1> dumpR(argv[1]); vector<int32_t> hd;
    if (dumpR.frames < 11) { fprintf(stderr, "not a partition dump\n"); return 2; }
    dumpR.read(0, 11, hd);
    if (hd[0] != 0x50415254 || hd[10] < 0 || dumpR.frames != 11 + (long)hd[10]) { fprintf(stderr, "not a partition dump\n"); return 2; }
    Hdr h{hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7], hd[8], hd[9]};
    TokIn tin; tin.r = &dumpR; tin.n = (size_t)hd[10];
    const std::string pePath = std::string(argv[1]) + ".pe";
    struct PeGone { std::string p; ~PeGone() { remove(p.c_str()); } } peGone{pePath};
    vector<int32_t> row;
    Enc e; { blkio::Writer pw(pePath.c_str()); Walker<Enc> w(h);
        while (!w.done()) { const long f0 = w.M0; row.assign((size_t)w.rowFrames() * h.N, 0); w.row(e, tin, true, row, f0); pw.put(row); }
        pw.finish(); }
    e.flush();
    TokIn tref; tref.r = &dumpR; tref.n = tin.n; TokCheck tchk; tchk.ref = &tref;
    std::unique_ptr<blkio::Reader<int32_t, 1>> encR; bool planeOk = true;
    if (argc > 2) { encR.reset(new blkio::Reader<int32_t, 1>(argv[2])); if ((long long)encR->frames != (long long)h.K * h.N) planeOk = false; }
    std::unique_ptr<blkio::Writer> outW; if (argc > 3) { outW.reset(new blkio::Writer(argv[3])); int32_t ph[5] = {0x504C4E45, (int32_t)h.K, h.k0, h.rate, (int32_t)h.L}; outW->put(ph, 5); }
    bool pdSame = true; { blkio::Reader<int32_t, 1> peR(pePath.c_str()); Dec dc; dc.p = e.out.data(); dc.n = e.out.size(); dc.init(); Walker<Dec> w(h); vector<int32_t> pe, en;
        while (!w.done()) { const long f0 = w.M0; const long rf = w.rowFrames(); row.assign((size_t)rf * h.N, 0); w.row(dc, tchk, false, row, f0);
            peR.read(f0 * h.N, (f0 + rf) * h.N, pe); if (pe != row) pdSame = false;
            if (encR && planeOk) { encR->read(f0 * h.N, (f0 + rf) * h.N, en); if (en != row) planeOk = false; }
            if (outW) outW->put(row); } }
    if (outW) outW->finish();
    bool ok = tchk.same && tchk.cnt == tin.n && pdSame;
    if (getenv("PARTBYTES")) {
        int32_t ph[10] = {(int32_t)h.K, h.N, h.k0, h.k1, h.rate, (int32_t)h.L, h.cw, h.lv, h.eq, (int32_t)e.out.size()};
        vector<uint8_t> buf((const uint8_t*)ph, (const uint8_t*)ph + sizeof ph); buf.insert(buf.end(), e.out.begin(), e.out.end()); encio::writeVec(getenv("PARTBYTES"), buf); }
    double secs = (double)h.L / h.rate;
    printf("partition mode %d (%s): %zu bytes = %.1f kb/s (mono) | symbols %s | decoded plane %s encoder plane\n", h.lv, h.eq ? "equal" : "unequal",
           e.out.size(), e.out.size() * 8.0 / secs / 1000, ok ? "BIT-EXACT" : "MISMATCH", planeOk ? "==" : "!=");
    if (getenv("TIERSTAT")) printf("tier stats: B %.0f B / %ld coef, C %.0f B / %ld coef, D %.0f B / %ld coef\n", g_tierBytes[1], g_tierCoef[1], g_tierBytes[2], g_tierCoef[2], g_tierBytes[3], g_tierCoef[3]);
    return ok && planeOk ? 0 : 1;
}
