// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAES encoder, block-wise (2026-10-30): the same payload as laes::encodeLaes (laes_file_enc.h), byte for byte, with the memory of a few
// pieces. Each master is read once into a work folder (int32 samples, SHA-256 and the unused low bits found on the way); its rate levels
// and octave layers are computed piece by piece (laes_stream.h) and do not depend on the file order, so they are made once for both orders
// the tool tries. Every candidate storage of a master is coded to a file; only the cheapest is kept. The payload is written to a file with
// its CRC-32 added up piece by piece.
#pragma once
#include <cmath>
#include <map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <thread>
#include "laes_stream.h"
#include "laes_file_enc.h"
#include "laes_flac.h"   // FLAC masters (2026-11-05)
namespace laess {
// disk guard (2026-11-05): while the work folder is written, the free space of its disk is looked at (at most every 0.5 s); below the
// reserve the run waits until there is room again (stop, --disk-stop: it stops, the work folder kept; removeNote, --disk-stop-remove: the tool removes it). With track, the largest size of the work folder seen is
// kept (--timing). The files written do not change.
struct DiskGuard { std::string dir; uint64_t reserve = 2ull << 30; bool track = false, stop = false, removeNote = false; uint64_t peak = 0; std::chrono::steady_clock::time_point last{}; std::mutex mu; };
inline DiskGuard*& diskGuard() { static DiskGuard* g = nullptr; return g; }
inline uint64_t folderBytes(const std::string& dir) { std::error_code ec; uint64_t s = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) { std::error_code e2; if (it->is_regular_file(e2)) s += it->file_size(e2); } return s; }
inline void diskCheck(bool now = false) {
    DiskGuard* g = diskGuard(); if (!g) return; std::lock_guard<std::mutex> lk(g->mu); const auto t = std::chrono::steady_clock::now();
    if (!now && t - g->last < std::chrono::milliseconds(500)) return; g->last = t;
    std::error_code ec; std::filesystem::space_info sp = std::filesystem::space(g->dir, ec);
    if (g->track) g->peak = std::max(g->peak, folderBytes(g->dir));
    if (ec || sp.available >= g->reserve) return;
    const std::string left = std::to_string((unsigned long long)(sp.available >> 20)) + " MB left, " + std::to_string((unsigned long long)(g->reserve >> 20)) + " MB kept free";
    if (g->stop) { printf("%s%llu%s\n", u8"  \u78c1\u76d8\u5feb\u6ee1\uff08\u5269 ", (unsigned long long)(sp.available >> 20), g->removeNote ? u8" MB\uff09\uff1a--disk-stop-remove\uff0c\u5df2\u505c\u4e0b\uff0c\u5de5\u4f5c\u6587\u4ef6\u5939\u5df2\u5220\u9664\uff0c\u6ca1\u6709\u5199\u51fa\u6587\u4ef6" : u8" MB\uff09\uff1a--disk-stop\uff0c\u5df2\u505c\u4e0b\uff0c\u5de5\u4f5c\u6587\u4ef6\u5939\u4fdd\u7559\uff1b\u4e4b\u540e\u518d\u8fd0\u884c\u540c\u4e00\u6761\u547d\u4ee4\uff0c\u4ece\u505c\u4e0b\u7684\u5730\u65b9\u63a5\u7740\u505a"); fflush(stdout); }
    if (g->stop) fail("the disk of the work folder is almost full (" + left + std::string(g->removeNote ? "); --disk-stop-remove: stopped, the work folder removed" : "); --disk-stop: stopped"));
    // the default (2026-11-05): wait, nothing removed; the run goes on once the disk has room again (other threads wait at their next check)
    printf("disk almost full (%s): free some space on the disk of %s and the encoding continues by itself;\n"
           "  or close this program and run the same command again later: it continues from the work folder\n", left.c_str(), g->dir.c_str());
    printf("%s%llu%s%s%s\n", u8"  \u78c1\u76d8\u5feb\u6ee1\uff08\u5269 ", (unsigned long long)(sp.available >> 20), u8" MB\uff09\uff1a\u8bf7\u5728 ", g->dir.c_str(), u8" \u6240\u5728\u7684\u78c1\u76d8\u4e0a\u6e05\u7406\u4e00\u4e9b\u7a7a\u95f4\uff0c\u7f16\u7801\u4f1a\u81ea\u52a8\u63a5\u7740\u505a\uff1b\u4e5f\u53ef\u4ee5\u5173\u6389\u7a0b\u5e8f\uff0c\u4e4b\u540e\u518d\u8fd0\u884c\u540c\u4e00\u6761\u547d\u4ee4\uff0c\u4ece\u5de5\u4f5c\u6587\u4ef6\u5939\u63a5\u7740\u505a"); fflush(stdout);
    do { std::this_thread::sleep_for(std::chrono::seconds(5)); sp = std::filesystem::space(g->dir, ec); } while (!ec && sp.available < g->reserve + (256ull << 20));
    if (ec) { printf("cannot read the free space of the disk of %s (%s): continuing\n", g->dir.c_str(), ec.message().c_str());   // FU13-8: not "room again" with a meaningless number
        printf("%s%s%s\n", u8"  无法读取 ", g->dir.c_str(), u8" 所在磁盘的剩余空间：接着做"); fflush(stdout); g->last = std::chrono::steady_clock::now(); return; }
    printf("disk has room again (%llu MB free): continuing\n", (unsigned long long)(sp.available >> 20)); printf("%s%llu%s\n", u8"  \u78c1\u76d8\u6709\u7a7a\u95f4\u4e86\uff08\u5269 ", (unsigned long long)(sp.available >> 20), u8" MB\uff09\uff1a\u63a5\u7740\u505a"); fflush(stdout); g->last = std::chrono::steady_clock::now(); }
// ---- resume (2026-11-05): small state files in the work folder, each written whole (a .new file, then renamed), so that a run stopped at
// any point (disk full, closed) continues where it was. They hold only what a full run computes again the same way.
inline void putState(const std::string& p, const std::string& text) { { Out o(p + ".new"); o.put(text.data(), text.size()); o.close(); }
    std::error_code ec; std::filesystem::rename(p + ".new", p, ec); if (ec) fail("cannot write " + p); }
inline long long sizeOrMinus(const std::string& p) { std::error_code ec; const auto n = std::filesystem::file_size(p, ec); return ec ? -1 : (long long)n; }   // -1: missing
inline bool getState(const std::string& p, std::string& text) { std::error_code ec; if (!std::filesystem::exists(p, ec)) return false; In in(p); text.assign((size_t)in.n, '\0'); if (in.n) in.read(0, &text[0], text.size()); return true; }
// one value per line
struct StateW { std::string s; void i(int64_t v) { s += std::to_string(v) + "\n"; } void d(double v) { char b[40]; snprintf(b, sizeof b, "%.17g\n", v); s += b; }
    void str(const std::string& v) { s += v + "\n"; } void hex(const uint8_t* p, size_t n) { char b[3]; for (size_t k = 0; k < n; k++) { snprintf(b, sizeof b, "%02x", p[k]); s += b; } s += "\n"; } };
struct StateR { const std::string& s; size_t at = 0; bool bad = false; explicit StateR(const std::string& t) : s(t) {}
    std::string str() { const size_t e = s.find('\n', at); if (e == std::string::npos) { bad = true; return std::string(); } std::string v = s.substr(at, e - at); at = e + 1; return v; }
    int64_t i() { const std::string v = str(); if (v.empty()) { bad = true; return 0; } return strtoll(v.c_str(), nullptr, 10); } double d() { return strtod(str().c_str(), nullptr); }
    void hex(uint8_t* p, size_t n) { const std::string v = str(); if (v.size() != 2 * n) { bad = true; return; } for (size_t k = 0; k < n; k++) p[k] = (uint8_t)strtoul(v.substr(2 * k, 2).c_str(), nullptr, 16); } };
