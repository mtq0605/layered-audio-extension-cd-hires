// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE v5 decoder as a library (lossy tier: routes full, 16, cd24), STREAMING: memory does not grow with the track length.
// Used by the command-line decoder (lae_dec.cpp), the encoder's internal decode check and the player plugins.
// Deterministic pipeline (identical on every platform): range decoding one row of roots (8 frames) at a time -> dequantise
// -> textbook IMDCT frame by frame (mdct_tb.h, strict IEEE double, overlap-add in a 2N window) -> base in 1/16 LSB24 = 4096 x CD
// - regenerated TPDF dither + 16 x depth -> integer half-band up-conversion (Q31, 64-bit, one rounding per output) -> + 16 x frequency
// layer (time domain) -> optional integer half-band decimation to the chosen octave -> one final rounding -> hard limiter.
// Every stage is pulled sample by sample through a short sliding window, so memory does not depend on the track length.
// Callers must check mdct::selftestOK() once before decoding (it refuses builds that do not follow the IEEE-754 order).
#pragma once
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
#include "mdct_tb.h"
using namespace lae;
typedef int32_t i32; static const int N = mdct::N;

static const int32_t* tab(int s, int& half) { switch (s) { case 0: half = kHalfLen0; return kHalf0; case 1: half = kHalfLen1; return kHalf1; case 2: half = kHalfLen2; return kHalf2; default: half = kHalfLen3; return kHalf3; } }
static inline int64_t rq31(int64_t a) { return (a + ((int64_t)1 << 30)) >> 31; }
struct Rd { const uint8_t* p; size_t n, i = 0; int32_t i32v() { int32_t v = 0; if (i + 4 <= n) memcpy(&v, p + i, 4); i += 4; return v; } const uint8_t* take(size_t k) { const uint8_t* q = p + i; i += k; return q; } };

// ---- one range-coded stream, decoded one row of roots at a time; frame(m) returns the dequantised coefficients of frame m
struct PlaneStream {
    Hdr h; Dec dc; std::unique_ptr<Walker<Dec>> w; std::vector<int32_t> rowBuf, tokens; long row0 = -1; int rows = 0;
    // checkpoints for seeking: the decoder state before the row at frame M0, kept every `every` rows (about 4 s of the stream)
    struct Mark { long M0; Dec dc; std::unique_ptr<Walker<Dec>> w; };
    std::vector<Mark> marks; long every = 1;
    bool open(const uint8_t* p, int32_t nbytes) { if (nbytes < 40) return false; Rd s{p, (size_t)nbytes}; int32_t f[10]; for (int i = 0; i < 10; i++) f[i] = s.i32v();
        h = Hdr{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]}; if (h.N != N || f[9] != nbytes - 40 || h.K <= 0 || h.k0 < 0 || h.k1 > N || h.k0 >= h.k1) return false;
        dc.p = p + 40; dc.n = f[9]; dc.init(); w.reset(new Walker<Dec>(h)); rowBuf.assign((size_t)part::ROOTT * N, 0);
        every = h.rate > 0 ? std::max<long>(1, (long)(4.0 * h.rate / ((double)part::ROOTT * N))) : 64; return true; }
    const int32_t* frame(long m) {   // frames are requested in increasing order (seekTo excepted)
        while (!(m >= row0 && m < row0 + rows)) { if (w->done()) return nullptr;
            if ((w->M0 / part::ROOTT) % every == 0 && (marks.empty() || marks.back().M0 < w->M0)) marks.push_back(Mark{w->M0, dc, std::unique_ptr<Walker<Dec>>(new Walker<Dec>(*w))});
            row0 = w->M0; rows = w->rowFrames(); std::fill(rowBuf.begin(), rowBuf.end(), 0); tokens.clear(); w->row(dc, tokens, false, rowBuf, row0); }
        return &rowBuf[(size_t)(m - row0) * N]; }
    // the next frame(m') with m' >= m continues from the nearest checkpoint at or before m, or from the current state when that is closer
    void seekTo(long m) {
        const long cur = row0 >= 0 ? row0 : w->M0; const bool forward = cur <= m; const Mark* best = nullptr;
        for (const Mark& k : marks) { if (k.M0 > m) break; best = &k; }
        if (best && (!forward || best->M0 > cur)) { dc = best->dc; w.reset(new Walker<Dec>(*best->w)); row0 = -1; rows = 0; } } };
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
    i32 get(long i) { if (i >= L) return 0; while (outHead <= i) step(); return outRing[i & (outRing.size() - 1)]; }
    // seeking: restart at frame ms with an empty overlap buffer. Output from signal index ms*N on is then exact (each sample is the sum
    // of the two frames that cover it, both added after the restart); the first stepped frame only finalises samples below ms*N
    void resetAt(long ms) { nextFrame = ms; std::fill(y.begin(), y.end(), 0.0); outHead = std::max(0L, (ms - 1) * N); for (auto* ps : parts) ps->seekTo(ms); } };
