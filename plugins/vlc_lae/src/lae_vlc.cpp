// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// liblae_plugin.dll: LAE input module for VLC 3.0 (Windows x64, 2026-11-04). A demux module: VLC asks it about every file it opens; it takes
// LAES files (LAE Lossless, "LAE2"; WAV / RF64 or FLAC form) and LAEY files (LAE v5 WAV, "LAE1") with any extension (.lae, .flac, .wav),
// and turns everything else down after reading the file's structure only (milliseconds), so plain FLAC / WAV stay with VLC's own modules.
// It delivers PCM (2 channels; 16-bit, or 24-bit and 32-bit in 32-bit words) at the rate of what plays, with seeking.
// VLC's plugin headers (LGPL-2.1-or-later) are compiled in: the module binary as a whole is distributed under LGPL-3.0-or-later; this
// source file is Apache-2.0.
// LAES rules, as in the other LAE players: a master plays as that master only after a full run matched its SHA-256 (spec 1.3); the result
// is kept by the SHA-256 of the whole file in the folder shared by every LAE player of this user (%APPDATA%\LAE\laes_verified.txt, only
// appended to), so a file checked in foobar2000, Kodi or a DirectShow player plays at once here and the other way round. The first time,
// every master of the file is checked in one pass with a progress dialog. Block positions for seeking: %APPDATA%\LAE\laes_index\<file
// SHA-256>.idx (never written into the media file). What plays: VLC's preferences (All -> Input / Codecs -> Demuxers -> LAE).
#define MODULE_NAME lae
#define MODULE_STRING "lae"
#define __PLUGIN__
#define VLC_MODULE_LICENSE VLC_LICENSE_LGPL_2_1_PLUS
#include <windows.h>
#include <xmmintrin.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>
// POSIX names VLC's headers use (VLC itself is built with MinGW, which has them)
typedef SSIZE_T ssize_t;
static inline int poll(struct pollfd* fds, unsigned n, int timeout) { return WSAPoll(fds, n, timeout); }
#define strcasecmp _stricmp
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
// texts: two standalone builds, English (the standard) and Chinese (/DLAE_LANG_ZH)
#ifdef LAE_LANG_ZH
#define LT(en, zh) zh
#else
#define LT(en, zh) en
#endif
#include <vlc_dialog.h>
#include <vlc_meta.h>
#include <vlc_aout.h>
#include "../../../src/lae_api.h"
#include "../../../src/lae_flacmeta.h"   // the FLAC form of LAEY: structure and Vorbis comments (2026-11-10)
#include "../../../src/lae_wavtags.h"   // tags of the WAV form (LIST/INFO + id3), shared by the LAE player plugins

namespace {
// the decoders need round-to-nearest and no flush-to-zero (x64: SSE only); every call into them runs with this state
struct FpPin { unsigned mx; FpPin() : mx(_mm_getcsr()) { _mm_setcsr(0x1F80u); } ~FpPin() { _mm_setcsr(mx); } };

// the folder shared by every LAE player of this user (%APPDATA%\LAE): SHA-256 results and block position records
std::wstring dataDir() { wchar_t p[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"APPDATA", p, MAX_PATH); std::wstring d = (n && n < MAX_PATH) ? std::wstring(p) : L".";
    d += L"\\LAE"; CreateDirectoryW(d.c_str(), nullptr); return d; }
std::string hex(const unsigned char* p, int n) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
std::wstring widen(const char* utf8) { const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0); std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n); w.resize(wcslen(w.c_str())); return w; }