inline void putInfo(StateW& w, const laes::MasterInfo& I) { w.i(I.bits); w.i(I.stages); w.i(I.w); w.i(I.storage); w.i(I.refTier); w.i(I.rate); w.i(I.frames); w.i(I.whole); w.i(I.frac); w.hex(I.sha, 32); w.d(I.layerBits); }
inline void getInfo(StateR& r, laes::MasterInfo& I) { I.bits = (int)r.i(); I.stages = (int)r.i(); I.w = (int)r.i(); I.storage = (int)r.i(); I.refTier = (int)r.i(); I.rate = (int)r.i(); I.frames = r.i(); I.whole = r.i(); I.frac = (uint32_t)r.i(); r.hex(I.sha, 32); I.layerBits = r.d(); }
// a layer (one channel) coded in pieces: blocks of B; a piece is a whole number of blocks (only the last piece may end in a short block).
// As laes::encodeLayer: the blocks' parameters are chosen in parallel threads, then written in order; the coded bytes go to a file.
struct LayerOut { int64_t B; laes::BitW w; Out o; int64_t done = 0; int thr = 0;   // thr: threads for the block plans, 0 = every core (FU13-7: the caller's count)
    LayerOut(const std::string& p, int64_t b, int threads = 0) : B(b), o(p), thr(threads) {}
    void feed(const int64_t* x, int64_t n) {
        const int64_t nb = (int64_t)((n + B - 1) / B); std::vector<laes::BlockPlan> plans((size_t)std::max<int64_t>(0, nb));
        // each thread takes the next block not yet taken (L5, 2026-11-03; blocks cost very different amounts, so fixed ranges left threads
        // waiting). Each block's plan depends only on its own samples, so the plans and the bytes written are the same.
        std::atomic<int64_t> next(0);
        auto work = [&]() { for (int64_t k = next++; k < nb; k = next++) { const int64_t s = (int64_t)k * B; plans[(size_t)k] = laes::planBlock(x + s, (int64_t)std::min<int64_t>(B, n - s)); } };
        unsigned hc = std::thread::hardware_concurrency(); int th = thr > 0 ? thr : (int)std::max(1u, hc); if (nb < 4) th = 1; th = (int)std::min<int64_t>(th, nb > 0 ? nb : 1);
        if (th <= 1) work();
        else { std::vector<std::thread> pool; std::vector<std::exception_ptr> xs((size_t)th);
            for (int t = 1; t < th; t++) { try { pool.emplace_back([&work, &xs, &next, nb, t]() { try { work(); } catch (...) { xs[(size_t)t] = std::current_exception(); next = nb; } }); } catch (...) { break; } }
            try { work(); } catch (...) { xs[0] = std::current_exception(); next = nb; }
            for (auto& p : pool) p.join(); for (auto& e : xs) if (e) std::rethrow_exception(e); }
        for (int64_t k = 0; k < nb; k++) { const int64_t s = (int64_t)k * B; laes::writeBlock(w, x + s, (int64_t)std::min<int64_t>(B, n - s), plans[(size_t)k]); }
        o.put(w.b.data(), w.b.size()); w.b.clear(); done += n; diskCheck(); }   // the whole bytes so far; the bits of a byte not yet full stay in w
    size_t finish() { const size_t bits = w.bits; w.flush(); o.put(w.b.data(), w.b.size()); w.b.clear(); o.close(); return bits; } };
// copies a byte range of one file into an output, with the payload's running CRC-32
struct CrcOut { Out& o; uint32_t crc = 0xFFFFFFFFu; explicit CrcOut(Out& out) : o(out) {}
    void put(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { crc ^= p[i]; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } o.put(p, n); }
    void u(uint64_t v, int n) { uint8_t t[8]; for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i)); put(t, (size_t)n); }
    void file(const std::string& p) { In in(p); std::vector<uint8_t> b; for (long long a = 0; a < in.n; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, in.n - a)); in.read(a, b.data(), b.size()); put(b.data(), b.size()); diskCheck(); } } };
// ---- a master (disk, 2026-11-05): its samples are read where they are, a WAV in place (no copy), a FLAC decoded once into the work
// folder at its own word length (raw: the sample file, rawOff: where the samples start, rawBy: bytes per sample); SHA-256, unused low
// bits w; its levels 0..stages-1 (samples >> w; 4 bytes per value when all fit, else 8; lby) - only the levels a candidate can read are
// kept, the others are removed once the level below is made - and its octave layers 1..stages coded (they do not depend on the file order)
struct Prep { std::string path, raw; long long rawOff = 0; int rawBy = 4, bits = 0, rate = 0, stages = 0, w = 0; bool derive = false; int threads = 1; int64_t frames = 0; uint8_t sha[32];
              std::vector<std::string> lev[2], dfile[2]; std::vector<int> lby[2]; std::vector<size_t> dbits[2];
              // --small-disk, a FLAC master (flacLive): no decoded copy; raw is the FLAC itself, its frames decoded again when read. fOff: the
              // byte offset of every frame and the end, fFirst: the first sample of every frame (kept in memory, 16 bytes per frame)
              bool flacLive = false; laesflac::Info fi; std::vector<long long> fOff; std::vector<int64_t> fFirst; };
// little-endian signed samples of by bytes (2, 3, 4) as int32 (a 32-bit sample keeps all 32 bits)
inline void unpackPcm(const uint8_t* q, size_t count, int by, int32_t* out) {
    for (size_t i = 0; i < count; i++, q += by) { uint32_t v = 0; for (int k = 0; k < by; k++) v |= (uint32_t)q[k] << (8 * k);
        if (by < 4 && (v >> (8 * by - 1) & 1)) v |= ~0u << (8 * by); out[i] = (int32_t)v; } }
inline std::string masterFlacFrame(const uint8_t* fp, size_t avail, const laesflac::Info& in, uint64_t frameNo, uint64_t have, V ch[2], size_t& used);
// frames [a, b) of a flacLive master as int32 L R: the frames that hold them decoded again, split over threads
inline void flacFrames(const Prep& m, int64_t a, int64_t b, int32_t* out) {
    if (a >= b) return; const std::vector<int64_t>& F = m.fFirst;
    const size_t f0 = (size_t)(std::upper_bound(F.begin(), F.end(), a) - F.begin()) - 1, f1 = (size_t)(std::lower_bound(F.begin(), F.end(), b) - F.begin());
    const size_t nf = f1 - f0; const int T = (int)std::max<size_t>(1, std::min<size_t>((size_t)std::max(1, m.threads), nf / 2)); std::vector<std::string> err((size_t)T);
    auto work = [&](int t) { try { const size_t i0 = f0 + nf * (size_t)t / (size_t)T, i1 = f0 + nf * (size_t)(t + 1) / (size_t)T; if (i0 >= i1) return;
            In in(m.raw); std::vector<uint8_t> fb((size_t)(m.fOff[i1] - m.fOff[i0])); in.read(m.fOff[i0], fb.data(), fb.size()); size_t at = 0;
            for (size_t i = i0; i < i1; i++) { V ch[2]; size_t used = 0; const std::string e = masterFlacFrame(fb.data() + at, fb.size() - at, m.fi, i, (uint64_t)F[i], ch, used);
                if (!e.empty()) { err[(size_t)t] = m.path + ": " + e; return; } at += used;
                for (size_t k = 0; k < ch[0].size(); k++) { const int64_t s = F[i] + (int64_t)k; if (s < a || s >= b) continue; out[(s - a) * 2] = (int32_t)ch[0][k]; out[(s - a) * 2 + 1] = (int32_t)ch[1][k]; } } }
        catch (const std::exception& x) { err[(size_t)t] = x.what(); } };
    if (T == 1) work(0); else { std::vector<std::thread> pool; for (int t = 1; t < T; t++) pool.emplace_back(work, t); work(0); for (auto& p : pool) p.join(); }
    for (auto& e : err) if (!e.empty()) fail(e); }
// one channel of a master's interleaved samples, shifted right by w (as RawSig over the int32 copy before)
struct PcmSig : Sig { std::unique_ptr<In> in; const Prep* pm; long long off; int by, c, w; std::vector<uint8_t> buf; std::vector<int32_t> x;
    PcmSig(const Prep& m, int ch) : pm(&m), off(m.rawOff), by(m.rawBy), c(ch), w(m.w) { n = m.frames;
        if (!m.flacLive) { in.reset(new In(m.raw)); if (off + n * 2 * by > in->n) fail("master samples shorter than the header says: " + m.raw); } }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); x.resize((size_t)(b - a) * 2);
        if (pm->flacLive) flacFrames(*pm, a, b, x.data());
        else { buf.resize((size_t)(b - a) * 2 * by); in->read(off + a * 2 * by, buf.data(), buf.size()); unpackPcm(buf.data(), x.size(), by, x.data()); }
        for (int64_t i = a; i < b; i++) out[i - a] = (int64_t)x[(size_t)(i - a) * 2 + c] >> w; } };
