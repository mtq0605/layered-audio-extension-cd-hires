// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// liblae: the C interface of lae_api.h on top of lae_decoder.h (the v5 decoder) and lae_cdx_read.h (base layer + extension file).
// Build (Windows): cl /LD /O2 /EHsc /std:c++17 /utf-8 src\lae_api.cpp /Fe:liblae.dll   (x64 or x86; LAE_API_BUILD and LAE_LIBRARY are set here)
#define LAE_API_BUILD
#define LAE_LIBRARY
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "lae_api.h"
#include "lae_decoder.h"
#include "lae_cdx_read.h"
#include "laes_file.h"   // laes::Sha256 (the file key of the seek checkpoints)
#include <mutex>
#include <chrono>
#include <array>
#include <filesystem>

namespace {
void setErr(char* err, int errlen, const char* m) { if (!err || errlen <= 0) return; size_t n = std::min(strlen(m), (size_t)errlen - 1); memcpy(err, m, n); err[n] = 0; }   // allocates nothing (safe in catch handlers)
void setErr(char* err, int errlen, const std::string& m) { setErr(err, errlen, m.c_str()); }
FILE* openU8(const char* path) {   // UTF-8 path
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0); if (wn <= 0) return nullptr;
    std::wstring w((size_t)wn, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &w[0], wn); return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path, "rb");
#endif
}
bool sizeOf(FILE* f, uint64_t& n) { long long e = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1; if (e < 0) return false; n = (uint64_t)e; return true; }
bool readU8(const char* path, std::vector<uint8_t>& b) {   // 64-bit sizes; refuses files that do not fit in memory
    FILE* f = openU8(path); if (!f) return false; uint64_t n = 0; bool ok = sizeOf(f, n) && n <= (size_t)-1 / 2 && _fseeki64(f, 0, SEEK_SET) == 0;
    if (ok) { try { b.resize((size_t)n); } catch (...) { ok = false; } }
    ok = ok && (n == 0 || fread(b.data(), 1, b.size(), f) == b.size()); fclose(f); return ok; }
