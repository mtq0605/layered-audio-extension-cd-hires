// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE standalone decoder (lossy tier: routes full, 16, cd24), STREAMING: memory does not grow with the track length.
// Reads a WAV written by lae_enc: the audio is the CD layer, the "LAE " chunk holds the depth layer (L, R) and the frequency layer
// (M, S, one stream per octave).
// usage: lae_dec <in.wav> <out.wav> [full|16|cd24] [output rate]   full = 24-bit route, 16 = CD + frequency layer, cd24 = CD + depth (base rate);
//        output rate: master rate (default) or any octave below it; frequency segments above that octave are skipped, not decoded
// Deterministic pipeline (identical on every platform): range decoding one row of roots (8 frames) at a time -> dequantise
// -> textbook IMDCT frame by frame (mdct_tb.h, strict IEEE double, overlap-add in a 2N window) -> base in 1/16 LSB24 = 4096 x CD
// - regenerated TPDF dither + 16 x depth -> integer half-band up-conversion (Q31, 64-bit, one rounding per output) -> + 16 x frequency
// layer (time domain) -> optional integer half-band decimation to the chosen octave -> one final rounding -> hard limiter.
// Every stage is pulled sample by sample through a short sliding window, so memory does not depend on the track length.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>
#include <memory>
#include "halfband.h"
#include "part_stream.h"
#include "wavio.h"
#include "mdct_tb.h"
using namespace lae;
typedef int32_t i32; static const int N = mdct::N;
// Exports used only by the encoder (leak compensation, limiter map): active only when the encoder compiles this decoder in.
#ifdef LAE_ENCODER_HOOKS
static const char* hookPath(const char* name) { return getenv(name); }
#else
static const char* hookPath(const char*) { return nullptr; }
#endif

static const int32_t* tab(int s, int& half) { switch (s) { case 0: half = kHalfLen0; return kHalf0; case 1: half = kHalfLen1; return kHalf1; case 2: half = kHalfLen2; return kHalf2; default: half = kHalfLen3; return kHalf3; } }
static inline int64_t rq31(int64_t a) { return (a + ((int64_t)1 << 30)) >> 31; }
struct Rd { const uint8_t* p; size_t n, i = 0; int32_t i32v() { int32_t v = 0; if (i + 4 <= n) memcpy(&v, p + i, 4); i += 4; return v; } const uint8_t* take(size_t k) { const uint8_t* q = p + i; i += k; return q; } };

// ---- one range-coded stream, decoded one row of roots at a time; frame(m) returns the dequantised coefficients of frame m
struct PlaneStream {
    Hdr h; Dec dc; std::unique_ptr<Walker<Dec>> w; std::vector<int32_t> rowBuf, tokens; long row0 = -1; int rows = 0;
    bool open(const uint8_t* p, int32_t nbytes) { if (nbytes < 40) return false; Rd s{p, (size_t)nbytes}; int32_t f[10]; for (int i = 0; i < 10; i++) f[i] = s.i32v();
        h = Hdr{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]}; if (h.N != N || f[9] != nbytes - 40 || h.K <= 0 || h.k0 < 0 || h.k1 > N || h.k0 >= h.k1) return false;
        dc.p = p + 40; dc.n = f[9]; dc.init(); w.reset(new Walker<Dec>(h)); rowBuf.assign((size_t)part::ROOTT * N, 0); return true; }
    const int32_t* frame(long m) {   // frames are requested in increasing order
        while (!(m >= row0 && m < row0 + rows)) { if (w->done()) return nullptr; row0 = w->M0; rows = w->rowFrames(); std::fill(rowBuf.begin(), rowBuf.end(), 0); tokens.clear(); w->row(dc, tokens, false, rowBuf, row0); }
        return &rowBuf[(size_t)(m - row0) * N]; } };
