// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// liblae, the part for both formats (API version 6, 2026-11-10): lae_probe_* and lae_any_* of lae_api.h.
// Written on top of the public lae_* (LAEY, lae_api.cpp) and laes_* (LAES, laes_api.cpp) functions only: no decoding is done here and
// nothing in the two decoders changes. Link the three files together. No floating point is used in this file.
// The format of a file is decided by its content alone (the first 4 bytes of the extension: "LAE1" = LAEY, "LAE2" = LAES); the file
// name is never looked at. The checks are the ones each format defines; they are listed as they are, not made alike.
#ifndef LAE_STATIC
#define LAE_API_BUILD
#endif
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _fseeki64 fseeko   // 64-bit file offsets
#define _ftelli64 ftello
#endif
#include "lae_api.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace {
void setE(char* err, int errlen, const std::string& m) { if (!err || errlen <= 0) return; const size_t n = std::min(m.size(), (size_t)errlen - 1); memcpy(err, m.data(), n); err[n] = 0; }
FILE* openPath(const char* path) {   // UTF-8 path
#ifdef _WIN32
    const int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0); if (wn <= 0) return nullptr;
    std::wstring w((size_t)wn, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &w[0], wn); return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path, "rb");
#endif
}
bool sizeOfFile(FILE* f, uint64_t& n) { const long long e = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1; if (e < 0) return false; n = (uint64_t)e; return true; }
int LAE_CALL readFp(void* ctx, uint64_t offset, void* buf, size_t len) {   // lae_read_fn over a FILE*
    FILE* f = (FILE*)ctx; return offset <= (uint64_t)INT64_MAX && _fseeki64(f, (long long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len ? 0 : -1; }
std::string hexOf(const unsigned char* p, int n) { static const char* d = "0123456789abcdef"; std::string r; for (int i = 0; i < n; i++) { r += d[p[i] >> 4]; r += d[p[i] & 15]; } return r; }

struct Choice { int route = -1, kind = -1, idx = -1, pack = 0, rate = 0, bits = 0; uint64_t frames = 0; std::string name, what; };
struct Check { std::string method, object, detail; int result = -1; bool atEnd = false; int master = -1; };

// the container from the first bytes, the format from the extension's first 4 bytes. The walk over the chunks / metadata blocks is the
// one of the LAES reader's quick open (RIFF, RF64 / BW64 and FLAC; it names the magic it met), so both formats are found the same way.
// Returns 0 (o filled; format NONE for a plain file) or -1 with the reason.
int probe(lae_read_fn rd, void* ctx, uint64_t size, lae_probe_info* o, std::string& why) {
    memset(o, 0, sizeof *o); why.clear();
    if (!rd) { why = "no read function"; return -1; }
    if (size < 12) return 0;
    unsigned char h[12]; if (rd(ctx, 0, h, 12) != 0) { why = "cannot read the file"; return -1; }
    if (!memcmp(h, "fLaC", 4)) o->container = LAE_CONTAINER_FLAC;
    else if ((!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4)) o->container = LAE_CONTAINER_WAV;
    else return 0;
    char mg[5] = {0}, le[256] = {0}; laes_file* q = laes_open_reader(rd, ctx, size, 1, mg, le, (int)sizeof le);
    if (q) { laes_close(q); o->format = LAE_FORMAT_LAES; memcpy(o->magic, "LAE2", 5); return 0; }
    if (!strcmp(mg, "LAE1")) { o->format = LAE_FORMAT_LAEY; memcpy(o->magic, "LAE1", 5); return 0; }
    if (!strcmp(le, "not LAES")) return 0;   // a plain WAV / FLAC file
    why = le; return -1; }
}

struct lae_any {
    FILE* fp = nullptr; lae_read_fn rd = nullptr; void* ctx = nullptr; uint64_t size = 0;
    lae_probe_info pi{}; lae_track* y = nullptr; laes_file* s = nullptr; laes_play* sp = nullptr;
    std::vector<Choice> ch; std::vector<Check> ck; std::string identity, err;
    int version = 0, baseRate = 0, baseBits = 0, defChoice = 0, usable = 1;
    int cur = -1, flags = 0, threads = 0; uint64_t pos = 0; bool ended = false, seeked = false;
    ~lae_any() { if (sp) laes_stop(sp); if (s) laes_close(s); if (y) lae_close(y); if (fp) fclose(fp); }
};

namespace {
void addCheck(lae_any& a, const char* method, const std::string& object, int result, const std::string& detail, bool atEnd = false, int master = -1) {
    Check c; c.method = method; c.object = object; c.result = result; c.detail = detail; c.atEnd = atEnd; c.master = master; a.ck.push_back(c); }

// LAEY: the track is opened by lae_open_reader, which makes every check of the format; a file that fails one does not open
std::string fillLaey(lae_any& a) {
    char le[400] = {0}; a.y = lae_open_reader(a.rd, a.ctx, a.size, le, (int)sizeof le);
    if (!a.y) return std::string("LAEY file (extension LAE1), not usable: ") + le;
    lae_info in; if (lae_get_info(a.y, &in)) return "LAEY: no track information";
    a.version = 5; a.baseRate = in.base_rate; a.baseBits = 16; // the layer every player reads is the CD layer only at 44.1 kHz; at 48 kHz it is the core layer (a CD holds 44.1 kHz only)
    const bool cd = in.base_rate == 44100; const std::string bl = cd ? "CD layer" : "core layer";
    a.identity = bl + " MD5 " + in.cd_md5;
    auto add = [&](int route, int rate, const std::string& name, const std::string& what) {   // only what the decoder itself accepts
        if (lae_start(a.y, route, route == LAE_ROUTE_16 || route == LAE_ROUTE_FULL ? rate : 0, nullptr, 0)) return;
        Choice c; c.route = route; c.name = name; c.what = what; int r = 0, b = 0; uint64_t f = 0; if (lae_output_format(a.y, &r, &b, &f)) return;
        c.rate = r; c.bits = b; c.frames = f; a.ch.push_back(c); };
    add(LAE_ROUTE_CD, 0, cd ? "cd" : "core", "the " + bl + " (what a player without LAE support reads from this file)");
    add(LAE_ROUTE_CD24, 0, cd ? "cd24" : "core24", bl + " + depth layer (24-bit, base rate)");
    for (int st = 1; st <= in.stages; st++) { const int r = in.base_rate << st;
        add(LAE_ROUTE_16, r, "16_" + std::to_string(r), "route 16: " + bl + " + frequency layer, 16-bit (the band of the " + bl + " at 16-bit precision; samples in 24-bit words)" + std::string(r == in.master_rate ? ", master rate" : ", an octave level")); }
    for (int st = 1; st <= in.stages; st++) { const int r = in.base_rate << st;
        add(LAE_ROUTE_FULL, r, "full_" + std::to_string(r), "route full: all layers (24-bit)" + std::string(r == in.master_rate ? ", master rate" : ", an octave level")); }
    lae_start(a.y, -1, 0, nullptr, 0);   // leaves the track unstarted (an unknown route is refused after the old decoder is dropped)
    if (a.ch.empty() || a.ch[0].route != LAE_ROUTE_CD) return "LAEY: the " + bl + " cannot be started";
    a.defChoice = (int)a.ch.size() - 1;   // route full at the master rate of pack 0 (the CD layer when nothing else starts)
    // extension packs (spec/lossy/SPEC.md, Section 5.6): after pack 0, one group per further pack, with its own master rate; names "pack<k>_cd24",
    // "pack<k>_16_<rate>", "pack<k>_full_<rate>". A pack is picked by the caller only; the name is shown and selects nothing.
    const int packs = lae_pack_count(a.y); std::vector<std::string> packFail;
    for (int k = 1; k < packs; k++) { char nm[260] = {0}; int pr = 0, ps = 0; lae_pack_info(a.y, k, &pr, &ps, nm, sizeof nm);
        const std::string pk = "pack" + std::to_string(k), lb = "pack " + std::to_string(k) + (nm[0] ? " \"" + std::string(nm) + "\"" : std::string());
        if (lae_select_pack(a.y, k)) { packFail.push_back(lb); continue; }
        const size_t first = a.ch.size();
        add(LAE_ROUTE_CD24, 0, pk + (cd ? "_cd24" : "_core24"), lb + ": " + bl + " + depth layer (24-bit, base rate)");
        for (int st = 1; st <= ps; st++) { const int r = in.base_rate << st; add(LAE_ROUTE_16, r, pk + "_16_" + std::to_string(r), lb + ": route 16, " + bl + " + frequency layer, 16-bit" + std::string(r == pr ? ", master rate" : ", an octave level")); }
        for (int st = 1; st <= ps; st++) { const int r = in.base_rate << st; add(LAE_ROUTE_FULL, r, pk + "_full_" + std::to_string(r), lb + ": route full, all layers (24-bit)" + std::string(r == pr ? ", master rate" : ", an octave level")); }
        for (size_t i = first; i < a.ch.size(); i++) a.ch[i].pack = k; }
    lae_select_pack(a.y, 0);
    addCheck(a, "CRC-32", "LAE1 extension block", 1, "");
    addCheck(a, "version and header fields", "LAE1 extension block (container v5)", 1, "");
    addCheck(a, "length", bl + " against the extension header", 1, std::to_string((unsigned long long)in.cd_frames) + " frames");
    if (a.pi.container == LAE_CONTAINER_FLAC) addCheck(a, "MD5", bl + " against FLAC STREAMINFO", 1, in.cd_md5);
    addCheck(a, "decoded to the last byte", "every entropy stream of the extension", 1, "at this open, or at an earlier open of the same file content when the player's record of it (by file SHA-256) exists");
    for (int k = 1; k < packs; k++) { char nm[260] = {0}; lae_pack_info(a.y, k, nullptr, nullptr, nm, sizeof nm); const std::string lb = "pack " + std::to_string(k) + (nm[0] ? " \"" + std::string(nm) + "\"" : std::string());
        addCheck(a, "decoded to the last byte", "every entropy stream of " + lb, std::find(packFail.begin(), packFail.end(), lb) == packFail.end() ? 1 : 0, "at this open"); }
    addCheck(a, "value range", "the streams the chosen route decodes", -1, "", true);
    return ""; }

// LAES: laes_open_reader reads the whole file once (file SHA-256, payload CRC-32 and structure, the core master and its hashes);
// a payload that fails leaves the core master playable. The masters' SHA-256 are known only after their last sample.
std::string fillLaes(lae_any& a) {
    char le[400] = {0}, mg[5] = {0}; a.s = laes_open_reader(a.rd, a.ctx, a.size, 0, mg, le, (int)sizeof le);
    if (!a.s) return std::string("LAES file (extension LAE2), not usable: ") + le;
    laes_file_info in; if (laes_get_info(a.s, &in)) return "LAES: no file information";
    a.version = 0; a.baseRate = in.base_rate; a.baseBits = in.core_bits; a.identity = "file SHA-256 " + hexOf(in.file_sha256, 32); a.usable = in.payload_ok ? 1 : 0;
    const std::string problem = laes_file_problem(a.s); int masters = 0; a.defChoice = 0;
    for (int i = 0; i < in.choices; i++) { laes_choice_info ci; if (laes_get_choice(a.s, i, &ci)) break;
        Choice c; c.kind = ci.kind; c.idx = i; c.rate = ci.rate; c.bits = ci.bits; c.frames = ci.frames; c.name = ci.name; c.what = ci.what;
        if (ci.kind == LAES_KIND_CORE) c.what += " (what a player without LAE support reads from this file)";
        if (ci.kind == LAES_KIND_MASTER) masters = std::max(masters, ci.master + 1);
        a.ch.push_back(c); }
    if (a.ch.empty() || a.ch[0].kind != LAES_KIND_CORE) return "LAES: the core master is not offered";
    // the default (SPEC 10.3, 2026-11-12): the entry the file names (a master itself; not set = the core master); the core master when the payload failed
    { const int d = laes_default_choice(a.s); if (in.payload_ok && d > 0 && d < (int)a.ch.size()) a.defChoice = d; }
    // which check failed is told by the reason: the core master's SHA-256 is compared only after the CRC-32 and the header passed
    const bool coreSha = problem.find("(SHA-256)") != std::string::npos;
    addCheck(a, "CRC-32 and structure", "LAE2 payload", problem.empty() || coreSha ? 1 : 0, problem.empty() || coreSha ? "" : problem);
    addCheck(a, "SHA-256", "core master against the LAE2 payload", problem.empty() ? 1 : (coreSha ? 0 : -1), problem.empty() ? "" : (coreSha ? problem : "not made: the payload failed before it"));
    if (in.is_flac) addCheck(a, "MD5 and sample count", "core master against FLAC STREAMINFO", 1, "");
    for (int i = 0; i < (int)a.ch.size(); i++) if (a.ch[(size_t)i].kind == LAES_KIND_MASTER) { laes_choice_info ci; laes_get_choice(a.s, a.ch[(size_t)i].idx, &ci);
        addCheck(a, "SHA-256", "master " + std::to_string(laes_master_number(a.s, ci.master)) + " (" + std::to_string(ci.rate) + " Hz " + std::to_string(ci.bits) + "-bit), all decoded samples", -1, "", true, ci.master); }
    return ""; }

lae_any* openImpl(lae_read_fn rd, void* ctx, uint64_t size, FILE* own, lae_probe_info* format, char* err, int errlen) {
    std::unique_ptr<lae_any> a; try { a.reset(new lae_any()); } catch (...) { if (own) fclose(own); throw; }   // F06-6: the file is closed also when this fails
    a->fp = own; a->rd = rd; a->ctx = ctx; a->size = size;
    std::string why; const int r = probe(rd, ctx, size, &a->pi, why); if (format) *format = a->pi;
    if (r) { setE(err, errlen, why); return nullptr; }
    if (a->pi.format == LAE_FORMAT_NONE) { setE(err, errlen, a->pi.container == LAE_CONTAINER_NONE ? "no LAE extension: not a WAV or FLAC file" : "no LAE extension: a plain WAV / FLAC file"); return nullptr; }
    why = a->pi.format == LAE_FORMAT_LAEY ? fillLaey(*a) : fillLaes(*a);
    if (!why.empty()) { setE(err, errlen, why); return nullptr; }
    return a.release(); }

void resetEnd(lae_any& a) { for (Check& c : a.ck) if (c.atEnd) { c.result = -1; c.detail.clear(); } a.ended = false; a.seeked = false; a.err.clear(); a.pos = 0; }

// the checks made after the last frame of the started choice (not after a seek: the part before it was not decoded in this run)
void finishEnd(lae_any& a) {
    if (a.ended) return; a.ended = true;
    if (a.seeked) { for (Check& c : a.ck) if (c.atEnd) c.detail = "not made: this run began with a seek"; return; }
    if (a.y) { uint64_t limited = 0; int se = 0; lae_stats(a.y, &limited, &se);
        for (Check& c : a.ck) if (c.atEnd) { const bool cdOnly = a.ch[(size_t)a.cur].route == LAE_ROUTE_CD;
            if (cdOnly) { c.result = -1; c.detail = "not made: this choice needs no extension stream"; }
            else { c.result = se ? 0 : 1; c.detail = se ? "a stream value is out of range" : "samples held by the hard limiter: " + std::to_string((unsigned long long)limited); } }
        if (se) a.err = "corrupt stream (values out of range)"; return; }
    if (a.sp) { int32_t tail[2]; laes_read(a.sp, tail, 1);   // past the last frame: returns 0, the hashes are final
        const int v = laes_verified(a.sp); if (v < 0) a.err = laes_play_error(a.sp);
        for (Check& c : a.ck) if (c.atEnd) { const int m = v < 0 ? -1 : laes_master_ok(a.sp, c.master);
            c.result = m; c.detail = m == 1 ? "" : m == 0 ? "the decoded master differs from the original" : (v < 0 ? "not made: " + a.err : "not part of this run"); }
        if (v == 0 && a.err.empty()) a.err = laes_play_error(a.sp); } }

int startImpl(lae_any* a, int choice, int flags, int threads, char* err, int errlen) {
    if (!a) { setE(err, errlen, "no file"); return -1; }
    if (choice < 0 || choice >= (int)a->ch.size()) { setE(err, errlen, "no such choice"); return -1; }
    if (a->sp) { laes_stop(a->sp); a->sp = nullptr; } a->cur = -1; resetEnd(*a);
    const Choice& C = a->ch[(size_t)choice]; char le[400] = {0};
    if (a->y) { if (lae_selected_pack(a->y) != C.pack && lae_select_pack(a->y, C.pack)) { setE(err, errlen, "the extension pack cannot be selected"); return -1; }
        if (lae_start(a->y, C.route, C.route == LAE_ROUTE_16 || C.route == LAE_ROUTE_FULL ? C.rate : 0, le, (int)sizeof le)) { setE(err, errlen, le); return -1; } }
    else { a->sp = (flags & LAE_ANY_VERIFY_ALL) ? laes_start_all(a->s, C.idx, threads, le, (int)sizeof le) : laes_start(a->s, C.idx, threads, le, (int)sizeof le);
        if (!a->sp) { setE(err, errlen, le); return -1; } }
    a->cur = choice; a->flags = flags; a->threads = threads; return 0; }

int64_t readImpl(lae_any* a, int32_t* out, int64_t frames) {
    if (!a || a->cur < 0 || !out || frames < 0) return -1;
    if (!a->err.empty() && !a->ended) return -1;
    const uint64_t total = a->ch[(size_t)a->cur].frames; if (a->pos >= total) { finishEnd(*a); return 0; }
    if (frames == 0) return 0;
    const int64_t n = a->y ? lae_read(a->y, out, frames) : laes_read(a->sp, out, frames);
    if (n < 0) { a->err = a->y ? "corrupt stream (values out of range)" : laes_play_error(a->sp); if (a->err.empty()) a->err = "decoding stopped"; return -1; }
    a->pos += (uint64_t)n;
    if (n == 0) { a->err = "the stream ended before its last frame"; return -1; }
    if (a->pos >= total) finishEnd(*a);
    return n; }

int seekImpl(lae_any* a, uint64_t frame) {
    if (!a || a->cur < 0) return -1; const Choice& C = a->ch[(size_t)a->cur]; if (frame > C.frames) frame = C.frames;
    if (frame == 0) return startImpl(a, a->cur, a->flags, a->threads, nullptr, 0);   // from the start: the checks at the end are made again
    if (a->y) { if (lae_seek(a->y, frame)) return -1; }
    else {   // a derived 16-bit version too: laes_start_at moves its dither state to the frame (2026-11-12, laes_play.h xs64Jump), bit-exact
        laes_play* p = laes_start_at(a->s, C.idx, frame, a->threads, nullptr, 0); if (!p) return -1;
        if (a->sp) laes_stop(a->sp); a->sp = p; }
    a->pos = frame; a->seeked = true; a->ended = false; a->err.clear(); for (Check& c : a->ck) if (c.atEnd) { c.result = -1; c.detail.clear(); }
    return 0; }
}

extern "C" {
LAE_API int LAE_CALL lae_probe_reader(lae_read_fn read, void* ctx, uint64_t size, lae_probe_info* out, char* err, int errlen) {
    if (!out) return -1;
    try { std::string why; const int r = probe(read, ctx, size, out, why); if (r) setE(err, errlen, why); return r; }
    catch (...) { setE(err, errlen, "out of memory or internal error"); return -1; } }
LAE_API int LAE_CALL lae_probe_file(const char* path, lae_probe_info* out, char* err, int errlen) {
    if (!out) return -1; memset(out, 0, sizeof *out); FILE* f = nullptr;
    try { uint64_t n = 0; if (!path || !(f = openPath(path)) || !sizeOfFile(f, n)) { if (f) fclose(f); setE(err, errlen, "cannot read the file"); return -1; }
        std::string why; const int r = probe(readFp, f, n, out, why); fclose(f); if (r) setE(err, errlen, why); return r; }
    catch (...) { if (f) fclose(f); setE(err, errlen, "out of memory or internal error"); return -1; } }
LAE_API const char* LAE_CALL lae_format_name(int format) { return format == LAE_FORMAT_LAEY ? "LAEY" : format == LAE_FORMAT_LAES ? "LAES" : ""; }
LAE_API lae_any* LAE_CALL lae_any_open_file(const char* path, lae_probe_info* format, char* err, int errlen) {
    if (format) memset(format, 0, sizeof *format); FILE* f = nullptr;
    try { uint64_t n = 0; if (!path || !(f = openPath(path)) || !sizeOfFile(f, n)) { if (f) fclose(f); setE(err, errlen, "cannot read the file"); return nullptr; }
        return openImpl(readFp, f, n, f, format, err, errlen); }   // the handle owns f from here on (closed with it, also when the open fails)
    catch (...) { setE(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API lae_any* LAE_CALL lae_any_open_reader(lae_read_fn read, void* ctx, uint64_t size, lae_probe_info* format, char* err, int errlen) {
    if (format) memset(format, 0, sizeof *format);
    try { if (!read) { setE(err, errlen, "no read function"); return nullptr; } return openImpl(read, ctx, size, nullptr, format, err, errlen); }
    catch (...) { setE(err, errlen, "out of memory or internal error"); return nullptr; } }
LAE_API int LAE_CALL lae_any_get_info(const lae_any* a, lae_any_info* out) {
    if (!a || !out) return -1; out->format = a->pi.format; out->container = a->pi.container; out->version = a->version; out->base_rate = a->baseRate; out->base_bits = a->baseBits;
    out->choices = (int)a->ch.size(); out->default_choice = a->defChoice; out->checks = (int)a->ck.size(); out->usable = a->usable; out->identity = a->identity.c_str(); return 0; }
LAE_API int LAE_CALL lae_any_get_choice(const lae_any* a, int index, lae_any_choice* out) {
    if (!a || !out || index < 0 || index >= (int)a->ch.size()) return -1; const Choice& c = a->ch[(size_t)index];
    out->route = c.route; out->laes_kind = c.kind; out->rate = c.rate; out->bits = c.bits; out->frames = c.frames; out->name = c.name.c_str(); out->what = c.what.c_str(); return 0; }
LAE_API int LAE_CALL lae_any_get_check(const lae_any* a, int index, lae_any_check* out) {
    if (!a || !out || index < 0 || index >= (int)a->ck.size()) return -1; const Check& c = a->ck[(size_t)index];
    out->method = c.method.c_str(); out->object = c.object.c_str(); out->result = c.result; out->detail = c.detail.c_str(); return 0; }
LAE_API int LAE_CALL lae_any_choice_pack(const lae_any* a, int index) { return a && a->y && index >= 0 && index < (int)a->ch.size() ? a->ch[(size_t)index].pack : -1; }
LAE_API int LAE_CALL lae_any_find_choice(const lae_any* a, const char* name) {
    if (!a || !name) return -1; const std::string n = name; int hit = -1;
    for (int i = 0; i < (int)a->ch.size(); i++) if (a->ch[(size_t)i].name == n) return i;
    // LAEY: cd, cd24 and core, core24 name the same two choices (the list uses the one that fits the file: cd at 44.1 kHz, core at 48 kHz)
    if (a->y && (n == "cd" || n == "core")) return a->ch.empty() ? -1 : 0;
    if (a->y && (n == "cd24" || n == "core24")) { for (int i = 0; i < (int)a->ch.size(); i++) if (a->ch[(size_t)i].route == LAE_ROUTE_CD24) return i; return -1; }
    if (a->y && (n == "16" || n == "full")) for (int i = 0; i < (int)a->ch.size(); i++) if (a->ch[(size_t)i].name.rfind(n + "_", 0) == 0) hit = i;   // the last: the master rate
    return hit; }
LAE_API int LAE_CALL lae_any_start(lae_any* a, int choice, int flags, int threads, char* err, int errlen) {
    try { return startImpl(a, choice, flags, threads, err, errlen); } catch (...) { setE(err, errlen, "out of memory or internal error"); return -1; } }
LAE_API int LAE_CALL lae_any_output_format(const lae_any* a, int* rate, int* bits, uint64_t* frames) {
    if (!a || a->cur < 0) return -1; const Choice& c = a->ch[(size_t)a->cur]; if (rate) *rate = c.rate; if (bits) *bits = c.bits; if (frames) *frames = c.frames; return 0; }
LAE_API int64_t LAE_CALL lae_any_read(lae_any* a, int32_t* out, int64_t frames) {
    try { return readImpl(a, out, frames); } catch (...) { if (a) { try { a->err = "out of memory or internal error"; } catch (...) {} } return -1; } }
LAE_API int LAE_CALL lae_any_seek(lae_any* a, uint64_t frame) { try { return seekImpl(a, frame); } catch (...) { return -1; } }
LAE_API const char* LAE_CALL lae_any_error(const lae_any* a) { return a ? a->err.c_str() : "no file"; }
LAE_API int LAE_CALL lae_any_verified(const lae_any* a) {
    if (!a || a->cur < 0) return 0; if (!a->err.empty()) return -1; if (!a->ended || a->seeked) return 0;
    for (const Check& c : a->ck) if (c.atEnd && c.result == 0) return -1;
    if (a->sp && laes_verified(a->sp) != 1) return -1;
    return 1; }
LAE_API void LAE_CALL lae_any_close(lae_any* a) { delete a; }
}