// frames [a, a + k) of a master as int32 L R (in: the master's sample file; not read for a flacLive master)
inline void masterFrames(In& in, const Prep& m, int64_t a, int64_t k, int32_t* out) { if (m.flacLive) { flacFrames(m, a, a + k, out); return; }
    std::vector<uint8_t> t((size_t)k * 2 * m.rawBy); in.read(m.rawOff + a * 2 * m.rawBy, t.data(), t.size()); unpackPcm(t.data(), (size_t)k * 2, m.rawBy, out); }
// a level file: by = 4 (int32) or 8 (int64) bytes per value
inline const std::string& keptLevel(const std::string& p) { if (p.empty()) fail("internal error: a level that was not kept"); return p; }
struct LevSig : Sig { In in; int by; std::vector<int32_t> buf;
    LevSig(const std::string& p, int b) : in(keptLevel(p)), by(b) { if (in.n % by) fail("bad signal file " + p); n = in.n / by; }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range");
        if (by == 8) { in.read(a * 8, out, (size_t)(b - a) * 8); return; } buf.resize((size_t)(b - a)); in.read(a * 4, buf.data(), buf.size() * 4); for (size_t i = 0; i < buf.size(); i++) out[i] = buf[i]; } };
// int64 values written in order: 4 bytes each while all fit in int32; at the first that does not, the file so far is widened to 8 bytes
struct LevOut { std::string p; std::unique_ptr<Out> o; int by = 4; std::vector<int32_t> t;
    explicit LevOut(const std::string& path) : p(path), o(new Out(path)) {}
    void put(const int64_t* d, size_t k) {
        if (by == 4) { bool fit = true; for (size_t i = 0; i < k && fit; i++) fit = d[i] >= INT32_MIN && d[i] <= INT32_MAX;
            if (fit) { t.resize(k); for (size_t i = 0; i < k; i++) t[i] = (int32_t)d[i]; o->put(t.data(), k * 4); return; }
            widen(); }
        o->put(d, k * 8); }
    void widen() { o->close(); o.reset(); const std::string q = p + ".i32"; std::error_code ec; std::filesystem::rename(p, q, ec); if (ec) fail("cannot rename " + p);
        { In in(q); o.reset(new Out(p)); std::vector<int32_t> a; V b; const long long P = 4ll << 20;
          for (long long x = 0; x < in.n; x += P) { const size_t k = (size_t)(std::min<long long>(P, in.n - x) / 4); a.resize(k); in.read(x, a.data(), k * 4); b.assign(a.begin(), a.end()); o->put(b.data(), k * 8); diskCheck(); } }
        std::filesystem::remove(q, ec); by = 8; }
    void close() { o->close(); } };
// --small-disk (D6, 2026-11-05): the largest level (stages - 1) is not kept; it is made again from the master whenever it is read (one
// lifting stage, the same values: fwdPiece gives the same samples for any range)
inline std::unique_ptr<Sig> levelOf(const Prep& m, int c, int k);
struct DerivedSig : Sig { std::unique_ptr<Sig> up; int th; V s, d;
    DerivedSig(const Prep& m, int c, int k) : up(levelOf(m, c, k + 1)), th(m.threads) { n = (up->n + 1) / 2; }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); fwdPiece(*up, a, b, s, d, th); std::copy(s.begin(), s.end(), out); } };
inline std::unique_ptr<Sig> levelOf(const Prep& m, int c, int k) { if (k >= m.stages) return std::unique_ptr<Sig>(new PcmSig(m, c));
    if (m.derive && m.lev[c][(size_t)k].empty()) return std::unique_ptr<Sig>(new DerivedSig(m, c, k)); return std::unique_ptr<Sig>(new LevSig(m.lev[c][(size_t)k], m.lby[c][(size_t)k])); }

// reads a 16 / 24 / 32-bit integer PCM WAV (PCM or EXTENSIBLE) as laes_tool readWav does (all 32 bits of a 32-bit file), header only first;
// or (probeMaster) a FLAC master's STREAMINFO: dataOff = its first audio frame, dataBytes = the size of its samples as PCM
struct WavInfo { int rate = 0, bits = 0; long long dataOff = -1, dataBytes = 0; int64_t frames = 0; bool flac = false; laesflac::Info fi;
    unsigned long long hdrLen = 0; };   // not 0: the header gives this data length (below 2^63), more than the file holds; dataBytes = what is there, the caller says so (2026-11-14)
inline std::string probeWav(const std::string& path, WavInfo& wi, long long* laeOff = nullptr, long long* laeLen = nullptr) {
    In in(path); uint8_t h[12]; if (in.n < 12) return path + ": not RIFF/WAVE"; in.read(0, h, 12);
    const bool rf64 = !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4);   // beyond 4 GB (EBU Tech 3306 / ITU-R BS.2088): 64-bit sizes in "ds64"
    if ((memcmp(h, "RIFF", 4) && !rf64) || memcmp(h + 8, "WAVE", 4)) return path + ": not RIFF/WAVE";
    long long pos = 12; int tag = 0, ch = 0; if (laeOff) *laeOff = -1;
    uint64_t dsData = 0, dsLae = 0;
    if (rf64) { uint8_t d[48] = {0}; if (in.n < 20 + 28) return path + ": RF64 without ds64"; in.read(12, d, 8); if (memcmp(d, "ds64", 4)) return path + ": RF64 without ds64";
        const uint32_t dl = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24; if (dl < 28) return path + ": RF64 ds64 too short";
        if (in.n < 20 + (long long)std::min<uint32_t>(dl, 40)) return path + ": RF64 ds64 truncated";   // FU13-8: a reason, not "read failed"
        in.read(20, d, (size_t)std::min<uint32_t>(dl, 40));
        auto le64 = [&](int at) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | d[at + i]; return v; }; dsData = le64(8);
        if (dl >= 40 && (d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24) >= 1 && !memcmp(d + 28, "LAE ", 4)) dsLae = le64(32); }
    while (pos + 8 <= in.n) { uint8_t c[8]; in.read(pos, c, 8); uint64_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint64_t)c[7] << 24; const long long body = pos + 8;
        if (rf64 && n == 0xFFFFFFFFu) n = !memcmp(c, "data", 4) ? dsData : !memcmp(c, "LAE ", 4) ? dsLae : n;
        if (!memcmp(c, "data", 4) && n >> 63) return path + ": the header gives a data length of 2^63 bytes or more";   // as a signed length it would be negative (2026-11-14)
        if (n > (uint64_t)(in.n - body)) { if (!memcmp(c, "data", 4)) wi.hdrLen = n; n = (uint64_t)(in.n - body); }   // a length beyond the end of the file: read as a cut-off file; compared unsigned, so pos only moves forward
        if (!memcmp(c, "fmt ", 4) && n >= 16) { uint8_t f[26] = {0}; in.read(body, f, n >= 26 ? 26 : 16); tag = f[0] | f[1] << 8; ch = f[2] | f[3] << 8; wi.rate = (int)(f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24); wi.bits = f[14] | f[15] << 8; if (tag == 0xFFFE && n >= 26) tag = f[24] | f[25] << 8; }
        else if (!memcmp(c, "data", 4)) { wi.dataOff = body; wi.dataBytes = (long long)n; }
        else if (!memcmp(c, "LAE ", 4) && laeOff) { *laeOff = body; *laeLen = (long long)n; }
        pos = body + (long long)n + (long long)(n & 1); }
    if (tag != 1 || ch != 2 || (wi.bits != 16 && wi.bits != 24 && wi.bits != 32) || wi.dataOff < 0) return path + ": need 16/24/32-bit integer PCM, 2 channels";
    wi.frames = (int64_t)(wi.dataBytes / (2 * (wi.bits / 8))); return ""; }
// the samples of frames [a, b) of a probed WAV as int32 L R
inline void wavFrames(In& in, const WavInfo& wi, long long a, long long b, std::vector<int32_t>& out) {
    const int by = wi.bits / 8; std::vector<uint8_t> t((size_t)(b - a) * 2 * by); in.read(wi.dataOff + a * 2 * by, t.data(), t.size()); out.resize((size_t)(b - a) * 2); unpackPcm(t.data(), out.size(), by, out.data()); }