int LAE_CALL readFile(void* ctx, uint64_t offset, void* buf, size_t len) {   // lae_read_fn over a FILE*
    FILE* f = (FILE*)ctx; return offset <= (uint64_t)INT64_MAX && _fseeki64(f, (long long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len ? 0 : -1; }
std::string md5hex(const std::vector<int16_t>& cd) {   // the whole-track MD5 over little-endian 16-bit samples, in blocks (no copy of the track)
    alignhash::Md5 m; uint8_t by[16384]; for (size_t i = 0; i < cd.size();) { const size_t n = std::min(cd.size() - i, sizeof by / 2);
        for (size_t k = 0; k < n; k++) { by[k * 2] = (uint8_t)cd[i + k]; by[k * 2 + 1] = (uint8_t)((uint16_t)cd[i + k] >> 8); } m.update(by, n * 2); i += n; }
    return m.hex(); }

// ---- the player's seek checkpoints (rule PL-7): made only by this library's own check of a file, kept only in the player's folder under
// the SHA-256 of the whole file, never written into or sent with the media file. Folder: %APPDATA%\LAE\lae_index (Windows) or
// $XDG_CACHE_HOME/lae/lae_index, ~/.cache/lae/lae_index; the environment variable LAE_INDEX_DIR overrides it ("off": no folder).
// At most 256 MB of .idx files: the least recently used ones are deleted (only *.idx in that folder). A missing or damaged record only
// means the check runs again; the output never depends on it (a seek from a checkpoint gives the samples of a decode from the start).
typedef std::array<uint8_t, 32> Sha;
const uint64_t INDEX_CAP = (uint64_t)256 << 20;
std::mutex g_indexMu;
std::string hexOf(const Sha& s) { static const char* d = "0123456789abcdef"; std::string r; for (uint8_t b : s) { r += d[b >> 4]; r += d[b & 15]; } return r; }
std::filesystem::path indexDir() {
#ifdef _WIN32
    if (const wchar_t* e = _wgetenv(L"LAE_INDEX_DIR")) { if (!wcscmp(e, L"off")) return {}; return std::filesystem::path(e); }
    if (const wchar_t* a = _wgetenv(L"APPDATA")) return std::filesystem::path(a) / L"LAE" / L"lae_index";
#else
    if (const char* e = getenv("LAE_INDEX_DIR")) { if (!strcmp(e, "off")) return {}; return std::filesystem::path(e); }
    if (const char* x = getenv("XDG_CACHE_HOME")) return std::filesystem::path(x) / "lae" / "lae_index";
    if (const char* h = getenv("HOME")) return std::filesystem::path(h) / ".cache" / "lae" / "lae_index";
#endif
    return {}; }
// record: "LAEYIDX1" | u32 version 1 | 32-byte key | u32 seconds between checkpoints | u32 streams | per stream: i64 K, i32 rate, k0, k1,
// u32 stream bytes, u32 checkpoints, each: i64 M0, u64 byte position, u32 width, u32 code, u64 symbols, i32 previous tier, i32 previous
// exponent, u32 values, values as u32 (index << 16 | value) | u32 CRC-32 of everything before
void putU(std::vector<uint8_t>& b, uint64_t v, int n) { for (int k = 0; k < n; k++) b.push_back((uint8_t)(v >> (8 * k))); }
std::vector<uint8_t> packIndex(const Sha& key, Decoder& D, const std::vector<std::vector<SavedMark>>& saved) {
    std::vector<uint8_t> b; const char* mg = "LAEYIDX1"; b.insert(b.end(), mg, mg + 8); putU(b, 1, 4); b.insert(b.end(), key.begin(), key.end());
    putU(b, (uint64_t)FILE_MARK_SECONDS, 4); std::vector<PlaneStream*> ps = D.allStreams(); putU(b, ps.size(), 4);
    for (size_t i = 0; i < ps.size(); i++) { const Hdr& h = ps[i]->h; putU(b, (uint64_t)h.K, 8); putU(b, (uint32_t)h.rate, 4); putU(b, (uint32_t)h.k0, 4); putU(b, (uint32_t)h.k1, 4);
        putU(b, ps[i]->dc0.n, 4); putU(b, saved[i].size(), 4);
        for (const SavedMark& s : saved[i]) { putU(b, (uint64_t)s.M0, 8); putU(b, s.i, 8); putU(b, s.width, 4); putU(b, s.code, 4); putU(b, s.pos, 8);
            putU(b, (uint32_t)s.prevTier, 4); putU(b, (uint32_t)s.prevE, 4); putU(b, s.diff.size(), 4); for (uint32_t x : s.diff) putU(b, x, 4); } }
    putU(b, crc32(b.data(), b.size()), 4); return b; }
// fills saved when the record belongs to this file and to these streams; any mismatch or damage: false (nothing used)
bool unpackIndex(const std::vector<uint8_t>& b, const Sha& key, Decoder& D, std::vector<std::vector<SavedMark>>& saved) {
    if (b.size() < 56 || memcmp(b.data(), "LAEYIDX1", 8)) return false;
    uint32_t stored = 0; memcpy(&stored, b.data() + b.size() - 4, 4); if (crc32(b.data(), b.size() - 4) != stored) return false;
    size_t at = 8; const size_t end = b.size() - 4; bool ok = true;
    auto get = [&](int n) -> uint64_t { if (at + (size_t)n > end) { ok = false; return 0; } uint64_t v = 0; for (int k = 0; k < n; k++) v |= (uint64_t)b[at + k] << (8 * k); at += (size_t)n; return v; };
    if (get(4) != 1 || memcmp(b.data() + at, key.data(), 32)) return false; at += 32;
    if (get(4) != (uint64_t)FILE_MARK_SECONDS) return false;
    std::vector<PlaneStream*> ps = D.allStreams(); if (get(4) != ps.size()) return false;
    std::vector<std::vector<SavedMark>> out(ps.size());
    for (size_t i = 0; i < ps.size() && ok; i++) { const Hdr& h = ps[i]->h; const size_t nModel = modelCount(h);
        if ((long)get(8) != h.K || (int)get(4) != h.rate || (int)get(4) != h.k0 || (int)get(4) != h.k1 || get(4) != ps[i]->dc0.n) return false;
        const uint64_t nm = get(4); if (nm > (uint64_t)h.K) return false; long last = 0;
        for (uint64_t j = 0; j < nm && ok; j++) { SavedMark s; s.M0 = (long)get(8); s.i = (size_t)get(8); s.width = (uint32_t)get(4); s.code = (uint32_t)get(4); s.pos = get(8);
            s.prevTier = (int)(int32_t)get(4); s.prevE = (int)(int32_t)get(4); const uint64_t nv = get(4); if (!ok || nv > nModel) return false;
            if (s.M0 <= last || s.M0 >= h.K || s.M0 % part::ROOTT || s.i > ps[i]->dc0.n || s.width < RC_TOP || s.prevTier < 0 || s.prevTier > 3 || s.prevE < 0 || s.prevE > 34) return false;
            for (uint64_t v = 0; v < nv && ok; v++) { const uint32_t x = (uint32_t)get(4); if ((x >> 16) >= nModel) return false; s.diff.push_back(x); }
            last = s.M0; out[i].push_back(std::move(s)); } }
    if (!ok || at != end) return false; saved.swap(out); return true; }
bool readPath(const std::filesystem::path& p, std::vector<uint8_t>& b) {
    std::error_code ec; const auto n = std::filesystem::file_size(p, ec); if (ec || n > ((uint64_t)64 << 20)) return false;
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = fopen(p.c_str(), "rb");
#endif
    if (!f) return false; b.resize((size_t)n); const bool ok = n == 0 || fread(b.data(), 1, b.size(), f) == b.size(); fclose(f); return ok; }
// writes the record (a temporary file renamed over the old one), then keeps the folder under INDEX_CAP (oldest *.idx deleted first)
void saveIndex(const std::filesystem::path& dir, const Sha& key, const std::vector<uint8_t>& rec) {
    std::lock_guard<std::mutex> g(g_indexMu); std::error_code ec; std::filesystem::create_directories(dir, ec);
    const std::filesystem::path target = dir / (hexOf(key) + ".idx"), tmp = dir / (hexOf(key) + ".idx.tmp" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
    FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
    FILE* f = fopen(tmp.c_str(), "wb");
#endif
    if (!f) return; const bool ok = fwrite(rec.data(), 1, rec.size(), f) == rec.size(); if (fclose(f) != 0 || !ok) { std::filesystem::remove(tmp, ec); return; }
    std::filesystem::rename(tmp, target, ec); if (ec) { std::filesystem::remove(tmp, ec); return; }
    struct Item { std::filesystem::path p; std::filesystem::file_time_type t; uint64_t n; }; std::vector<Item> items; uint64_t total = 0;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code e2; if (!it->is_regular_file(e2) || it->path().extension() != ".idx") continue;
        Item x{it->path(), it->last_write_time(e2), (uint64_t)it->file_size(e2)}; if (e2) continue; total += x.n; items.push_back(std::move(x)); }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t < b.t; });
    for (const Item& x : items) { if (total <= INDEX_CAP) break; if (x.p == target) continue; std::error_code e3; if (std::filesystem::remove(x.p, e3)) total -= x.n; } }