// ---- pulled signal stages with a cached sliding window (all values in 1/16 LSB24)
struct Src { long n = 0; virtual int64_t get(long i) = 0; virtual ~Src() {} };
struct Cached : Src { std::vector<int64_t> ring; long lo = 0, hi = 0;   // computed [lo, hi)
    Cached() : ring(1 << 14) {}
    virtual int64_t compute(long i) = 0;
    int64_t get(long i) override { if (i < lo) {
#ifdef LAE_LIBRARY
            g_streamErr = true; return 0;   // a library must never end its host process: reported as a stream error
#else
            fprintf(stderr, "stream window underrun\n"); exit(5);
#endif
        }
        while (hi <= i) { ring[hi & (ring.size() - 1)] = compute(hi); hi++; if (hi - lo > (long)ring.size()) lo = hi - (long)ring.size(); }
        return ring[i & (ring.size() - 1)]; }
    static int64_t mirror(Src& in, long i) { long n = in.n; if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; if (i < 0) i = 0; if (i > n - 1) i = n - 1; return in.get(i); }
    void resetAt(long s) { lo = hi = s; } };   // seeking: the next value computed is index s (compute(i) depends only on the inputs at fixed offsets of i)
// xorshift32 (13, 17, 5) advanced by `steps` steps in O(log steps): the step is linear over GF(2), so it is applied as a 32 x 32 bit matrix power
static inline uint32_t xsApply(const uint32_t* col, uint32_t v) { uint32_t r = 0; for (int j = 0; j < 32; j++) if ((v >> j) & 1u) r ^= col[j]; return r; }
static inline uint32_t xsJump(uint32_t s, uint64_t steps) {
    uint32_t M[32], M2[32]; for (int j = 0; j < 32; j++) { uint32_t v = 1u << j; v ^= v << 13; v ^= v >> 17; v ^= v << 5; M[j] = v; }
    while (steps) { if (steps & 1) s = xsApply(M, s); for (int j = 0; j < 32; j++) M2[j] = xsApply(M, M[j]); memcpy(M, M2, sizeof M); steps >>= 1; }
    return s; }
struct BaseSrc : Cached {   // 4096 x CD - dither + 16 x depth, base rate, padded length nb
    const std::vector<int16_t>* cd; const int16_t* edge; long pb, ncd; int ch; const uint8_t* dfl; long nbk; ImdctStream* depth; uint32_t st[2] = {0x12345678u, 0x12345678u + 0x9E3779B9u};
    int16_t sample(long i) { return i < pb ? edge[i * 2 + ch] : (i < pb + ncd ? (*cd)[(i - pb) * 2 + ch] : edge[2 * pb + (i - pb - ncd) * 2 + ch]); }
    int64_t compute(long i) override { int64_t d16 = 0;
        for (int cc = 0; cc < 2; cc++) { uint32_t& s = st[cc]; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u1 = (s >> 20) & 0xFFF; s ^= s << 13; s ^= s >> 17; s ^= s << 5; int u2 = (s >> 20) & 0xFFF;
            if (cc == ch && nbk && dfl[cc * nbk + i / 4096]) d16 = u1 + u2 - 4096; }
        return 4096LL * sample(i) - d16 + (depth ? 16LL * depth->get(i) : 0); }
    void ditherAt(long i) { st[0] = xsJump(0x12345678u, 2ULL * (uint64_t)i); st[1] = xsJump(0x12345678u + 0x9E3779B9u, 2ULL * (uint64_t)i); } };   // the state after i samples (2 steps per sample)
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

