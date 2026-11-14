// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAES playback decoder (2026-11-01): one choice of an LAES file (the core, a master, a rate level of a master, or a derived 16-bit
// version) decoded in time order, piece by piece, for players. The values are those of laes::decodeLaes / laess::decodeLaesS and of
// `laes dec`: the same lifting, reference and layer code (laes_stream.h) applied to pieces that are computed when they are first asked for.
//   - Only the masters the choice needs are decoded: the chosen one and the tiers it refers to (its chain).
//   - Signals are nodes that compute values [a, b) on request and keep a window of recent values; requests go forward in time, so a
//     layer's bit stream is read once from its start (no index or entry table is read or made; LAES rules T-1, T-2).
//   - The file is read through a callback (no whole-file buffer). open() reads it once from start to end: the SHA-256 of the whole file
//     (a player can remember a verified file by it), the payload CRC-32, the core (its SHA-256, and the STREAMINFO MD5 of the FLAC form).
//   - A master is presented as that master only when its SHA-256 matches (spec 1.3): the SHA-256 of every master of the chain is computed
//     while the choice is decoded and is known after the last sample (verified()). A player shows a choice only after a verified run.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <exception>
#include <condition_variable>
#include <deque>
#include <map>
#include <atomic>
#include <algorithm>
#include "laes_stream.h"
#include "laes_flac.h"
#include "laes_choices.h"
namespace laesp {
#ifdef LAESP_PROFILE   // exclusive time per node kind (tests only)
#include <chrono>
struct Prof { double t[6] = {0}; std::atomic<int64_t> computed{0}, behind{0}, jumps{0}; static Prof& g() { static Prof p; return p; } };
struct ProfScope { int k; double* parent; std::chrono::steady_clock::time_point s; double child = 0; static thread_local ProfScope* cur;
    explicit ProfScope(int kind) : k(kind), parent(nullptr), s(std::chrono::steady_clock::now()) { prev = cur; cur = this; }
    ~ProfScope() { const double all = std::chrono::duration<double>(std::chrono::steady_clock::now() - s).count(); Prof::g().t[k] += all - child; cur = prev; if (prev) prev->child += all; }
    ProfScope* prev; };
thread_local ProfScope* ProfScope::cur = nullptr;
#define LAESP_PROF(k) ProfScope prof_scope_(k)
#else
#define LAESP_PROF(k)
#endif
using laes::V; using laess::fail;
// ---- byte sources
struct ByteSrc { virtual ~ByteSrc() {} virtual uint64_t size() const = 0; virtual void read(uint64_t at, void* d, size_t n) = 0; };   // read throws laess::Fail
// a file given by a read callback: fn(offset, buffer, bytes) returns 0 on success
struct FnSrc : ByteSrc { std::function<int(uint64_t, void*, size_t)> fn; uint64_t n = 0; std::mutex mx;   // reads from both channel threads, one at a time
    uint64_t size() const override { return n; }
    void read(uint64_t at, void* d, size_t k) override { if (!k) return; std::lock_guard<std::mutex> g(mx); if (at > n || k > n - at || fn(at, d, k) != 0) fail("read failed"); } };
// the concatenation of byte ranges of another source (the payload: one range in the WAV form, the APPLICATION pieces in the FLAC form)
struct SegSrc : ByteSrc { ByteSrc* base = nullptr; std::vector<uint64_t> off, len, start; uint64_t total = 0;
    void add(uint64_t o, uint64_t l) { if (!l) return; off.push_back(o); len.push_back(l); start.push_back(total); total += l; }
    uint64_t size() const override { return total; }
    void read(uint64_t at, void* d, size_t k) override { if (at > total || k > total - at) fail("read failed: payload range");
        uint8_t* p = (uint8_t*)d; size_t s = (size_t)(std::upper_bound(start.begin(), start.end(), at) - start.begin()) - 1;
        while (k) { const uint64_t in = at - start[s], t = std::min<uint64_t>(k, len[s] - in); base->read(off[s] + in, p, (size_t)t); p += t; at += t; k -= (size_t)t; if (k) s++; } } };
// the bit reader of laes_codec.h (MSB first) over a byte range of a source, read through a window of 1 MiB; a read of up to 57 bits
// takes the 8 bytes around the position at once. Bit positions are 64-bit (a layer may exceed 2^32 bits on a 32-bit build).
struct BitRS { ByteSrc* src = nullptr; uint64_t off = 0, len = 0, pos = 0; bool fail = false; std::vector<uint8_t> w; uint64_t wb = 0, we = 0;
    uint64_t left() const { return len * 8 - pos; }
    void fill(uint64_t byte) {   // the window from byte on (at most 1 MiB, at most to the end)
        wb = byte; we = std::min<uint64_t>(len, byte + (1u << 20)); w.resize((size_t)(we - wb)); if (we > wb) src->read(off + wb, w.data(), w.size()); }
    uint64_t get(int n) { if (fail || n < 0 || n > 64 || (uint64_t)n > left()) { fail = true; return 0; } if (n == 0) return 0;
        if (n > 57) { const uint64_t hi = get(n - 32); return (hi << 32) | get(32); }
        const uint64_t v = peek(n); pos += (uint64_t)n; return v; }
    // the next n bits (1 <= n <= 57, n <= left()) without consuming them
    uint64_t peek(int n) { const uint64_t byte = pos >> 3; const uint64_t need = std::min<uint64_t>(8, len - byte);
        if (w.empty() || byte < wb || byte + need > we) fill(byte);
        const uint8_t* p = w.data() + (size_t)(byte - wb); uint64_t v = 0; for (uint64_t i = 0; i < 8; i++) v = (v << 8) | (i < need ? p[i] : 0);
        v <<= (pos & 7); return v >> (64 - n); }
    // the zeros before the next 1, which is consumed too (laes::unaryOf); fails past qmax or at the end of the data
    uint64_t unary(uint64_t qmax) { uint64_t q = 0;
        for (;;) { if (fail) return q; const uint64_t l = left(); if (l == 0) { fail = true; return q; }
            const int n = (int)std::min<uint64_t>(57, l); const uint64_t v = peek(n);
            if (v == 0) { pos += (uint64_t)n; q += (uint64_t)n; if (q > qmax) { fail = true; return q; } continue; }
            int z = 0; while (!((v >> (n - 1 - z)) & 1)) z++;
            q += (uint64_t)z; pos += (uint64_t)z + 1; if (q > qmax) fail = true; return q; } }
    int64_t getS(int n) { if (n < 1 || n > 63) { fail = true; return 0; } uint64_t v = get(n); if (v >> (n - 1) & 1) v |= ~0ull << n; return (int64_t)v; }
    void reset() { pos = 0; fail = false; } };
// ---- nodes: signals computed on request, keeping the values [c0, c0 + size) of recent requests
struct Node : laess::Sig { V cache; int64_t c0 = 0, keep = 1 << 17, chunk = 1 << 17; int threads = 1;
    virtual void compute(int64_t a, int64_t b, V& out) = 0;   // values [a, b), a pure function of the inputs (layers: see LayerNode)
    void get(int64_t a, int64_t b, int64_t* out) override {
        if (a < 0 || b > n || a > b) fail("internal error: signal range"); if (a == b) return;
        const int64_t c1 = c0 + (int64_t)cache.size();
#ifdef LAESP_PROFILE
        if (a < c0) Prof::g().behind += b - a; if (a > c0 + (int64_t)cache.size()) Prof::g().jumps++;
#endif
        if (a < c0) { V t; compute(a, b, t); std::copy(t.begin(), t.end(), out); return; }   // behind the window: computed again, not kept
        if (a > c1) { cache.clear(); c0 = a; }   // a jump forward: the window starts again
        const int64_t e1 = c0 + (int64_t)cache.size();
        // no read-ahead (2026-11-12): exactly what is asked. A master that refers to another asks its tier for its own range plus the filter's
        // reach (128 samples and T); any rounding up ("+ a chunk", or "to the next multiple of chunk") grows level by level along a reference
        // chain (255 masters each referring to the one before: the first masters ran far ahead of their own SHA-256 pass, which then found its
        // samples dropped and recomputed them, each recomputation asking its tier behind its window too; a self-check of 255 x 5 min ran for
        // hours). Requests come in pieces of 2^16 (output, SHA-256) or a tier's range, so the pieces stay large. Values unchanged.
        if (b > e1) { const int64_t e = b; V t; compute(e1, e, t); if (cache.empty()) cache.swap(t); else cache.insert(cache.end(), t.begin(), t.end());
#ifdef LAESP_PROFILE
            Prof::g().computed += e - e1;
#endif
        }
        std::copy(cache.begin() + (a - c0), cache.begin() + (b - c0), out);
        const int64_t drop = a - keep - c0; if (drop > chunk) { cache.erase(cache.begin(), cache.begin() + drop); c0 += drop; } } };
// the core (one channel), held in memory as int32
struct CoreNode : laess::Sig { const std::vector<int32_t>* v = nullptr;
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n || a > b) fail("internal error: signal range"); for (int64_t i = a; i < b; i++) out[i - a] = (*v)[(size_t)i]; } };
// where each block of a layer begins (bit position), noted while the layer is decoded in this run of the player, so that a later start
// at any frame can begin at the block that holds it. Memory only: the file holds no index (rules T-1, T-2) and nothing is saved.
struct LayerIdx { std::mutex m; std::vector<uint64_t> bit;   // bit[k]: position of block k (known for k < size)
    // every kSparse-th position, loaded from the player's own record (File::importIndex): sp[i] = position of block i * kSparse, for the
    // spEnd blocks of the layer
    static const size_t kSparse = 16; std::vector<uint64_t> sp; size_t spEnd = 0;
    void note(size_t k, uint64_t p) { std::lock_guard<std::mutex> g(m); if (k == bit.size()) bit.push_back(p); }
    // the last known block at or before k (block 0 at bit 0 is always known)
    size_t atMost(size_t k, uint64_t& p) { std::lock_guard<std::mutex> g(m); size_t best = 0; p = 0;
        if (!bit.empty()) { best = std::min(k, bit.size() - 1); p = bit[best]; }
        if (!sp.empty()) { const size_t j = std::min(k / kSparse, sp.size() - 1); if (j * kSparse > best || bit.empty()) { best = j * kSparse; p = sp[j]; } }
        return best; }
    size_t known() { std::lock_guard<std::mutex> g(m); return std::max(bit.size(), spEnd); } };