// SHA-256 results per file content and master, shared with the other LAE players: "<file SHA-256> m<k> ok|fail". Lines are only appended
// (players running at once keep each other's results); a missing result is looked up again (another player may have checked it meanwhile).
class Verified {
public:
    int get(const unsigned char sha[32], int m) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1); auto it = map.find(k);
        if (it == map.end()) { loaded = false; load(); it = map.find(k); } return it == map.end() ? 0 : it->second ? 1 : -1; }
    void put(const unsigned char sha[32], int m, bool ok) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1);
        auto it = map.find(k); if (it != map.end() && it->second == ok) return; map[k] = ok;
        FILE* f = _wfopen((dataDir() + L"\\laes_verified.txt").c_str(), L"ab"); if (!f) return; fprintf(f, "%s %s\n", k.c_str(), ok ? "ok" : "fail"); fclose(f); }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, bool> map;
    void load() { if (loaded) return; loaded = true; FILE* f = _wfopen((dataDir() + L"\\laes_verified.txt").c_str(), L"rb"); if (!f) return; char line[512];
        while (fgets(line, sizeof line, f)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); const size_t a = s.rfind(' ');
            if (a != std::string::npos) map[s.substr(0, a)] = s.substr(a + 1) == "ok"; } fclose(f); }
};
Verified g_verified;

// what plays (as the other LAE players): layers 0 = all, 1 = CD or core layer + depth (base rate), 2 = CD or core layer only, 3 = CD or core layer + frequency layer (no depth layer). LAES (SPEC 10.3): the first master of the
// list (the menu order); of it the highest rate up to maxrate; a word above maxbits: that entry's own derived 16-bit version. Never another master.
int pick(const laes_file* f, int layers, int maxrate, int maxbits) {
    laes_file_info fi; laes_get_info(f, &fi); if (!fi.payload_ok || layers == 2) return 0;
    const int capR = layers == 1 ? fi.base_rate : maxrate;
    auto capped = [&](int i) { laes_choice_info c; laes_get_choice(f, i, &c);   // word length above the maximum: this entry's own derived 16-bit version, if the file offers one
        if (maxbits > 0 && c.bits > maxbits) for (int d = 0; d < fi.choices; d++) if (laes_choice_from(f, d) == i) return d;
        return i; };
    // SPEC 10.3 (2026-11-12): what plays without a choice of the user is the file's default entry (set when the file was encoded: a
    // master itself, else the core master), never an entry picked by rate, size or device. The limits work inside that master only.
    int d = laes_default_choice(f); if (d < 0 || d >= fi.choices) d = 0;
    laes_choice_info dc; laes_get_choice(f, d, &dc); if (dc.kind != LAES_KIND_MASTER) return capped(0);
    for (int i = d; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(f, i, &c);
        if (i > d && (c.kind == LAES_KIND_MASTER || c.kind == LAES_KIND_CORE)) break;
        if (c.master != dc.master || c.kind == LAES_KIND_DERIVED16) continue;
        if (capR > 0 && c.rate > capR) continue;
        return capped(i); }
    return capped(0); }



// the file: a local file read directly (the decoders read from several threads), else VLC's stream (one read at a time)
struct Src {
    HANDLE h = INVALID_HANDLE_VALUE; stream_t* s = nullptr; std::mutex m; uint64_t size = 0;
    ~Src() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    static int LAE_CALL read(void* ctx, uint64_t off, void* buf, size_t len) { Src* d = (Src*)ctx; std::lock_guard<std::mutex> g(d->m); uint8_t* p = (uint8_t*)buf;
        if (d->h != INVALID_HANDLE_VALUE) { LARGE_INTEGER li; li.QuadPart = (LONGLONG)off; if (!SetFilePointerEx(d->h, li, nullptr, FILE_BEGIN)) return -1;
            while (len) { DWORD k = 0; const DWORD want = (DWORD)(len < (1u << 30) ? len : (1u << 30)); if (!ReadFile(d->h, p, want, &k, nullptr) || k == 0) return -1; p += k; len -= k; } return 0; }
        if (vlc_stream_Seek(d->s, off) != VLC_SUCCESS) return -1;
        while (len) { const ssize_t k = vlc_stream_Read(d->s, p, len); if (k <= 0) return -1; p += k; len -= (size_t)k; } return 0; }
};