// the record of this file, if one is there and fits; a record that is used becomes the most recently used one
bool loadIndex(const std::filesystem::path& dir, const Sha& key, Decoder& D, std::vector<std::vector<SavedMark>>& saved) {
    std::lock_guard<std::mutex> g(g_indexMu); const std::filesystem::path p = dir / (hexOf(key) + ".idx"); std::vector<uint8_t> b;
    if (!readPath(p, b) || !unpackIndex(b, key, D, saved)) return false;
    std::error_code ec; std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec); return true; }
}

struct lae_track {
    std::vector<int16_t> cd; std::vector<uint8_t> lae; lae_info info{};
    std::unique_ptr<Decoder> dec; int route = -1, outRate = 0, wrate = 0, bits = 0; uint64_t pos = 0, outFrames = 0; bool err = false;
    std::vector<std::vector<SavedMark>> saved;   // seek checkpoints of every stream (from the check at open or the player's record)
};
// checks the v5 pair (all decoder checks) and fills the information; takes cd and lae. key: SHA-256 of the whole file (the player's
// checkpoint record is looked up by it); the first time a file is met, every entropy stream is checked to its end and the record is made
static lae_track* finish(std::vector<int16_t>&& cd, std::vector<uint8_t>&& lae, int pair, const Sha& key, char* err, int errlen) {
    auto t = std::make_unique<lae_track>(); t->cd.swap(cd); t->lae.swap(lae);
    { Decoder D; int code = 0; std::string e = D.openRef(t->cd, t->lae, "full", 0, &code); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
      lae_info& i = t->info; i.master_rate = D.rate; i.base_rate = D.base; i.stages = D.stages; i.cutoff_hz = D.fc; i.cd_frames = (uint64_t)D.ncd; i.master_frames = (uint64_t)D.Lm;
      const std::filesystem::path dir = indexDir();
      if (dir.empty() || !loadIndex(dir, key, D, t->saved)) {
          e = D.checkAll(t->saved, D.seekThreads); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
          if (!dir.empty()) saveIndex(dir, key, packIndex(key, D, t->saved)); } }
    std::string h = md5hex(t->cd); memcpy(t->info.cd_md5, h.c_str(), 33); t->info.from_pair = pair; return t.release(); }