// a layer (n values in blocks of B) read from its byte range. The blocks are decoded in order by a thread of the node's own, at most
// `ahead` values beyond the latest request, so that the layers of all masters and channels decode at the same time (each layer is
// sequential in itself); the values are those of decodeBlock in order. A request far ahead or behind starts the thread again at the last
// block whose position is noted (block 0 when none is).
struct LayerNode : Node { BitRS rd; int64_t B = 4096; uint64_t restarts = 0; int64_t ahead = 1 << 19; std::shared_ptr<LayerIdx> ix;
    ~LayerNode() { halt(); }
    void compute(int64_t a, int64_t b, V& out) override { LAESP_PROF(0); out.resize((size_t)(b - a));
        if (!started) start(a);
        else { int64_t h0, hE; { std::lock_guard<std::mutex> g0(m); h0 = q0; hE = qEnd; }   // read under the lock: the thread moves them (FU14-4)
          if (a < h0) { halt(); restarts++; start(a); }   // behind the kept blocks
          else if (a > hE + ahead && ix) { uint64_t p; if ((int64_t)ix->atMost((size_t)(a / B), p) * B > hE) { halt(); start(a); } } }   // far ahead: jump
        std::unique_lock<std::mutex> g(m); if (b > want) { want = b; cv.notify_all(); }
        cv.wait(g, [&] { return err || qEnd >= b; }); if (err) std::rethrow_exception(err);
        int64_t at = q0; size_t qi = 0; for (int64_t i = a; i < b; qi++) { const V& blk = q[qi]; const int64_t e = at + (int64_t)blk.size();
            if (i < e) { const int64_t f = std::min(b, e); std::copy(blk.begin() + (i - at), blk.begin() + (f - at), out.begin() + (i - a)); i = f; } at = e; }
        while (!q.empty() && q0 + (int64_t)q.front().size() <= a) { q0 += (int64_t)q.front().size(); q.pop_front(); }   // blocks wholly before this request
        cv.notify_all(); }
private:
    std::thread th; std::mutex m; std::condition_variable cv; std::deque<V> q; int64_t q0 = 0, qEnd = 0, want = 0, skip = 0; bool stop = false, started = false; std::exception_ptr err;   // skip: blocks wholly before it are not kept (FU14-4)
    void start(int64_t a) { uint64_t p = 0; const size_t k = ix ? ix->atMost((size_t)(a / B), p) : 0;
        rd.reset(); rd.pos = k ? p : 0; q.clear(); q0 = qEnd = (int64_t)(k ? k : 0) * B; want = q0; skip = a; stop = false; err = nullptr; started = true;
        const int64_t s0 = q0; th = std::thread([this, s0] { produce(s0); }); }
    void halt() { { std::lock_guard<std::mutex> g(m); stop = true; cv.notify_all(); } if (th.joinable()) th.join(); }
    void produce(int64_t s) {
        for (; s < n;) {
            { std::unique_lock<std::mutex> g(m); cv.wait(g, [&] { return stop || qEnd - want < ahead; }); if (stop) return; }
            if (ix) ix->note((size_t)(s / B), rd.pos);
            const int64_t k = (int64_t)std::min<int64_t>(B, n - s); V blk((size_t)k); bool ok = false;
            try { ok = laes::decodeBlock(rd, blk.data(), k); } catch (...) { std::lock_guard<std::mutex> g(m); err = std::current_exception(); cv.notify_all(); return; }
            std::lock_guard<std::mutex> g(m); if (!ok) { err = std::make_exception_ptr(laess::Fail("LAES layer damaged")); cv.notify_all(); return; }
            if (q.empty() && s + k <= skip) { q0 += k; qEnd += k; s += k; cv.notify_all(); continue; }   // wholly before the request that started the thread: not kept (FU14-4)
            q.push_back(std::move(blk)); qEnd += k; s += k; cv.notify_all(); } } };
// level j of a master stored as a difference: layer + the reference of its tier (laess::refPiece)
struct AddRefNode : Node { Node* e = nullptr; laess::Sig* src = nullptr; std::vector<int64_t> h; int64_t wj = 0; int up = 0;
    int dc = -1000;   // -1000: not looked at yet; -999: h is a filter; else h is 2^31 at one tap: the reference is src[i + wj + dc] scaled by 2^up
    V tmp;            // the tier's samples for a whole-sample reference (kept between calls: no new array each time)
    // the layer is read straight from its node's decoded blocks (e->compute: the same values as e->get; this node is the layer's only reader,
    // so the layer node keeps no copy of its own)
    void compute(int64_t a, int64_t b, V& out) override { e->compute(a, b, out); LAESP_PROF(1);
        if (dc == -1000) { int c = -1, nz = 0; for (int t = 0; t < (int)h.size(); t++) if (h[(size_t)t]) { nz++; c = t; } dc = (nz == 1 && h[(size_t)c] == (1LL << 31)) ? c - 127 : -999; }
        // a whole-sample offset (2026-11-12): the same values as laess::refPiece's own shortcut (src[i + wj + c - 127] << up, or rounded down
        // by 2^-up), added straight into out, without the +-127 window and the separate result array; at the ends (mirrored indices) and for
        // a fractional offset, refPiece as before
        const int64_t s0 = a + wj + dc, s1 = b + wj + dc;
        if (dc != -999 && s0 >= 0 && s1 <= src->n) { tmp.resize((size_t)(b - a)); src->get(s0, s1, tmp.data()); const size_t k = out.size();
            if (up >= 0) { for (size_t i = 0; i < k; i++) out[i] += (int64_t)((uint64_t)tmp[i] << up); }
            else { const int sh = -up; const int64_t rnd = 1LL << (sh - 1); for (size_t i = 0; i < k; i++) out[i] += (tmp[i] + rnd) >> sh; }
            return; }
        V r; laess::refPiece(*src, h, wj, up, a, b, r, threads); for (size_t i = 0; i < out.size(); i++) out[i] += r[i]; } };
