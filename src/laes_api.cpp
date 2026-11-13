// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// liblae, LAE Lossless part: the laes_* functions of lae_api.h on top of laes_play.h. Compile with strict floating point (the reference
// taps are computed in double): MSVC /fp:strict, GCC / Clang -ffp-contract=off. Link together with lae_api.cpp.
#ifndef LAE_STATIC
#define LAE_API_BUILD
#endif
#ifdef _WIN32
#define NOMINMAX
#endif
#include "lae_api.h"
#include "laes_play.h"

struct laes_file { laesp::FnSrc src; laesp::File f; };
struct laes_play { laesp::Player p; std::string err; };
namespace {
void setErrS(char* err, int errlen, const std::string& m) { if (!err || errlen <= 0) return; size_t n = std::min(m.size(), (size_t)errlen - 1); memcpy(err, m.data(), n); err[n] = 0; }
}
extern "C" {
LAE_API laes_file* LAE_CALL laes_open_reader(lae_read_fn read, void* ctx, uint64_t size, int quick_info, char lae_magic[5], char* err, int errlen) {
    char mg[5] = {0}; if (lae_magic) lae_magic[0] = 0;
    if (!read) { setErrS(err, errlen, "no read function"); return nullptr; }
    laes_file* h = nullptr;
    try { h = new laes_file(); h->src.n = size; h->src.fn = [read, ctx](uint64_t at, void* d, size_t k) { return read(ctx, at, d, k) == 0 ? 0 : 1; };
        const std::string e = h->f.open(h->src, mg, quick_info != 0); if (lae_magic) memcpy(lae_magic, mg, 5);
        if (!e.empty()) { setErrS(err, errlen, e); delete h; return nullptr; }
        return h; }
    catch (const std::bad_alloc&) { setErrS(err, errlen, "out of memory"); }
    catch (const std::exception& x) { setErrS(err, errlen, x.what()); }
    delete h; return nullptr; }
LAE_API int LAE_CALL laes_get_info(const laes_file* h, laes_file_info* out) {
    if (!h || !out) return -1; const laesp::File& f = h->f;
    out->is_flac = f.flac; out->base_rate = f.baseRate; out->core_bits = f.coreBits; out->choices = (int)f.choices.size(); out->core_frames = (uint64_t)f.coreFrames;
    out->payload_ok = f.why.empty(); memcpy(out->file_sha256, f.fileSha, 32); out->quick = f.quick; return 0; }
LAE_API const char* LAE_CALL laes_file_problem(const laes_file* h) { return h ? h->f.why.c_str() : "no file"; }
LAE_API int LAE_CALL laes_get_choice(const laes_file* h, int i, laes_choice_info* out) {
    if (!h || !out || i < 0 || i >= (int)h->f.choices.size()) return -1; const laesp::Choice& c = h->f.choices[(size_t)i];
    out->kind = c.kind; out->master = c.m; out->level = c.level; out->rate = c.rate; out->bits = c.bits; out->frames = (uint64_t)c.frames; out->name = c.name.c_str(); out->what = c.what.c_str(); return 0; }
LAE_API int LAE_CALL laes_get_tag(const laes_file* h, int i, const char** key, const char** value) {
    if (!h || i < 0 || i >= (int)h->f.tags.size()) return -1; if (key) *key = h->f.tags[(size_t)i].first.c_str(); if (value) *value = h->f.tags[(size_t)i].second.c_str(); return 0; }
LAE_API const char* LAE_CALL laes_master_name(const laes_file* h, int m) {
    if (!h || !h->f.why.empty() || m < 0 || m >= (int)h->f.ms.size()) return nullptr; return h->f.ms[(size_t)m].I.name.c_str(); }
LAE_API int LAE_CALL laes_master_flags(const laes_file* h, int m) {
    if (!h || !h->f.why.empty() || m < 0 || m >= (int)h->f.ms.size()) return -1; return h->f.ms[(size_t)m].I.flags; }
LAE_API int LAE_CALL laes_master_number(const laes_file* h, int m) {
    if (!h || !h->f.why.empty() || m < 0 || m >= (int)h->f.ms.size()) return 0; std::vector<laes::MasterInfo> mi; for (const auto& M : h->f.ms) mi.push_back(M.I); return laesc::displayNo(mi, (size_t)m); }
LAE_API int LAE_CALL laes_default_choice(const laes_file* h) { return h ? h->f.defaultChoice : 0; }
LAE_API int LAE_CALL laes_core_flags(const laes_file* h) { return h && h->f.why.empty() ? h->f.head.coreFlags : 0; }
LAE_API int LAE_CALL laes_choice_from(const laes_file* h, int i) {
    if (!h || i < 0 || i >= (int)h->f.choices.size()) return -1; const laesp::Choice& c = h->f.choices[(size_t)i]; return c.kind == 3 ? c.from : -1; }
static laes_play* startImpl(laes_file* h, int choice, int threads, bool all, char* err, int errlen, uint64_t frame = 0) {
    if (!h) { setErrS(err, errlen, "no file"); return nullptr; }
    laes_play* p = nullptr;
    try { p = new laes_play(); if (threads <= 0) threads = std::max(1, laesfast::defaultThreads() / 2);
        const std::string e = p->p.start(h->f, choice, threads, all, (int64_t)frame); if (!e.empty()) { setErrS(err, errlen, e); delete p; return nullptr; } return p; }
    catch (const std::bad_alloc&) { setErrS(err, errlen, "out of memory"); }
    catch (const std::exception& x) { setErrS(err, errlen, x.what()); }
    delete p; return nullptr; }
LAE_API laes_play* LAE_CALL laes_start(laes_file* h, int choice, int threads, char* err, int errlen) { return startImpl(h, choice, threads, false, err, errlen); }
LAE_API laes_play* LAE_CALL laes_start_all(laes_file* h, int choice, int threads, char* err, int errlen) { return startImpl(h, choice, threads, true, err, errlen); }
LAE_API laes_play* LAE_CALL laes_start_at(laes_file* h, int choice, uint64_t frame, int threads, char* err, int errlen) { return startImpl(h, choice, threads, false, err, errlen, frame); }
LAE_API void LAE_CALL laes_index_layers(laes_file* h, const volatile int* cancel, int threads) {
    if (!h) return; std::atomic<bool> stop{false}; std::atomic<bool> done{false};
    std::thread watch([&] { while (!done) { if (cancel && *cancel) stop = true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); } });
    try { h->f.indexLayers(&stop, threads > 0 ? threads : std::max(1, laesfast::defaultThreads() / 2)); } catch (...) {}
    done = true; watch.join(); }