// ---- IMDCT of a sum of planes (octave segments are disjoint), sample by sample: out(i) = llround(y[N + i]) as mdct::inverse
struct ImdctStream {
    std::vector<PlaneStream*> parts; long L = 0, K = 0, nextFrame = 0; std::vector<double> y; std::vector<i32> outRing; long outHead = 0;   // signal samples [0, outHead) produced
    void init(long len) { L = len; K = mdct::frames(L); y.assign(4 * N, 0.0); outRing.assign(1 << 15, 0); }
    void step() {   // add frame m; padded samples [mN, mN + N) become final
        long m = nextFrame++; double c[N], u[N]; for (int k = 0; k < N; k++) c[k] = 0;
        bool have = false; for (auto* ps : parts) { const int32_t* f = ps->frame(m); if (!f) continue; have = true; for (int k = 0; k < N; k++) c[k] += f[k]; }
        long s = m * N; double* Y = y.data(); auto at = [&](long p) -> double& { return Y[p % (4 * N)]; };
        if (have && m < K) { mdct::dct4(c, u);
            for (int n = 0; n < N / 2; n++) { at(s + n) += u[N / 2 + n] * mdct::WIN[n]; at(s + N / 2 + n) += -u[N - 1 - n] * mdct::WIN[N / 2 + n];
                at(s + N + n) += -u[N / 2 - 1 - n] * mdct::WIN[N + n]; at(s + 3 * N / 2 + n) += -u[n] * mdct::WIN[3 * N / 2 + n]; } }
        for (long p = s; p < s + N; p++) { double v = at(p); at(p) = 0.0; long i = p - N; if (i >= 0 && i < L) { if (v > 1073741823.0 || v < -1073741823.0) { g_streamErr = true; v = v > 0 ? 1073741823.0 : -1073741823.0; }   // beyond any valid signal: corrupt
                outRing[outHead & (outRing.size() - 1)] = (i32)std::llround(v); outHead++; } } }
    i32 get(long i) { if (i >= L) return 0; while (outHead <= i) step(); return outRing[i & (outRing.size() - 1)]; } };
// ---- pulled signal stages with a cached sliding window (all values in 1/16 LSB24)
struct Src { long n = 0; virtual int64_t get(long i) = 0; virtual ~Src() {} };
struct Cached : Src { std::vector<int64_t> ring; long lo = 0, hi = 0;   // computed [lo, hi)
    Cached() : ring(1 << 14) {}
    virtual int64_t compute(long i) = 0;
    int64_t get(long i) override { if (i < lo) { fprintf(stderr, "stream window underrun\n"); exit(5); }
        while (hi <= i) { ring[hi & (ring.size() - 1)] = compute(hi); hi++; if (hi - lo > (long)ring.size()) lo = hi - (long)ring.size(); }
        return ring[i & (ring.size() - 1)]; }
    static int64_t mirror(Src& in, long i) { long n = in.n; if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; if (i < 0) i = 0; if (i > n - 1) i = n - 1; return in.get(i); } };
struct BaseSrc : Cached {   // 4096 x CD - dither + 16 x depth, base rate, padded length nb
    const std::vector<int16_t>* cd; const int16_t* edge; long pb, ncd; int ch; const uint8_t* dfl; long nbk; ImdctStream* depth; uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u};
    int16_t sample(long i) { return i < pb ? edge[i * 2 + ch] : (i < pb + ncd ? (*cd)[(i - pb) * 2 + ch] : edge[2 * pb + (i - pb - ncd) * 2 + ch]); }
    int64_t compute(long i) override { int64_t d16 = 0;
        for (int cc = 0; cc < 2; cc++) { uint32_t& s = st[cc]; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
            if (cc == ch && nbk && dfl[cc * nbk + i / 4096]) d16 = u1 + u2 - 4096; }
        return 4096LL * sample(i) - d16 + (depth ? 16LL * depth->get(i) : 0); } };