// a FLAC master (2026-11-05): STREAMINFO (2 channels, 16 / 24 / 32 bits, a sample count) and where the audio frames start
inline std::string probeFlac(const std::string& path, WavInfo& wi) {
    In in(path); long long p = 4; bool last = false, haveSi = false; laesflac::Info& fi = wi.fi;
    while (!last) { if (p + 4 > in.n) return path + ": FLAC metadata truncated"; uint8_t bh[4]; in.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const long long len = (long long)bh[1] << 16 | bh[2] << 8 | bh[3]; p += 4;
        if (p + len > in.n) return path + ": FLAC metadata truncated";
        if (type == 0) { if (haveSi) return path + ": two STREAMINFO blocks (FLAC allows one)"; if (len != 34) return path + ": bad STREAMINFO"; uint8_t s[34]; in.read(p, s, 34); laesflac::BR r{s, 34}; fi.minBs = (int)r.get(16); fi.maxBs = (int)r.get(16); r.get(24); r.get(24);
            fi.rate = (int)r.get(20); fi.channels = (int)r.get(3) + 1; fi.bps = (int)r.get(5) + 1; fi.total = r.get(36); memcpy(fi.md5, s + 18, 16); haveSi = true; }
        p += len; }
    if (!haveSi) return path + ": no STREAMINFO";
    if (fi.channels != 2 || (fi.bps != 16 && fi.bps != 24 && fi.bps != 32) || fi.total == 0 || fi.maxBs < 16) return path + ": need a 2-channel 16/24/32-bit FLAC with a sample count";
    wi.flac = true; wi.rate = fi.rate; wi.bits = fi.bps; wi.frames = (int64_t)fi.total; wi.dataOff = p; wi.dataBytes = (long long)fi.total * 2 * (fi.bps / 8); return ""; }
inline std::string probeMaster(const std::string& path, WavInfo& wi) { uint8_t h[4] = {0}; { In in(path); if (in.n >= 4) in.read(0, h, 4); } return !memcmp(h, "fLaC", 4) ? probeFlac(path, wi) : probeWav(path, wi); }
// one FLAC frame of a master (laesflac::decodeFrame, plus the stereo decorrelations a FLAC encoder uses: left / side, side / right,
// mid / side, the side channel one bit wider). Appends to ch[0] / ch[1], used = its size in bytes.
inline std::string masterFlacFrame(const uint8_t* fp, size_t avail, const laesflac::Info& in, uint64_t frameNo, uint64_t have, V ch[2], size_t& used) {
    using namespace laesflac; BR r{fp, avail};
    if (r.get(15) != 0x7FFC) return "lost frame sync"; const int strategy = (int)r.get(1); const int bsc = (int)r.get(4), src = (int)r.get(4), chc = (int)r.get(4), ssc = (int)r.get(3);
    if (r.get(1)) return "frame header reserved bit set";
    if (chc != 1 && chc != 8 && chc != 9 && chc != 10) return "frame without 2 channels";
    uint64_t num = r.get(8); { int extra = 0; if (num == 0xFF) return "bad frame number"; if (num >= 0xFE) { if (strategy == 0) return "bad frame number"; extra = 6; } else if (num >= 0xFC) extra = 5; else if (num >= 0xF8) extra = 4; else if (num >= 0xF0) extra = 3; else if (num >= 0xE0) extra = 2; else if (num >= 0xC0) extra = 1; else if (num >= 0x80) return "bad frame number";
        if (extra) { num &= (0x3F >> extra); for (int i = 0; i < extra; i++) { const uint64_t b = r.get(8); if ((b & 0xC0) != 0x80) return "bad frame number"; num = num << 6 | (b & 0x3F); } } }
    int64_t n; if (bsc == 1) n = 192; else if (bsc >= 2 && bsc <= 5) n = 576L << (bsc - 2); else if (bsc == 6) n = (int64_t)r.get(8) + 1; else if (bsc == 7) { n = (int64_t)r.get(16) + 1; if (n == 65536) return "bad block size"; } else if (bsc >= 8) n = 256L << (bsc - 8); else return "bad block size";
    if (n > in.maxBs || have + (uint64_t)n > in.total) return "block larger than STREAMINFO allows";
    if (src == 12) r.get(8); else if (src == 13 || src == 14) r.get(16); else if (src == 15) return "bad sample rate code";
    const int bps = ssc == 0 ? in.bps : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 4 ? 16 : ssc == 5 ? 20 : ssc == 6 ? 24 : ssc == 7 ? 32 : 0; if (bps != in.bps) return "frame bit depth differs from STREAMINFO";
    if (r.fail || (r.pos & 7)) return "frame header truncated"; const size_t hb = r.pos / 8; const uint8_t c8 = (uint8_t)r.get(8); if (r.fail || c8 != crc8(fp, hb)) return "frame header CRC-8 mismatch";
    if (strategy == 0 && num != frameNo) return "frame number out of order";
    if (strategy == 1 && num != have) return "frame sample number out of order";   // variable block size: the header gives the first sample's number (FU13-8)
    const size_t base = ch[0].size(); for (int c = 0; c < 2; c++) { ch[c].resize(base + (size_t)n);
        const bool side = (chc == 8 && c == 1) || (chc == 9 && c == 0) || (chc == 10 && c == 1); if (!readSubframe(r, ch[c].data() + base, n, bps + (side ? 1 : 0))) return "damaged subframe"; }
    int64_t* a = ch[0].data() + base; int64_t* b = ch[1].data() + base;
    if (chc == 8) for (int64_t i = 0; i < n; i++) b[i] = a[i] - b[i];
    else if (chc == 9) for (int64_t i = 0; i < n; i++) a[i] = a[i] + b[i];
    else if (chc == 10) for (int64_t i = 0; i < n; i++) { const int64_t md = (int64_t)((uint64_t)a[i] << 1) | (b[i] & 1); a[i] = (md + b[i]) >> 1; b[i] = (md - b[i]) >> 1; }
    { const int64_t lim = 1LL << (bps - 1); for (int64_t i = 0; i < n; i++) if (a[i] < -lim || a[i] >= lim || b[i] < -lim || b[i] >= lim) return "a sample does not fit the bit depth of STREAMINFO"; }   // FU13-5: a stereo decorrelation can leave it
    r.pos = (r.pos + 7) & ~(size_t)7; const size_t fb = r.pos / 8; if (fb + 2 > r.n) return "frame truncated";
    if (crc16(fp, fb) != (uint16_t)(fp[fb] << 8 | fp[fb + 1])) return "frame CRC-16 mismatch";
    used = fb + 2; return ""; }