struct Sys {
    Src src; laes_file* laes = nullptr; laes_play* play = nullptr; lae_track* track = nullptr; es_out_id_t* es = nullptr;
    int choice = 0, rate = 0, bits = 16; uint64_t frames = 0, pos = 0; std::vector<int32_t> buf; std::thread indexer; volatile int cancel = 0;
    bool preparse = false;   // VLC's preparsing: only the length and tags are read
    lae_wavtags::List wtags;   // the tags of a WAV-form file (LAEY, LAES WAV), read at open; empty for the FLAC form
    ~Sys() { cancel = 1; if (indexer.joinable()) indexer.join(); FpPin pin; if (play) laes_stop(play); if (laes) laes_close(laes); if (track) lae_close(track); }
};

// the first play of a file content with a master: every master is decoded and checked in one pass, with a progress dialog
void checkAll(demux_t* d, Sys* sy) {
    laes_file_info fi; laes_get_info(sy->laes, &fi); int first = -1, nm = 0;
    for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(sy->laes, i, &c); if (c.kind == LAES_KIND_MASTER) { nm++; if (first < 0) first = i; } }
    char err[256]; laes_play* p = laes_start_all(sy->laes, first, 0, err, sizeof err); if (!p) return; laes_choice_info c; laes_get_choice(sy->laes, first, &c);
    vlc_dialog_id* dlg = vlc_dialog_display_progress(d, false, 0.f, nullptr, "LAE decoder: LAES", "First play of this file: checking all %d masters (SHA-256), once per file", nm);
    std::vector<int32_t> b(65536 * 2); uint64_t got = 0; int64_t k; int last = -1;
    while ((k = laes_read(p, b.data(), 65536)) > 0) { got += (uint64_t)k; const int pc = (int)(100.0 * (double)got / (double)(c.frames ? c.frames : 1));
        if (dlg && pc != last) { vlc_dialog_update_progress(d, dlg, (float)pc / 100.f); last = pc; } }
    if (laes_verified(p) >= 0) for (int m = 0; m < nm; m++) { const int r = laes_master_ok(p, m); if (r >= 0) g_verified.put(fi.file_sha256, m, r == 1); }
    if (dlg) vlc_dialog_release(d, dlg); laes_stop(p); }

bool seekTo(Sys* sy, uint64_t frame) {
    FpPin pin; char err[256]; if (frame > sy->frames) frame = sy->frames;
    if (sy->laes) {   // every choice starts at the frame, a derived 16-bit version too (its dither state is moved there, bit-exact; 2026-11-12)
        if (sy->play) laes_stop(sy->play); sy->play = laes_start_at(sy->laes, sy->choice, frame, 0, err, sizeof err);
        if (!sy->play) { sy->play = laes_start(sy->laes, sy->choice, 0, err, sizeof err); frame = 0; } sy->pos = frame; return sy->play != nullptr; }
    if (sy->track) { if (lae_seek(sy->track, frame) != 0) return false; sy->pos = frame; return true; }
    return false; }