// one inverse stage: x (n values) from s (lower level) and d (octave layer)
struct UpNode : Node { laess::Sig* s = nullptr; laess::Sig* d = nullptr;
    void compute(int64_t a, int64_t b, V& out) override { LAESP_PROF(2); const int64_t pa = a / 2, pb = std::min(s->n, (b + 1) / 2); V x; laess::invPiece(*s, *d, n, pa, pb, x, threads);
        out.assign(x.begin() + (a - 2 * pa), x.begin() + (b - 2 * pa)); } };
// one forward stage: the lower level s of x
struct DownNode : Node { laess::Sig* x = nullptr;
    void compute(int64_t a, int64_t b, V& out) override { LAESP_PROF(3); V d; laess::fwdPiece(*x, a, b, out, d, threads); } };

// ---- an opened LAES file
struct Master { laes::MasterInfo I; int j = 0; uint64_t layOff[2][4] = {{0}}, layLen[2][4] = {{0}}; };   // layers per channel: [0] level j, [k - j] octave k
struct Choice { int kind = 0; int m = -1, level = 0, from = -1; int rate = 0, bits = 0; int64_t frames = 0; std::string name, what; };   // kind 0 core, 1 master, 2 rate level, 3 derived 16-bit
struct File {
    bool flac = false; int baseRate = 0, coreBits = 0; int64_t coreFrames = 0; std::vector<int32_t> core[2];
    uint8_t fileSha[32] = {0}; std::string why;            // why: "" when the payload passed every check; else the reason (the core still plays)
    std::vector<Master> ms; std::vector<Choice> choices; std::vector<std::pair<std::string, std::string>> tags;
    laes::Head head;        // the header's default master and core master flags (payload version 2; 2026-11-12)
    int defaultChoice = 0;  // the choice a player plays when the user has chosen nothing (spec/lossless/SPEC.md, Section 10.3): the default master, else the core master (0)
    std::mutex idxM; std::map<uint64_t, std::shared_ptr<LayerIdx>> idx;
    SegSrc payload; bool quick = false;
    // block positions of the layers (by the layer's byte offset in the payload), noted while layers are decoded (memory only)
    std::shared_ptr<LayerIdx> layerIndex(uint64_t off) { std::lock_guard<std::mutex> g(idxM); auto& p = idx[off]; if (!p) p = std::make_shared<LayerIdx>(); return p; }
    // decodes every layer once, entropy decoding only (seconds), noting where each block begins, so that later starts at any frame can
    // begin near it; layers run on up to `threads` threads; *cancel stops it early
    void indexLayers(const std::atomic<bool>* cancel, int threads);
    // the player's own record of block positions (never part of the file): every 16th block start of every layer, keyed by the file's
    // SHA-256 and checked by CRC-32. exportIndex is empty until every layer is noted in full; importIndex refuses a record that does not
    // match this file (SHA-256, layers, rising positions inside each layer, CRC-32).
    std::vector<uint8_t> exportIndex();
    bool importIndex(const uint8_t* d, size_t n);
    struct LayerRef { uint64_t off, len; int64_t n; int64_t B; };
    std::vector<LayerRef> layerList() const;
    // opens src: reads it once from start to end. Returns "" when the file is an LAES file (then why tells whether the payload is usable),
    // "not LAES" when it has no LAE2 payload (a plain WAV / FLAC, or an LAEY file: magic in laeMagic), or the reason it cannot be read.
    // quickInfo: only the structure is read (for a player's track info): no CRC-32, no core, no hashes; such a File cannot be played.
    std::string open(ByteSrc& src, char laeMagic[5], bool quickInfo = false);
private:
    std::string parsePayload();
    void makeChoices();
};

// SHA-256 on a thread of its own, fed in order (at most 4 buffers waiting), so that hashing overlaps with decoding
struct Hasher { laes::Sha256 sha; std::thread t; std::mutex m; std::condition_variable cv; std::deque<std::vector<uint8_t>> q; bool end = false;
    void push(std::vector<uint8_t>&& b) { std::unique_lock<std::mutex> g(m); if (!t.joinable()) t = std::thread([this] { loop(); });
        cv.wait(g, [this] { return q.size() < 4; }); q.push_back(std::move(b)); cv.notify_all(); }
    void final(uint8_t out[32]) { { std::lock_guard<std::mutex> g(m); end = true; cv.notify_all(); } if (t.joinable()) t.join(); sha.final(out); }
    ~Hasher() { { std::lock_guard<std::mutex> g(m); end = true; q.clear(); cv.notify_all(); } if (t.joinable()) t.join(); }
private:
    void loop() { for (;;) { std::vector<uint8_t> b; { std::unique_lock<std::mutex> g(m); cv.wait(g, [this] { return end || !q.empty(); }); if (q.empty()) return; b = std::move(q.front()); q.pop_front(); cv.notify_all(); }
            sha.update(b.data(), b.size()); } } };
// ---- decoding one choice in time order
struct Player {
    File* f = nullptr; Choice C; int threads = 1;
    int64_t pos = 0; bool done = false, ok = false, noHash = false;
    std::string err;
    // the nodes: per decoded master, per channel; tiers' lower levels are shared
    struct MD { int idx = -1; std::vector<std::unique_ptr<Node>> own; Node* top[2] = {nullptr, nullptr}; std::vector<Node*> lev[2]; Hasher sha; int64_t hashed = 0; bool shaOk = false; };
    std::vector<std::unique_ptr<MD>> chain; CoreNode coreSig[2];
    uint64_t rng[2] = {20261029ull + 0x9E3779B97F4A7C15ull * 1, 20261029ull + 0x9E3779B97F4A7C15ull * 2};
    // the derived 16-bit dither's xorshift64 state after `steps` steps (2 per sample, SPEC 10.1), in O(log steps): the step is linear over
    // GF(2), so it is applied as a 64 x 64 bit matrix raised to the power `steps` (2026-11-12; the same method as LAEY's xsJump). The state
    // depends only on the channel and the sample's index, never on the audio, so a derived version starts at any frame bit for bit as from 0.
    static uint64_t xs64Apply(const uint64_t M[64], uint64_t s) { uint64_t r = 0; for (int j = 0; j < 64; j++) if ((s >> j) & 1) r ^= M[j]; return r; }
    static uint64_t xs64Jump(uint64_t s, uint64_t steps) {
        uint64_t M[64], M2[64]; for (int j = 0; j < 64; j++) { uint64_t v = 1ull << j; v ^= v << 13; v ^= v >> 7; v ^= v << 17; M[j] = v; }
        while (steps) { if (steps & 1) s = xs64Apply(M, s); for (int j = 0; j < 64; j++) M2[j] = xs64Apply(M, M[j]); memcpy(M, M2, sizeof M); steps >>= 1; }
        return s; }
    // allMasters: every master of the file is decoded and checked in this run (one pass), not only the ones the choice needs
    // startFrame > 0: output begins at that frame (the layers begin at their noted block positions, see LayerIdx); no SHA-256 is computed,
    // so this is only for a master already verified for this file content. A derived 16-bit version starts there too: its dither state is
    // moved to that frame (xs64Jump), the output is the same bit for bit as when decoded from frame 0 (2026-11-12).
    std::string start(File& file, int choice, int nThreads, bool allMasters = false, int64_t startFrame = 0);
    // the next frames (interleaved L R, int32 in the choice's word length); returns the number written (0 at the end)
    int64_t read(int32_t* out, int64_t frames);
    // after the last sample: every master of the chain matched its SHA-256 (the core: always true)
    bool verified() const { return done && ok; }
    // after the last sample: master m (index in the file) matched its SHA-256 (1), did not (0), or is not part of this run (-1)
    int masterOk(int m) const { if (!done) return -1; for (auto& d : chain) if (d->idx == m) return d->shaOk ? 1 : 0; return -1; }
private:
    MD* need(int m);
    Node* levelOf(MD& D, int c, int k);
    laess::Sig* tierLevel(int tier, int c, int k);
    void advanceHashes(double frac);
};

// ======== implementation
inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// CRC-32 (IEEE 802.3, reflected, as in the LAES payload) by slicing-by-8: eight bytes per step through eight tables; the same value as the
// bytewise table (laes_selftest / the plug-in tests compare with laes::decodeLaes). Portable: phones without CRC instructions run it too.
inline const uint32_t (*crcTables())[256] { static uint32_t t[8][256]; static std::once_flag once;
    std::call_once(once, [] { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[0][i] = c; }
        for (int k = 1; k < 8; k++) for (uint32_t i = 0; i < 256; i++) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF]; }); return t; }