struct UpSrc : Cached { Src* in; const int32_t* h; int half;
    UpSrc(Src* x, int s) : in(x) { h = tab(s, half); n = 2 * x->n; }
    int64_t compute(long o) override { if ((o & 1) == 0) return in->get(o / 2); int64_t a = 0;
        for (int j = 1, t = 0; j <= half; j += 2, t++) a += (int64_t)h[t] * (mirror(*in, (o - j) / 2) + mirror(*in, (o + j) / 2));
        return rq31(2 * a); } };
struct MixSrc : Cached { Src* up; ImdctStream *mt, *st; int ch;   // up(base) truncated / zero-padded to Lp, + 16 x frequency layer (L/R from M/S)
    int64_t compute(long i) override { int64_t u = i < up->n ? up->get(i) : 0; if (!mt) return u; int64_t m = mt->get(i), s = st->get(i); int64_t r = m - (s >> 1); return u + 16 * (ch ? r : r + s); } };   // 64-bit: no overflow on any input
struct FreqSrc : Cached { ImdctStream *mt, *st; int ch;   // frequency layer alone (L/R), 1/16 LSB24 (leak compensation, LEAKOUT)
    int64_t compute(long i) override { int64_t m = mt->get(i), s = st->get(i), r = m - (s >> 1); return 16 * (ch ? r : r + s); } };