bool startLaes(demux_t* d, Sys* sy) {
    laes_file_info fi; laes_get_info(sy->laes, &fi);
    sy->choice = pick(sy->laes, (int)var_InheritInteger(d, "lae-layers"), (int)var_InheritInteger(d, "lae-maxrate"), (int)var_InheritInteger(d, "lae-maxbits"));
    laes_choice_info c; laes_get_choice(sy->laes, sy->choice, &c);
    if (c.master >= 0) { if (g_verified.get(fi.file_sha256, c.master) == 0) checkAll(d, sy);
        if (g_verified.get(fi.file_sha256, c.master) != 1) { msg_Warn(d, LT("LAES: master %d: SHA-256 mismatch; playing the core master", "LAES：母带 %d：SHA-256 不符；播放核心母带"), c.master + 1); sy->choice = 0; laes_get_choice(sy->laes, 0, &c); } }
    sy->rate = c.rate; sy->bits = c.bits; sy->frames = c.frames; if (!seekTo(sy, 0)) return false;
    msg_Info(d, LT("LAES: playing %s", "LAES：播放 %s"), c.name);
    // block positions for seeking: the saved record, else one entropy-decoding pass on a thread of its own, then saved
    const std::wstring rec = dataDir() + L"\\laes_index\\" + widen(hex(fi.file_sha256, 32).c_str()) + L".idx"; laes_file* f = sy->laes;
    sy->indexer = std::thread([sy, rec, f] { FpPin pin; std::vector<uint8_t> b; FILE* r = _wfopen(rec.c_str(), L"rb");
        if (r) { _fseeki64(r, 0, SEEK_END); const int64_t n = _ftelli64(r); _fseeki64(r, 0, SEEK_SET); if (n > 0 && n < (64ll << 20)) { b.resize((size_t)n); if (fread(b.data(), 1, b.size(), r) != b.size()) b.clear(); } fclose(r); }
        if (!b.empty() && laes_index_load(f, b.data(), (int64_t)b.size()) == 0) return;
        laes_index_layers(f, &sy->cancel, 0); if (sy->cancel) return; const int64_t n = laes_index_save(f, nullptr, 0); if (n <= 0) return;
        b.resize((size_t)n); if (laes_index_save(f, b.data(), n) != n) return; CreateDirectoryW((dataDir() + L"\\laes_index").c_str(), nullptr);
        const std::wstring tmp = rec + L".part"; FILE* w = _wfopen(tmp.c_str(), L"wb"); if (!w) return; const bool ok = fwrite(b.data(), 1, b.size(), w) == b.size(); fclose(w);
        if (!ok || !MoveFileExW(tmp.c_str(), rec.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str()); });
    return true; }

bool startLaey(demux_t* d, Sys* sy) {
    char err[512] = ""; if (!lae_selftest()) { msg_Err(d, LT("LAE: decoder self-test failed", "LAE：解码器自检失败")); return false; }
    sy->track = lae_open_reader(&Src::read, &sy->src, sy->src.size, err, sizeof err); if (!sy->track) { msg_Err(d, "LAE: %s", err); return false; }
    // LAEY extension packs: the pack number the user set (0 = the main body); a file without that pack plays pack 0. Never chosen automatically.
    { const int pk = (int)var_InheritInteger(d, "lae-pack"); if (pk > 0 && pk < lae_pack_count(sy->track) && lae_select_pack(sy->track, pk) != 0) msg_Warn(d, LT("LAEY: extension pack %d unavailable; playing pack 1", "LAEY：扩展包 %d 不可用；播放扩展包 1"), pk + 1); }
    const int layers = (int)var_InheritInteger(d, "lae-layers"); int route = layers == 2 ? LAE_ROUTE_CD : layers == 1 ? LAE_ROUTE_CD24 : layers == 3 ? LAE_ROUTE_16 : LAE_ROUTE_FULL;
    lae_info li; lae_get_info(sy->track, &li); int out = 0; const int cap = (int)var_InheritInteger(d, "lae-maxrate");
    if ((route == LAE_ROUTE_FULL || route == LAE_ROUTE_16) && cap > 0) { out = li.master_rate; while (out > cap && out > li.base_rate) out /= 2;
        if (out == li.base_rate) { route = route == LAE_ROUTE_FULL ? LAE_ROUTE_CD24 : LAE_ROUTE_CD; out = 0; } }   // the cap reaches the base rate: all layers = CD layer + depth layer (24-bit); route 16 has no depth layer
    if (lae_start(sy->track, route, out, err, sizeof err) != 0 && lae_start(sy->track, LAE_ROUTE_CD, 0, err, sizeof err) != 0) { msg_Err(d, "LAE: %s", err); return false; }
    int r, b; uint64_t fr; lae_output_format(sy->track, &r, &b, &fr); sy->rate = r; sy->bits = b == 16 ? 16 : 24; sy->frames = fr; sy->pos = 0; return true; }

int64_t timeOf(const Sys* sy, uint64_t frame) { return (int64_t)(frame * (uint64_t)CLOCK_FREQ / (uint64_t)sy->rate); }

int Demux(demux_t* d) {
    Sys* sy = (Sys*)d->p_sys; if (sy->preparse) return VLC_DEMUXER_EOF; const int by = sy->bits == 16 ? 2 : 4; const int64_t want = sy->rate / 25;   // 40 ms per block
    sy->buf.resize((size_t)want * 2); int64_t k;
    { FpPin pin; k = sy->play ? laes_read(sy->play, sy->buf.data(), want) : sy->track ? lae_read(sy->track, sy->buf.data(), want) : -1; }
    if (k < 0) { msg_Err(d, "LAE: %s", sy->play ? laes_play_error(sy->play) : "read error"); return VLC_DEMUXER_EGENERIC; }
    if (k == 0) { es_out_SetPCR(d->out, VLC_TS_0 + timeOf(sy, sy->pos)); return VLC_DEMUXER_EOF; }   // the clock past the last block, so it is released too
    block_t* bl = block_Alloc((size_t)k * 2 * (size_t)by); if (!bl) return VLC_DEMUXER_EGENERIC;
    if (by == 2) { int16_t* o = (int16_t*)bl->p_buffer; for (int64_t i = 0; i < k * 2; i++) o[i] = (int16_t)sy->buf[(size_t)i]; }
    else if (sy->bits == 24) { uint32_t* o = (uint32_t*)bl->p_buffer; for (int64_t i = 0; i < k * 2; i++) o[i] = (uint32_t)sy->buf[(size_t)i] << 8; }   // at the top of the 32-bit word
    else memcpy(bl->p_buffer, sy->buf.data(), (size_t)k * 8);
    bl->i_dts = bl->i_pts = VLC_TS_0 + timeOf(sy, sy->pos); bl->i_length = timeOf(sy, sy->pos + (uint64_t)k) - timeOf(sy, sy->pos);
    es_out_SetPCR(d->out, bl->i_pts); es_out_Send(d->out, sy->es, bl); sy->pos += (uint64_t)k;
    return VLC_DEMUXER_SUCCESS; }

void addMeta(Sys* sy, vlc_meta_t* m) {
    auto put = [m](const char* key, const char* val) { std::string K = key; for (char& ch : K) ch = (char)toupper((unsigned char)ch);
        if (K == "TITLE") vlc_meta_SetTitle(m, val); else if (K == "ARTIST") vlc_meta_SetArtist(m, val); else if (K == "ALBUM") vlc_meta_SetAlbum(m, val);
        else if (K == "ALBUMARTIST" || K == "ALBUM ARTIST") vlc_meta_SetAlbumArtist(m, val); else if (K == "GENRE") vlc_meta_SetGenre(m, val);
        else if (K == "TRACKNUMBER") vlc_meta_SetTrackNum(m, val); else if (K == "DISCNUMBER") vlc_meta_SetDiscNumber(m, val); else if (K == "DATE") vlc_meta_SetDate(m, val);
        else if (K == "COMMENT") vlc_meta_SetDescription(m, val); else if (K == "COPYRIGHT") vlc_meta_SetCopyright(m, val); else if (K == "ENCODER") vlc_meta_SetEncodedBy(m, val);
        else vlc_meta_AddExtra(m, key, val); };
    if (!sy->wtags.empty()) { for (auto& kv : sy->wtags) put(kv.first.c_str(), kv.second.c_str()); return; }   // WAV form: INFO + id3
    if (!sy->laes) return;
    for (int k = 0; ; k++) { const char* key; const char* val; if (laes_get_tag(sy->laes, k, &key, &val) != 0) break; put(key, val); } }   // FLAC form: Vorbis comments

int Control(demux_t* d, int q, va_list args) {
    Sys* sy = (Sys*)d->p_sys;
    switch (q) {
    case DEMUX_CAN_SEEK: case DEMUX_CAN_PAUSE: case DEMUX_CAN_CONTROL_PACE: *va_arg(args, bool*) = true; return VLC_SUCCESS;
    case DEMUX_SET_PAUSE_STATE: return VLC_SUCCESS;
    case DEMUX_GET_PTS_DELAY: *va_arg(args, int64_t*) = INT64_C(1000) * var_InheritInteger(d, "file-caching"); return VLC_SUCCESS;
    case DEMUX_GET_LENGTH: *va_arg(args, int64_t*) = timeOf(sy, sy->frames); return VLC_SUCCESS;
    case DEMUX_GET_TIME: *va_arg(args, int64_t*) = timeOf(sy, sy->pos); return VLC_SUCCESS;
    case DEMUX_GET_POSITION: *va_arg(args, double*) = sy->frames ? (double)sy->pos / (double)sy->frames : 0.0; return VLC_SUCCESS;
    case DEMUX_SET_TIME: { const int64_t t = va_arg(args, int64_t); return seekTo(sy, t <= 0 ? 0 : (uint64_t)t * (uint64_t)sy->rate / (uint64_t)CLOCK_FREQ) ? VLC_SUCCESS : VLC_EGENERIC; }
    case DEMUX_SET_POSITION: { const double p = va_arg(args, double); return seekTo(sy, p <= 0 ? 0 : (uint64_t)(p * (double)sy->frames)) ? VLC_SUCCESS : VLC_EGENERIC; }
    case DEMUX_GET_META: addMeta(sy, va_arg(args, vlc_meta_t*)); return VLC_SUCCESS;
    default: return VLC_EGENERIC; } }

// the structure only (no hashing): an LAES or LAEY file, or not ours
bool looksLae(stream_t* s, uint64_t size, char mg[5]) {
    const uint8_t* p; if (vlc_stream_Peek(s, &p, 12) < 12) return false;
    if (memcmp(p, "fLaC", 4) != 0 && !((!memcmp(p, "RIFF", 4) || !memcmp(p, "RF64", 4) || !memcmp(p, "BW64", 4)) && !memcmp(p + 8, "WAVE", 4))) return false;
    Src q; q.s = s; q.size = size; char err[256]; FpPin pin; laes_file* f = laes_open_reader(&Src::read, &q, size, 1, mg, err, sizeof err);
    if (f) { laes_close(f); memcpy(mg, "LAE2", 5); }
    vlc_stream_Seek(s, 0);   // the next module reads from the start
    return !strcmp(mg, "LAE2") || !strcmp(mg, "LAE1"); }

int Open(vlc_object_t* obj) {
    demux_t* d = (demux_t*)obj; bool seekable = false; vlc_stream_Control(d->s, STREAM_CAN_SEEK, &seekable); if (!seekable) return VLC_EGENERIC;
    uint64_t size = 0; if (vlc_stream_GetSize(d->s, &size) != VLC_SUCCESS || size < 12) return VLC_EGENERIC;
    char mg[5] = ""; if (!looksLae(d->s, size, mg)) return VLC_EGENERIC;
    Sys* sy = new Sys(); sy->src.size = size;
    if (d->psz_file) { sy->src.h = CreateFileW(widen(d->psz_file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr); }
    if (sy->src.h == INVALID_HANDLE_VALUE) sy->src.s = d->s;
    sy->wtags = lae_wavtags::read([sy](uint64_t off, void* buf, size_t n) { return Src::read(&sy->src, off, buf, n) == 0; }, size);   // WAV form only
    if (!strcmp(mg, "LAE1") && sy->wtags.empty()) {   // LAEY in the FLAC form: its Vorbis comments
        const laeyflac::ReadFn rd = [sy](uint64_t off, void* buf, size_t n) { return Src::read(&sy->src, off, buf, n) == 0; }; laeyflac::Layout L; if (laeyflac::layout(rd, size, L).empty()) sy->wtags = laeyflac::comments(rd, L); }
    bool ok;
    { FpPin pin; char err[512] = "", m2[5] = "";
      if (!strcmp(mg, "LAE2") && d->b_preparsing) {   // VLC reads the length and tags for its playlist: the structure only (no check, no decoding)
          sy->preparse = true; sy->laes = laes_open_reader(&Src::read, &sy->src, size, 1, m2, err, sizeof err); ok = sy->laes != nullptr;
          if (ok) { sy->choice = pick(sy->laes, (int)var_InheritInteger(d, "lae-layers"), (int)var_InheritInteger(d, "lae-maxrate"), (int)var_InheritInteger(d, "lae-maxbits"));
              laes_choice_info c; laes_get_choice(sy->laes, sy->choice, &c); sy->rate = c.rate; sy->bits = c.bits; sy->frames = c.frames; } }
      else if (!strcmp(mg, "LAE2")) { sy->laes = laes_open_reader(&Src::read, &sy->src, size, 0, m2, err, sizeof err); ok = sy->laes && startLaes(d, sy); if (!sy->laes) msg_Err(d, "LAES: %s", err); }
      else ok = startLaey(d, sy); }
    if (!ok) { delete sy; return VLC_EGENERIC; }
    es_format_t fmt; es_format_Init(&fmt, AUDIO_ES, sy->bits == 16 ? VLC_CODEC_S16L : VLC_CODEC_S32L);
    fmt.audio.i_rate = (unsigned)sy->rate; fmt.audio.i_channels = 2; fmt.audio.i_physical_channels = AOUT_CHANS_STEREO;
    fmt.audio.i_bitspersample = sy->bits == 16 ? 16 : 32; fmt.audio.i_blockalign = 2 * fmt.audio.i_bitspersample / 8;
    fmt.i_bitrate = (unsigned)((double)size * 8 / ((double)sy->frames / sy->rate));
    sy->es = es_out_Add(d->out, &fmt); es_format_Clean(&fmt);
    d->p_sys = (demux_sys_t*)sy; d->pf_demux = Demux; d->pf_control = Control; return VLC_SUCCESS; }

void Close(vlc_object_t* obj) { delete (Sys*)((demux_t*)obj)->p_sys; }

const int kLayers[] = {0, 3, 1, 2};   // CD layer = the 16-bit layer of a 44.1 kHz file; in a 48 kHz file it is the core layer (a CD holds 44.1 kHz only)
const char* const kLayersText[] = {LT("All layers", "全部层"), LT("Frequency layer only", "仅频率层"), LT("Bit-depth layer only", "仅位深层"), LT("CD/core layer only", "仅 CD 层/核心层")};
const int kRates[] = {0, 192000, 96000, 48000}; const char* const kRatesText[] = {LT("No limit", "不限"), "192 kHz", "96 kHz", "48 kHz"};
const int kBits[] = {0, 16, 24, 32}; const char* const kBitsText[] = {LT("No limit", "不限"), LT("16-bit", "16 位"), LT("24-bit", "24 位"), LT("32-bit", "32 位")};
}  // namespace

vlc_module_begin()
    set_shortname("LAE")
    set_description(LT("LAE decoder (LAES / LAEY)", "LAE 解码器（LAES / LAEY）"))
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_DEMUX)
    set_capability("demux", 200)
    set_callbacks(Open, Close)
    add_shortcut("lae", "flac")   // "flac": VLC tries the module named for a file's extension first; this one turns plain FLAC down at once
    add_integer("lae-layers", 0, LT("LAEY layers", "LAEY 层"), LT("Layers decoded from LAEY files", "LAEY 文件解码的层"), false)
        change_integer_list(kLayers, kLayersText)   // VLC attaches a list to the setting just before it
    add_integer("lae-pack", 0, LT("LAEY extension pack", "LAEY 扩展包"), LT("Extension pack played from LAEY files with several packs (0 = the first)", "有多个扩展包的 LAEY 文件播放哪个扩展包（0 = 第一个）"), false)
    add_integer("lae-maxrate", 0, LT("Maximum sample rate", "最高采样率"), LT("Highest sample rate decoded", "解码的最高采样率"), false)
        change_integer_list(kRates, kRatesText)
    add_integer("lae-maxbits", 0, LT("LAES maximum bit depth", "LAES 最高位深"), LT("Highest bit depth of the LAES version played", "播放的 LAES 版本的最高位深"), false)
        change_integer_list(kBits, kBitsText)
vlc_module_end()