inline const uint32_t* crcTable() { return crcTables()[0]; }
inline uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) { const uint32_t (*T)[256] = crcTables();
    for (; n >= 8; n -= 8, p += 8) { const uint32_t a = crc ^ (p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24), b = p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        crc = T[7][a & 0xFF] ^ T[6][(a >> 8) & 0xFF] ^ T[5][(a >> 16) & 0xFF] ^ T[4][a >> 24] ^ T[3][b & 0xFF] ^ T[2][(b >> 8) & 0xFF] ^ T[1][(b >> 16) & 0xFF] ^ T[0][b >> 24]; }
    for (; n; n--, p++) crc = T[0][(crc ^ *p) & 0xFF] ^ (crc >> 8); return crc; }
// the core master is held in memory, two channels of int32 (8 bytes per frame): a frame count this program cannot hold is refused before
// any array is sized by it. A 64-bit program holds every count a file can state (FLAC STREAMINFO: below 2^36; WAV: the file's bytes / 4);
// a 32-bit program holds fewer than 2^29 frames. Without this a 32-bit program cut a STREAMINFO count of 2^32 or more to 32 bits for the
// array and went on comparing write positions with the 64-bit count (2026-11-14).
inline bool coreFramesFit(uint64_t frames) { return frames <= (uint64_t)(SIZE_MAX / 8); }
inline std::string File::open(ByteSrc& src, char laeMagic[5], bool quickInfo) {
    laeMagic[0] = 0; quick = quickInfo; crcTable(); { std::lock_guard<std::mutex> g(idxM); idx.clear(); } const uint64_t n = src.size(); if (n < 12) return "not LAES";
    // WAV: RIFF, or RF64 / BW64 (EBU Tech 3306, ITU-R BS.2088) for files beyond 4 GB: their 64-bit sizes are in the first chunk, "ds64"
    uint8_t h[12]; src.read(0, h, 12); flac = !memcmp(h, "fLaC", 4); const bool rf64 = (!memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4);
    const bool wav = (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) || rf64; if (!flac && !wav) return "not LAES";
    payload = SegSrc(); payload.base = &src; tags.clear();
    uint64_t audioOff = 0, audioLen = 0; laesflac::Info fi; int fmtTag = 0, fmtCh = 0;
    if (flac) {   // metadata: STREAMINFO, the "LAE2" APPLICATION pieces, VORBIS_COMMENT tags
        uint64_t p = 4; bool last = false, haveSi = false; bool lae1 = false;
        while (!last) { if (p + 4 > n) return "FLAC metadata truncated"; uint8_t bh[4]; src.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const uint64_t len = (uint64_t)bh[1] << 16 | bh[2] << 8 | bh[3]; p += 4;
            if (p + len > n) return "FLAC metadata truncated";
            if (type == 0) { if (haveSi) return "two STREAMINFO blocks (FLAC allows one)"; if (len != 34) return "bad STREAMINFO"; uint8_t s[34]; src.read(p, s, 34); laesflac::BR r{s, 34}; fi.minBs = (int)r.get(16); fi.maxBs = (int)r.get(16); r.get(24); r.get(24);
                fi.rate = (int)r.get(20); fi.channels = (int)r.get(3) + 1; fi.bps = (int)r.get(5) + 1; fi.total = r.get(36); memcpy(fi.md5, s + 18, 16); haveSi = true; }
            else if (type == 2 && len >= 4) { uint8_t id[4]; src.read(p, id, 4); if (!memcmp(id, "LAE2", 4)) payload.add(p + 4, len - 4); else if (!memcmp(id, "LAE1", 4)) lae1 = true; }
            else if (type == 4 && len >= 8 && len < (1u << 24)) { std::vector<uint8_t> b((size_t)len); src.read(p, b.data(), b.size()); size_t q = 0;
                auto u32 = [&](uint32_t& v) { if (q + 4 > b.size()) return false; v = le32(&b[q]); q += 4; return true; }; uint32_t vl, cnt;
                if (u32(vl) && q + vl <= b.size()) { q += vl; if (u32(cnt)) for (uint32_t i = 0; i < cnt; i++) { uint32_t l; if (!u32(l) || q + l > b.size()) break; std::string kv((const char*)&b[q], l); q += l;
                    const size_t eq = kv.find('='); if (eq != std::string::npos) tags.push_back({kv.substr(0, eq), kv.substr(eq + 1)}); } } }
            p += len; }
        if (!haveSi) return "no STREAMINFO";
        if (!payload.total) { if (lae1) memcpy(laeMagic, "LAE1", 5); return "not LAES"; }
        if (fi.total == 0 || fi.maxBs < 16 || fi.channels != 2 || (fi.bps != 16 && fi.bps != 24 && fi.bps != 32)) return "core master is not 2-channel 16/24/32-bit FLAC with a sample count";
        audioOff = p; audioLen = n - p; baseRate = fi.rate; coreBits = fi.bps; coreFrames = (int64_t)fi.total; }
    else {   // RIFF/WAVE chunks (the last 'fmt ', 'data' and 'LAE ' count; sizes clamped to the file), LIST/INFO tags
        uint64_t pos = 12, laeOff = 0, laeLen = 0; bool haveLae = false, haveData = false;
        uint64_t dsData = 0; std::vector<std::pair<std::string, uint64_t>> dsTable;   // RF64: the data size and the table of other large chunks
        if (rf64) { uint8_t c[8]; if (n < 20 + 28) return "RF64 without ds64"; src.read(12, c, 8); if (memcmp(c, "ds64", 4) || le32(c + 4) < 28) return "RF64 without ds64";
            const uint32_t dl = le32(c + 4); std::vector<uint8_t> d(std::min<uint64_t>(dl, n - 20)); src.read(20, d.data(), d.size()); if (d.size() < 28) return "RF64 ds64 truncated";
            auto le64 = [&](size_t at) { return (uint64_t)le32(&d[at]) | (uint64_t)le32(&d[at + 4]) << 32; }; dsData = le64(8); const uint32_t tn = le32(&d[24]);
            for (uint32_t i = 0; i < tn && 28 + (size_t)i * 12 + 12 <= d.size(); i++) dsTable.push_back({std::string((const char*)&d[28 + (size_t)i * 12], 4), le64(28 + (size_t)i * 12 + 4)}); }
        while (pos + 8 <= n) { uint8_t c[8]; src.read(pos, c, 8); uint64_t len = le32(c + 4); const uint64_t body = pos + 8;
            if (rf64 && len == 0xFFFFFFFFu) { if (!memcmp(c, "data", 4)) len = dsData; else for (auto& t : dsTable) if (!memcmp(c, t.first.data(), 4)) { len = t.second; break; } }   // the 64-bit size
            if (len > n - body) len = n - body;
            if (!memcmp(c, "fmt ", 4) && len >= 16) { uint8_t f[26] = {0}; src.read(body, f, len >= 26 ? 26 : 16); fmtTag = f[0] | f[1] << 8; fmtCh = f[2] | f[3] << 8; baseRate = (int)le32(f + 4); coreBits = f[14] | f[15] << 8;
                if (fmtTag == 0xFFFE && len >= 26) fmtTag = f[24] | f[25] << 8; }
            else if (!memcmp(c, "data", 4)) { audioOff = body; audioLen = len; haveData = true; }
            else if (!memcmp(c, "LAE ", 4)) { laeOff = body; laeLen = len; haveLae = true; }
            else if (!memcmp(c, "LIST", 4) && len >= 4 && len < (1u << 24)) { std::vector<uint8_t> b((size_t)len); src.read(body, b.data(), b.size());
                if (!memcmp(b.data(), "INFO", 4)) for (size_t q = 4; q + 8 <= b.size();) { const uint32_t l = le32(&b[q + 4]); if (q + 8 + l > b.size()) break; std::string v((const char*)&b[q + 8], l); while (!v.empty() && !v.back()) v.pop_back();
                    tags.push_back({std::string((const char*)&b[q], 4), v}); q += 8 + l + (l & 1); } }
            pos = body + len + (len & 1); }
        if (!haveLae || laeLen < 4) return "not LAES";
        char mg[4]; src.read(laeOff, mg, 4); if (memcmp(mg, "LAE2", 4)) { if (!memcmp(mg, "LAE1", 4)) memcpy(laeMagic, "LAE1", 5); return "not LAES"; }
        if (fmtTag != 1 || fmtCh != 2 || (coreBits != 16 && coreBits != 24 && coreBits != 32) || !haveData) return "core master is not 2-channel 16/24/32-bit integer PCM";
        payload.add(laeOff, laeLen); coreFrames = (int64_t)(audioLen / (uint64_t)(2 * (coreBits / 8))); }
    memcpy(laeMagic, "LAE2", 5);
    if (quick) { why = payload.total < 68 ? "LAES block too short" : parsePayload(); makeChoices(); return ""; }
    if (!coreFramesFit(flac ? fi.total : (uint64_t)coreFrames)) return "core master too long for this program (a 32-bit program holds fewer than 2^29 frames)";
    // one pass over the whole file: its SHA-256, the payload CRC-32 (all but its last 4 bytes), the core
    laes::Sha256 fs; uint32_t crc = 0xFFFFFFFFu; const uint64_t crcEnd = payload.total >= 4 ? payload.total - 4 : 0;
    for (int c = 0; c < 2; c++) { core[c].clear(); core[c].reserve((size_t)coreFrames); }
    std::vector<uint8_t> fb; size_t fbHead = 0; uint64_t frameNo = 0; std::string coreErr; alignhash::Md5 md5; const int by = coreBits / 8;
    std::vector<uint8_t> carry;   // WAV: bytes of a sample frame split across two reads
    auto flacFrames = [&](bool final) {   // decodes the complete frames in fb (a frame is at most a few hundred kB; 4 MB stay ahead unless final)
        while (coreErr.empty() && fbHead < fb.size() && (final || fb.size() - fbHead > (4u << 20))) { V ch[2]; size_t used = 0;
            const std::string e = laesflac::decodeFrame(fb.data() + fbHead, fb.size() - fbHead, fi, frameNo, (uint64_t)core[0].size(), ch, used);
            if (!e.empty()) { coreErr = e; break; } fbHead += used; frameNo++;
            std::vector<uint8_t> mb; mb.reserve(ch[0].size() * 2 * ((fi.bps + 7) / 8));
            for (size_t i = 0; i < ch[0].size(); i++) for (int c = 0; c < 2; c++) { core[c].push_back((int32_t)ch[c][i]); const uint64_t v = (uint64_t)ch[c][i]; for (int k = 0; k < (fi.bps + 7) / 8; k++) mb.push_back((uint8_t)(v >> (8 * k))); }
            md5.update(mb.data(), mb.size()); }
        if (fbHead > (16u << 20)) { fb.erase(fb.begin(), fb.begin() + fbHead); fbHead = 0; } };
    // three threads, each fed in file order: this one reads and computes the CRC-32; the file SHA-256 (Hasher) and the core (FLAC frames
    // decoded and hashed, or WAV samples) run on threads of their own. The results are those of one pass in order.
    // a FLAC core of fixed block size is kept whole and decoded in parallel at the end (flacParallel); otherwise frame by frame as it comes
    const bool par = flac && fi.minBs == fi.maxBs && fi.maxBs > 0 && fi.total > 0;
    auto flacParallel = [&]() -> bool {
        const size_t T = std::min<size_t>(32, std::max(1u, std::thread::hardware_concurrency())); const uint64_t bs = (uint64_t)fi.maxBs;
        uint64_t k0 = 0; if (T < 2 || fb.size() < (4u << 20) || !laesflac::frameHeaderAt(fb.data(), fb.size(), fi, k0) || k0 != 0) return false;
        // the parts begin at frame headers (sync, header CRC-8, a rising frame number) near equal byte shares; each part must then end exactly
        // where the next begins, with the frame number it begins with, or the core is decoded in order instead
        std::vector<size_t> off{0}; std::vector<uint64_t> num{0};
        for (size_t t = 1; t < T; t++) { size_t p = std::max(off.back() + 1, fb.size() / T * t); uint64_t k = 0;
            for (; p + 16 < fb.size(); p++) if (fb[p] == 0xFF && (fb[p + 1] & 0xFE) == 0xF8 && laesflac::frameHeaderAt(fb.data() + p, fb.size() - p, fi, k) && k > num.back()) break;
            if (p + 16 >= fb.size()) break; off.push_back(p); num.push_back(k); }
        const size_t S = num.size(); off.push_back(fb.size());
        for (int c = 0; c < 2; c++) core[c].assign((size_t)fi.total, 0);
        const uint64_t tot = (uint64_t)core[0].size(); if (tot != fi.total) { for (int c = 0; c < 2; c++) core[c].clear(); return false; }   // every write below is checked against the arrays' own length
        std::vector<size_t> endOff(S, 0); std::vector<uint64_t> endNum(S, 0), endAt(S, 0); std::vector<int> bad(S, 0), shortAt(S, -1);
        std::vector<std::thread> th; th.reserve(S);
        for (size_t s = 0; s < S; s++) th.emplace_back([&, s] { try { size_t p = off[s]; uint64_t f = num[s];
                while (p < off[s + 1]) { if (shortAt[s] >= 0) { bad[s] = 1; return; }   // only the last frame of the core may be shorter
                    const uint64_t at = f * bs; if (at >= tot) { bad[s] = 1; return; } V ch[2]; size_t used = 0;
                    if (!laesflac::decodeFrame(fb.data() + p, fb.size() - p, fi, f, at, ch, used).empty() || ch[0].size() > tot - at) { bad[s] = 1; return; }
                    for (int c = 0; c < 2; c++) { int32_t* d = core[c].data() + at; for (size_t i = 0; i < ch[c].size(); i++) d[i] = (int32_t)ch[c][i]; }
                    if (ch[0].size() != bs) shortAt[s] = 1; p += used; f++; endAt[s] = at + ch[0].size(); }
                endOff[s] = p; endNum[s] = f; } catch (...) { bad[s] = 1; } });
        for (auto& x : th) x.join();
        bool ok = true; for (size_t s = 0; s < S; s++) ok = ok && !bad[s] && endOff[s] == off[s + 1] && (s + 1 == S ? endAt[s] == fi.total : endNum[s] == num[s + 1] && shortAt[s] < 0);
        if (!ok) { for (int c = 0; c < 2; c++) core[c].clear(); return false; }
        // the MD5 of the audio, in order, as frame by frame
        const int fby = (fi.bps + 7) / 8; std::vector<uint8_t> mb; mb.reserve(1u << 20);
        for (size_t i = 0; i < (size_t)fi.total; i++) { for (int c = 0; c < 2; c++) { const uint64_t v = (uint64_t)(int64_t)core[c][i]; for (int q = 0; q < fby; q++) mb.push_back((uint8_t)(v >> (8 * q))); }
            if (mb.size() >= (1u << 20)) { md5.update(mb.data(), mb.size()); mb.clear(); } }
        md5.update(mb.data(), mb.size()); fbHead = fb.size(); frameNo = endNum[S - 1]; return true; };
    auto coreFeed = [&](const uint8_t* p, size_t k) {
        if (flac) { fb.insert(fb.end(), p, p + k); if (!par) flacFrames(false); return; }
        carry.insert(carry.end(), p, p + k); const size_t whole = carry.size() / (size_t)(2 * by) * (size_t)(2 * by);
        for (size_t q = 0; q < whole && (int64_t)core[0].size() < coreFrames; q += (size_t)by * 2) for (int c = 0; c < 2; c++) { const uint8_t* s = &carry[q + (size_t)c * by]; uint32_t v = 0;
            for (int t = 0; t < by; t++) v |= (uint32_t)s[t] << (8 * t); if (coreBits < 32 && (v >> (coreBits - 1) & 1)) v |= ~0u << coreBits; core[c].push_back((int32_t)v); }
        carry.erase(carry.begin(), carry.begin() + whole); };
    std::mutex cm; std::condition_variable ccv; std::deque<std::vector<uint8_t>> cq; bool cend = false;   // core pieces waiting (at most 4)
    std::thread ct([&] { for (;;) { std::vector<uint8_t> b; { std::unique_lock<std::mutex> g(cm); ccv.wait(g, [&] { return cend || !cq.empty(); }); if (cq.empty()) return; b = std::move(cq.front()); cq.pop_front(); ccv.notify_all(); }
        try { if (coreErr.empty()) coreFeed(b.data(), b.size()); } catch (const std::exception& x) { coreErr = x.what(); } } });
    struct EndCore { std::mutex& m; std::condition_variable& cv; bool& end; std::thread& t; ~EndCore() { { std::lock_guard<std::mutex> g(m); end = true; cv.notify_all(); } if (t.joinable()) t.join(); } } endCore{cm, ccv, cend, ct};
    { Hasher hs; std::vector<uint8_t> buf(4u << 20);
      for (uint64_t a = 0; a < n; a += buf.size()) { const size_t k = (size_t)std::min<uint64_t>(buf.size(), n - a); src.read(a, buf.data(), k); hs.push(std::vector<uint8_t>(buf.begin(), buf.begin() + (ptrdiff_t)k));
        for (size_t s = 0; s < payload.off.size(); s++) {   // payload bytes in this read
            const uint64_t o0 = std::max(a, payload.off[s]), o1 = std::min(a + k, payload.off[s] + payload.len[s]);
            if (o0 >= o1) continue; const uint64_t p0 = payload.start[s] + (o0 - payload.off[s]); const uint64_t e1 = std::min(o1, o0 + (crcEnd > p0 ? crcEnd - p0 : 0));
            if (e1 > o0) crc = crcUpdate(crc, buf.data() + (size_t)(o0 - a), (size_t)(e1 - o0)); }
        const uint64_t c0 = std::max(a, audioOff), c1 = std::min(a + k, audioOff + audioLen);   // core bytes in this read
        if (c0 < c1) { std::unique_lock<std::mutex> g(cm); ccv.wait(g, [&] { return cq.size() < 4; }); cq.emplace_back(buf.begin() + (ptrdiff_t)(c0 - a), buf.begin() + (ptrdiff_t)(c1 - a)); ccv.notify_all(); } }
      { std::lock_guard<std::mutex> g(cm); cend = true; ccv.notify_all(); } ct.join(); hs.final(fileSha); }
    crc ^= 0xFFFFFFFFu; (void)fs;
    if (flac) { if (!(par && coreErr.empty() && flacParallel())) flacFrames(true); if (!coreErr.empty()) return "FLAC core master: " + coreErr;
        if ((uint64_t)core[0].size() != fi.total) return "FLAC core master: sample count differs from STREAMINFO";
        std::string hx = md5.hex(); bool zero = true; for (int i = 0; i < 16; i++) zero &= fi.md5[i] == 0; uint8_t m[16]; for (int i = 0; i < 16; i++) m[i] = (uint8_t)std::stoul(hx.substr(i * 2, 2), nullptr, 16);
        if (zero || memcmp(m, fi.md5, 16)) return "FLAC core master: MD5 mismatch"; }
    coreFrames = (int64_t)core[0].size();
    // the payload: CRC-32, header, master records, the layers' byte ranges
    why = ""; if (payload.total < 68) why = "LAES block too short";
    else { uint8_t t[4]; payload.read(payload.total - 4, t, 4); if (le32(t) != crc) why = "LAES block damaged (CRC-32)"; }
    if (why.empty()) why = parsePayload();
    makeChoices(); return ""; }