LAE_API int64_t LAE_CALL laes_index_save(laes_file* h, void* buf, int64_t cap) {
    if (!h) return 0; try { const std::vector<uint8_t> r = h->f.exportIndex(); if (buf && cap >= (int64_t)r.size()) memcpy(buf, r.data(), r.size()); return (int64_t)r.size(); } catch (...) { return 0; } }
LAE_API int LAE_CALL laes_index_load(laes_file* h, const void* data, int64_t len) {
    if (!h || !data || len <= 0) return -1; try { return h->f.importIndex((const uint8_t*)data, (size_t)len) ? 0 : -1; } catch (...) { return -1; } }
LAE_API int64_t LAE_CALL laes_read(laes_play* p, int32_t* out, int64_t frames) {
    if (!p || !out || frames < 0) return -1;
    try { const int64_t k = p->p.read(out, frames); return p->p.err.empty() ? k : -1; }
    catch (const std::bad_alloc&) { p->p.err = "out of memory"; } catch (const std::exception& x) { p->p.err = x.what(); }
    return -1; }
LAE_API int LAE_CALL laes_verified(const laes_play* p) { if (!p) return -1; if (!p->p.err.empty()) return -1; return p->p.verified() ? 1 : 0; }
LAE_API const char* LAE_CALL laes_play_error(const laes_play* p) {
    if (!p) return "no decoder"; if (!p->p.err.empty()) return p->p.err.c_str(); if (p->p.done && !p->p.ok) return "SHA-256 mismatch: the decoded master differs from the original"; return ""; }
LAE_API int LAE_CALL laes_master_ok(const laes_play* p, int m) { return p && p->p.err.empty() ? p->p.masterOk(m) : -1; }
LAE_API void LAE_CALL laes_stop(laes_play* p) { delete p; }
LAE_API void LAE_CALL laes_close(laes_file* h) { delete h; }
}
