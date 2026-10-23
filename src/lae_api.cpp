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
}

struct lae_track {
    std::vector<int16_t> cd; std::vector<uint8_t> lae; lae_info info{};
    std::unique_ptr<Decoder> dec; int route = -1, outRate = 0, wrate = 0, bits = 0; uint64_t pos = 0, outFrames = 0; bool err = false;
};
// checks the v5 pair (all decoder checks) and fills the information; takes cd and lae
static lae_track* finish(std::vector<int16_t>&& cd, std::vector<uint8_t>&& lae, int pair, char* err, int errlen) {
    auto t = std::make_unique<lae_track>(); t->cd.swap(cd); t->lae.swap(lae);
    { Decoder D; int code = 0; std::string e = D.openRef(t->cd, t->lae, "full", 0, &code); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
      lae_info& i = t->info; i.master_rate = D.rate; i.base_rate = D.base; i.stages = D.stages; i.cutoff_hz = D.fc; i.cd_frames = (uint64_t)D.ncd; i.master_frames = (uint64_t)D.Lm; }
    std::string h = md5hex(t->cd); memcpy(t->info.cd_md5, h.c_str(), 33); t->info.from_pair = pair; return t.release(); }
// C++ linkage (not inside extern "C": with /EHsc, exceptions leaving an extern "C" function would skip the destructors of its locals)
static lae_track* openBytes(const uint8_t* b, size_t len, char* err, int errlen) {
    int rate = 0; std::vector<int16_t> pcm; std::vector<uint8_t> lae; std::string e = cdxr::parseWav(b, len, rate, pcm, &lae); if (!e.empty()) { setErr(err, errlen, e); return nullptr; }
    if (lae.empty()) { setErr(err, errlen, "no LAE chunk: plain CD audio"); return nullptr; }
    return finish(std::move(pcm), std::move(lae), 0, err, errlen); }
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
    auto bulk = [&](uint64_t off, uint8_t* q, uint64_t n) { const uint64_t step = (uint64_t)4 << 20;   // in blocks, so that the host can abort between them
        for (uint64_t i = 0; i < n; i += step) if (!get(off + i, q + i, (size_t)std::min(step, n - i))) return false; return true; };
    std::vector<int16_t> pcm((size_t)(dLen / 4 * 2)); std::vector<uint8_t> lae((size_t)lLen);
    if (!bulk(dOff, (uint8_t*)pcm.data(), dLen / 4 * 4) || !bulk(lOff, lae.data(), lLen)) { setErr(err, errlen, "cannot read the file"); return nullptr; }
    const uint16_t one = 1; if (*(const uint8_t*)&one == 0) for (auto& s : pcm) s = (int16_t)(((uint16_t)s >> 8) | ((uint16_t)s << 8));   // big-endian host
    return finish(std::move(pcm), std::move(lae), 0, err, errlen); }

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
        return finish(std::move(cd), std::move(block), 1, err, errlen); }
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