inline std::string File::parsePayload() {
    ms.clear(); head = laes::Head(); const uint64_t end = payload.total - 4; uint64_t pos = 0; bool trunc = false;
    auto u = [&](int k) -> uint64_t { if (pos + k > end) { trunc = true; return 0; } uint8_t t[8]; payload.read(pos, t, (size_t)k); pos += k; uint64_t v = 0; for (int i = 0; i < k; i++) v |= (uint64_t)t[i] << (8 * i); return v; };
    uint64_t magic = u(4), ver = u(2), hs = u(2), base = u(4), cbits = u(1), ch = u(1), nm = u(1), cmode = u(1), nc = u(8); uint8_t csha[32] = {0}; if (pos + 32 <= end) { payload.read(pos, csha, 32); pos += 32; } else trunc = true;
    uint8_t hb[8] = {0}; if (pos + 8 <= end) { payload.read(pos, hb, 8); pos += 8; } else trunc = true;
    if (trunc || magic != 0x3245414Cull) return "not an LAES block"; if ((ver != 1 && ver != 2) || hs != 64) return "unsupported LAES version";
    if ((base != 44100 && base != 48000) || (int)base != baseRate || (int)cbits != coreBits || ch != 2 || cmode > 1 || !laes::headerCountOk(ver, nm) || !laes::headFrom(ver, hb, nm, head)) return "invalid LAES header";
    if (nc != (uint64_t)coreFrames) return "core master length differs from the LAES header";
    if (!quick) { laes::Sha256 s; std::vector<uint8_t> b; b.reserve(1 << 16);   // the core's SHA-256 (16 bit: int16 LE; 24 / 32 bit: int32 LE)
      for (int64_t i = 0; i < coreFrames; i++) for (int c = 0; c < 2; c++) { const uint32_t v = (uint32_t)core[c][(size_t)i]; const int k = coreBits == 16 ? 2 : 4; for (int q = 0; q < k; q++) b.push_back((uint8_t)(v >> (8 * q)));
          if (b.size() >= (1 << 16)) { s.update(b.data(), b.size()); b.clear(); } }
      s.update(b.data(), b.size()); uint8_t hh[32]; s.final(hh); if (memcmp(hh, csha, 32)) return "core master differs from the one this LAES block was made for (SHA-256)"; }
    for (uint64_t mi = 0; mi < nm; mi++) { Master M; laes::MasterInfo& I = M.I;
        I.bits = (int)u(1); I.stages = (int)u(1); I.w = (int)u(1); I.storage = (int)u(1); I.refTier = (int)u(1);
        const uint64_t nl = u(1), fl = u(1), mp = u(1); I.rate = (int)u(4); u(4); const uint64_t F64 = u(8);
        I.whole = (int64_t)u(8); I.frac = (uint32_t)u(4); u(4); if (pos + 32 <= end) { payload.read(pos, I.sha, 32); pos += 32; } else trunc = true;
        if (trunc) return "LAES block truncated";
        if (ver == 2) { if (pos + nl > end) return "LAES block truncated"; std::string nm2((size_t)nl, '\0'); if (nl) payload.read(pos, &nm2[0], (size_t)nl); pos += nl;
            if (fl & ~(uint64_t)laes::kNoDerive) return "invalid master record"; if (!laes::validMasterName((const uint8_t*)nm2.data(), nm2.size())) return "invalid master name"; I.name = nm2; I.flags = (int)fl; I.menu = (int)mp; }
        if ((I.bits != 16 && I.bits != 24 && I.bits != 32) || I.stages > 3 || I.w >= I.bits || I.storage > 2 || (I.storage != 1 && I.refTier > (int)mi) || (I.storage == 2 && I.refTier < 1)
            || (I.storage == 1 && I.refTier != 0) || I.rate != (baseRate << I.stages) || F64 < 1 || ((F64 + (1ull << I.stages) - 1) >> I.stages) > nc + (1ull << 24)
            || I.whole <= -(1LL << 40) || I.whole >= (1LL << 40)) return "invalid master record";
        if (F64 > (uint64_t)INT64_MAX) return "master too long for this decoder"; I.frames = (int64_t)F64;   // frame counts are int64_t on every platform (32 and 64 bit)
        M.j = I.storage == 2 ? std::min(I.stages, ms[(size_t)I.refTier - 1].I.stages) : 0;
        for (int c = 0; c < 2; c++) for (int k = M.j; k <= I.stages; k++) { const uint64_t l = u(4); if (trunc || pos + l > end) return "LAES block truncated"; M.layOff[c][k - M.j] = pos; M.layLen[c][k - M.j] = l; pos += l; }
        ms.push_back(M); }
    { std::vector<laes::MasterInfo> mi; for (const Master& M : ms) mi.push_back(M.I); if (!laes::menuOk(mi)) return "invalid menu positions"; }
    if (pos != end) return "unexpected bytes after the last master";
    return ""; }