static Sha shaOf(laes::Sha256& s) { Sha k{}; s.final(k.data()); return k; }
// C++ linkage (not inside extern "C": with /EHsc, exceptions leaving an extern "C" function would skip the destructors of its locals)
static lae_track* openBytes(const uint8_t* b, size_t len, char* err, int errlen) {
    int rate = 0; std::vector<int16_t> pcm; std::vector<uint8_t> lae; std::string e = cdxr::parseWav(b, len, rate, pcm, &lae); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
    if (lae.empty()) { setErr(err, errlen, "no LAE chunk: plain CD audio"); return nullptr; }
    laes::Sha256 s; s.update(b, len); return finish(std::move(pcm), std::move(lae), 0, shaOf(s), err, errlen); }
// the same RIFF walk as cdxr::parseWav (the last 'fmt ', 'data' and "LAE " chunk count; a truncated last chunk is tolerated), but through the
// host's reader: only the 'data' and "LAE " bodies are read, straight into the track's own vectors
static lae_track* openReader(lae_read_fn rd, void* ctx, uint64_t size, char* err, int errlen) {
    auto get = [&](uint64_t off, void* p, size_t n) { return rd(ctx, off, p, n) == 0; };
    auto u16 = [](const uint8_t* q) { return (int)(q[0] | q[1] << 8); }; auto u32 = [](const uint8_t* q) { return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24; };
    uint8_t h[12]; if (size < 12) { setErr(err, errlen, "not a WAV file"); return nullptr; }
    if (!get(0, h, 12)) { setErr(err, errlen, "cannot read the file"); return nullptr; }
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { setErr(err, errlen, "not a WAV file"); return nullptr; }
    uint64_t p = 12, dOff = 0, dLen = 0, lOff = 0, lLen = 0; int fmt = 0, ch = 0, bits = 0; bool haveFmt = false, haveData = false, haveLae = false;
    while (size - p >= 8) { uint8_t c[8]; if (!get(p, c, 8)) { setErr(err, errlen, "cannot read the file"); return nullptr; }
        const uint64_t body = p + 8; uint64_t n = u32(c + 4); if (n > size - body) n = size - body;
        if (!memcmp(c, "fmt ", 4)) { if (n < 16) { setErr(err, errlen, "bad fmt chunk"); return nullptr; }
            uint8_t f[26]; const size_t k = n >= 26 ? 26 : 16; if (!get(body, f, k)) { setErr(err, errlen, "cannot read the file"); return nullptr; }
            fmt = u16(f); ch = u16(f + 2); bits = u16(f + 14); if (fmt == 0xFFFE && k == 26) fmt = u16(f + 24); haveFmt = true; }
        else if (!memcmp(c, "data", 4)) { dOff = body; dLen = n; haveData = true; }
        else if (!memcmp(c, "LAE ", 4)) { lOff = body; lLen = n; haveLae = true; }
        if (n + (n & 1) >= size - body) break; p = body + n + (n & 1); }
    if (!haveFmt || !haveData) { setErr(err, errlen, "not a PCM WAV file"); return nullptr; }
    if (fmt != 1 || ch != 2 || bits != 16) { setErr(err, errlen, "not 16-bit stereo PCM"); return nullptr; }
    if (!haveLae || lLen == 0) { setErr(err, errlen, "no LAE chunk: plain CD audio"); return nullptr; }
    if (dLen / 4 * 4 > (size_t)-1 / 2 || lLen > (size_t)-1 / 2) { setErr(err, errlen, "the file is too large for this decoder"); return nullptr; }
    // the whole file once, in blocks (so that the host can abort between them): its SHA-256 (the key of the player's checkpoint record),
    // and the 'data' and "LAE " bodies copied into the track's own vectors
    std::vector<int16_t> pcm((size_t)(dLen / 4 * 2)); std::vector<uint8_t> lae((size_t)lLen); const uint64_t pcmLen = dLen / 4 * 4;
    laes::Sha256 sh; std::vector<uint8_t> blk((size_t)4 << 20);
    auto copyPart = [](uint64_t at, const uint8_t* q, uint64_t n, uint64_t off, uint64_t len, uint8_t* to) {   // the part of [at, at + n) inside [off, off + len)
        const uint64_t a = std::max(at, off), e = std::min(at + n, off + len); if (a < e) memcpy(to + (a - off), q + (a - at), (size_t)(e - a)); };
    for (uint64_t at = 0; at < size; at += blk.size()) { const uint64_t n = std::min<uint64_t>(blk.size(), size - at);
        if (!get(at, blk.data(), (size_t)n)) { setErr(err, errlen, "cannot read the file"); return nullptr; }
        sh.update(blk.data(), (size_t)n); copyPart(at, blk.data(), n, dOff, pcmLen, (uint8_t*)pcm.data()); copyPart(at, blk.data(), n, lOff, lLen, lae.data()); }
    const uint16_t one = 1; if (*(const uint8_t*)&one == 0) for (auto& s : pcm) s = (int16_t)(((uint16_t)s >> 8) | ((uint16_t)s << 8));   // big-endian host
    return finish(std::move(pcm), std::move(lae), 0, shaOf(sh), err, errlen); }