// ---- one track: open (all checks, in the order of the command-line decoder), then read the output frames in order
struct Decoder {
    std::vector<int16_t> cdOwn; std::vector<uint8_t> laeOwn;   // used by open(): the CD layer (interleaved L/R) and the "LAE " chunk body (with its CRC)
    std::string mode; int rate = 0, base = 0, stages = 0, P = 0, fc = 0, mi = 0, eq = 0, wm = 0, nseg = 0, nused = 0, d = 0, wrate = 0;
    long Lm = 0, nb = 0, pb = 0, ncd = 0, Lp = 0, Po = 0, Lo = 0, pos = 0, clip = 0;
    PlaneStream dS[2]; std::vector<PlaneStream> fS; ImdctStream dI[2], mI, sI; bool haveF = false;
    std::vector<std::unique_ptr<Src>> keep; Src* outSrc[2] = {nullptr, nullptr};
    template <class T, class... A> T* add(A&&... a) { keep.emplace_back(std::make_unique<T>(std::forward<A>(a)...)); return static_cast<T*>(keep.back().get()); }   // owned before anything else can throw
    BaseSrc* baseSrc[2] = {nullptr, nullptr}; MixSrc* mix[2] = {nullptr, nullptr}; std::vector<UpSrc*> ups[2]; std::vector<DecSrc*> decs[2];   // the output chain (seek)
    // returns "" when ready; otherwise the message and *code (1 = not this format / bad arguments, 3 = corrupt: the CD audio still plays)
    std::string open(std::vector<int16_t>&& cdLayer, std::vector<uint8_t>&& chunk, const std::string& route, int outRate, int* code) {
        cdOwn.swap(cdLayer); laeOwn.swap(chunk); return openRef(cdOwn, laeOwn, route, outRate, code); }
    // as open(), but the caller keeps cd and lae alive and unchanged while this decoder is used
    std::string openRef(const std::vector<int16_t>& cd, const std::vector<uint8_t>& lae, const std::string& route, int outRate, int* code) {
        mode = route; auto fail = [&](int c, const char* m) { if (code) *code = c; return std::string(m); };
        if (mode != "full" && mode != "16" && mode != "cd24") return fail(1, "route must be full, 16 or cd24");
        if (lae.empty()) return fail(1, "no LAE chunk: plain CD audio");
        if (lae.size() < 8) return fail(3, "corrupt LAE chunk");
        // the chunk's first 4 bytes name the format: "LAE1" = LAEY (lossy, this decoder), "LAE2" = LAES (lossless, decoded by laes)
        if (!memcmp(lae.data(), "LAE2", 4)) return fail(1, "this is an LAES file (lossless, extension LAE2): decode it with laes; this decoder reads LAEY (extension LAE1)");
        { uint32_t stored; memcpy(&stored, lae.data() + lae.size() - 4, 4); if (crc32(lae.data(), lae.size() - 4) != stored) return fail(3, "LAE chunk damaged (CRC mismatch): refusing to decode; the CD audio itself still plays"); }
        Rd r{lae.data(), lae.size() - 4};
        if (r.i32v() != 0x3145414C || r.i32v() != 5) return fail(1, "unknown LAE chunk version (this decoder reads v5)");
        rate = r.i32v(); base = r.i32v(); stages = r.i32v(); Lm = r.i32v(); P = r.i32v(); fc = r.i32v(); mi = r.i32v(); eq = r.i32v(); wm = r.i32v(); nb = r.i32v(); pb = r.i32v();
        // header consistency: every field is checked before it is used to address anything
        bool hdrOk = (base == 44100 || base == 48000) && stages >= 1 && stages <= 3 && rate == (base << stages) && P > 0 && P <= 65536 && P % (1 << stages) == 0
                     && pb == (P >> stages) && Lm > 0 && Lm <= 2000000000L - 2L * P && nb == ((Lm + 2L * P) >> stages) && mi >= 0 && mi < part::NMODES;
        if (!hdrOk || 8 * (size_t)pb > r.n - r.i) return fail(3, "corrupt LAE header: refusing to decode");
        const int16_t* edge = (const int16_t*)r.take(8 * (size_t)pb); int32_t ndf = r.i32v();
        if (ndf != 2 * ((nb + 4095) / 4096) || r.i > r.n || (size_t)ndf > r.n - r.i) return fail(3, "corrupt dither flags: refusing to decode");
        const uint8_t* dfl = r.take((size_t)ndf);
        ncd = (long)cd.size() / 2; if (ncd != nb - 2 * pb || r.i > r.n || mi < 0 || mi >= part::NMODES || stages < 1 || stages > 3) return fail(3, "CD audio does not match the LAE chunk (edited or misaligned): refusing to decode");
        int orate = outRate ? outRate : rate; if (orate <= 0) return fail(1, "output rate must be a positive number");
        d = 0; while (d <= stages && (rate >> d) > orate) d++;
        if (mode != "cd24" && ((rate >> d) != orate || d >= stages)) { if (code) *code = 1; return "output rate must be " + std::to_string(rate) + " / 2^n and above " + std::to_string(base); }
        // streams: depth L, depth R, then n segments for M, n for S
        auto take = [&](PlaneStream* ps, bool use) { int32_t nbytes = r.i32v(); size_t start = r.i; if (nbytes < 40 || start + nbytes > r.n) return false;
            if (use && !ps->open(r.p + start, nbytes)) return false; r.i = start + nbytes; return true; };
        for (int c = 0; c < 2; c++) if (!take(&dS[c], mode != "16")) return fail(3, "corrupt depth stream");
        nseg = r.i32v(); if (nseg < 1 || nseg > 8) return fail(3, "corrupt frequency layer");
        const int kcut = (int)lround((double)N * (orate / 2.0) / (rate / 2.0)); nused = 0;
        fS.clear(); fS.resize(2 * (size_t)nseg); std::vector<bool> used(2 * nseg, false);
        for (int c = 0; c < 2; c++) for (int sg = 0; sg < nseg; sg++) { int k0s = 0; if (r.i + 4 + 12 <= r.n) memcpy(&k0s, r.p + r.i + 4 + 8, 4);   // header field 3 = start bin
            bool use = mode != "cd24" && k0s < kcut;
            used[c * nseg + sg] = use; if (use && c == 0) nused++;
            if (!take(&fS[c * nseg + sg], use)) return fail(3, "corrupt frequency stream"); }
        Lp = Lm + 2 * P;
        for (int c = 0; c < 2; c++) { dI[c].init(nb); if (mode != "16") dI[c].parts.push_back(&dS[c]); }
        mI.init(Lp); sI.init(Lp); for (int sg = 0; sg < nseg; sg++) { if (used[sg]) mI.parts.push_back(&fS[sg]); if (used[nseg + sg]) sI.parts.push_back(&fS[nseg + sg]); }
        haveF = !mI.parts.empty();
        for (int c = 0; c < 2; c++) {
            auto* b = add<BaseSrc>(); b->n = nb; b->cd = &cd; b->edge = edge; b->pb = pb; b->ncd = ncd; b->ch = c; b->dfl = dfl; b->nbk = ndf / 2; b->depth = mode != "16" ? &dI[c] : nullptr; baseSrc[c] = b;
            if (mode == "cd24") { outSrc[c] = b; continue; }
            Src* s = b; for (int k = 0; k < stages; k++) { auto* u = add<UpSrc>(s, k); ups[c].push_back(u); s = u; }
            auto* mx = add<MixSrc>(); mx->n = Lp; mx->up = s; mx->mt = haveF ? &mI : nullptr; mx->st = haveF ? &sI : nullptr; mx->ch = c; mix[c] = mx; s = mx;
            for (int k = 0; k < d; k++) { auto* dd = add<DecSrc>(s, stages - 1 - k); decs[c].push_back(dd); s = dd; }
            outSrc[c] = s; }
        if (mode == "cd24") { Po = pb; Lo = nb - 2 * pb; wrate = base; } else { Po = P >> d; Lo = Lm >> d; wrate = orate; }
        pos = 0; clip = 0; if (code) *code = 0; return ""; }
    // the entropy checkpoints of another decoder of the same chunk (another route): a stream's checkpoints depend only on its bytes, so
    // they are valid here and a route change seeks as fast as the old decoder would have (output unchanged: a seek from a checkpoint
    // gives the samples of a decode from the start)
    void adoptMarks(const Decoder& o) {
        auto copy = [](PlaneStream& to, const PlaneStream& from) { if (!to.w || !from.w || !to.marks.empty()) return;
            for (const PlaneStream::Mark& m : from.marks) to.marks.push_back(PlaneStream::Mark{m.M0, m.dc, std::unique_ptr<Walker<Dec>>(new Walker<Dec>(to.h, *m.w))}); };
        for (int c = 0; c < 2; c++) copy(dS[c], o.dS[c]);
        for (size_t k = 0; k < fS.size() && k < o.fS.size(); k++) copy(fS[k], o.fS[k]); }
    // the read position summed over the streams this route decodes (bytes; it moves back when a seek restores a checkpoint, and two
    // calls with only reads in between differ by the bytes those reads consumed: the extension layers' share of the bitrate)
    uint64_t streamBytes() const { uint64_t n = 0; for (int c = 0; c < 2; c++) for (const PlaneStream* p : dI[c].parts) n += p->dc.i;
        for (const PlaneStream* p : mI.parts) n += p->dc.i; for (const PlaneStream* p : sI.parts) n += p->dc.i; return n; }
    // the next frames of the output (24-bit values, interleaved L/R, after the hard limiter); returns the number of frames written.
    // clipPos (optional, encoder's limiter map): positions of limited samples per channel, in master-rate frames of the padded signal.
    long read(int32_t* out, long frames, std::vector<int32_t>* clipPos = nullptr) {
        long n = std::min(frames, Lo - pos);
        for (long k = 0; k < n; k++, pos++) for (int c = 0; c < 2; c++) { int64_t v = (outSrc[c]->get(Po + pos) + 8) >> 4;
            if (v > 8388607 || v < -8388608) { if (clipPos) clipPos[c].push_back((int32_t)(pos + P)); clip++; v = v > 0 ? 8388607 : -8388608; }   // hard limiter
            out[k * 2 + c] = (int32_t)v; }
        return n; }
    // to output frame T, bit-exact (the samples read() gives when it reaches T from the start). The entropy streams continue from their
    // last checkpoint at or before the frames needed, or decode forward to them; every signal stage restarts a little before the first
    // index it must produce (each stage's value at index i depends only on its inputs at fixed offsets of i; the dither generator is
    // advanced to the index; each IMDCT sample is the sum of the two frames that cover it). Returns false near the start of the track,
    // where the caller simply restarts (just as fast).
    bool seek(long T) {
        if (T < 0) T = 0; if (T > Lo) T = Lo;
        const long margin = 4096; long s = Po + T - margin; if (s < 8 * margin) return false;
        std::vector<long> sDec(decs[0].size()), sUp(ups[0].size()); long sMix = 0, sBase;
        if (mode == "cd24") sBase = s;
        else { for (int k = (int)decs[0].size() - 1; k >= 0; k--) { sDec[(size_t)k] = s; s = 2 * s - decs[0][(size_t)k]->half; }
            sMix = s;
            for (int k = (int)ups[0].size() - 1; k >= 0; k--) { sUp[(size_t)k] = s; s = (s - ups[0][(size_t)k]->half) / 2; }
            sBase = s; }
        if (sBase < margin) return false;
        for (int c = 0; c < 2; c++) { baseSrc[c]->resetAt(sBase); baseSrc[c]->ditherAt(sBase);
            for (size_t k = 0; k < ups[c].size(); k++) ups[c][k]->resetAt(sUp[k]);
            if (mix[c]) mix[c]->resetAt(sMix);
            for (size_t k = 0; k < decs[c].size(); k++) decs[c][k]->resetAt(sDec[k]); }
        if (mode != "16") for (int c = 0; c < 2; c++) dI[c].resetAt(sBase / N);
        if (mode != "cd24" && haveF) { mI.resetAt(sMix / N); sI.resetAt(sMix / N); }
        pos = T; return true; }
    // the frequency layer alone at the base rate (encoder's leak compensation): writes int32 L/R pairs; returns base-rate frames
    long writeLeak(FILE* fl) {
        Src* lk[2]; for (int c = 0; c < 2; c++) { auto* fs = add<FreqSrc>(); fs->n = Lp; fs->mt = &mI; fs->st = &sI; fs->ch = c; Src* s = fs;
            for (int k = 0; k < stages; k++) { auto* dd = add<DecSrc>(s, stages - 1 - k); s = dd; } lk[c] = s; }
        long nlk = lk[0]->n; for (long i = 0; i < nlk; i++) for (int c = 0; c < 2; c++) { int32_t v = (int32_t)lk[c]->get(i); fwrite(&v, 4, 1, fl); }
        return nlk; }
};