inline void File::makeChoices() {   // the same list and order as `laes dec --list` (all masters offered until a run proves otherwise)
    choices.clear();
    std::vector<laes::MasterInfo> mi; if (why.empty()) for (const Master& M : ms) mi.push_back(M.I);   // a rejected payload: the core master only
    const laes::Head hd = why.empty() ? head : laes::Head();
    const std::vector<laesc::Spec> sp = laesc::build(baseRate, coreBits, coreFrames, mi, nullptr, &hd); defaultChoice = laesc::defaultIndex(sp, hd);
    for (const laesc::Spec& s : sp) { Choice c; c.kind = s.kind; c.m = s.m; c.level = s.level; c.from = s.from; c.rate = s.rate; c.bits = s.bits; c.frames = s.frames;
        c.name = s.name; c.what = s.what; choices.push_back(c); } }

// every layer of every master: byte range, values, block length
inline std::vector<File::LayerRef> File::layerList() const {
    std::vector<LayerRef> ls;
    for (const Master& M : ms) { const laes::MasterInfo& I = M.I;
        for (int c = 0; c < 2; c++) for (int k = M.j; k <= I.stages; k++)
            ls.push_back({M.layOff[c][k - M.j], M.layLen[c][k - M.j], k == M.j ? (int64_t)laes::levLen(I.frames, I.stages - M.j) : (int64_t)laes::levLen(I.frames, I.stages - k) / 2, 4096L << (k == M.j ? M.j : k - 1)}); }
    return ls; }