// step 1: the master's samples (one pass: SHA-256 of the samples as int32 LE, unused low bits). A WAV stays where it is; a FLAC is decoded
// into stem_pcm.raw at its own word length and checked against its STREAMINFO MD5.
inline std::string prepRead(const std::string& path, const std::string& stem, Prep& m) {
    WavInfo wi; std::string e = probeMaster(path, wi); if (!e.empty()) return e;
    m.path = path; m.rate = wi.rate; m.bits = wi.bits; m.frames = wi.frames; m.rawBy = wi.bits / 8; laes::Sha256 s; int64_t orv = 0;
    if (!wi.flac) { m.raw = path; m.rawOff = wi.dataOff; In in(path); std::vector<int32_t> x; const long long P = 1 << 20;
        for (long long a = 0; a < wi.frames; a += P) { const long long b = std::min<long long>(wi.frames, a + P); wavFrames(in, wi, a, b, x);
            for (int32_t v : x) orv |= (int64_t)v; s.update((const uint8_t*)x.data(), x.size() * 4); } }   // little-endian hosts
    else { m.flacLive = m.derive; m.raw = m.flacLive ? path : stem + "_pcm.raw"; m.rawOff = 0; m.fi = wi.fi; m.fOff.clear(); m.fFirst.clear(); In in(path); std::unique_ptr<Out> o; if (!m.flacLive) o.reset(new Out(m.raw));alignhash::Md5 md5; const laesflac::Info& fi = wi.fi;
        std::vector<uint8_t> fb, pb; std::vector<int32_t> x; size_t head = 0; long long at = wi.dataOff, fbOff = wi.dataOff; uint64_t frameNo = 0, have = 0; const int by = m.rawBy;
        for (;;) {
            if (at < in.n && fb.size() - head < (8u << 20)) { const size_t k = (size_t)std::min<long long>(16 << 20, in.n - at); const size_t o0 = fb.size(); fb.resize(o0 + k); in.read(at, fb.data() + o0, k); at += (long long)k; continue; }
            if (head >= fb.size() || have == fi.total) break;
            if (m.flacLive) { m.fOff.push_back(fbOff + (long long)head); m.fFirst.push_back((int64_t)have); }
            V ch[2]; size_t used = 0; e = masterFlacFrame(fb.data() + head, fb.size() - head, fi, frameNo, have, ch, used); if (!e.empty()) return path + ": " + e;
            head += used; frameNo++; const size_t k = ch[0].size(); have += k; x.resize(k * 2); for (size_t i = 0; i < k; i++) { x[i * 2] = (int32_t)ch[0][i]; x[i * 2 + 1] = (int32_t)ch[1][i]; }
            for (int32_t v : x) orv |= (int64_t)v; s.update((const uint8_t*)x.data(), x.size() * 4);
            pb.resize(x.size() * (size_t)by); for (size_t i = 0; i < x.size(); i++) for (int q = 0; q < by; q++) pb[i * (size_t)by + (size_t)q] = (uint8_t)((uint32_t)x[i] >> (8 * q));
            md5.update(pb.data(), pb.size()); if (o) o->put(pb.data(), pb.size());
            if (head > (32u << 20)) { fb.erase(fb.begin(), fb.begin() + (long long)head); fbOff += (long long)head; head = 0; diskCheck(); } }
        if (o) o->close(); if (m.flacLive) m.fOff.push_back(fbOff + (long long)head); if (have != fi.total) return path + ": sample count differs from STREAMINFO";
        bool zero = true; for (int i = 0; i < 16; i++) zero &= fi.md5[i] == 0;
        if (zero) printf("note: %s has no MD5 in its STREAMINFO: its decoding is checked only by this encoder's own FLAC reader\n", path.c_str());
        if (!zero) { const std::string hx = md5.hex(); char want[33]; for (int i = 0; i < 16; i++) snprintf(want + 2 * i, 3, "%02x", fi.md5[i]); if (hx != want) return path + ": decoded audio differs from its STREAMINFO MD5"; } }
    s.final(m.sha);
    m.w = 0; if (orv) { while (m.w < 31 && !((orv >> m.w) & 1)) m.w++; }   // laes::unusedBits
    return ""; }
// the two channels at once (speed, 2026-11-05): channel 1 in a thread of its own, each with half the threads and half the piece (about the
// memory of one channel after the other). Each channel's work reads and writes only that channel, so the files and values are the same.
// par: only when the signal is at least a whole piece long (shorter, two channels at once would hold more than one channel did)
template <class F> inline void bothChannels(bool par, F&& f) { if (!par) { f(0); f(1); return; }
    std::exception_ptr ex[2]; std::thread t([&] { try { f(1); } catch (...) { ex[1] = std::current_exception(); } });
    try { f(0); } catch (...) { ex[0] = std::current_exception(); } t.join(); for (auto& e : ex) if (e) std::rethrow_exception(e); }
inline int64_t halfPiece(int64_t piece) { return std::max<int64_t>(4096, piece / 2 / 4096 * 4096); }
// step 2: levels and octave layers (stages from the base rate). piece: base-level samples per piece (a multiple of 4096).
// otherStages: the stages of the other masters (disk, 2026-11-05): level k is kept when a candidate can read it - the base level (0), or
// the highest level this master shares with another (min of the two stages); the others are removed once the level below is made.
inline std::string prepLevels(Prep& m, int baseRate, const std::string& stem, int64_t piece, int threads, const std::vector<int>& otherStages) {
    int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
    m.stages = st; std::vector<bool> keep((size_t)st + 1, false); keep[0] = true; for (int o : otherStages) keep[(size_t)std::min(st, std::max(0, o))] = true;
    if (m.derive && st >= 1) keep[(size_t)st - 1] = false;   // --small-disk: the largest level is not kept (made again from the master, one stage)
    const bool par = laes::levLen(m.frames, st) >= piece; const int64_t pc = par ? halfPiece(piece) : piece; const int thc = par ? std::max(1, threads / 2) : threads;
    bothChannels(par, [&](int c) { m.lev[c].assign((size_t)st, std::string()); m.lby[c].assign((size_t)st, 4); m.dfile[c].assign((size_t)st + 1, std::string()); m.dbits[c].assign((size_t)st + 1, 0);
        for (int k = st; k > 0; k--) { { std::unique_ptr<Sig> x = levelOf(m, c, k); const int64_t ne = (x->n + 1) / 2;
                const std::string sc = stem + "_c" + std::to_string(c); m.lev[c][(size_t)(k - 1)] = sc + "_L" + std::to_string(k - 1); m.dfile[c][(size_t)k] = sc + "_D" + std::to_string(k);
                LevOut so(m.lev[c][(size_t)(k - 1)]); LayerOut lo(m.dfile[c][(size_t)k], 4096L << (k - 1), thc); V s, d; const int64_t P = pc << (k - 1);
                for (int64_t a = 0; a < ne; a += P) { fwdPiece(*x, a, std::min(ne, a + P), s, d, thc); so.put(s.data(), s.size()); diskCheck(); if (!d.empty()) lo.feed(d.data(), (int64_t)d.size()); }
                so.close(); m.lby[c][(size_t)(k - 1)] = so.by; m.dbits[c][(size_t)k] = lo.finish(); }
            if (k < st && !keep[(size_t)k]) { std::error_code ec; std::filesystem::remove(m.lev[c][(size_t)k], ec); m.lev[c][(size_t)k].clear(); } }   // read by no candidate
        if (st > 0 && !keep[0]) { std::error_code ec; std::filesystem::remove(m.lev[c][0], ec); m.lev[c][0].clear(); } });
    return ""; }
// resume: a prepared master (steps 1 and 2) as a state file, and back (false: missing, damaged or a file of it gone)
inline std::string prepState(const Prep& m) { StateW w; w.str(m.path); w.str(m.raw); w.i(m.rawOff); w.i(m.rawBy); w.i(m.bits); w.i(m.rate); w.i(m.stages); w.i(m.w); w.i(m.frames); w.hex(m.sha, 32);
    for (int c = 0; c < 2; c++) { w.i((int64_t)m.lev[c].size()); for (size_t k = 0; k < m.lev[c].size(); k++) { w.str(m.lev[c][k]); w.i(m.lby[c][k]); w.i(m.lev[c][k].empty() ? 0 : sizeOrMinus(m.lev[c][k])); }
        w.i((int64_t)m.dfile[c].size()); for (size_t k = 0; k < m.dfile[c].size(); k++) { w.str(m.dfile[c][k]); w.i((int64_t)m.dbits[c][k]); w.i(m.dfile[c][k].empty() ? 0 : sizeOrMinus(m.dfile[c][k])); } }
    w.i(m.flacLive); w.i(m.fi.rate); w.i(m.fi.channels); w.i(m.fi.bps); w.i(m.fi.minBs); w.i(m.fi.maxBs); w.i((int64_t)m.fi.total); w.hex(m.fi.md5, 16);
    w.i((int64_t)m.fOff.size()); for (size_t k = 0; k < m.fOff.size(); k++) { w.i(m.fOff[k]); } w.i((int64_t)m.fFirst.size()); for (size_t k = 0; k < m.fFirst.size(); k++) { w.i(m.fFirst[k]); }
    w.i(sizeOrMinus(m.raw)); w.str("end"); return w.s; }