struct DecSrc : Cached { Src* in; const int32_t* h; int half;
    DecSrc(Src* x, int s) : in(x) { h = tab(s, half); n = x->n / 2; }
    int64_t compute(long i) override { long c = 2 * i; int64_t a = in->get(c) * ((int64_t)1 << 30);
        for (int j = 1, t = 0; j <= half; j += 2, t++) a += (int64_t)h[t] * (mirror(*in, c - j) + mirror(*in, c + j));
        return rq31(a); } };

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: lae_dec <in.wav> <out.wav> [full|16|cd24] [output rate]\n"); return 1; }
    std::string mode = argc > 3 ? argv[3] : "full";
    if (!mdct::selftestOK()) { printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the format requires (see docs/BUILD.md); refusing to decode\n"); return 4; }
    wavio::Wav w; if (!wavio::read(argv[1], w) || w.bits != 16 || w.ch != 2) { printf("not a 16-bit stereo WAV\n"); return 1; }
    if (w.lae.empty()) { printf("no LAE chunk: plain CD audio\n"); return 1; }
    if (w.lae.size() < 8) { printf("corrupt LAE chunk\n"); return 3; }
    { uint32_t stored; memcpy(&stored, w.lae.data() + w.lae.size() - 4, 4); if (crc32(w.lae.data(), w.lae.size() - 4) != stored) { printf("LAE chunk damaged (CRC mismatch): refusing to decode; the CD audio itself still plays\n"); return 3; } }
    Rd r{w.lae.data(), w.lae.size() - 4};
    if (r.i32v() != 0x3145414C || r.i32v() != 5) { printf("unknown LAE chunk version (this decoder reads v5)\n"); return 1; }
    int rate = r.i32v(), base = r.i32v(), stages = r.i32v(); long Lm = r.i32v(); int P = r.i32v(); int fc = r.i32v(), mi = r.i32v(), eq = r.i32v(), wm = r.i32v(); long nb = r.i32v(), pb = r.i32v();
    // header consistency: every field is checked before it is used to address anything
    bool hdrOk = (base == 44100 || base == 48000) && stages >= 1 && stages <= 3 && rate == (base << stages) && P > 0 && P <= 65536 && P % (1 << stages) == 0
                 && pb == (P >> stages) && Lm > 0 && Lm <= 2000000000L - 2L * P && nb == ((Lm + 2L * P) >> stages) && mi >= 0 && mi < part::NMODES;
    if (!hdrOk || 8 * (size_t)pb > r.n - r.i) { printf("corrupt LAE header: refusing to decode\n"); return 3; }
    const int16_t* edge = (const int16_t*)r.take(8 * (size_t)pb); int32_t ndf = r.i32v();
    if (ndf != 2 * ((nb + 4095) / 4096) || r.i > r.n || (size_t)ndf > r.n - r.i) { printf("corrupt dither flags: refusing to decode\n"); return 3; }
    const uint8_t* dfl = r.take((size_t)ndf);
    long ncd = (long)w.pcm16.size() / 2; if (ncd != nb - 2 * pb || r.i > r.n || mi < 0 || mi >= part::NMODES || stages < 1 || stages > 3) { printf("CD audio does not match the LAE chunk (edited or misaligned): refusing to decode\n"); return 3; }
    int orate = argc > 4 ? atoi(argv[4]) : rate; if (orate <= 0) { printf("output rate must be a positive number\n"); return 1; }
    int d = 0; while (d <= stages && (rate >> d) > orate) d++;
    if (mode != "cd24" && ((rate >> d) != orate || d >= stages)) { printf("output rate must be %d / 2^n and above %d\n", rate, base); return 1; }
    // streams: depth L, depth R, then n segments for M, n for S
    auto take = [&](PlaneStream* ps, bool use) { int32_t nbytes = r.i32v(); size_t start = r.i; if (nbytes < 40 || start + nbytes > r.n) return false;
        if (use && !ps->open(r.p + start, nbytes)) return false; r.i = start + nbytes; return true; };
    PlaneStream dS[2]; for (int c = 0; c < 2; c++) if (!take(&dS[c], mode != "16")) { printf("corrupt depth stream\n"); return 3; }
    int nseg = r.i32v(); if (nseg < 1 || nseg > 8) { printf("corrupt frequency layer\n"); return 3; }
    const int kcut = (int)lround((double)N * (orate / 2.0) / (rate / 2.0)); int nused = 0;
    std::vector<PlaneStream> fS(2 * nseg); std::vector<bool> used(2 * nseg, false);
    for (int c = 0; c < 2; c++) for (int sg = 0; sg < nseg; sg++) { int k0s = 0; if (r.i + 4 + 12 <= r.n) memcpy(&k0s, r.p + r.i + 4 + 8, 4);   // header field 3 = start bin
        bool use = mode != "cd24" && k0s < kcut;
        used[c * nseg + sg] = use; if (use && c == 0) nused++;
        if (!take(&fS[c * nseg + sg], use)) { printf("corrupt frequency stream\n"); return 3; } }
    long Lp = Lm + 2 * P;
    ImdctStream dI[2], mI, sI; for (int c = 0; c < 2; c++) { dI[c].init(nb); if (mode != "16") dI[c].parts.push_back(&dS[c]); }
    mI.init(Lp); sI.init(Lp); for (int sg = 0; sg < nseg; sg++) { if (used[sg]) mI.parts.push_back(&fS[sg]); if (used[nseg + sg]) sI.parts.push_back(&fS[nseg + sg]); }
    bool haveF = !mI.parts.empty();
    std::vector<std::unique_ptr<Src>> keep; Src* outSrc[2]; long Po, Lo; int wrate;
    if (hookPath("LEAKOUT") && haveF) {   // leak compensation (encoder only): the decoded frequency layer brought down to the base rate (int32, 1/16 LSB24, L/R interleaved)
        Src* lk[2]; for (int c = 0; c < 2; c++) { auto* fs = new FreqSrc(); fs->n = Lp; fs->mt = &mI; fs->st = &sI; fs->ch = c; keep.emplace_back(fs); Src* s = fs;
            for (int k = 0; k < stages; k++) { auto* dd = new DecSrc(s, stages - 1 - k); keep.emplace_back(dd); s = dd; } lk[c] = s; }
        FILE* fl = fopen(hookPath("LEAKOUT"), "wb"); if (!fl) return 1; long nlk = lk[0]->n;
        for (long i = 0; i < nlk; i++) for (int c = 0; c < 2; c++) { int32_t v = (int32_t)lk[c]->get(i); fwrite(&v, 4, 1, fl); }
        fclose(fl); printf("leak written: %ld base-rate samples/ch\n", nlk); return 0; }
    for (int c = 0; c < 2; c++) {
        auto* b = new BaseSrc(); b->n = nb; b->cd = &w.pcm16; b->edge = edge; b->pb = pb; b->ncd = ncd; b->ch = c; b->dfl = dfl; b->nbk = ndf / 2; b->depth = mode != "16" ? &dI[c] : nullptr; keep.emplace_back(b);
        if (mode == "cd24") { outSrc[c] = b; continue; }
        Src* s = b; for (int k = 0; k < stages; k++) { auto* u = new UpSrc(s, k); keep.emplace_back(u); s = u; }
        auto* mx = new MixSrc(); mx->n = Lp; mx->up = s; mx->mt = haveF ? &mI : nullptr; mx->st = haveF ? &sI : nullptr; mx->ch = c; keep.emplace_back(mx); s = mx;
        for (int k = 0; k < d; k++) { auto* dd = new DecSrc(s, stages - 1 - k); keep.emplace_back(dd); s = dd; }
        outSrc[c] = s; }
    if (mode == "cd24") { Po = pb; Lo = nb - 2 * pb; wrate = base; } else { Po = P >> d; Lo = Lm >> d; wrate = orate; }
    if (!wavio::fits32((size_t)Lo * 6, 0)) { printf("output would exceed the 4 GB limit of WAV: choose a lower output rate\n"); return 1; }
    FILE* fo = fopen(argv[2], "wb"); if (!fo) { printf("cannot write %s\n", argv[2]); return 1; }
    { std::vector<uint8_t> hdr; wavio::hdr(hdr, wrate, 2, 24, (size_t)Lo * 6, 0); fwrite(hdr.data(), 1, hdr.size(), fo); }
    bool wantMap = hookPath("CLIPMAP") && d == 0 && mode != "cd24";
    std::vector<uint8_t> buf; buf.reserve(1 << 16); long clip = 0; std::vector<int32_t> clipPos[2];
    for (long i = 0; i < Lo; i++) {
        for (int c = 0; c < 2; c++) { int64_t v = (outSrc[c]->get(Po + i) + 8) >> 4;
            if (v > 8388607 || v < -8388608) { if (wantMap) clipPos[c].push_back((int32_t)(i + P)); clip++; v = v > 0 ? 8388607 : -8388608; }   // hard limiter
            buf.push_back((uint8_t)v); buf.push_back((uint8_t)(v >> 8)); buf.push_back((uint8_t)(v >> 16)); }
        if (buf.size() >= 60000) { fwrite(buf.data(), 1, buf.size(), fo); buf.clear(); } }
    fwrite(buf.data(), 1, buf.size(), fo); if ((Lo * 6) & 1) fputc(0, fo); fclose(fo);
    if (wantMap) { FILE* fm = fopen(hookPath("CLIPMAP"), "wb"); if (fm) { for (int c = 0; c < 2; c++) fwrite(clipPos[c].data(), 4, clipPos[c].size(), fm); fclose(fm); } }   // channel 0 first, as before
    if (g_streamErr) { remove(argv[2]); printf("corrupt stream (values out of range): refusing to decode\n"); return 3; }
    printf("LAE decode OK: %s -> %s | %s, %d Hz 24 bit, %ld samples/ch | mode %s (share by %s) %s, CD cut-off %d Hz | frequency segments used %d of %d | hard-limited samples %ld\n",
           argv[1], argv[2], mode == "16" ? "16-bit route" : (mode == "cd24" ? "CD + depth" : "24-bit route"), wrate, Lo, part::MODES[mi].name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), eq ? "equal" : "unequal", fc, nused, nseg, clip);
    return 0; }