// record: "LAEI" 1, file SHA-256 (32), uint32 layers; per layer: uint64 offset, uint64 blocks, then the positions of blocks 0, 16, 32 ...
// as differences (LEB128); uint32 CRC-32 of everything before it. Little-endian.
inline std::vector<uint8_t> File::exportIndex() {
    const std::vector<LayerRef> ls = layerList(); std::vector<uint8_t> o{'L', 'A', 'E', 'I', 1}; o.insert(o.end(), fileSha, fileSha + 32);
    auto u = [&](uint64_t v, int n) { for (int i = 0; i < n; i++) o.push_back((uint8_t)(v >> (8 * i))); };
    auto leb = [&](uint64_t v) { do { uint8_t b = (uint8_t)(v & 0x7F); v >>= 7; if (v) b |= 0x80; o.push_back(b); } while (v); };
    u(ls.size(), 4);
    for (const LayerRef& l : ls) { const uint64_t nb = (uint64_t)((l.n + l.B - 1) / l.B); std::shared_ptr<LayerIdx> ix = layerIndex(l.off);
        std::lock_guard<std::mutex> g(ix->m); if (ix->bit.size() < nb) return {};   // not noted in full yet
        u(l.off, 8); u(nb, 8); uint64_t prev = 0; for (uint64_t b = 0; b < nb; b += LayerIdx::kSparse) { const uint64_t p = ix->bit[(size_t)b]; leb(p - prev); prev = p; } }
    u(crcUpdate(0xFFFFFFFFu, o.data(), o.size()) ^ 0xFFFFFFFFu, 4); return o; }
inline bool File::importIndex(const uint8_t* d, size_t n) {
    if (!d || n < 5 + 32 + 4 + 4 || memcmp(d, "LAEI\1", 5) || memcmp(d + 5, fileSha, 32)) return false;
    auto le = [&](size_t at, int k) { uint64_t v = 0; for (int i = 0; i < k; i++) v |= (uint64_t)d[at + (size_t)i] << (8 * i); return v; };
    if ((uint32_t)le(n - 4, 4) != (crcUpdate(0xFFFFFFFFu, d, n - 4) ^ 0xFFFFFFFFu)) return false;
    const std::vector<LayerRef> ls = layerList(); size_t at = 37; const size_t end = n - 4;
    if (le(at, 4) != ls.size()) return false; at += 4;
    std::vector<std::vector<uint64_t>> got(ls.size());
    for (size_t i = 0; i < ls.size(); i++) { const LayerRef& l = ls[i]; const uint64_t nb = (uint64_t)((l.n + l.B - 1) / l.B);
        if (at + 16 > end || le(at, 8) != l.off || le(at + 8, 8) != nb) return false; at += 16;
        uint64_t prev = 0; for (uint64_t b = 0; b < nb; b += LayerIdx::kSparse) { uint64_t v = 0; int sh = 0;
            for (;;) { if (at >= end || sh > 63) return false; const uint8_t c = d[at++]; v |= (uint64_t)(c & 0x7F) << sh; sh += 7; if (!(c & 0x80)) break; }
            const uint64_t p = prev + v; if ((b == 0 ? p != 0 : p <= prev) || p >= l.len * 8) return false; got[i].push_back(p); prev = p; } }
    if (at != end) return false;
    for (size_t i = 0; i < ls.size(); i++) { std::shared_ptr<LayerIdx> ix = layerIndex(ls[i].off); std::lock_guard<std::mutex> g(ix->m);
        ix->sp = std::move(got[i]); ix->spEnd = (size_t)((ls[i].n + ls[i].B - 1) / ls[i].B); }
    return true; }
inline void File::indexLayers(const std::atomic<bool>* cancel, int threads) {
    const std::vector<LayerRef> ls = layerList(); typedef LayerRef L;
    // a failure in any of the threads (a read of the file fails, no memory, no thread) stops the others and comes back as this call's
    // exception once every thread has ended; before 2026-11-14 it ended the whole process (an exception leaving a thread: std::terminate)
    std::atomic<size_t> next{0}; std::atomic<bool> failed{false}; std::mutex em; std::exception_ptr err;
    auto fault = [&] { std::lock_guard<std::mutex> g(em); if (!err) err = std::current_exception(); failed = true; };
    auto work = [&] { try { for (;;) { const size_t i = next++; if (i >= ls.size() || failed) return; const L& l = ls[i]; std::shared_ptr<LayerIdx> ix = layerIndex(l.off);
            if ((int64_t)ix->known() * l.B >= l.n) continue;   // noted already
            BitRS rd; rd.src = &payload; rd.off = l.off; rd.len = l.len; V blk;
            for (int64_t s = 0, kb = 0; s < l.n; s += l.B, kb++) { if (failed || (cancel && cancel->load())) return;
                ix->note((size_t)kb, rd.pos); const int64_t k = (int64_t)std::min<int64_t>(l.B, l.n - s); blk.resize((size_t)k);
                if (!laes::decodeBlock(rd, blk.data(), k)) break; } } } catch (...) { fault(); } };
    std::vector<std::thread> ts; const int nt = std::max(1, std::min<int>(threads, (int)ls.size()));
    try { for (int t = 1; t < nt; t++) ts.emplace_back(work); } catch (...) { fault(); }
    work(); for (auto& t : ts) t.join(); if (err) std::rethrow_exception(err); }

// ---- player
inline Player::MD* Player::need(int m) {   // the nodes of master m (and of its tiers, first)
    for (auto& d : chain) if (d->idx == m) return d.get();
    const Master& M = f->ms[(size_t)m]; const laes::MasterInfo& I = M.I;
    if (I.storage != 1 && I.refTier > 0) need(I.refTier - 1);
    std::unique_ptr<MD> D(new MD()); D->idx = m; const int j = M.j;
    for (int c = 0; c < 2; c++) {
        auto layer = [&](int k) { std::unique_ptr<LayerNode> L(new LayerNode()); L->rd.src = &f->payload; L->rd.off = M.layOff[c][k - j]; L->rd.len = M.layLen[c][k - j]; L->ix = f->layerIndex(M.layOff[c][k - j]);
            L->B = 4096L << (k == j ? j : k - 1); L->n = k == j ? laes::levLen(I.frames, I.stages - j) : laes::levLen(I.frames, I.stages - k) / 2; L->threads = threads; Node* p = L.get(); D->own.push_back(std::move(L)); return p; };
        Node* x = layer(j);
        if (I.storage != 1) { std::unique_ptr<AddRefNode> A(new AddRefNode()); A->e = x; A->n = x->n; A->threads = threads;
            if (I.refTier == 0) { A->src = &coreSig[c]; A->up = (I.bits - I.w) - f->coreBits; }
            else { const laes::MasterInfo& T = f->ms[(size_t)I.refTier - 1].I; A->src = tierLevel(I.refTier - 1, c, j); A->up = (I.bits - I.w) - (T.bits - T.w); }
            A->wj = I.whole * (1LL << j) + (j ? (int64_t)(I.frac >> (32 - j)) : 0); A->h = laes::refTaps((uint32_t)((uint64_t)I.frac << j)); x = A.get(); D->own.push_back(std::move(A)); }
        // levels j .. S are the steps of the reconstruction: level k of the decoded master (spec 6.6: the forward stages of its samples) equals
        // the value the inverse stages had at level k, since each lifting stage is exactly undone by its forward stage (same integer
        // operations, same symmetric extension); only levels below j need forward stages (levelOf)
        D->lev[c].assign((size_t)I.stages + 1, nullptr); D->lev[c][(size_t)j] = x;
        for (int k = j + 1; k <= I.stages; k++) { Node* d = layer(k); std::unique_ptr<UpNode> U(new UpNode()); U->s = x; U->d = d; U->n = laes::levLen(I.frames, I.stages - k); U->threads = threads;
            if (U->n != x->n + d->n) fail("LAES octave layer length"); x = U.get(); D->own.push_back(std::move(U)); D->lev[c][(size_t)k] = x; }
        D->top[c] = x; }
    chain.push_back(std::move(D)); return chain.back().get(); }