inline bool prepFromState(const std::string& t, Prep& m) { StateR r(t); std::error_code ec;
    m.path = r.str(); m.raw = r.str(); m.rawOff = r.i(); m.rawBy = (int)r.i(); m.bits = (int)r.i(); m.rate = (int)r.i(); m.stages = (int)r.i(); m.w = (int)r.i(); m.frames = r.i(); r.hex(m.sha, 32);
    for (int c = 0; c < 2; c++) { const int64_t nl = r.i(); if (r.bad || nl < 0 || nl > 4) return false; m.lev[c].assign((size_t)nl, std::string()); m.lby[c].assign((size_t)nl, 4);
        for (int64_t k = 0; k < nl; k++) { m.lev[c][(size_t)k] = r.str(); m.lby[c][(size_t)k] = (int)r.i(); const long long n = r.i(); if (!m.lev[c][(size_t)k].empty() && sizeOrMinus(m.lev[c][(size_t)k]) != n) return false; }
        const int64_t nd = r.i(); if (r.bad || nd < 0 || nd > 5) return false; m.dfile[c].assign((size_t)nd, std::string()); m.dbits[c].assign((size_t)nd, 0);
        for (int64_t k = 0; k < nd; k++) { m.dfile[c][(size_t)k] = r.str(); m.dbits[c][(size_t)k] = (size_t)r.i(); const long long n = r.i(); if (!m.dfile[c][(size_t)k].empty() && sizeOrMinus(m.dfile[c][(size_t)k]) != n) return false; } }
    m.flacLive = r.i() != 0; m.fi.rate = (int)r.i(); m.fi.channels = (int)r.i(); m.fi.bps = (int)r.i(); m.fi.minBs = (int)r.i(); m.fi.maxBs = (int)r.i(); m.fi.total = (uint64_t)r.i(); r.hex(m.fi.md5, 16);
    const int64_t no = r.i(); if (r.bad || no < 0) return false; m.fOff.resize((size_t)no); for (int64_t k = 0; k < no; k++) m.fOff[(size_t)k] = r.i();
    const int64_t nf = r.i(); if (r.bad || nf < 0) return false; m.fFirst.resize((size_t)nf); for (int64_t k = 0; k < nf; k++) m.fFirst[(size_t)k] = r.i();
    const long long nr = r.i(); (void)ec; return !r.bad && r.str() == "end" && nr >= 0 && sizeOrMinus(m.raw) == nr; }   // every file whole, as it was written
// the core in the encoder (memory, 2026-11-05): int32 samples (every 16 / 24 / 32-bit core value fits), half of laes::Pcm's int64.
// toCore32 frees the Pcm's channels as it goes; hashCore32 = laes::hashAnyCore (the same bytes)
struct Core32 { int rate = 0, bits = 0; std::vector<int32_t> c[2]; int64_t frames() const { return (int64_t)c[0].size(); } };
inline Core32 toCore32(laes::Pcm& p) { Core32 k; k.rate = p.rate; k.bits = p.bits; for (int c = 0; c < 2; c++) {
        for (int64_t v : p.c[c]) if (v < INT32_MIN || v > INT32_MAX) fail("internal error: a core master sample beyond 32 bits");   // cannot happen: 16 / 24 / 32-bit cores only
        k.c[c].assign(p.c[c].begin(), p.c[c].end()); V().swap(p.c[c]); } return k; }
inline void hashCore32(const Core32& p, uint8_t out[32]) { laes::Sha256 s; std::vector<uint8_t> b; b.reserve(1 << 16); const int by = p.bits == 16 ? 2 : 4;   // int16 LE for 16 bit, else int32 LE
    for (int64_t i = 0; i < p.frames(); i++) for (int c = 0; c < 2; c++) { const uint32_t v = (uint32_t)p.c[c][(size_t)i]; for (int k = 0; k < by; k++) b.push_back((uint8_t)(v >> (8 * k))); if (b.size() >= (1 << 16)) { s.update(b.data(), b.size()); b.clear(); } }
    s.update(b.data(), b.size()); s.final(out); }
struct Core32Sig : Sig { const std::vector<int32_t>* v; explicit Core32Sig(const std::vector<int32_t>& x) : v(&x) { n = (int64_t)x.size(); }
    void get(int64_t a, int64_t b, int64_t* out) override { if (a < 0 || b > n) fail("internal error: signal range"); std::copy(v->begin() + a, v->begin() + b, out); } };
// the derived core straight into int32, a piece at a time (memory, 2026-11-05): the same values and random numbers in the same order as
// the former deriveCoreS (channel 0, then channel 1), without the whole int64 channel first
inline Core32 deriveCore32(const std::vector<Prep*>& ms, int base, int coreBits, size_t& src, bool& dithered, uint64_t seed) {
    src = 0; for (size_t i = 0; i < ms.size(); i++) if (ms[i]->bits > ms[src]->bits) src = i;
    for (size_t i = 0; i < ms.size(); i++) if (ms[i]->bits == coreBits) { src = i; break; }
    const Prep& m = *ms[src]; struct Rng { uint64_t s; explicit Rng(uint64_t sd) : s(sd * 2654435761ull + 88172645463325252ull) {} uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double uni() { return (double)(next() >> 11) / 9007199254740992.0 - 0.5; } } r(seed);
    Core32 core; core.rate = base; core.bits = coreBits; const int sig = m.bits - m.w, drop = sig - coreBits; const int64_t mx = (1LL << (coreBits - 1)) - 1, mn = -mx - 1; dithered = drop > 0;
    for (int c = 0; c < 2; c++) { std::unique_ptr<Sig> x = levelOf(m, c, 0); core.c[c].resize((size_t)x->n); V buf; const int64_t P = 1 << 20;
        for (int64_t a = 0; a < x->n; a += P) { const int64_t b = std::min(x->n, a + P); buf.resize((size_t)(b - a)); x->get(a, b, buf.data());
            for (int64_t i = a; i < b; i++) { const int64_t xv = buf[(size_t)(i - a)]; int64_t v;
                if (drop <= 0) v = xv << (-drop);
                else { const double u1 = r.uni(), u2 = r.uni(); v = (int64_t)std::floor((double)xv / std::ldexp(1.0, drop) + u1 + u2 + 0.5); }   // u1 first: the order is fixed (SPEC 11.1), not left to the compiler
                core.c[c][(size_t)i] = (int32_t)std::max(mn, std::min(mx, v)); } } }
    return core; }