extern "C" {
LAE_API int LAE_CALL lae_api_version(void) { return LAE_API_VERSION; }
LAE_API int LAE_CALL lae_selftest(void) { try { return mdct::selftestOK() ? 1 : 0; } catch (...) { return 0; } }
LAE_API lae_track* LAE_CALL lae_open_file(const char* path, char* err, int errlen) {
    FILE* f = nullptr;
    try { uint64_t n = 0; if (!path || !(f = openU8(path)) || !sizeOf(f, n)) { if (f) fclose(f); setErr(err, errlen, "cannot read the file"); return nullptr; }
        lae_track* t = openReader(readFile, f, n, err, errlen); fclose(f); return t; }
    catch (...) { if (f) fclose(f); setErr(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API lae_track* LAE_CALL lae_open_memory(const void* wav, size_t len, char* err, int errlen) {
    try { if (!wav) { setErr(err, errlen, "no input"); return nullptr; } return openBytes((const uint8_t*)wav, len, err, errlen); }
    catch (...) { setErr(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API lae_track* LAE_CALL lae_open_reader(lae_read_fn read, void* ctx, uint64_t size, char* err, int errlen) {
    try { if (!read) { setErr(err, errlen, "no input"); return nullptr; } return openReader(read, ctx, size, err, errlen); }
    catch (...) { setErr(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API lae_track* LAE_CALL lae_open_pair(const int16_t* pcm, uint64_t frames, int rate, const void* lax, size_t laxlen, char* err, int errlen) {
    try { if (!pcm || !lax) { setErr(err, errlen, "no input"); return nullptr; }
        std::vector<cdxr::Ext> x(1); x[0].name = "extension file"; x[0].bytes.assign((const uint8_t*)lax, (const uint8_t*)lax + laxlen); cdxr::check(x[0]);
        if (!x[0].bad.empty()) { setErr(err, errlen, "extension file not usable: " + x[0].bad); return nullptr; }
        if ((uint32_t)rate != x[0].t.rate) { setErr(err, errlen, "the base layer's sample rate differs from the extension file"); return nullptr; }
        std::vector<int16_t> cd;
        if (frames == x[0].t.ncd) { cd.assign(pcm, pcm + frames * 2); if (md5hex(cd) != x[0].t.md5hex()) cd.clear(); }   // F3: the base layer is exactly the track
        if (cd.empty()) {   // otherwise the track is searched in the base layer by content (section 6, no TOC)
            cdxr::Disc d; cdxr::place(d, 0, pcm, (long long)frames); cdxr::Report r = cdxr::read(d, x, cdxr::WDEF);
            for (auto& tr : r.tracks) if (tr.state == cdxr::Track::PASS) cd.swap(tr.cd);
            if (cd.empty()) { setErr(err, errlen, "the base layer does not contain this track (" + (r.unused.empty() ? std::string("no match") : r.unused[0]) + "): play the base layer"); return nullptr; } }
        std::vector<uint8_t> block(x[0].bytes.begin(), x[0].bytes.begin() + (ptrdiff_t)x[0].blockLen);
        // a pair has no single file: the key is the SHA-256 of the track's CD audio (16-bit little-endian) followed by the extension file
        laes::Sha256 sh; const char* tag = "LAEYPAIR"; sh.update((const uint8_t*)tag, 8);
        { uint8_t by[16384]; for (size_t i = 0; i < cd.size();) { const size_t n = std::min(cd.size() - i, sizeof by / 2);
              for (size_t k = 0; k < n; k++) { by[k * 2] = (uint8_t)cd[i + k]; by[k * 2 + 1] = (uint8_t)((uint16_t)cd[i + k] >> 8); } sh.update(by, n * 2); i += n; } }
        sh.update(x[0].bytes.data(), x[0].bytes.size());
        return finish(std::move(cd), std::move(block), 1, shaOf(sh), err, errlen); }
    catch (...) { setErr(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API lae_track* LAE_CALL lae_open_pair_files(const char* base_wav, const char* lax_file, char* err, int errlen) {
    try { std::vector<uint8_t> b, x; if (!base_wav || !readU8(base_wav, b)) { setErr(err, errlen, "cannot read the base-layer file"); return nullptr; }
        if (!lax_file || !readU8(lax_file, x)) { setErr(err, errlen, "cannot read the extension file"); return nullptr; }
        int rate = 0; std::vector<int16_t> pcm; std::vector<uint8_t> lae; std::string e = cdxr::parseWav(b, rate, pcm, &lae); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
        return lae_open_pair(pcm.data(), pcm.size() / 2, rate, x.data(), x.size(), err, errlen); }
    catch (...) { setErr(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API int LAE_CALL lae_get_info(const lae_track* t, lae_info* out) { if (!t || !out) return -1; *out = t->info; return 0; }
LAE_API int LAE_CALL lae_start(lae_track* t, int route, int out_rate, char* err, int errlen) {
    try { if (!t) return -1; std::unique_ptr<Decoder> old = std::move(t->dec); t->route = -1; t->err = false; t->pos = 0;   // a failed start leaves the track unstarted (read/seek refuse it)
        if (route == LAE_ROUTE_CD) { if (out_rate && out_rate != t->info.base_rate) { setErr(err, errlen, "the CD route plays at the base rate only"); return -1; }
            t->route = route; t->outRate = 0; t->wrate = t->info.base_rate; t->bits = 16; t->outFrames = t->info.cd_frames; return 0; }
        const char* m = route == LAE_ROUTE_CD24 ? "cd24" : route == LAE_ROUTE_16 ? "16" : route == LAE_ROUTE_FULL ? "full" : nullptr;
        if (!m) { setErr(err, errlen, "unknown route"); return -1; }
        if (!mdct::selftestOK()) { setErr(err, errlen, "decoder self-test failed in this thread (floating-point state): play the CD layer"); return -1; }
        auto D = std::make_unique<Decoder>(); int code = 0;
        std::string e = D->openRef(t->cd, t->lae, m, route == LAE_ROUTE_CD24 ? 0 : out_rate, &code); if (!e.empty()) { setErr(err, errlen, e); return -1; }
        if (route == LAE_ROUTE_CD24 && out_rate && out_rate != D->wrate) { setErr(err, errlen, "the CD24 route plays at the base rate only"); return -1; }
        if (old) D->adoptMarks(*old);   // a restart or another route keeps the seek checkpoints decoded so far
        D->useSaved(t->saved);   // and the checkpoints of the check at open (or of the player's record of this file)
        t->route = route; t->outRate = out_rate; t->wrate = D->wrate; t->bits = 24; t->outFrames = (uint64_t)D->Lo; t->dec = std::move(D); return 0; }
    catch (...) { setErr(err, errlen, "out of memory or internal error"); return -1; } }
LAE_API int LAE_CALL lae_output_format(const lae_track* t, int* rate, int* bits, uint64_t* frames) {
    if (!t || t->route < 0) return -1; if (rate) *rate = t->wrate; if (bits) *bits = t->bits; if (frames) *frames = t->outFrames; return 0; }
LAE_API int64_t LAE_CALL lae_read(lae_track* t, int32_t* out, int64_t frames) {
    try { if (!t || t->route < 0 || !out || frames < 0 || t->err) return -1;
        if (t->route == LAE_ROUTE_CD) { uint64_t n = std::min<uint64_t>((uint64_t)frames, t->outFrames - t->pos);
            for (uint64_t i = 0; i < n * 2; i++) out[i] = t->cd[(size_t)(t->pos * 2 + i)]; t->pos += n; return (int64_t)n; }
        if (!t->dec) return -1;
        bool before = g_streamErr; g_streamErr = false;   // the flag is per thread: attribute this call's errors to this track only
        int64_t done = 0; while (done < frames) { long chunk = (long)std::min<int64_t>(frames - done, 1 << 20); long n = t->dec->read(out + done * 2, chunk); if (n <= 0) break; done += n; }
        bool e = g_streamErr; g_streamErr = before; t->pos += (uint64_t)done; if (e) { t->err = true; return -1; } return done; }
    catch (...) { t->err = true; return -1; } }
LAE_API int LAE_CALL lae_seek(lae_track* t, uint64_t frame) {
    try { if (!t || t->route < 0) return -1; if (frame > t->outFrames) frame = t->outFrames;
        if (t->route == LAE_ROUTE_CD) { t->pos = frame; return 0; }
        if (!t->dec) return -1;
        if (frame != t->pos && t->dec->seek((long)frame)) { t->pos = frame; return 0; }   // bit-exact fast seek (checkpoints + warm restart)
        if (frame < t->pos) { if (lae_start(t, t->route, t->outRate, nullptr, 0)) return -1; }   // near the start: restart and decode forward
        std::vector<int32_t> scratch(2 * 8192); while (t->pos < frame) { int64_t n = lae_read(t, scratch.data(), (int64_t)std::min<uint64_t>(8192, frame - t->pos)); if (n <= 0) return -1; }
        return 0; }
    catch (...) { return -1; } }
LAE_API int LAE_CALL lae_stats(const lae_track* t, uint64_t* limited, int* stream_error) {
    if (!t) return -1; if (limited) *limited = t->dec ? (uint64_t)t->dec->clip : 0; if (stream_error) *stream_error = t->err ? 1 : 0; return 0; }
LAE_API int LAE_CALL lae_stream_bytes(const lae_track* t, uint64_t* bytes) {
    if (!t || !bytes || t->route < 0) return -1; *bytes = t->dec ? t->dec->streamBytes() : 0; return 0; }
LAE_API void LAE_CALL lae_close(lae_track* t) { delete t; }
}