inline Node* Player::levelOf(MD& D, int c, int k) {   // level k of a decoded master, from its samples by forward stages (spec 6.6)
    const int S = f->ms[(size_t)D.idx].I.stages; if (k >= S) return D.top[c];
    if (!D.lev[c][(size_t)k]) {   // only below the master's level j
        std::unique_ptr<DownNode> N(new DownNode()); N->x = levelOf(D, c, k + 1); N->n = (N->x->n + 1) / 2; N->threads = threads; N->keep = D.top[c]->keep; N->chunk = D.top[c]->chunk;
        D.lev[c][(size_t)k] = N.get(); D.own.push_back(std::move(N)); }
    return D.lev[c][(size_t)k]; }
inline laess::Sig* Player::tierLevel(int tier, int c, int k) { MD* T = need(tier); return levelOf(*T, c, k); }
// runs f(0) on a second thread and f(1) here (the two channels share no node), then rethrows the first exception
template <class F> inline void bothChannels(F f) {
    std::exception_ptr e0; std::thread t([&] { try { f(0); } catch (...) { e0 = std::current_exception(); } });
    std::exception_ptr e1; try { f(1); } catch (...) { e1 = std::current_exception(); } t.join();
    if (e0) std::rethrow_exception(e0); if (e1) std::rethrow_exception(e1); }
inline std::string Player::start(File& file, int choice, int nThreads, bool allMasters, int64_t startFrame) {
    f = &file; threads = std::max(1, nThreads); chain.clear(); pos = 0; done = false; ok = false; noHash = false; err.clear();
    if (f->quick) return "opened for track info only";
    if (choice < 0 || choice >= (int)f->choices.size()) return "no such choice";
    C = f->choices[(size_t)choice];
    if (startFrame < 0 || startFrame > C.frames) return "start position outside the track";
    for (int c = 0; c < 2; c++) rng[c] = xs64Jump(20261029ull + 0x9E3779B97F4A7C15ull * (uint64_t)(c + 1), 2 * (uint64_t)startFrame);   // the dither state at the start frame (0: the seed)
    if (startFrame > 0) { pos = startFrame; noHash = true; }
    for (int c = 0; c < 2; c++) { coreSig[c].v = &f->core[c]; coreSig[c].n = (int64_t)f->core[c].size(); }
    try { const Choice& S = C.kind == 3 ? f->choices[(size_t)C.from] : C; if (S.kind != 0) need(S.m);
        if (allMasters) for (int m = 0; m < (int)f->ms.size(); m++) need(m);   // every master decoded (and hashed) once, in this one run
        if (chain.empty()) return "";
        if (S.kind == 2) for (auto& d : chain) if (d->idx == S.m) for (int c = 0; c < 2; c++) levelOf(*d, c, S.level);   // made now: the channels run in parallel later
        for (auto& d : chain) for (auto& nd : d->own) nd->threads = std::max(1, threads / 2);   // two channels at once, each with half the threads
        // nodes that other masters read at an offset (tiers) keep a window that covers the largest time offset in the chain
        int64_t span = 0; for (auto& d : chain) { const laes::MasterInfo& I = f->ms[(size_t)d->idx].I; span = std::max<int64_t>(span, (I.whole < 0 ? -I.whole : I.whole) + 2); }
        // and the reach of a reference chain (2026-11-12): each level runs at most the filter's reach (128 + T) ahead of the one that refers to it
        const int64_t chainReach = (int64_t)chain.size() * ((span << 3) + 256);
        for (auto& d : chain) for (auto& nd : d->own) { nd->keep = std::max<int64_t>(nd->keep, (span << 3) + 4 * nd->chunk + 4096 + chainReach); } }
    catch (const std::exception& x) { return x.what(); }
    return ""; }
inline void Player::advanceHashes(double frac) {   // every master of the chain hashed up to the same share of its length as the output
    for (auto& d : chain) { const laes::MasterInfo& I = f->ms[(size_t)d->idx].I; const int64_t want = frac >= 1.0 ? I.frames : std::min<int64_t>(I.frames, (int64_t)(frac * (double)I.frames));
        V a, b; std::vector<uint8_t> buf;
        while (d->hashed < want) { const int64_t e = std::min(want, d->hashed + (int64_t)(1 << 16)); a.resize((size_t)(e - d->hashed)); b.resize(a.size());
            bothChannels([&](int c) { d->top[c]->get(d->hashed, e, (c ? b : a).data()); });
            LAESP_PROF(4); buf.resize(a.size() * 8); for (size_t i = 0; i < a.size(); i++) { const uint32_t l = (uint32_t)(int32_t)(a[i] << I.w), r = (uint32_t)(int32_t)(b[i] << I.w); for (int q = 0; q < 4; q++) { buf[i * 8 + q] = (uint8_t)(l >> (8 * q)); buf[i * 8 + 4 + q] = (uint8_t)(r >> (8 * q)); } }
            d->sha.push(std::move(buf)); buf = std::vector<uint8_t>(); d->hashed = e; } } }
inline int64_t Player::read(int32_t* out, int64_t frames) {
    if (done || !err.empty()) return 0;
    try { const int64_t k = std::min(frames, C.frames - pos); if (k <= 0) { done = true; return 0; }
        const Choice& S = C.kind == 3 ? f->choices[(size_t)C.from] : C; V x[2];
        bothChannels([&](int c) { x[c].resize((size_t)k);
            if (S.kind == 0) { for (int64_t i = 0; i < k; i++) x[c][(size_t)i] = f->core[c][(size_t)(pos + i)]; return; }
            MD* D = nullptr; for (auto& d : chain) if (d->idx == S.m) D = d.get(); const laes::MasterInfo& I = f->ms[(size_t)S.m].I;
            Node* src = S.kind == 1 ? D->top[c] : D->lev[c][(size_t)S.level]; src->get(pos, pos + k, x[c].data());
            if (S.kind == 1) for (auto& v : x[c]) v <<= I.w;
            else { const int64_t mx = (1LL << (S.bits - 1)) - 1, mn = -(1LL << (S.bits - 1)); for (auto& v : x[c]) v = std::max(mn, std::min(mx, v << I.w)); } });
        { LAESP_PROF(5); for (int64_t i = 0; i < k; i++) for (int c = 0; c < 2; c++) { int64_t v = x[c][(size_t)i];
            if (C.kind == 3) { const int sft = S.bits - 16; uint64_t& st = rng[c]; auto nx = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st >> (64 - sft); };   // as `laes dec`
                const int64_t r1 = (int64_t)nx(), r2 = (int64_t)nx(); v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (v + r1 - r2 + (1LL << (sft - 1))) >> sft)); }
            out[(size_t)(i * 2 + c)] = (int32_t)v; } }
        pos += k; if (noHash) { if (pos >= C.frames) done = true; return k; }
        advanceHashes((double)pos / (double)C.frames);
        if (pos >= C.frames) { advanceHashes(1.0); ok = true; for (auto& d : chain) { uint8_t h[32]; d->sha.final(h); d->shaOk = !memcmp(h, f->ms[(size_t)d->idx].I.sha, 32); ok &= d->shaOk; } done = true; }
        return k; }
    catch (const std::exception& e) { err = e.what(); return 0; } }
}  // namespace laesp