// ---- the payload (laes::encodeLaes, block-wise). ms: the masters in file order with their storage choice (MasterIn without samples).
struct MasterSel { int storage = -1, refTier = 0; int64_t whole = 0; uint32_t frac = 0; std::string name; int flags = 0; int menu = 0; };   // name, flags, menu position: payload version 2 (laes_file.h)
// coded candidates shared between the file orders the tool tries (L5, 2026-11-03): a candidate's layers depend only on the master, its
// storage, the tier signal (the core or a given master), the level j and T, never on the file order. Key -> layer bits and the two layer
// sizes only (disk, 2026-11-05): a candidate's files are removed as soon as a smaller one is found, with or without a cache; the one chosen
// is coded again when its files are gone (the same bytes: the coding depends only on the key's inputs; its bits are checked to match).
struct CachedCand { double b[2] = {0, 0}; };   // the layer bits of each channel, added to the candidate's sum in the same order
using CandCache = std::map<std::string, CachedCand>;
inline std::string encodeLaesS(const Core32& core, int baseRate, const std::vector<const Prep*>& ms, const std::vector<MasterSel>& sel, std::vector<laes::MasterInfo>& info,
                               const std::string& bodyPath, const std::string& stem, int coreMode, int64_t piece, int threads, CandCache* cache = nullptr,
                               const std::string& cacheLog = std::string(), std::vector<std::string>* kept = nullptr,   // resume (2026-11-05): kept != null
                               const std::vector<laes::MasterInfo>* fixed = nullptr,   // laes add (2026-11-11): the first fixed->size() masters are already
                               const laes::Head& head = laes::Head()) {   // the default master and the core master's flags (2026-11-12)
    // in the file: their records (info) are taken as they are, nothing is coded or written for them, and later masters may refer to them
    // (ms and sel for them: their decoded samples and absolute T). The body then holds the header and the new masters only.
    using namespace laes; info.assign(ms.size(), MasterInfo()); const size_t nFixed = fixed ? fixed->size() : 0; for (size_t i = 0; i < nFixed && i < info.size(); i++) info[i] = (*fixed)[i]; uint8_t sh[32]; hashCore32(core, sh);
    if (core.bits != 16 && core.bits != 24 && core.bits != 32) return "core master word length must be 16, 24 or 32"; if (coreMode != 0 && coreMode != 1) return "core master mode must be 0 or 1";
    Out bo(bodyPath); CrcOut o(bo);
    if (ms.empty() || ms.size() > 255) return "1 to 255 masters";
    int ver = ms.size() > 16 ? 2 : 1; for (const MasterSel& x : sel) { if (!validMasterName(x.name)) return "a master name must be UTF-8 of at most 255 bytes without control characters"; if (x.flags & ~kNoDerive) return "unknown master flag"; if (!x.name.empty() || x.flags || x.menu) ver = 2; }
    { std::vector<MasterInfo> t(sel.size()); for (size_t i = 0; i < sel.size(); i++) t[i].menu = sel[i].menu; if (!menuOk(t)) return "menu positions must be 1 ... M, each once"; }
    if (head.def < 0 || head.def > (int)ms.size()) return "the default master must be 0 (the core master) or 1 ... M"; if (head.coreFlags & ~kNoDerive) return "unknown core master flag";
    if (head.def || head.coreFlags) ver = 2;
    o.u(0x3245414Cull, 4); o.u((uint64_t)ver, 2); o.u(64, 2); o.u((uint64_t)baseRate, 4); o.u((uint64_t)core.bits, 1); o.u(2, 1); o.u(ms.size(), 1); o.u((uint64_t)coreMode, 1); o.u((uint64_t)core.frames(), 8); o.put(sh, 32);
    { uint8_t hb[8]; headTo(ver, head, hb); o.put(hb, 8); }
    for (size_t mi = 0; mi < ms.size(); mi++) { if (mi < nFixed) { const Prep& m = *ms[mi]; if (m.bits != info[mi].bits || m.w != info[mi].w || m.rate != info[mi].rate || m.frames != info[mi].frames) return "master " + std::to_string(mi + 1) + " decoded differs from its record"; continue; }
        const Prep& m = *ms[mi]; MasterInfo& I = info[mi];
        if (m.bits != 16 && m.bits != 24 && m.bits != 32) return "master word length must be 16, 24 or 32";
        int st = 0; while ((baseRate << st) < m.rate && st < 4) st++; if ((baseRate << st) != m.rate || st > 3) return "master rate must be the base rate times 1, 2, 4 or 8";
        // FU13-5 (2026-11-14): input this format cannot hold is refused here, not first by the check of the written file
        const std::string mn = "master " + std::to_string(mi + 1);
        if (m.frames < 1) return mn + " has no samples";
        if ((uint64_t)levLen(m.frames, st) > (uint64_t)core.frames() + (1ull << 24)) return mn + " is too long: its base level may exceed the core master by at most 2^24 samples";
        if (sel[mi].storage >= 0 && (sel[mi].whole <= -(1LL << 40) || sel[mi].whole >= (1LL << 40))) return mn + ": T must lie within +-2^40 base samples";
        I.bits = m.bits; I.stages = st; I.w = m.w; I.rate = m.rate; I.frames = m.frames; memcpy(I.sha, m.sha, 32);
        auto above = [&](int j) { double b = 0; for (int c = 0; c < 2; c++) for (int k = j + 1; k <= st; k++) b += (double)m.dbits[c][(size_t)k]; return b; };
        struct Cand { int storage, tier, j; int64_t whole; uint32_t frac; double bits; std::string e[2]; bool coded; };
        std::vector<Cand> cs; const bool autoSel = sel[mi].storage < 0;
        auto relT = [&](int tier, int64_t& wh, uint32_t& fr) {
            if (!autoSel) { wh = sel[mi].whole; fr = sel[mi].frac; return true; }
            const int64_t lim = 1LL << 30; auto X = [&](size_t k) { return sel[k].whole * 4294967296LL + (int64_t)sel[k].frac; };
            if (sel[mi].whole <= -lim || sel[mi].whole >= lim || (tier && (sel[(size_t)tier - 1].whole <= -lim || sel[(size_t)tier - 1].whole >= lim))) return false;
            const int64_t x = X(mi) - (tier ? X((size_t)tier - 1) : 0); wh = x >> 32; fr = (uint32_t)(x & 0xFFFFFFFFLL); return true; };
        if (autoSel || sel[mi].storage == 1) cs.push_back({1, 0, 0, 0, 0, 0, {}, false});
        for (int t = 0; t <= (int)mi; t++) {
            if (!autoSel && t != sel[mi].refTier) continue; if (t > (int)mi) break;
            if (autoSel || sel[mi].storage == 0) cs.push_back({0, t, 0, 0, 0, 0, {}, false});
            if (t >= 1 && (autoSel || sel[mi].storage == 2)) { const int j = std::min(st, info[(size_t)t - 1].stages); if (j >= 1 || !autoSel) cs.push_back({2, t, j, 0, 0, 0, {}, false}); } }
        if (!autoSel && (sel[mi].refTier < 0 || sel[mi].refTier > (int)mi || sel[mi].storage > 2 || (sel[mi].storage == 2 && sel[mi].refTier == 0))) return "reference tier must be the core master or an earlier master (storage 2: an earlier master)";
        std::string cerr;   // codes a candidate into its two layer files; lb: the bits of each channel
        auto code = [&](Cand& C, double lb[2]) -> bool {
            const bool par = levLen(I.frames, st) >= piece; const int64_t pc = par ? halfPiece(piece) : piece; const int thc = par ? std::max(1, threads / 2) : threads;
            std::string ce[2];   // the two channels at once (par)
            if (C.storage == 1) { bothChannels(par, [&](int c) { std::unique_ptr<Sig> x = levelOf(m, c, 0); LayerOut lo(C.e[c], 4096, thc); V v; const int64_t P = pc;
                    for (int64_t a = 0; a < x->n; a += P) { const int64_t b = std::min(x->n, a + P); v.resize((size_t)(b - a)); x->get(a, b, v.data()); lo.feed(v.data(), (int64_t)v.size()); }
                    lb[c] = (double)lo.finish(); }); }
            else { const int j = C.j; const int64_t nj = levLen(I.frames, st - j);
                int up; if (C.tier == 0) up = (m.bits - I.w) - core.bits; else up = (m.bits - I.w) - (info[(size_t)C.tier - 1].bits - info[(size_t)C.tier - 1].w);
                const int64_t wj = C.whole * (1LL << j) + (j ? (int64_t)(C.frac >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)C.frac << j);
                const std::vector<int64_t> h = refTaps(fj);
                bothChannels(par, [&](int c) { std::unique_ptr<Sig> src; if (C.tier == 0) src.reset(new Core32Sig(core.c[c])); else src = levelOf(*ms[(size_t)C.tier - 1], c, j);
                    std::unique_ptr<Sig> x = levelOf(m, c, j); if (x->n != nj) { ce[c] = "internal error: level length"; return; }
                    LayerOut lo(C.e[c], 4096L << j, thc); V v, r; const int64_t P = pc << j;
                    for (int64_t a = 0; a < nj; a += P) { const int64_t b = std::min<int64_t>(nj, a + P); v.resize((size_t)(b - a)); x->get(a, b, v.data()); refPiece(*src, h, wj, up, a, b, r, thc);
                        for (size_t i = 0; i < v.size(); i++) v[i] -= r[i]; lo.feed(v.data(), (int64_t)v.size()); }
                    lb[c] = (double)lo.finish(); });
                for (int c = 0; c < 2; c++) if (!ce[c].empty()) { cerr = ce[c]; return false; } }
            C.coded = true; return true; };
        // more than 16 masters (2026-11-11, spec/lossless/SPEC.md, Section 11.2): every candidate is first coded on 4 excerpts of 2^15 base-rate samples
        // (x 2^j at level j) at positions given only by the length (the centres of the four quarters, aligned down to the block length),
        // never by the content; the 3 with the smallest estimate are coded in full and the smallest of those wins, as before. Up to 16
        // masters every candidate is coded in full (the file is the same byte for byte as before). Nothing about this is stored in the file.
        std::vector<bool> inShort; const size_t kShort = 3;
        auto makeShort = [&]() { if (!(autoSel && ms.size() > 16 && cs.size() > kShort) || getenv("LAES_NO_SHORTLIST")) return;   // the variable: tests (full search, to compare)
            std::vector<std::pair<double, size_t>> est;
            for (size_t q = 0; q < cs.size(); q++) { const Cand& C = cs[q]; int64_t wh = 0; uint32_t fr = 0; if (C.storage != 1 && !relT(C.tier, wh, fr)) continue;
                const int j = C.j; const int64_t nj = levLen(I.frames, st - j), B = 4096LL << j, L = (1LL << 15) << j;
                std::vector<std::pair<int64_t, int64_t>> segs; if (nj <= 4 * L) segs.push_back({0, nj});
                else for (int s = 0; s < 4; s++) { int64_t a = (2 * s + 1) * (nj / 8) + ((2 * s + 1) * (nj % 8)) / 8 - L / 2; a = a / B * B; a = std::max<int64_t>(0, std::min<int64_t>(a, nj - L) / B * B); segs.push_back({a, a + L}); }
                int64_t got = 0; double pb = 0; const std::string pf = stem + "_m" + std::to_string(mi) + "_probe.lay";
                int up = 0; if (C.storage != 1) up = C.tier == 0 ? (m.bits - I.w) - core.bits : (m.bits - I.w) - (info[(size_t)C.tier - 1].bits - info[(size_t)C.tier - 1].w);
                const int64_t wj = wh * (1LL << j) + (j ? (int64_t)(fr >> (32 - j)) : 0); const uint32_t fj = (uint32_t)((uint64_t)fr << j); const std::vector<int64_t> h = refTaps(fj);
                for (int c = 0; c < 2; c++) { std::unique_ptr<Sig> x = levelOf(m, c, j), src; if (C.storage != 1) { if (C.tier == 0) src.reset(new Core32Sig(core.c[c])); else src = levelOf(*ms[(size_t)C.tier - 1], c, j); }
                    LayerOut lo(pf, B, threads); V v, r;
                    for (const auto& sg : segs) { v.resize((size_t)(sg.second - sg.first)); x->get(sg.first, sg.second, v.data());
                        if (C.storage != 1) { refPiece(*src, h, wj, up, sg.first, sg.second, r, threads); for (size_t i = 0; i < v.size(); i++) v[i] -= r[i]; }
                        lo.feed(v.data(), (int64_t)v.size()); if (c == 0) got += (int64_t)v.size(); }
                    pb += (double)lo.finish(); remove(pf.c_str()); }
                est.push_back({above(j) + pb * (double)nj / (double)std::max<int64_t>(1, got), q}); }
            std::stable_sort(est.begin(), est.end(), [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) { return a.first < b.first; });
            inShort.assign(cs.size(), false); for (size_t k = 0; k < est.size() && k < kShort; k++) inShort[est[k].second] = true; };
        int best = -1; const std::string stP = stem + "_m" + std::to_string(mi) + ".done"; std::string stT; bool restored = false;
        if (kept && getState(stP, stT)) {   // resume: this master's choice was made before (its two layer files kept)
            StateR r(stT); const int64_t q = r.i(); Cand R; R.whole = r.i(); R.frac = (uint32_t)r.i(); R.bits = r.d(); R.e[0] = r.str(); const long long n0 = r.i(); R.e[1] = r.str(); const long long n1 = r.i();
            bool tOk = false; if (!r.bad && q >= 0 && q < (int64_t)cs.size()) { int64_t wh = 0; uint32_t fr = 0; tOk = cs[(size_t)q].storage == 1 ? (R.whole == 0 && R.frac == 0) : (relT(cs[(size_t)q].tier, wh, fr) && wh == R.whole && fr == R.frac); }   // the T of this run
            if (tOk && n0 >= 0 && sizeOrMinus(R.e[0]) == n0 && n1 >= 0 && sizeOrMinus(R.e[1]) == n1) {   // the files whole, as they were written
                Cand& C = cs[(size_t)q]; C.whole = R.whole; C.frac = R.frac; C.bits = R.bits; C.e[0] = R.e[0]; C.e[1] = R.e[1]; C.coded = true; best = (int)q; restored = true; } }
        if (best < 0) makeShort();
        if (best < 0) for (size_t q = 0; q < cs.size(); q++) { if (!inShort.empty() && !inShort[q]) continue; Cand& C = cs[q]; C.bits = above(C.j);
            for (int c = 0; c < 2; c++) C.e[c] = stem + "_m" + std::to_string(mi) + "_k" + std::to_string(q) + "_c" + std::to_string(c) + ".lay";
            if (C.storage != 1 && !relT(C.tier, C.whole, C.frac)) { C.e[0].clear(); continue; }
            std::string key; if (cache) { key = m.raw + "|" + std::to_string(C.storage) + "|" + (C.storage == 1 ? std::string("-") : C.tier == 0 ? std::string("core") : ms[(size_t)C.tier - 1]->raw) + "|" + std::to_string(C.j);
                if (C.storage != 1) key += "|" + std::to_string(C.whole) + "|" + std::to_string(C.frac); }
            auto hit = cache ? cache->find(key) : CandCache::iterator();
            if (cache && hit != cache->end()) { for (int c = 0; c < 2; c++) C.bits += hit->second.b[c]; }
            else { double lb[2]; if (!code(C, lb)) return cerr; for (int c = 0; c < 2; c++) { C.bits += lb[c]; if (cache) (*cache)[key].b[c] = lb[c]; }
                if (cache && !cacheLog.empty()) { FILE* f = fopen(cacheLog.c_str(), "ab"); if (!f) fail("cannot write " + cacheLog); fprintf(f, "%s\t%.17g\t%.17g\n", key.c_str(), lb[0], lb[1]); if (fclose(f)) fail("cannot write " + cacheLog); } }
            if (best < 0 || C.bits < cs[(size_t)best].bits) { if (best >= 0 && cs[(size_t)best].coded) for (int c = 0; c < 2; c++) remove(cs[(size_t)best].e[c].c_str()); best = (int)q; }
            else if (C.coded) for (int c = 0; c < 2; c++) remove(C.e[c].c_str()); }
        if (best < 0) return "no usable storage for master " + std::to_string(mi + 1);
        if (!cs[(size_t)best].coded) { Cand& B = cs[(size_t)best]; double lb[2]; if (!code(B, lb)) return cerr; double b = above(B.j); for (int c = 0; c < 2; c++) b += lb[c];
            if (b != B.bits) return "internal error: a candidate coded again differs in size"; }   // its files were removed in the other order
        if (kept && !restored) { const Cand& B = cs[(size_t)best]; StateW w; w.i(best); w.i(B.storage == 1 ? 0 : B.whole); w.i(B.storage == 1 ? 0 : B.frac); w.d(B.bits); w.str(B.e[0]); w.i(sizeOrMinus(B.e[0])); w.str(B.e[1]); w.i(sizeOrMinus(B.e[1])); putState(stP, w.s); }
        // DEC 15 (2026-11-15): a master stored in full keeps its own T (relative to the core master, its tier 0) in the record; decoders
        // do not use it (Section 6.6), but laes add / extract read it, so a master referring to it keeps its T relative to the core master
        Cand& B = cs[(size_t)best]; I.storage = B.storage; I.refTier = B.storage == 1 ? 0 : B.tier; I.whole = B.storage == 1 ? sel[mi].whole : B.whole; I.frac = B.storage == 1 ? sel[mi].frac : B.frac; I.layerBits = B.bits;
        I.name = sel[mi].name; I.flags = sel[mi].flags; I.menu = sel[mi].menu;
        o.u(I.bits, 1); o.u(I.stages, 1); o.u(I.w, 1); o.u(I.storage, 1); o.u(I.refTier, 1); o.u(ver == 2 ? I.name.size() : 0, 1); o.u(ver == 2 ? (uint64_t)I.flags : 0, 1); o.u(ver == 2 ? (uint64_t)I.menu : 0, 1); o.u((uint64_t)I.rate, 4); o.u(0, 4); o.u((uint64_t)I.frames, 8);
        o.u((uint64_t)I.whole, 8); o.u(I.frac, 4); o.u(0, 4); o.put(I.sha, 32); if (ver == 2) o.put((const uint8_t*)I.name.data(), I.name.size());
        for (int c = 0; c < 2; c++) { if (sizeOrMinus(B.e[c]) > 0xFFFFFFFFLL) return mn + ": a layer is larger than 4 GiB, more than an LAES layer can hold";   // the u32 layer length
            for (int k = B.j + 1; k <= st; k++) if (sizeOrMinus(m.dfile[c][(size_t)k]) > 0xFFFFFFFFLL) return mn + ": a layer is larger than 4 GiB, more than an LAES layer can hold"; }
        for (int c = 0; c < 2; c++) { { In e(B.e[c]); o.u((uint64_t)e.n, 4); } o.file(B.e[c]); if (kept) kept->push_back(B.e[c]); else remove(B.e[c].c_str());
            for (int k = B.j + 1; k <= st; k++) { { In d(m.dfile[c][(size_t)k]); o.u((uint64_t)d.n, 4); } o.file(m.dfile[c][(size_t)k]); } } }
    const uint32_t crc = o.crc ^ 0xFFFFFFFFu; uint8_t t[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)}; bo.put(t, 4); bo.close();
    return ""; }
}  // namespace laess
