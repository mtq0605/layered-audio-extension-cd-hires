// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// foo_input_lae: LAE (Layered Audio Extension) decoder for foobar2000: 1.4 / 1.5 and 2.x 32-bit (x86 build), 2.x 64-bit (x64 build).
// Plays LAE v5 WAV files (.wav with an "LAE " chunk after the CD layer, or the same file named .lae) through liblae
// (src/lae_api.h, compiled into this DLL). WAV files without an LAE chunk are refused at once, so foobar2000 hands them to its
// standard WAV decoder. Rules of the LAE reader (spec/cdx/SPEC.md R17, R20): a track is verified completely before it
// plays in high resolution (chunk CRC, header and stream structure); when it cannot be, the whole track plays as the CD layer; no
// automatic switch between CD and extension inside a track (only the user's choice in the layer menu switches, at the current position).
// LAE Lossless (LAES, 2026-11-01): WAV files whose LAE chunk begins with LAE2 and FLAC files with "LAE2" APPLICATION blocks (other FLAC
// files are refused at once and go to foobar2000's FLAC decoder). One choice plays (the core, a master, a rate level; settings below);
// a worker thread decodes it into a temporary file that Windows deletes on close. A choice plays as that master only after a full run
// matched its SHA-256 (spec 1.3); the result is remembered by the SHA-256 of the whole file (laes_verified.txt in the profile folder),
// so an unchanged file plays at once the next time, a changed one is checked again. Seeking goes to any decoded position at once and
// waits for the worker beyond it; a switch or a jump starts at the block that holds the time, using block positions found while
// decoding and kept in memory for the session only (the file holds no index; LAES rules T-1, T-2).
// Built against the foobar2000 SDK (x86: SDK-2019-12-27; x64: SDK-2026-10-01; BSD-style licence, not part of this repository).
// Layers: menu Playback > LAE (applies at once); the same settings in Preferences > Advanced > Decoding > LAE decoder.
// Texts are Chinese when the Windows display language is Chinese, English otherwise.
#include <foobar2000/SDK/foobar2000.h>
#include <xmmintrin.h>
#include <float.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <exception>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <vector>
#include <string>
#include "lae_api.h"
#include "lae_flacmeta.h"  // the FLAC form of LAEY: structure and Vorbis comments (2026-11-10)
#include "lae_wavtags.h"   // tags of the WAV form (LIST/INFO + id3), shared by the LAE player plugins

namespace {
// ---- texts: two standalone builds, English (the standard) and Chinese (its translation); /DLAE_LANG_ZH = the
// Chinese build.
#ifdef LAE_LANG_ZH
constexpr bool zh() { return true; }
#else
constexpr bool zh() { return false; }
#endif
const char* T(const char* en, const char* cn) { return zh() ? cn : en; }
}

DECLARE_COMPONENT_VERSION(T("LAE decoder", "LAE 解码器"), "0.30",
    T("Decoder for LAE (Layered Audio Extension) audio.\n"
      "LAES (lossless): core master, masters, sample-rate levels, derived 16-bit versions; masters verified by SHA-256.\n"
      "LAEY (lossy): CD/core layer, bit-depth layer, frequency layer, up to the master sample rate.\n"
      "Containers: WAV, RF64, FLAC (.lae, .wav, .flac).\n"
      "liblae (Apache-2.0). foobar2000 SDK (c) Peter Pawlowski (BSD licence).",
      "LAE（分层音频扩展）音频解码器。\n"
      "LAES（无损）：核心母带、各母带、采样率级别、派生 16 位版本；母带经 SHA-256 校验。\n"
      "LAEY（有损）：CD 层/核心层、位深层、频率层，最高至母带采样率。\n"
      "封装：WAV、RF64、FLAC（.lae、.wav、.flac）。\n"
      "liblae（Apache-2.0）。foobar2000 SDK (c) Peter Pawlowski（BSD 许可）。"));
VALIDATE_COMPONENT_FILENAME("foo_input_lae.dll");

namespace {
// ---- settings
const GUID guid_branch = { 0x736992c4, 0x2a6d, 0x4b6c, { 0xbf, 0x6c, 0x89, 0x37, 0xb8, 0xfa, 0x64, 0x29 } };
const GUID guid_layers = { 0x86633863, 0xc826, 0x4270, { 0xb8, 0x3d, 0x6a, 0x17, 0xda, 0x17, 0xe2, 0x99 } };
const GUID guid_maxrate = { 0xdef0b7ec, 0xae79, 0x4b8b, { 0x96, 0xab, 0x8e, 0x46, 0x1f, 0x25, 0x3d, 0x77 } };
const GUID guid_autoorder = { 0xe1eb67b6, 0xd817, 0x412a, { 0xb9, 0x9f, 0x0f, 0x3a, 0x6e, 0x0d, 0x46, 0xfc } };
advconfig_branch_factory g_branch(T("LAE decoder", "LAE 解码器"), guid_branch, advconfig_branch::guid_branch_decoding, 0);
advconfig_integer_factory g_layers(T("LAEY layers (0 = all layers, 1 = bit-depth layer only, 2 = CD/core layer only, 3 = frequency layer only)", "LAEY 层（0 = 全部层，1 = 仅位深层，2 = 仅 CD 层/核心层，3 = 仅频率层）"),
    guid_layers, guid_branch, 0, 0, 0, 3);
advconfig_integer_factory g_maxrate(T("Maximum sample rate, Hz (0 = master sample rate)", "最高采样率，Hz（0 = 母带采样率）"),
    guid_maxrate, guid_branch, 1, 0, 0, 1536000);
advconfig_checkbox_factory g_autoOrder(T("Give LAE priority over other .wav / .flac decoders", "LAE 优先于其他 .wav / .flac 解码器"),
    guid_autoorder, guid_branch, 2, true);
const GUID guid_maxbits = { 0x3f4c2a91, 0x6d0e, 0x4b8a, { 0x9e, 0x51, 0x27, 0xc3, 0x84, 0x1d, 0xa6, 0x5b } };
advconfig_integer_factory g_maxbits(T("LAES maximum bit depth (0 = no limit, 16, 24, 32)", "LAES 最高位深（0 = 不限，16、24、32）"),
    guid_maxbits, guid_branch, 3, 0, 0, 32);
const GUID guid_verifyadd = { 0x52e8d0a4, 0x19b7, 0x4c3e, { 0xa6, 0x2f, 0x81, 0x0d, 0x5c, 0x93, 0x7b, 0xe4 } };
advconfig_checkbox_factory g_verifyOnAdd(T("LAES (lossless): check files in the background when they are added to the media library (then they play at once)",
    "LAES（无损）：音乐入库时在后台校验（之后播放不用等）"), guid_verifyadd, guid_branch, 4, true);
const GUID guid_prefetch = { 0x8c41e2d7, 0x3b6a, 0x4f19, { 0x92, 0x5e, 0x0a, 0xd4, 0x71, 0x38, 0xc6, 0xbf } };
advconfig_checkbox_factory g_prefetch(T("LAES (lossless): keep the other masters ready while playing, for switching without a gap (uses more CPU)",
    "LAES（无损）：播放时让其他母带在后台待命，切换几乎无间断（多用一些 CPU）"), guid_prefetch, guid_branch, 5, true);

// ---- the decoder's identity in foobar2000's decoder priority list. foobar2000 puts a decoder it has never seen at the top of a saved
// list, but builds the first list of a new profile in no fixed order. When a decoder that also takes .wav files is ahead of LAE and the
// user did not put it there (first start of a new profile, or a newly installed decoder), LAE moves on to its next GUID, which foobar2000
// then sees as new and puts at the top (after a restart). The old GUID stays at the bottom of the list as an unnamed old decoder.
const GUID guid_input[] = {
    { 0x5c5f5ced, 0xb09e, 0x4a4f, { 0xa1, 0xe0, 0x40, 0x10, 0x07, 0x74, 0x8f, 0x57 } },
    { 0x936d4fef, 0x1f61, 0x45ea, { 0x8d, 0x7e, 0x1f, 0xcf, 0x07, 0x51, 0x5e, 0x7b } },
    { 0x5c2425f9, 0x80b3, 0x4b50, { 0x91, 0x0d, 0x7f, 0x0d, 0xac, 0x04, 0xf2, 0x5f } },
    { 0x256b21b8, 0x3a2f, 0x4262, { 0x91, 0x9d, 0xe5, 0x58, 0xa4, 0x25, 0x5d, 0x74 } } };
const unsigned kInputGuids = sizeof guid_input / sizeof guid_input[0];
const GUID guid_cfg_slot = { 0x5f1dc4d6, 0x525a, 0x4b5d, { 0x90, 0x16, 0xdb, 0x41, 0x2d, 0x5e, 0x87, 0x7e } };
const GUID guid_cfg_promoted = { 0x108e268b, 0xf67e, 0x4000, { 0x88, 0x1a, 0xab, 0x59, 0x32, 0x0b, 0x54, 0x12 } };
const GUID guid_cfg_known = { 0xfb1a94ae, 0x3079, 0x4cb7, { 0xbd, 0xd7, 0x41, 0x24, 0xcd, 0x18, 0xcf, 0x6d } };
// the GUID slot: read once from the configuration and fixed for the session (foobar2000 knows a decoder by one GUID per session);
// a move is saved for the next start
#if FOOBAR2000_TARGET_VERSION >= 81   // the 2.0 SDK (x64 build): the same stored settings through its legacy cfg_var classes
using cfg_var_legacy::cfg_var; typedef cfg_var_legacy::cfg_bool cfg_bool_l; typedef cfg_var_legacy::cfg_string cfg_string_l;
#else
typedef cfg_bool cfg_bool_l; typedef cfg_string cfg_string_l;
#endif
class cfg_slot : public cfg_var {
public:
    explicit cfg_slot(const GUID& g) : cfg_var(g) {}
    t_uint32 current = 0, next = 0;
protected:
    void get_data_raw(stream_writer* s, abort_callback& a) { s->write_lendian_t(next, a); }
    void set_data_raw(stream_reader* s, t_size, abort_callback& a) { t_uint32 v = 0; s->read_lendian_t(v, a); current = next = std::min<t_uint32>(v, kInputGuids - 1); }
};
cfg_slot g_slot(guid_cfg_slot);
cfg_bool_l g_promoted(guid_cfg_promoted, false);   // LAE has moved itself at least once (a record; the decision uses g_known)
cfg_string_l g_known(guid_cfg_known, "");          // GUIDs of the decoders seen at the last start (to tell a newly installed one)

// ---- the decoder needs strict IEEE-754 double arithmetic: round to nearest, no flush-to-zero, 53-bit x87 precision (x86)
struct FpPin {   // x87 and SSE are saved and restored separately (_controlfp_s would report and write back a mix of the two)
    unsigned mx; unsigned x87 = 0;
    FpPin() : mx(_mm_getcsr()) {
#if defined(_M_IX86)
        unsigned cur; __control87_2(0, 0, &x87, nullptr); __control87_2(_RC_NEAR | _PC_53, _MCW_RC | _MCW_PC, &cur, nullptr);
#endif
        _mm_setcsr(0x1F80u); }
    ~FpPin() {
#if defined(_M_IX86)
        unsigned cur; __control87_2(x87, _MCW_RC | _MCW_PC, &cur, nullptr);
#endif
        _mm_setcsr(mx); }
};
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// liblae reads the file through foobar2000's file object (archives, abort); exceptions are carried across the C interface by hand
struct FileReader {
    service_ptr_t<file> f; abort_callback& abort; std::exception_ptr ep;
    static int LAE_CALL read(void* ctx, uint64_t offset, void* buf, size_t len) {
        FileReader* r = (FileReader*)ctx;
        try { r->f->seek(offset, r->abort); r->f->read_object(buf, len, r->abort); return 0; } catch (...) { r->ep = std::current_exception(); return -1; } }
};
// the layer every player reads: the CD layer only at 44.1 kHz (a CD holds nothing else); at 48 kHz it is the core layer
std::string baseName(int base) { return base == 44100 ? T("CD layer", "CD 层") : T("core layer", "核心层"); }
std::string khz(int hz) { char s[32]; snprintf(s, sizeof s, hz % 1000 ? "%.1f kHz" : "%.0f kHz", hz / 1000.0); return s; }

uint64_t nowMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// the folder shared by every LAE player of this user (foobar2000, Kodi, DirectShow): SHA-256 results and block position records, so a file
// checked in one player plays at once in the others
std::wstring laeSharedDir() { wchar_t p[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", p, MAX_PATH);
    std::wstring d = n && n < MAX_PATH ? std::wstring(p) : std::wstring(L"."); d += L"\\LAE"; CreateDirectoryW(d.c_str(), nullptr); return d; }
// ---- LAES (LAE Lossless): which masters passed a full decode with their SHA-256 (spec 1.3), remembered by the SHA-256 of the whole file
// content (not its path or date: a changed byte means a new check). A choice plays as its master (or a rate level / derived version of
// it) only when that master is recorded as ok. File: laes_verified.txt in the profile folder, one line per result:
// <file SHA-256 hex> m<master index from 1> ok|fail
class VerifiedStore {
public:
    // a hint by path and size that the file was checked before (only for the scale of the progress bar; the check itself goes by content)
    bool hint(const std::string& path, uint64_t size) { std::lock_guard<std::mutex> g(mx); load(); return map.count("file " + std::to_string(size) + " " + path) != 0; }
    void putHint(const std::string& path, uint64_t size) { if (hint(path, size)) return; std::lock_guard<std::mutex> g(mx); const std::string k = "file " + std::to_string(size) + " " + path; map[k] = 1;
        FILE* f = _wfopen(this->path().c_str(), L"ab"); if (!f) return; fprintf(f, "%s ok\n", k.c_str()); fclose(f); }
    int get(const unsigned char sha[32], int m) { std::lock_guard<std::mutex> g(mx); load(); auto it = map.find(key(sha, m));
        if (it == map.end()) { loaded = false; load(); it = map.find(key(sha, m)); }   // another player may have checked it meanwhile
        return it == map.end() ? 0 : it->second; }   // 1 ok, -1 fail, 0 unknown
    void put(const unsigned char sha[32], int m, bool ok) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = key(sha, m); auto it = map.find(k);
        if (it != map.end() && it->second == (ok ? 1 : -1)) return; map[k] = ok ? 1 : -1;
        FILE* f = _wfopen(path().c_str(), L"ab"); if (!f) return; fprintf(f, "%s %s\n", k.c_str(), ok ? "ok" : "fail"); fclose(f); }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, int> map;
    static std::string key(const unsigned char sha[32], int m) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < 32; i++) { s += d[sha[i] >> 4]; s += d[sha[i] & 15]; } return s + " m" + std::to_string(m + 1); }
    // the file shared by every LAE player of this user (%APPDATA%\LAE); lines are only appended, so players running at once keep each
    // other's results. Up to 0.14 the file was in the foobar2000 profile folder: it is read as well.
    static std::wstring path() { return laeSharedDir() + L"\\laes_verified.txt"; }
    static std::wstring oldPath() { pfc::string8 p = core_api::get_profile_path(); const char* s = p.get_ptr(); if (!strncmp(s, "file://", 7)) s += 7;
        return std::wstring(pfc::stringcvt::string_wide_from_utf8(s)) + L"\\laes_verified.txt"; }
    void load() { if (loaded) return; loaded = true;
        for (const std::wstring& pth : {oldPath(), path()}) { FILE* f = _wfopen(pth.c_str(), L"rb"); if (!f) continue; char line[512];
            while (fgets(line, sizeof line, f)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                const size_t a = s.rfind(' '); if (a == std::string::npos) continue; map[s.substr(0, a)] = s.substr(a + 1) == "ok" ? 1 : -1; }
            fclose(f); } }
};
VerifiedStore g_verified;
// measured decoding speeds as real-time factors (seconds of audio per second of decoding) on this machine, by the SHA-256 of the file:
// "all" = every master in one pass (the background or first check; any single choice decodes at least this fast), or a choice's name
// (measured while it played). The buffer at the next start is sized from them instead of being measured first. File: laes_speed.txt.
class SpeedStore {
public:
    double get(const unsigned char sha[32], const std::string& what) { std::lock_guard<std::mutex> g(mx); load(); auto it = map.find(key(sha, what)); return it == map.end() ? 0 : it->second; }
    void put(const unsigned char sha[32], const std::string& what, double rtf) { if (!(rtf > 0)) return; std::lock_guard<std::mutex> g(mx); load(); map[key(sha, what)] = rtf;
        FILE* f = _wfopen(file().c_str(), L"ab"); if (!f) return; fprintf(f, "%s %.3f\n", key(sha, what).c_str(), rtf); fclose(f); }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, double> map;
    static std::string key(const unsigned char sha[32], const std::string& what) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < 32; i++) { s += d[sha[i] >> 4]; s += d[sha[i] & 15]; } return s + " " + what; }
    static std::wstring file() { pfc::string8 p = core_api::get_profile_path(); const char* s = p.get_ptr(); if (!strncmp(s, "file://", 7)) s += 7;
        return std::wstring(pfc::stringcvt::string_wide_from_utf8(s)) + L"\\laes_speed.txt"; }
    void load() { if (loaded) return; loaded = true; FILE* f = _wfopen(file().c_str(), L"rb"); if (!f) return; char line[600];
        while (fgets(line, sizeof line, f)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t a = s.rfind(' '); if (a == std::string::npos) continue; map[s.substr(0, a)] = atof(s.c_str() + a + 1); }
        fclose(f); }
};
SpeedStore g_speed;
// the choice the user picked for a track (context menu "LAES: choose what plays"), by path; it wins over the general settings.
// File: laes_choices.txt in the profile folder, one line per track: <choice name> TAB <path>; an empty name = automatic.
class ChoiceStore {
public:
    std::string get(const char* path) { std::lock_guard<std::mutex> g(mx); load(); auto it = map.find(path); return it == map.end() ? std::string() : it->second; }
    void set(const char* path, const std::string& name) { std::lock_guard<std::mutex> g(mx); load(); if (name.empty()) map.erase(path); else map[path] = name;
        FILE* f = _wfopen(file().c_str(), L"wb"); if (!f) return; for (auto& e : map) fprintf(f, "%s\t%s\n", e.second.c_str(), e.first.c_str()); fclose(f); }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, std::string> map;
    // up to 0.10 the name lacked its backslash, so the file sat beside the profile folder ("<profile>laes_choices.txt"): read that once
    static std::wstring file(bool old = false) { pfc::string8 p = core_api::get_profile_path(); const char* s = p.get_ptr(); if (!strncmp(s, "file://", 7)) s += 7;
        return std::wstring(pfc::stringcvt::string_wide_from_utf8(s)) + (old ? L"laes_choices.txt" : L"\\laes_choices.txt"); }
    void load() { if (loaded) return; loaded = true; FILE* f = _wfopen(file().c_str(), L"rb"); if (!f) f = _wfopen(file(true).c_str(), L"rb"); if (!f) return; char line[4096];
        while (fgets(line, sizeof line, f)) { std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t t = s.find('\t'); if (t != std::string::npos && t > 0) map[s.substr(t + 1)] = s.substr(0, t); }
        fclose(f); }
};
ChoiceStore g_choices;
// ---- LAEY extension packs (spec/lossy/SPEC.md, Section 5.6): the further packs of a file, found from the payload's structure with a few
// small reads (nothing is decoded; liblae checks everything when the track is opened for playing). rd(off, buf, n) reads n bytes at
// payload offset off; n = the payload without its CRC. Pack k (1 ..) = the list's entry k - 1; pack 0 is the main body.
struct PackRow { std::string name; int rate = 0, stages = 0; uint32_t frames = 0; };
std::vector<PackRow> scanPacks(const std::function<bool(uint64_t, void*, size_t)>& rd, uint64_t n, std::string* mainName = nullptr) {
    std::vector<PackRow> out; uint8_t b[24];
    auto u32 = [&](uint64_t o, uint32_t& v) { if (o > n || n - o < 4 || !rd(o, b, 4)) return false; v = le32(b); return true; };
    uint32_t pb = 0; if (n < 56 || !u32(48, pb) || pb > 65536) return out;
    uint64_t i = 52 + 8ull * pb; uint32_t ndf = 0; if (!u32(i, ndf)) return out; i += 4; if (ndf > n - i) return out; i += ndf;
    auto rec = [&]() { uint32_t nb = 0; if (!u32(i, nb)) return false; i += 4; if (nb < 40 || nb > n - i) return false; i += nb; return true; };
    if (!rec() || !rec()) return out; uint32_t ns = 0; if (!u32(i, ns)) return out; i += 4; if (ns < 1 || ns > 8) return out;
    for (uint32_t k = 0; k < 2 * ns; k++) if (!rec()) return out;
    while (out.size() < 8 && n - i >= 9) { uint8_t h[9]; if (!rd(i, h, 9) || memcmp(h, "LPAK", 4)) break; const uint32_t len = le32(h + 4), nl = h[8];
        if (len < 1 || len > n - i - 8 || 1 + nl + 52 > len) break; PackRow q; if (nl) { q.name.resize(nl); if (!rd(i + 9, &q.name[0], nl)) break; }
        if (!rd(i + 9 + nl, b, 24) || le32(b) != 0x3145414Cu || le32(b + 4) != 5) break;
        q.rate = (int)le32(b + 8); q.stages = (int)le32(b + 16); q.frames = le32(b + 20); out.push_back(q); i += 8 + (uint64_t)len; }
    // the main body's name: one optional record after the last pack record, "LNAM" | 1 byte length | the name (display only)
    if (mainName) { mainName->clear(); uint8_t h[5]; if (n - i >= 5 && rd(i, h, 5) && !memcmp(h, "LNAM", 4) && (uint64_t)h[4] <= n - i - 5 && h[4]) { mainName->resize(h[4]); if (!rd(i + 5, &(*mainName)[0], h[4])) mainName->clear(); } }
    return out; }
// the pack the user chose for this file (0 = the main body): kept by path in the player's own record, as "laeypack=<k>"
int packOf(const char* path) { const std::string v = g_choices.get(path); return v.rfind("laeypack=", 0) == 0 ? atoi(v.c_str() + 9) : 0; }
// "176.4 kHz / 24 bit, master 3" and the like
// SPEC 10.3: 'master 3 "name" · 176.4 kHz / 24 bit' - the number (its place in the menu order) always, the publisher's name when the
// master has one; rate levels and derived versions carry the name of the master they come from. maxName > 0 cuts a long name (menus;
// the track's properties show it in full). The name is only shown, it never chooses anything.
std::string cutUtf8(const std::string& s, size_t maxChars) { size_t n = 0, i = 0; while (i < s.size()) { if (n == maxChars) return s.substr(0, i) + "\xE2\x80\xA6";
        i++; while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++; n++; } return s; }
std::string masterText(const laes_file* f, int master, size_t maxName, bool lower = false) {   // lower: inside a sentence ("derived from master 3")
    if (master < 0) return lower ? T("core master", "核心母带") : T("Core master", "核心母带");
    std::string s = (lower ? T("master ", "母带 ") : T("Master ", "母带 ")) + std::to_string(laes_master_number(f, master)); const char* nm = laes_master_name(f, master);
    if (nm && *nm) s += T(" \"", "「") + (maxName ? cutUtf8(nm, maxName) : std::string(nm)) + T("\"", "」");
    return s; }
std::string choiceText(const laes_file* f, const laes_choice_info& c, size_t maxName = 0, int index = -1) {
    // SPEC 10.3 (2026-11-12): only a master itself (and the core master) is "lossless" and carries its length and the default mark; a
    // rate level and a derived 16-bit version are "derived from master N" with their rate and word length, nothing more
    const std::string fmt = " \xC2\xB7 " + khz(c.rate) + " / " + std::to_string(c.bits) + T("-bit", " 位");
    const std::string who = masterText(f, c.kind == LAES_KIND_CORE ? -1 : c.master, maxName, c.kind == LAES_KIND_LEVEL || c.kind == LAES_KIND_DERIVED16);
    if (c.kind == LAES_KIND_CORE || c.kind == LAES_KIND_MASTER) { const uint64_t sec = c.rate > 0 ? (c.frames + (uint64_t)c.rate / 2) / (uint64_t)c.rate : 0; char tm[32]; snprintf(tm, sizeof tm, "%llu:%02llu", (unsigned long long)(sec / 60), (unsigned long long)(sec % 60));
        return who + fmt + " \xC2\xB7 " + tm + " \xC2\xB7 " + T("Lossless", "无损") + (index >= 0 && laes_default_choice(f) == index ? std::string(" \xC2\xB7 ") + T("Default", "默认") : std::string()); }
    return std::string(T("Derived from ", "由")) + who + T("", "派生") + fmt; }   // nothing after the rate and word length (spec/lossless/SPEC.md, Section 10.3)

// masters that failed their SHA-256 check, by path (this run of the player): the menu, which opens the file without reading it all and
// so has no file hash, marks them "unavailable" instead of leaving them out (SPEC 10.3)
struct FailedMasters { std::mutex mx; std::map<std::string, std::set<int>> byPath;
    void note(const std::string& path, int m, bool failed) { std::lock_guard<std::mutex> g(mx); if (failed) byPath[path].insert(m); else { auto it = byPath.find(path); if (it != byPath.end()) it->second.erase(m); } }
    bool has(const std::string& path, int m) { std::lock_guard<std::mutex> g(mx); auto it = byPath.find(path); return it != byPath.end() && it->second.count(m) != 0; } };
FailedMasters g_failed;

// after a run to the end: every master it decoded is recorded (ok or fail)
void recordRun(const laes_play* p, const unsigned char sha[32], int masters) { for (int m = 0; m < masters; m++) { const int r = laes_master_ok(p, m); if (r >= 0) g_verified.put(sha, m, r == 1); } }
int mastersOf(const laes_file* f) { laes_file_info fi; laes_get_info(f, &fi); int n = 0; for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(f, i, &c); if (c.kind == LAES_KIND_MASTER) n++; } return n; }
void noteFailed(const std::string& path, const unsigned char sha[32], int masters) { for (int m = 0; m < masters; m++) g_failed.note(path, m, g_verified.get(sha, m) < 0); }
// event log of LAES playback (initialize, seek, switch, stop) for finding playback faults: written only while laes_debug.log exists in the
// profile folder (create an empty file to turn it on, delete it to turn it off)
void dbg(const char* fmt, ...) { static std::mutex mx; std::lock_guard<std::mutex> g(mx); pfc::string8 p = core_api::get_profile_path(); const char* s = p.get_ptr(); if (!strncmp(s, "file://", 7)) s += 7;
    FILE* fo = _wfopen((std::wstring(pfc::stringcvt::string_wide_from_utf8(s)) + L"\\laes_debug.log").c_str(), L"r+b"); if (!fo) return; _fseeki64(fo, 0, SEEK_END);   // r+b: only an existing file
    char b[600]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); fprintf(fo, "%llu %s\n", (unsigned long long)nowMs(), b); fclose(fo); }
// a file read through a foobar2000 file object of its own (file objects are not shared between threads)
// For the progress window it also times the full read of an LAES file: the core (FLAC frames decoded and hashed, or WAV samples) costs
// far more per byte than the rest (hashed only), so the bar follows the time each part takes, not the bytes. The speeds measured on this
// machine are kept (laes_speed.txt, key "machine") and used for the parts not yet reached.
const unsigned char kMachine[32] = {0};
struct OwnReader { service_ptr_t<file> f; abort_callback_impl abort; std::atomic<uint64_t> bytes{0};   // bytes: read so far
    // path: for opening the file again after park() (2026-11-06): a kept file that nothing plays holds no handle, so the user can write,
    // move or delete it (tags, for example) while the decoder state stays in memory
    std::string path; std::mutex fm;
    static int LAE_CALL read(void* ctx, uint64_t off, void* buf, size_t len) { OwnReader* r = (OwnReader*)ctx; std::lock_guard<std::mutex> g(r->fm);
        try { if (!r->f.is_valid()) filesystem::g_open(r->f, r->path.c_str(), filesystem::open_mode_read, r->abort);
            r->f->seek(off, r->abort); r->f->read_object(buf, len, r->abort); r->bytes += len; r->timed(off, len); return 0; } catch (...) { return -1; } }
    void park() { std::lock_guard<std::mutex> g(fm); if (!path.empty()) f.release(); }
    // the core's byte range (FLAC: after the metadata blocks; WAV: the last 'data' chunk); call once after f is opened
    void locateCore(uint64_t size) { uint64_t coreOff = 0, coreLen = 0; try { uint8_t h[12]; f->seek(0, abort); f->read_object(h, 12, abort);
            if (!memcmp(h, "fLaC", 4)) { uint64_t p = 4; for (;;) { uint8_t b[4]; f->seek(p, abort); f->read_object(b, 4, abort); p += 4 + ((uint64_t)b[1] << 16 | b[2] << 8 | b[3]); if ((b[0] & 0x80) || p >= size) break; }
                coreOff = (std::min)(p, size); coreLen = size - coreOff; }
            else if (!memcmp(h, "RIFF", 4) || !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) { uint64_t p = 12, ds = 0; if (memcmp(h, "RIFF", 4)) { uint8_t d[16]; f->seek(20, abort); f->read_object(d, 16, abort); ds = (uint64_t)le32(d + 8) | (uint64_t)le32(d + 12) << 32; }   // RF64: data size in ds64
                while (p + 8 <= size) { uint8_t c[8]; f->seek(p, abort); f->read_object(c, 8, abort); uint64_t n = le32(c + 4); if (ds && n == 0xFFFFFFFFu && !memcmp(c, "data", 4)) n = ds; if (n > size - p - 8) n = size - p - 8;
                    if (!memcmp(c, "data", 4)) { coreOff = p + 8; coreLen = n; } p += 8 + n + (n & 1); } } } catch (...) { coreLen = 0; }
        std::lock_guard<std::mutex> g(m); this->coreOff = coreOff; this->coreLen = coreLen; total = size; }
    // seconds still needed to read and check the whole file; the share of that work done (0..1)
    double remaining() const { std::lock_guard<std::mutex> g(m); double s = 0; for (int r = 0; r < 2; r++) s += (double)(len(r) - (std::min)(len(r), done[r])) / rate(r); return s; }
    double fraction() const { std::lock_guard<std::mutex> g(m); double a = 0, b = 0; for (int r = 0; r < 2; r++) { a += (double)(std::min)(len(r), done[r]) / rate(r); b += (double)len(r) / rate(r); } return b > 0 ? a / b : 0; }
    void remember() const { std::lock_guard<std::mutex> g(m); for (int r = 0; r < 2; r++) if (busy[r] >= 0.3) g_speed.put(kMachine, kWhat[r], done[r] / busy[r]); }
private:
    static constexpr const char* kWhat[2] = {"open_rest_bytes_per_s", "open_core_bytes_per_s"};
    mutable std::mutex m; uint64_t coreOff = 0, coreLen = 0, total = 0, done[2] = {0, 0}, prevLen = 0; double busy[2] = {0, 0}; uint64_t tPrev = 0; int prevR = -1; mutable double known[2] = {-1, -1};
    uint64_t len(int r) const { return r ? coreLen : total - (std::min)(total, coreLen); }
    double rate(int r) const { if (busy[r] >= 0.3 && done[r]) return done[r] / busy[r];   // measured in this read
        if (known[r] < 0) known[r] = g_speed.get(kMachine, kWhat[r]); return known[r] > 0 ? known[r] : r ? 30e6 : 800e6; }   // earlier reads, or a first guess
    void timed(uint64_t off, size_t n) { std::lock_guard<std::mutex> g(m); const uint64_t now = nowMs(); const int r = coreLen && off >= coreOff && off < coreOff + coreLen ? 1 : 0;
        if (prevR >= 0) { busy[prevR] += (now - tPrev) / 1000.0; done[prevR] += prevLen; }   // the previous piece was used up by now
        prevR = r; prevLen = n; tPrev = now; } };

// ---- progress window: foobar2000's own progress dialog (modeless, shown only when the wait is longer than half a second), with a
// Cancel button. Used while a track cannot play yet (the file check, the first check of a master, buffering) and for "Verify LAES".
struct LaesProgress {   // one progress bar for the whole wait: permille 0..1000, what is being done, since when
    enum { kIdle = 0, kReading = 1, kChecking = 2, kBuffering = 3 };
    std::atomic<int> phase{kIdle}, masters{0}; std::atomic<int> permille{0}; std::atomic<uint64_t> t0{0};
    std::atomic<unsigned> episode{0}; std::atomic<bool> cancel{false}, shown{false}; std::string name;
};
std::string mmss(double s) { if (s < 0) s = 0; char b[16]; snprintf(b, sizeof b, "%d:%02d", (int)s / 60, (int)s % 60); return b; }
class laes_progress_cb : public threaded_process_callback {
public:
    laes_progress_cb(std::shared_ptr<LaesProgress> p, bool stopPlayback) : m_p(std::move(p)), m_stopPlayback(stopPlayback), m_ep(m_p->episode.load()) {}
    void run(threaded_process_status& st, abort_callback& a) {
        while (m_p->episode.load() == m_ep) {
            if (a.is_aborting()) { m_p->cancel = true; if (m_stopPlayback) fb2k::inMainThread([] { playback_control::get()->stop(); }); break; }
            const LaesProgress& p = *m_p; const double f = p.permille.load() / 1000.0, el = (nowMs() - p.t0.load()) / 1000.0;
            std::string t;
            switch (p.phase.load()) {
            case LaesProgress::kReading: t = T("Verifying file (SHA-256, CRC-32)", "校验文件（SHA-256、CRC-32）"); break;
            case LaesProgress::kChecking: t = std::string(T("Verifying ", "校验 ")) + std::to_string(p.masters) + T(" masters (SHA-256)", " 个母带（SHA-256）"); break;
            case LaesProgress::kBuffering: t = T("Pre-decoding", "预解码"); break;
            default: t = T("Starting", "启动"); }
            char pct[96]; snprintf(pct, sizeof pct, "%.0f %%", f * 100.0);
            t += std::string("\n") + pct + T(" · elapsed ", " · 已用 ") + mmss(el) + (f > 0.02 ? std::string(T(" · remaining about ", " · 剩余约 ")) + mmss(el * (1.0 - f) / f) : std::string());
            st.set_item((m_p->name + "\n" + t).c_str()); st.set_progress_float((std::min)(1.0, f)); Sleep(200); }
        m_p->shown = false; }
private:
    std::shared_ptr<LaesProgress> m_p; bool m_stopPlayback; unsigned m_ep;
};
// opens the window for the current episode of p (once); closeProgress ends the episode and the window
void showProgress(const std::shared_ptr<LaesProgress>& p, bool stopPlayback) {
    if (p->shown.exchange(true)) return; std::shared_ptr<LaesProgress> q = p;
    fb2k::inMainThread([q, stopPlayback] { threaded_process::g_run_modeless(new service_impl_t<laes_progress_cb>(q, stopPlayback),
        threaded_process::flag_show_abort | threaded_process::flag_show_progress | threaded_process::flag_show_item | threaded_process::flag_show_delayed,
        core_api::get_main_window(), T("LAE decoder: LAES", "LAE 解码器：LAES")); }); }
void closeProgress(const std::shared_ptr<LaesProgress>& p) { p->episode++; p->phase = LaesProgress::kIdle; }
// the share of the bar for reading the file before a check (the decoding takes the rest)
const int kReadShare = 60;

// the decoded PCM of one choice (int32 L R per frame) in a temporary file that Windows deletes when it is closed (also after a crash)
class PcmCache {
public:
    ~PcmCache() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool create(uint64_t bytes, std::string& why) {
        wchar_t dir[MAX_PATH + 1]; if (!GetTempPathW(MAX_PATH, dir)) { why = "no temporary folder"; return false; }
        ULARGE_INTEGER freeB; if (GetDiskFreeSpaceExW(dir, &freeB, nullptr, nullptr) && freeB.QuadPart < bytes + (512ull << 20)) {
            why = pfc::string_formatter() << "not enough free space in the temporary folder (" << (unsigned)((bytes >> 20) + 512) << " MB needed)"; return false; }
        static std::atomic<unsigned> n{0}; wchar_t name[MAX_PATH + 64]; swprintf(name, MAX_PATH + 64, L"%slaes_play_%u_%u.pcm", dir, GetCurrentProcessId(), n++);
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE) { why = "cannot create the temporary file"; return false; } return true; }
    bool write(uint64_t at, const void* d, size_t n) { OVERLAPPED o{}; o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32); DWORD w = 0; return WriteFile(h, d, (DWORD)n, &w, &o) && w == n; }
    bool read(uint64_t at, void* d, size_t n) { OVERLAPPED o{}; o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32); DWORD r = 0; return ReadFile(h, d, (DWORD)n, &r, &o) && r == n; }
private:
    HANDLE h = INVALID_HANDLE_VALUE;
};

// ---- LAES files opened (read once in full: file SHA-256, CRC-32, core) and kept for this session, so that switching to another
// choice of the same file does not read it again. An entry is used again only while the file's size and modification time are the
// ones it was read with; otherwise the file is read again. At most 3 files are kept (the core of each is held in memory).
void parkIdleLaes(bool tryOnly);   // below: closes the handles of kept files that nothing uses
struct SharedLaes { OwnReader rd; laes_file* f = nullptr; std::string path; t_filesize size = 0; t_filetimestamp ts = 0; std::atomic<bool> indexing{false};
    // after the full read: every layer is entropy-decoded once on a thread of its own (seconds) so that a start at any frame (switching,
    // seeking) can begin at the block that holds it; the positions are kept in memory only (laes_index_layers)
    std::thread indexer; volatile int cancel = 0;
    // the positions are also kept for later sessions in laes_index\<file SHA-256>.idx in the shared LAE folder (every 16th block start, tens of
    // kB; never written into the media file): loaded when present and matching, else made once by the pass above and saved
    void startIndex() { indexing = true; indexer = std::thread([this] { FpPin pin; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        struct Done { SharedLaes* s; ~Done() { s->indexing = false; parkIdleLaes(true); } } done{this};   // try only: the cache may be destroying this entry
        const std::wstring rec = recordPath();
        if (!rec.empty()) { std::vector<uint8_t> d; FILE* fi = _wfopen(rec.c_str(), L"rb"); if (!fi) fi = _wfopen(recordPath(true).c_str(), L"rb");
            if (fi) { _fseeki64(fi, 0, SEEK_END); const int64_t n = _ftelli64(fi); _fseeki64(fi, 0, SEEK_SET);
                if (n > 0 && n < (64ll << 20)) { d.resize((size_t)n); if (fread(d.data(), 1, d.size(), fi) != d.size()) d.clear(); } fclose(fi); }
            if (!d.empty() && laes_index_load(f, d.data(), (int64_t)d.size()) == 0) { dbg("index record loaded (%u bytes)", (unsigned)d.size()); return; } }
        laes_index_layers(f, &cancel, 0); if (cancel || rec.empty()) return;
        const int64_t n = laes_index_save(f, nullptr, 0); if (n <= 0) return; std::vector<uint8_t> b((size_t)n); if (laes_index_save(f, b.data(), n) != n) return;
        const std::wstring tmp = rec + L".part"; FILE* fo = _wfopen(tmp.c_str(), L"wb"); if (!fo) return; const bool ok = fwrite(b.data(), 1, b.size(), fo) == b.size(); fclose(fo);
        if (!ok || !MoveFileExW(tmp.c_str(), rec.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str()); else dbg("index record saved (%u bytes)", (unsigned)b.size()); }); }
    // in the folder shared by the LAE players (%APPDATA%\LAE\laes_index); old: the foobar2000 profile folder (up to 0.14), read when the shared one is missing
    std::wstring recordPath(bool old = false) const { laes_file_info fi; if (laes_get_info(f, &fi) != 0) return std::wstring();
        static const wchar_t* hx = L"0123456789ABCDEF"; std::wstring name; for (int i = 0; i < 32; i++) { name += hx[fi.file_sha256[i] >> 4]; name += hx[fi.file_sha256[i] & 15]; }
        pfc::string8 p = core_api::get_profile_path(); const char* s = p.get_ptr(); if (!strncmp(s, "file://", 7)) s += 7;
        const std::wstring dir = (old ? std::wstring(pfc::stringcvt::string_wide_from_utf8(s)) : laeSharedDir()) + L"\\laes_index"; if (!old) CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\" + name + L".idx"; }
    ~SharedLaes() { cancel = 1; if (indexer.joinable()) indexer.join(); if (f) { FpPin pin; laes_close(f); } } };
class LaesFileCache {
public:
    static LaesFileCache& get() { static LaesFileCache c; return c; }
    std::shared_ptr<SharedLaes> find(const std::string& path, t_filesize size, t_filetimestamp ts) { std::lock_guard<std::mutex> g(mx);
        for (size_t i = 0; i < items.size(); i++) if (items[i]->path == path && items[i]->size == size && items[i]->ts == ts) { auto e = items[i]; items.erase(items.begin() + i); items.push_back(e); return e; }
        return nullptr; }
    void keep(const std::shared_ptr<SharedLaes>& e) { std::lock_guard<std::mutex> g(mx);
        for (size_t i = 0; i < items.size(); i++) if (items[i]->path == e->path) { items.erase(items.begin() + i); break; }
        items.push_back(e); while (items.size() > 3) items.erase(items.begin()); }
    void clear() { std::lock_guard<std::mutex> g(mx); items.clear(); }
    // closes the file handle of every kept entry that nothing but this cache uses and whose indexer is done (the next read opens it again)
    void parkIdle(bool tryOnly) { std::unique_lock<std::mutex> g(mx, std::defer_lock); if (tryOnly) { if (!g.try_lock()) return; } else g.lock();
        for (auto& e : items) if (e.use_count() == 1 && !e->indexing) e->rd.park(); }
private:
    std::mutex mx; std::vector<std::shared_ptr<SharedLaes>> items;
};
void parkIdleLaes(bool tryOnly) { LaesFileCache::get().parkIdle(tryOnly); }

// decoded samples of a choice around the playing position, made ahead of a switch by LaesPrefetcher: the decoder that made them (positioned
// at b1) and the frames [b0, b1) it gave, interleaved L R. A worker that adopts it continues from b1 instead of starting a decoder.
struct Prefetched { std::shared_ptr<SharedLaes> sh; laes_play* p = nullptr; uint64_t b0 = 0, b1 = 0; std::vector<int32_t> pcm;
    ~Prefetched() { if (p) { FpPin pin; laes_stop(p); } } };

// one LAES choice decoded by a worker thread into a PcmCache. The worker opens the file itself (one full read: file SHA-256, CRC-32,
// core), then decodes the choice in time order. When a master of the file is not yet recorded as verified for this file content, every
// master of the file is decoded and checked in the same run (one pass) and nothing plays before the end; afterwards every choice of the
// file plays at once. When the decoder is slower than playback (the 32-bit build), playback starts once enough is decoded to reach the
// end without a pause. Whenever playback has to wait, one progress bar shows how far it is (Cancel stops playback).
class LaesWorker {
public:
    // start > 0: decoding begins at that frame (switching or seeking inside a track; only for a master already verified for this file
    // content: otherwise the worker checks the whole file from frame 0 first)
    // adopt: samples and the decoder prepared by LaesPrefetcher for this choice (switching without starting a decoder)
    LaesWorker(std::string path, uint64_t size, int choice, int master, uint64_t frames, int rate, uint64_t start = 0, std::unique_ptr<Prefetched> adopt = nullptr)
        : m_path(std::move(path)), m_size(size), m_choice(choice), m_master(master), m_frames(frames), m_rate(rate), m_base(adopt ? adopt->b0 : start), m_written(adopt ? adopt->b0 : start), m_adopt(std::move(adopt)), m_prog(std::make_shared<LaesProgress>()) {
        m_prog->name = pfc::string_filename_ext(m_path.c_str()).get_ptr(); m_prog->t0 = nowMs(); m_guessCheck = master >= 0 && !g_verified.hint(m_path, size);
        m_thread = std::thread([this] { run(); }); }
    ~LaesWorker() { closeProgress(m_prog); m_stop = true; { SharedLaes* o = m_opening.load(); if (o) o->rd.abort.abort(); }   // a read in progress (not yet shared) stops
        if (m_thread.joinable()) m_thread.join(); if (m_play) { FpPin pin; laes_stop(m_play); } }
    // waits until frames [from, from + n) can be played, with the progress bar when that takes a while. Returns false when they never
    // will (error / mismatch / end).
    bool waitFor(uint64_t from, uint64_t n, abort_callback& a) {
        int r = ready(from, n); if (r >= 0) return r == 1;
        m_prog->t0 = nowMs(); m_prog->permille = 0; showProgress(m_prog, true);
        for (;;) { try { a.check(); } catch (...) { closeProgress(m_prog); throw; }
            r = ready(from, n); if (r >= 0) { closeProgress(m_prog); return r == 1; }
            Sleep(50); } }
    bool read(uint64_t from, int32_t* out, uint64_t n) { return from >= m_base && m_cache.read((from - m_base) * 8, out, (size_t)n * 8); }
    uint64_t base() const { return m_base; } uint64_t written() const { return m_written; } bool checking() const { return m_checkAll || !m_opened; }
    // the opened file, once this worker plays a verified master (for LaesPrefetcher); null before
    std::shared_ptr<SharedLaes> shared() const { return m_opened && !m_checkAll ? m_sh : nullptr; }
    bool failed() const { return m_failed; } const std::string& why() const { return m_why; }
private:
    // 1: playable now, 0: never (error / end), -1: not yet (sets the progress)
    int ready(uint64_t from, uint64_t n) {
        if (m_failed) return 0;
        if (from < m_base) return 0;   // before this worker's start: the caller starts another
        const uint64_t need = (std::min)(m_frames, from + n), w = m_written;
        if (m_done) return from < m_frames && w >= need ? 1 : 0;
        // the bar follows the time: the share of this wait gone by, from the time still needed (the speeds measured so far on this machine)
        const double ckRtf = g_speed.get(kMachine, "check_rtf"), ckLeft = ckRtf > 0 && m_rate > 0 ? (double)(m_frames - (std::min)(m_frames, w)) / m_rate / ckRtf : -1;
        if (!m_opened) { SharedLaes* o = m_opening.load(); m_prog->phase = LaesProgress::kReading;
            if (!o) return -1;
            if (!m_guessCheck) { barTo(o->rd.fraction()); return -1; }
            if (ckLeft >= 0) barLeft(o->rd.remaining() + ckLeft); else barTo(o->rd.fraction() * kReadShare / 1000.0);   // a check follows: its time, or a share
            return -1; }
        if (m_checkAll) { m_prog->phase = LaesProgress::kChecking;
            const uint64_t t1 = m_t1.load(), w1 = m_w1.load(); const double el = t1 ? (nowMs() - t1) / 1000.0 : 0;
            if (el > 1 && w > w1) barLeft((double)(m_frames - (std::min)(m_frames, w)) * el / (double)(w - w1));   // measured in this run
            else if (ckLeft >= 0) barLeft(ckLeft); else barTo(kReadShare / 1000.0 + (1000 - kReadShare) / 1000.0 * (double)w / (double)(std::max)(m_frames, (uint64_t)1));
            return -1; }
        // the decoding speed in steady state: frames per second after the first piece (start-up costs are not counted). Until it is known
        // (about 0.5 s of decoding), playback starts with a quarter of a second decoded ahead; once it is known and slower than playback,
        // enough must be decoded that the rest arrives before it is needed
        const uint64_t t1 = m_t1.load(), w1 = m_w1.load(); const double el = t1 ? (nowMs() - t1) / 1000.0 : 0, speed = el > 0.5 && w > w1 ? (double)(w - w1) / el : 0;
        // before it is measured: the speed this file decoded at before on this machine (this choice, or all masters at once as a lower bound)
        const double rtf = speed > 0 ? speed / m_rate : m_knownRtf, few = (std::max)((double)(need - (std::min)(need, from)), m_rate * 0.25);
        const double lead = rtf == 0 || rtf >= 1.05 ? few : (std::max)(few, (double)(m_frames - from) * (1.0 - rtf) * 1.1);
        if (w >= need && (double)(w - from) >= lead) return 1;
        const uint64_t target = (std::max)(need, from + (uint64_t)lead); m_prog->phase = LaesProgress::kBuffering;
        if (rtf > 0 && m_rate > 0) barLeft((double)(target - (std::min)(target, w)) / (rtf * m_rate));
        else barTo((double)(w > from ? w - from : 0) / (double)(std::max)(target - from, (uint64_t)1));
        return -1; }
    // the bar never goes back within one wait (waitFor starts it at 0)
    void barTo(double f) { const int p = (int)(1000.0 * (std::min)(0.99, (std::max)(0.0, f))); if (p > m_prog->permille.load()) m_prog->permille = p; }
    void barLeft(double secondsLeft) { const double el = (nowMs() - m_prog->t0.load()) / 1000.0; barTo(el + secondsLeft > 0 ? el / (el + secondsLeft) : 0); }
    void run() {
        FpPin pin; char err[512] = ""; char mg[5];
        if (m_adopt) {   // a prefetched start: its samples go to the cache, its decoder continues
            m_sh = m_adopt->sh; m_f = m_sh->f; laes_file_info fi; laes_get_info(m_f, &fi); memcpy(m_sha, fi.file_sha256, 32);
            { laes_choice_info ci; if (laes_get_choice(m_f, m_choice, &ci) == 0) m_name = ci.name; } m_knownRtf = (std::max)(g_speed.get(m_sha, m_name), g_speed.get(m_sha, "all"));
            std::string why; if (!m_cache.create((m_frames - m_base) * 8, why)) { fail(why); return; }
            if (!m_adopt->pcm.empty() && !m_cache.write(0, m_adopt->pcm.data(), m_adopt->pcm.size() * 4)) { fail(T("cannot write the temporary file (disk full?)", "无法写入临时文件（磁盘已满？）")); return; }
            m_play = m_adopt->p; m_adopt->p = nullptr; m_written = m_adopt->b1; m_adopt.reset(); m_opened = true; decodeLoop(); return; }
        // the file as opened earlier in this session (same size and modification time), or read now in full
        t_filestats st{}; try { bool wr = false; filesystem::g_get_stats(m_path.c_str(), st, wr, m_statAbort); } catch (...) { st.m_size = m_size; st.m_timestamp = 0; }
        m_sh = st.m_timestamp ? LaesFileCache::get().find(m_path, st.m_size, st.m_timestamp) : nullptr;
        if (!m_sh) { auto e = std::make_shared<SharedLaes>(); e->path = m_path; e->rd.path = m_path; e->size = st.m_size; e->ts = st.m_timestamp; m_opening = e.get();
            try { filesystem::g_open(e->rd.f, m_path.c_str(), filesystem::open_mode_read, e->rd.abort); e->rd.locateCore(m_size); } catch (const std::exception& x) { m_opening = nullptr; fail(std::string("cannot open the file: ") + x.what()); return; }
            e->f = laes_open_reader(&OwnReader::read, &e->rd, m_size, 0, mg, err, sizeof err); m_opening = nullptr; if (e->f) e->rd.remember();
            if (!e->f) { fail(err); return; }
            m_sh = e; e->startIndex(); if (st.m_timestamp) LaesFileCache::get().keep(e); }
        m_f = m_sh->f; { laes_choice_info ci; if (laes_get_choice(m_f, m_choice, &ci) == 0) m_name = ci.name; }
        laes_file_info fi; laes_get_info(m_f, &fi); memcpy(m_sha, fi.file_sha256, 32); m_knownRtf = (std::max)(g_speed.get(m_sha, m_name), g_speed.get(m_sha, "all"));
        if (!fi.payload_ok && m_choice != 0) { fail(laes_file_problem(m_f)); return; }
        const int nm = mastersOf(m_f); m_prog->masters = nm; noteFailed(m_path, m_sha, nm);
        if (m_master >= 0) { const int v = g_verified.get(m_sha, m_master); if (v < 0) { fail(T("SHA-256 mismatch (verified earlier, file unchanged)", "SHA-256 不符（此前已校验，文件未变）")); return; }
            bool all = true; for (int m = 0; m < nm; m++) all &= g_verified.get(m_sha, m) != 0;
            m_checkAll = v == 0;   // its master is not checked yet: check every master of the file now, in this one run
            if (m_checkAll && m_base) { m_base = 0; m_written = 0; }   if (m_checkAll) dbg("worker: master %d not verified: checks all from 0", m_master);   // a check runs from frame 0
            if (m_checkAll) { const std::string msg = std::string(T("LAES: verifying ", "LAES：校验 ")) + std::to_string(nm) +
                  T(" masters (SHA-256, first playback only)", " 个母带（SHA-256，仅首次播放）");
                fb2k::inMainThread([msg] { console::print(msg.c_str()); }); }
            (void)all; }
        std::string why; if (!m_cache.create((m_frames - m_base) * 8, why)) { fail(why); return; }
        m_play = m_checkAll ? laes_start_all(m_f, m_choice, 0, err, sizeof err) : m_base ? laes_start_at(m_f, m_choice, m_base, 0, err, sizeof err) : laes_start(m_f, m_choice, 0, err, sizeof err); if (!m_play) { fail(err); return; }
        m_opened = true; decodeLoop(); }
    // decodes from m_written on into the cache; at the end of a run from frame 0, records the masters' results
    void decodeLoop() {
        std::vector<int32_t> buf(65536 * 2); uint64_t at = m_written; int pieces = 0;
        while (!m_stop) { const int64_t want = pieces < 4 ? 8192 : 65536;   // small first pieces: the first audio is there sooner
            const int64_t k = laes_read(m_play, buf.data(), want); if (k < 0) { fail(laes_play_error(m_play)); return; } if (k == 0) break;
            if (!m_cache.write((at - m_base) * 8, buf.data(), (size_t)k * 8)) { fail(T("cannot write the temporary file (disk full?)", "无法写入临时文件（磁盘已满？）")); return; } at += (uint64_t)k; m_written = at;
            if (++pieces == 1) { m_w1 = at; m_t1 = nowMs(); } }   // the speed is measured from here on
        { const uint64_t t1 = m_t1.load(); const double el = t1 ? (nowMs() - t1) / 1000.0 : 0; const uint64_t w = m_written, w1 = m_w1;   // the speed, for the next start
          if (el >= 3 && w > w1 && m_rate > 0) { const double rtf = (double)(w - w1) / m_rate / el; g_speed.put(m_sha, m_checkAll ? std::string("all") : m_name, rtf); if (m_checkAll) g_speed.put(kMachine, "check_rtf", rtf); } }
        if (m_stop) return;
        if (m_base) { m_done = true; return; }   // a run from a later frame checks nothing (its master was verified before)
        const int nm = mastersOf(m_f);
        recordRun(m_play, m_sha, nm); noteFailed(m_path, m_sha, nm); { bool all = true; for (int m = 0; m < nm; m++) all &= g_verified.get(m_sha, m) > 0; if (all) g_verified.putHint(m_path, m_size); }
        if (m_master >= 0 && laes_master_ok(m_play, m_master) != 1) { fail(laes_play_error(m_play)); return; }
        m_done = true; }
    void fail(const std::string& why) { dbg("worker fail: %s", why.c_str()); m_why = why; m_failed = true; m_done = true; }
    std::string m_path; uint64_t m_size; int m_choice, m_master; uint64_t m_frames; int m_rate; std::atomic<uint64_t> m_base; std::atomic<uint64_t> m_written; std::atomic<uint64_t> m_t1{0}, m_w1{0};
    std::shared_ptr<SharedLaes> m_sh; std::atomic<SharedLaes*> m_opening{nullptr}; abort_callback_impl m_statAbort; std::thread m_thread; std::atomic<bool> m_stop{false}, m_done{false}, m_failed{false}, m_opened{false}, m_checkAll{false}; bool m_guessCheck = false;
    std::string m_why, m_name; unsigned char m_sha[32] = {0}; std::unique_ptr<Prefetched> m_adopt; std::atomic<double> m_knownRtf{0};
    laes_file* m_f = nullptr; laes_play* m_play = nullptr; PcmCache m_cache; std::shared_ptr<LaesProgress> m_prog;
};

// a worker that is replaced (switching, seeking) is stopped and freed on a thread of its own, so that playback does not wait for its
// threads to end; foobar2000 waits for these at exit (laes_quit)
std::atomic<int> g_retiring{0};
void retireWorker(std::unique_ptr<LaesWorker> w) { if (!w) return; LaesWorker* p = w.release(); g_retiring++;
    try { std::thread([p] { delete p; g_retiring--; parkIdleLaes(false); }).detach(); } catch (...) { delete p; g_retiring--; parkIdleLaes(false); } }

// ---- the other masters kept ready (setting; on by default): while one choice plays, a thread of lowered priority keeps a few seconds
// around the playing time decoded for every other master of the file that is verified for this file content, each started at its
// position by the noted block positions and then decoded at the pace of playback. A switch takes these samples and their decoder over
// (LaesWorker adopt), so the new master plays without starting a decoder. The seconds kept follow the free memory of the machine.
class LaesPrefetcher {
public:
    LaesPrefetcher(std::shared_ptr<SharedLaes> sh, std::vector<std::pair<int, laes_choice_info>> cs) : m_sh(std::move(sh)) {   // (choice number, its info)
        for (auto& c : cs) { Item it; it.idx = c.first; it.ci = c.second; m_items.push_back(std::move(it)); }
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
        m_keep = ms.ullAvailPhys < (2ull << 30) || ms.ullAvailVirtual < (1ull << 30) ? 1.0 : 3.0;   // seconds ahead: less on a machine short of memory
        m_thread = std::thread([this] { run(); }); }
    ~LaesPrefetcher() { m_stop = true; if (m_thread.joinable()) m_thread.join(); }
    void setTime(double t) { m_time = t; }
    // the prepared samples of choice index `idx` (laes choice number) when they hold frame `at`; null otherwise
    std::unique_ptr<Prefetched> take(int idx, uint64_t at) { m_hold++; std::lock_guard<std::mutex> g(m_mx); m_hold--;   // the thread lets go of the lock within one piece
        for (auto& it : m_items) if (it.idx == idx && it.p && it.b0 <= at && at < it.b1) {
            std::unique_ptr<Prefetched> r(new Prefetched()); r->sh = m_sh; r->p = it.p; it.p = nullptr; r->b0 = it.b0; r->b1 = it.b1; r->pcm.swap(it.pcm); it.b0 = it.b1 = 0; return r; }
        return nullptr; }
private:
    struct Item { int idx = -1; laes_choice_info ci{}; laes_play* p = nullptr; uint64_t b0 = 0, b1 = 0; std::vector<int32_t> pcm; };
    std::shared_ptr<SharedLaes> m_sh; std::vector<Item> m_items; std::mutex m_mx; std::thread m_thread; std::atomic<bool> m_stop{false}; std::atomic<int> m_hold{0}; std::atomic<double> m_time{0}; double m_keep = 3.0;
    void run() { FpPin pin; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); std::vector<int32_t> buf(16384 * 2); char err[256];
        while (!m_stop) { bool busy = false;
            for (auto& it : m_items) { if (m_stop) break; while (m_hold.load() > 0 && !m_stop) Sleep(1);   // a switch is taking samples: it gets the lock first
                std::lock_guard<std::mutex> g(m_mx); if (it.idx < 0) continue;
                const uint64_t pos = (std::min)(it.ci.frames, (uint64_t)(m_time.load() * it.ci.rate)), ahead = (uint64_t)(m_keep * it.ci.rate), behind = (uint64_t)((m_keep >= 3 ? 3.0 : 1.5) * it.ci.rate);
                // nothing yet, or the playing time moved away (a seek): before the kept samples, or further ahead than decoding catches up with soon
                if (!it.p || pos < it.b0 || pos > it.b1 + behind + 2 * (uint64_t)it.ci.rate) {
                    if (it.p) laes_stop(it.p); it.pcm.clear(); const uint64_t s = pos > behind ? pos - behind : 0;
                    it.p = laes_start_at(m_sh->f, it.idx, s, 2, err, sizeof err); it.b0 = it.b1 = s; dbg("prefetch %d start at %llu (pos %llu)", it.idx, (unsigned long long)s, (unsigned long long)pos); if (!it.p) { it.idx = -1; continue; } }
                if (it.b1 < (std::min)(it.ci.frames, pos + ahead)) { const int64_t k = laes_read(it.p, buf.data(), 16384);
                    if (k <= 0) dbg("prefetch %d read %lld at %llu", it.idx, (long long)k, (unsigned long long)it.b1);
                    if (k <= 0) { laes_stop(it.p); it.p = nullptr; continue; }
                    it.pcm.insert(it.pcm.end(), buf.begin(), buf.begin() + k * 2); it.b1 += (uint64_t)k; busy = true; }
                if (pos > behind && it.b0 + behind < pos - behind) { const uint64_t drop = (std::min)((pos - behind) - it.b0, it.b1 - it.b0);   // older than the kept seconds
                    it.pcm.erase(it.pcm.begin(), it.pcm.begin() + (size_t)drop * 2); it.b0 += drop; } }
            if (!busy) Sleep(30); }
        std::lock_guard<std::mutex> g(m_mx); for (auto& it : m_items) if (it.p) { laes_stop(it.p); it.p = nullptr; } }
};
void retirePrefetcher(std::unique_ptr<LaesPrefetcher> p) { if (!p) return; LaesPrefetcher* q = p.release(); g_retiring++;
    try { std::thread([q] { delete q; g_retiring--; parkIdleLaes(false); }).detach(); } catch (...) { delete q; g_retiring--; parkIdleLaes(false); } }

// ---- checking LAES files ahead of playback: files added to the media library (setting) and the context menu command (one progress
// bar per file; Cancel skips the file). One file at a time, on a background thread of lowered priority; all masters of a file not yet
// recorded are decoded and checked in one pass.
class LaesVerifier {
public:
    static LaesVerifier& get() { static LaesVerifier v; return v; }
    void add(const char* path, bool manual) { std::lock_guard<std::mutex> g(mx); for (auto& q : queue) if (q.first == path) { q.second |= manual; return; }
        queue.push_back({path, manual}); cv.notify_one(); if (!thread.joinable()) thread = std::thread([this] { loop(); }); }
    void quit() { { std::lock_guard<std::mutex> g(mx); stop = true; queue.clear(); } abort.abort(); cv.notify_one(); if (thread.joinable()) thread.join(); }
private:
    std::mutex mx; std::condition_variable cv; std::deque<std::pair<std::string, bool>> queue; std::thread thread; bool stop = false; abort_callback_impl abort;
    void loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) { std::pair<std::string, bool> p; { std::unique_lock<std::mutex> g(mx); cv.wait(g, [this] { return stop || !queue.empty(); }); if (stop) return; p = queue.front(); queue.pop_front(); }
            try { one(p.first, p.second); } catch (...) {} } }
    static void say(const std::string& s) { const std::string m = s; fb2k::inMainThread([m] { console::print(m.c_str()); }); }
    bool stopping() { std::lock_guard<std::mutex> g(mx); return stop; }
    void one(const std::string& path, bool manual) {
        dbg("verify %s manual %d", path.c_str(), (int)manual);
        FpPin pin; OwnReader rd; filesystem::g_open(rd.f, path.c_str(), filesystem::open_mode_read, abort);
        const t_filesize size = rd.f->get_size(abort); char err[512] = "", mg[5];
        const std::string name = pfc::string_filename_ext(path.c_str()).get_ptr();
        std::shared_ptr<LaesProgress> pr = std::make_shared<LaesProgress>(); pr->name = name; pr->phase = LaesProgress::kReading; pr->t0 = nowMs();
        if (manual) showProgress(pr, false);
        // the bar follows the time: the file read (timed by OwnReader) and the check after it (the machine's measured check speed)
        double secs = 0; { laes_file* q = laes_open_reader(&OwnReader::read, &rd, size, 1, mg, err, sizeof err); if (q) { laes_file_info qi; laes_get_info(q, &qi); if (qi.base_rate > 0) secs = (double)qi.core_frames / qi.base_rate; laes_close(q); } }
        rd.locateCore(size); const double ckRtf = g_speed.get(kMachine, "check_rtf"); const double ckAll = ckRtf > 0 && secs > 0 ? secs / ckRtf : -1;
        auto bar = [&](double left, double share) { int p; if (left >= 0) { const double el = (nowMs() - pr->t0.load()) / 1000.0; p = (int)(1000.0 * (el + left > 0 ? el / (el + left) : 0)); } else p = (int)(1000.0 * share);
            p = (std::min)(990, p); if (p > pr->permille.load()) pr->permille = p; };
        std::atomic<bool> reading{true}; std::thread ticker([&] { while (reading) { bar(ckAll >= 0 ? rd.remaining() + ckAll : -1, rd.fraction() * kReadShare / 1000.0); Sleep(100); } });
        laes_file* f = laes_open_reader(&OwnReader::read, &rd, size, 0, mg, err, sizeof err); reading = false; ticker.join(); if (f) rd.remember();
        if (!f) { closeProgress(pr); return; }   // not LAES (or unreadable): nothing to do
        laes_file_info fi; laes_get_info(f, &fi);
        if (!fi.payload_ok) { closeProgress(pr); say("LAES " + name + ": " + laes_file_problem(f) + T("; playing the core master", "；播放核心母带")); laes_close(f); return; }
        const int nm = mastersOf(f); pr->masters = nm; noteFailed(path, fi.file_sha256, nm); bool todo = false; for (int m = 0; m < nm; m++) todo |= g_verified.get(fi.file_sha256, m) == 0;
        if (todo && !stopping() && !pr->cancel) {
            int choice = 1; laes_choice_info ci{}; for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(f, i, &c); if (c.kind == LAES_KIND_MASTER) { choice = i; ci = c; break; } }
            laes_play* p = laes_start_all(f, choice, (std::max)(1, (int)std::thread::hardware_concurrency() / (manual ? 1 : 2)), err, sizeof err);
            if (p) { pr->phase = LaesProgress::kChecking; std::vector<int32_t> buf(65536 * 2); const uint64_t t0 = nowMs(); bool stopped = false; uint64_t got = 0;
                for (;;) { if (stopping() || pr->cancel) { stopped = true; break; } const int64_t k = laes_read(p, buf.data(), 65536); if (k <= 0) break; got += (uint64_t)k;
                    const double el = (nowMs() - t0) / 1000.0; bar(el > 1 && got ? (double)(ci.frames - (std::min)(ci.frames, got)) * el / (double)got : ckAll >= 0 ? ckAll * (1.0 - (double)got / (double)(std::max)(ci.frames, (uint64_t)1)) : -1.0, kReadShare / 1000.0 + (1000 - kReadShare) / 1000.0 * (double)got / (double)(std::max)(ci.frames, (uint64_t)1)); }
                if (!stopped && got > 0) { const double el = (nowMs() - t0) / 1000.0; if (el > 0 && ci.rate > 0) { g_speed.put(fi.file_sha256, "all", (double)got / ci.rate / el); if (manual && got == ci.frames) g_speed.put(kMachine, "check_rtf", (double)got / ci.rate / el); } }   // all masters at once: a lower bound for any choice
                if (!stopped && laes_verified(p) >= 0) { recordRun(p, fi.file_sha256, nm); noteFailed(path, fi.file_sha256, nm); unsigned ok = 0; for (int m = 0; m < nm; m++) ok += laes_master_ok(p, m) == 1; if (ok == (unsigned)nm) g_verified.putHint(path, size);
                    say(std::string((pfc::string_formatter() << "LAES " << name.c_str() << T(": ", "：") << ok << " / " << nm << T(" masters verified", " 个母带校验通过")
                        << (ok == (unsigned)nm ? "" : T("; SHA-256 mismatch in the others", "；其余 SHA-256 不符")) << " (" << (unsigned)((nowMs() - t0) / 1000) << " s)").get_ptr())); }
                laes_stop(p); } }
        else if (manual) say("LAES " + name + T(": all masters verified earlier", "：全部母带此前已校验"));
        if (pr->cancel) say("LAES " + name + T(": verification cancelled", "：校验已取消"));
        closeProgress(pr); laes_close(f); }
};
}

// ---- tags of the WAV form (LAEY, and LAES in the WAV form), 2026-11-06: the RIFF LIST/INFO chunk and the "id3 " chunk (ID3v2.3 / 2.4) under
// foobar2000's field names. ID3 first (it holds every field; laes / lae_enc write the same values to both); INFO adds what ID3 lacks. Text
// that is not UTF-8 (INFO from older programs, ID3 encoding 0) is read in the system code page / Latin-1. Without this, foobar2000 showed
// the raw <IART> / <INAM> or nothing, and a tag edit dropped the fields it had not read.
namespace wavtags {   // the shared reader (src/lae_wavtags.h), read through foobar2000's file object
typedef lae_wavtags::List List;
inline List read(service_ptr_t<file>& f, t_filesize size, abort_callback& a) {
    return lae_wavtags::read([&](uint64_t off, void* buf, size_t n) { try { f->seek(off, a); f->read_object(buf, n, a); return true; } catch (...) { return false; } }, size); }
}  // namespace wavtags

class input_lae : public input_stubs {
public:
    ~input_lae() { if (m_laes) dbg("%p closed at %llu", (void*)this, (unsigned long long)m_pos);
        m_pf.reset(); m_worker.reset();
        if (m_lq) { FpPin pin; laes_close(m_lq); }
        if (m_track) { FpPin pin; lae_close(m_track); }
        if (m_playback) { std::lock_guard<std::mutex> g(s_lock); s_live.erase(std::remove(s_live.begin(), s_live.end(), this), s_live.end()); }
        parkIdleLaes(false); }   // a file this track played stays decodable in the cache, but no longer open
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_io_unsupported_format();   // tags are not written here: foobar2000's own WAV / FLAC writer does it (it gets every field from get_info)
        m_file = p_filehint; input_open_file_helper(m_file, p_path, p_reason, p_abort); m_path = p_path;
        // cheap check first: a RIFF/WAVE whose chunk list contains "LAE ", or a FLAC file with "LAE2" APPLICATION blocks (anything else
        // goes to the next decoder). The first 4 bytes of the LAE chunk name the format: LAE1 = LAEY, LAE2 = LAES.
        const t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size < 12 + 8 + 24) throw exception_io_unsupported_format();
        uint8_t h[12]; m_file->read_object(h, 12, p_abort);
        if (!memcmp(h, "fLaC", 4)) { m_size = size;
            // FLAC form: "LAE2" APPLICATION blocks = LAES; "LAE1" blocks = LAEY (2026-11-10: the CD layer in the FLAC frames, the extension in the
            // blocks; its tags are the Vorbis comments). A plain FLAC, or an LAEY file liblae refuses, goes to the next decoder (foobar2000's
            // FLAC decoder, which plays the CD layer).
            const laeyflac::ReadFn rd = [&](uint64_t off, void* buf, size_t n) { try { m_file->seek(off, p_abort); m_file->read_object(buf, n, p_abort); return true; } catch (...) { return false; } };
            laeyflac::Layout L; bool notLaey = true; uint8_t hd[24];
            if (laeyflac::layout(rd, size, L).empty() && !L.lae.empty()) {
                p_abort.check();
                if (!laeyflac::check(L, notLaey).empty() || !laeyflac::head(rd, L, hd, 24) || le32(hd) != 0x3145414Cu || le32(hd + 4) != 5) throw exception_io_unsupported_format();
                m_master = (int)le32(hd + 8); m_base = (int)le32(hd + 12); m_stages = (int)le32(hd + 16); m_masterFrames = le32(hd + 20);
                if ((m_base != 44100 && m_base != 48000) || m_stages < 1 || m_stages > 3 || m_master != (m_base << m_stages)) throw exception_io_unsupported_format();
                m_flacForm = true; m_wavTags = laeyflac::comments(rd, L);
                if (L.laeLen > 4) initPacks([&](uint64_t off, void* buf, size_t n) { uint8_t* o = (uint8_t*)buf;   // the payload lies in the pieces of the "LAE1" blocks
                    for (size_t i = 0; i < L.lae.size() && n; i++) { if (off >= L.lae[i].len) { off -= L.lae[i].len; continue; } const size_t k = (size_t)(std::min)((uint64_t)n, L.lae[i].len - off);
                        if (!rd(L.lae[i].off + off, o, k)) return false; o += k; n -= k; off = 0; }
                    return n == 0; }, L.laeLen - 4);
                if (p_reason == input_open_decode) load(p_abort);
                return; }
            p_abort.check(); laesOpen(p_abort); return; }
        if ((!memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4)) { m_size = size; m_wavForm = true; m_wavTags = wavtags::read(m_file, size, p_abort); laesOpen(p_abort); return; }   // RF64 (beyond 4 GB): LAES only
        if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) throw exception_io_unsupported_format();
        t_filesize pos = 12, laeAt = 0, laeLen = 0;
        while (pos + 8 <= size) { uint8_t c[8]; m_file->seek(pos, p_abort); m_file->read_object(c, 8, p_abort); const t_filesize n = le32(c + 4);
            if (!memcmp(c, "LAE ", 4)) { laeAt = pos + 8; laeLen = (std::min)(n, size - laeAt); break; } pos += 8 + n + (n & 1); }
        if (!laeAt || laeAt + 24 > size) throw exception_io_unsupported_format();
        uint8_t hd[24]; m_file->seek(laeAt, p_abort); m_file->read_object(hd, 24, p_abort);
        if (le32(hd) == 0x3245414Cu) { m_size = size; m_wavForm = true; m_wavTags = wavtags::read(m_file, size, p_abort); laesOpen(p_abort); return; }   // LAE2: LAES
        if (size > 0xFFFFFFFFull + 8) throw exception_io_unsupported_format();
        if (le32(hd) != 0x3145414Cu || le32(hd + 4) != 5) throw exception_io_unsupported_format();   // not container v5
        m_master = (int)le32(hd + 8); m_base = (int)le32(hd + 12); m_stages = (int)le32(hd + 16); m_masterFrames = le32(hd + 20);
        if ((m_base != 44100 && m_base != 48000) || m_stages < 1 || m_stages > 3 || m_master != (m_base << m_stages)) throw exception_io_unsupported_format();
        m_size = size; m_wavForm = true; m_wavTags = wavtags::read(m_file, size, p_abort);
        if (laeLen > 4) initPacks([&](uint64_t off, void* buf, size_t n) { try { m_file->seek(laeAt + off, p_abort); m_file->read_object(buf, n, p_abort); return true; } catch (...) { return false; } }, laeLen - 4);
        if (p_reason == input_open_decode) load(p_abort);
    }
    void get_info(file_info& p_info, abort_callback&) {
        if (m_laes) { laesInfo(p_info); return; }
        p_info.set_length((double)m_masterFrames / m_master);
        const bool cd = m_cdOnly || m_fallbackNow;   // known only to a decoding instance
        p_info.info_set_int("samplerate", cd ? m_base : outRate());
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", cd || layers() == 2 || layers() == 3 ? 16 : 24);   // shown: no depth layer = 16 bit
        p_info.info_set("encoding", "lossy");   // LAEY is the lossy tier (LAES is the lossless one)
        p_info.info_set("codec", "LAEY");
        p_info.info_set("lae_master", pfc::string_formatter() << (m_master0 ? m_master0 : m_master) << " Hz");   // pack 0's master rate (the layer menu is built from it)
        if (!m_packs.empty()) { p_info.info_set_int("lae_packs", (t_int64)m_packs.size()); p_info.info_set_int("lae_pack", m_pack);   // further packs: "<master rate> <name>"; the pack in use
            for (size_t k = 0; k < m_packs.size(); k++) p_info.info_set((std::string("lae_pack_") + std::to_string(k + 1)).c_str(), (std::to_string(m_packs[k].rate) + " " + m_packs[k].name).c_str()); }
        if (!m_mainName.empty()) p_info.info_set("lae_pack_0", m_mainName.c_str());   // the main body's name, when the file stores one
        p_info.info_set(m_base == 44100 ? "lae_cd_layer" : "lae_core_layer", pfc::string_formatter() << "16 bit / " << m_base << " Hz");
        p_info.info_set("lae_layers", (baseName(m_base) + " " + khz(m_base) + T(" / 16-bit · bit-depth layer 24-bit · frequency layer up to ",
            " / 16 位 · 位深层 24 位 · 频率层最高 ") + khz(m_master)).c_str());
        p_info.info_set_bitrate((t_int64)((m_size * 8) / ((double)m_masterFrames / m_master) / 1000.0 + 0.5));   // the whole file's average
        for (auto& kv : m_wavTags) p_info.meta_add(kv.first.c_str(), kv.second.c_str());
    }
    t_filestats get_file_stats(abort_callback& p_abort) { return m_file->get_stats(p_abort); }
#if FOOBAR2000_TARGET_VERSION >= 81
    t_filestats2 get_stats2(uint32_t f, abort_callback& a) { return m_file->get_stats2_(f, a); }
#endif
    void decode_initialize(unsigned p_flags, abort_callback& p_abort) {
        if (m_laes) { m_testing = (p_flags & input_flag_testing_integrity) != 0;
            if ((p_flags & input_flag_playback) && !m_playback) { m_playback = true; std::lock_guard<std::mutex> g(s_lock); s_live.push_back(this); }
            dbg("%p initialize flags %u path %s", (void*)this, p_flags, m_path.get_ptr());
            m_pos = 0; m_laesPlayed = false; m_laesFallback.clear(); m_laesFellBack = false; laesStart(laesPick(), p_abort); if (!m_laesFellBack.load()) refreshStoredInfo(); return; }
        if (!m_track && !m_cdOnly) load(p_abort);
        // integrity tests (input.h): a track that can only play as its CD layer fails, even when only the CD layer was asked for
        m_testing = (p_flags & input_flag_testing_integrity) != 0;
        if (m_testing && m_cdOnly) throw exception_io_data((std::string("LAE: ") + m_fallback).c_str());
        if ((p_flags & input_flag_playback) && !m_playback) { m_playback = true; std::lock_guard<std::mutex> g(s_lock); s_live.push_back(this); }
        m_pos = 0; m_fallbackNow = false;
        if (m_cdOnly) { m_rate = m_base; m_bits = 16; m_fallbackNow = true; m_route = LAE_ROUTE_CD; m_report = true; return; }
        startRoute(m_testing);
        if (!m_fallbackNow) refreshStoredInfo();
    }
    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        enum { block = 4096 };
        if (m_laes) return laesRun(p_chunk, p_abort);
        applyLayerChange();   // e.g. the next track was opened before the user changed the layers
        if (m_cdOnly) {   // the CD layer itself (the LAE extension could not be verified)
            const size_t frames = m_cd.size() / 2; if (m_pos >= frames) return false; const size_t n = (std::min)((size_t)block, frames - m_pos);
            p_chunk.set_data_fixedpoint(&m_cd[m_pos * 2], n * 4, m_rate, 2, 16, audio_chunk::channel_config_stereo); m_pos += n; return true; }
        int64_t got; { FpPin pin; m_buf.resize(2 * block); got = lae_read(m_track, m_buf.data(), block); }
        if (got < 0) throw exception_io_data(T("LAEY: stream error (extension layer damaged)", "LAEY：码流错误（扩展层损坏）"));   // never switch to the CD layer inside a track
        if (got == 0) return false;
        const int bps = m_bits / 8; m_bytes.resize((size_t)got * 2 * bps);
        for (size_t i = 0; i < (size_t)got * 2; i++) { const int32_t v = m_buf[i]; for (int k = 0; k < bps; k++) m_bytes[i * bps + k] = (uint8_t)(v >> (8 * k)); }
        p_chunk.set_data_fixedpoint(m_bytes.data(), m_bytes.size(), m_rate, 2, m_bits, audio_chunk::channel_config_stereo);
        m_pos += (size_t)got; countBitrate((uint64_t)got); return true;
    }
    void decode_seek(double p_seconds, abort_callback& p_abort) {
        if (m_laes) {   // the menus switch by seeking to the audible time: a new choice begins right there (not where decoding had got to)
            dbg("%p seek %.3f s: m_pos %llu rate %d choice %d gen %u/%u", (void*)this, p_seconds, (unsigned long long)m_pos, m_rate, m_laesChoice, m_gen, s_layerGen.load());
            if (laesApplyChange(p_abort, p_seconds)) return;
            const uint64_t to = (std::min)(m_laesFrames, (uint64_t)audio_math::time_to_samples(p_seconds, m_rate));
            // before this worker's start (it can never give that), or beyond what it will have within 2 s: a new worker begins right there
            if (!m_worker ? to < m_laesFrames : !m_worker->failed() && (to < m_worker->base() || (!m_worker->checking() && to > m_worker->written() + (uint64_t)m_rate * 2))) laesStart(m_laesChoice, p_abort, to);
            m_pos = (size_t)to; m_report = true; return; }   // m_report: foobar2000 drops the dynamic info on a seek; the status bar shows what plays again
        applyLayerChange();   // the layer menu seeks to the current position, so the change is heard at once
        const uint64_t frame = audio_math::time_to_samples(p_seconds, m_rate);
        m_report = true;   // foobar2000 1.5 drops the dynamic info on a seek: the status bar shows the layers again
        if (m_cdOnly) { m_pos = (size_t)(std::min)((uint64_t)(m_cd.size() / 2), frame); return; }
        FpPin pin; if (lae_seek(m_track, frame) != 0) throw exception_io_data(T("LAEY: seek failed", "LAEY：跳转失败")); m_pos = (size_t)frame; resetBitrate();
    }
    bool decode_can_seek() { return true; }
    // The status bar shows the sample rate of the info foobar2000 has stored for the file. It follows a change of rate inside a track (the
    // layer menu), but not the rate reported with a track's first block (decode_get_dynamic_info): a track started with other layers than
    // the ones the file was last read with kept the old rate ("LAE CD layer | 1411 kbps | 352800 Hz", foobar2000 2.26.2, 2026-11-10). So when
    // a playback start plays another rate or word length than the stored info names, foobar2000 is asked to read the file's info again (once
    // per start, from the main thread, no dialog): get_info then describes what the current settings play. Not after a fallback to the CD
    // layer or core master (get_info cannot know of it; the status bar's label says so).
    void refreshStoredInfo() {
        if (!m_playback || m_testing || m_rate <= 0) return;
        try { metadb_handle_ptr h; metadb::get()->handle_create(h, make_playable_location(m_path.get_ptr(), 0));
            metadb_info_container::ptr stored; if (!h->get_info_ref(stored)) return;
            const t_int64 rate = stored->info().info_get_int("samplerate"), bits = stored->info().info_get_int("bitspersample");
            const int shownBits = !m_laes && m_route == LAE_ROUTE_16 ? 16 : m_bits;   // as get_info shows it (no depth layer = 16 bit)
            if (rate <= 0 || (rate == m_rate && (bits <= 0 || bits == shownBits))) return;
            fb2k::inMainThread([h] { try { metadb_handle_list one; one.add_item(h);
                metadb_io_v2::get()->load_info_async(one, metadb_io::load_info_force, core_api::get_main_window(),
                    metadb_io_v2::op_flag_no_errors | metadb_io_v2::op_flag_background | metadb_io_v2::op_flag_delay_ui, nullptr); } catch (...) {} }); }
        catch (...) {} }
    // the layers and format that actually play, once per start (the file's info describes what the settings ask for): codec "LAE" + the
    // layers is what the status bar shows; lae_route gives the reason when the CD layer plays instead. And the live bitrate, once a second.
    bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
        if (!m_report && !m_reportRate) return false;
        p_timestamp_delta = 0;
        if (m_laes) { p_out.info_set("codec", m_laesLabel.c_str()); p_out.info_set_int("samplerate", m_rate); p_out.info_set_int("bitspersample", m_bits);
            p_out.info_set("encoding", "lossless");
            if (!m_laesFallback.empty()) p_out.info_set("laes_note", m_laesFallback.c_str()); else p_out.info_remove("laes_note");
            m_report = m_reportRate = false; return true; }
        if (m_report) {
            p_out.info_set("codec", routeLabel().c_str()); p_out.info_set_int("samplerate", m_rate); p_out.info_set_int("bitspersample", m_route == LAE_ROUTE_16 ? 16 : m_bits);
            if (m_fallbackNow) p_out.info_set("lae_route", (baseName(m_base) + T(" (", "（") + m_fallback + T(")", "）")).c_str()); else p_out.info_remove("lae_route");
            if (m_route == LAE_ROUTE_CD) p_out.info_set_bitrate_vbr((t_int64)std::lround(cdKbps())); }
        if (m_reportRate) p_out.info_set_bitrate_vbr(m_kbps);
        m_report = m_reportRate = false; return true; }
    bool decode_get_dynamic_info_track(file_info&, double&) { return false; }
    void decode_on_idle(abort_callback& p_abort) { if (!m_laes) m_file->on_idle(p_abort); }
    void retag(const file_info&, abort_callback&) { throw exception_io_unsupported_format(); }
    void remove_tags(abort_callback&) { throw exception_io_unsupported_format(); }
    // a forward seek decodes the entropy streams up to the target (no seek table in v5): tell the host to avoid needless seeks
    size_t extended_param(const GUID& p_type, size_t, void*, size_t) { return p_type == input_params::seeking_expensive ? 1 : 0; }

    static bool g_is_our_content_type(const char*) { return false; }
    static bool g_is_our_path(const char*, const char* p_extension) {
        return stricmp_utf8(p_extension, "wav") == 0 || stricmp_utf8(p_extension, "lae") == 0 || stricmp_utf8(p_extension, "flac") == 0; }
    static const char* g_get_name() { return T("LAE decoder", "LAE 解码器"); }
    static const GUID g_get_guid() { return guid_input[g_slot.current]; }
    static std::atomic<unsigned> s_layerGen;   // bumped by the layer menu
    static std::atomic<uint64_t> s_menuSeekMs;   // when a menu last seeked to switch the playing track (the switch is made at that seek)
    // whether the playback decoder of this path would play other layers with the current settings (the layer menu seeks only then)
    static bool s_wouldChange(const char* path) {
        std::lock_guard<std::mutex> g(s_lock);
        for (const input_lae* p : s_live) if ((!path || !strcmp(p->m_path.c_str(), path)) && p->wouldChange()) return true;
        return false; }   // path null: any playback decoder (a cue-sheet track's path is the .cue, not the audio file)

private:
    static std::mutex s_lock; static std::vector<const input_lae*> s_live;   // decoders opened for playback
    int layers() const { return (int)g_layers.get(); }
    int askedRoute() const { return layers() == 2 ? LAE_ROUTE_CD : layers() == 1 ? LAE_ROUTE_CD24 : layers() == 3 ? LAE_ROUTE_16 : LAE_ROUTE_FULL; }
    static bool hiRate(int route) { return route == LAE_ROUTE_FULL || route == LAE_ROUTE_16; }   // the routes that play above the base rate
    int outRate() const {   // all layers: the highest octave of the master not above the setting (and above the base rate)
        if (layers() != 0 && layers() != 3) return m_base;
        const int cap = (int)g_maxrate.get(); int r = m_master;
        while (cap > 0 && r > cap && (r >> 1) > m_base) r >>= 1;
        return r; }
    bool wouldChange() const {   // called under s_lock from the menu (main thread): reads only the atomics the decoding thread publishes
        if (m_laes) return !m_laesFellBack.load() && laesPick() != m_laesChoiceNow.load();
        if (m_cdOnly || m_fallbackNowA.load()) return false;   // a track that fell back to the CD layer keeps it (no second attempt mid-track)
        if (packWanted() != m_packNow.load()) return true;
        const int r = askedRoute();
        return r != m_routeNow.load() || (hiRate(r) && outRate() != m_rateNow.load()); }
    double cdKbps() const { return m_base * 32 / 1000.0; }   // the CD layer: 16 bit x 2 channels, uncompressed
    // starts the route the settings ask for. On failure the whole track plays as the CD layer, unless noFallback (an integrity test, or a
    // layer change after audio has played: then the track stops with an error, as on a stream error, rather than switching to CD)
    void startRoute(bool noFallback) {
        FpPin pin; char err[512] = ""; m_gen = s_layerGen.load(); m_fallbackNow = false;
        { const int want = packWanted();   // the user chose another pack of this file in the menu: its own master rate and layers from here on
          if (want != m_pack && m_track && !m_cdOnly) { if (lae_select_pack(m_track, want) == 0) usePack(want); else console::formatter() << T("LAEY: pack ", "LAEY：扩展包 ") << want + 1 << T(" unavailable; keeping pack ", " 不可用；保持扩展包 ") << m_pack + 1;
              m_packNow = m_pack; } }
        const int route = askedRoute(); m_route = route;
        if (lae_start(m_track, route, hiRate(route) ? outRate() : 0, err, sizeof err) != 0) {
            if (noFallback) throw exception_io_data((std::string("LAE: ") + err).c_str());
            console::formatter() << T("LAEY: ", "LAEY：") << err << T("; playing the CD/core layer", "；播放 CD 层/核心层");
            m_fallback = err; m_fallbackNow = true; m_route = LAE_ROUTE_CD;
            if (lae_start(m_track, LAE_ROUTE_CD, 0, err, sizeof err) != 0) throw exception_io_data(err); }
        uint64_t frames = 0; lae_output_format(m_track, &m_rate, &m_bits, &frames);
        m_routeNow = m_route; m_rateNow = m_rate; m_fallbackNowA = m_fallbackNow; resetBitrate();
        m_report = true;   // the status bar shows the layers that actually play (decode_get_dynamic_info)
    }
    // the user chose other layers in the menu: a playback decoder continues at the same time position with them (a user's choice, not a
    // fallback). Conversion, ReplayGain scans and integrity tests keep the layers they started with.
    void applyLayerChange() {
        if (!m_playback || m_testing || m_cdOnly || !m_track || m_gen == s_layerGen.load()) return;
        const unsigned gen = s_layerGen.load();   // the generation before the settings are read (as in startRoute)
        if (!wouldChange()) { m_gen = gen; return; }
        const double t = m_rate > 0 ? (double)m_pos / m_rate : 0;
        startRoute(m_pos > 0);
        const uint64_t frame = audio_math::time_to_samples(t, m_rate);
        FpPin pin; if (frame && lae_seek(m_track, frame) != 0) throw exception_io_data(T("LAEY: seek failed", "LAEY：跳转失败")); m_pos = (size_t)frame; resetBitrate();
    }
    // live bitrate: the CD layer's fixed rate + the extension bytes the decoder consumed per second of output. After a start or seek the
    // window begins after the first block (that block may include the entropy catch-up to the target: not this second's audio)
    void resetBitrate() { m_winFrames = 0; m_needBase = true; }
    void countBitrate(uint64_t frames) {
        if (m_route == LAE_ROUTE_CD || m_rate <= 0) return;
        if (m_needBase) { uint64_t b = 0; if (lae_stream_bytes(m_track, &b) == 0) { m_winBytes = b; m_winFrames = 0; m_needBase = false; } return; }
        m_winFrames += frames; if (m_winFrames < (uint64_t)m_rate) return;
        uint64_t b = 0; if (lae_stream_bytes(m_track, &b) != 0) return;
        if (b >= m_winBytes) { m_kbps = (t_int64)std::lround(cdKbps() + (double)(b - m_winBytes) * 8.0 * m_rate / (double)m_winFrames / 1000.0); m_reportRate = true; }
        m_winBytes = b; m_winFrames = 0; }
    std::string routeLabel() const {
        // "LAEY · <extension pack name> · <layers>" (LAEY is lossy: no "master" wording; the pack name as the encoder
        // stored it, the pack number when it has none; files with one pack show no pack)
        const std::string sep = " \xC2\xB7 ", only = std::string(T("", "仅 ")) + baseName(m_base) + T(" only", "");
        if (m_fallbackNow) return "LAEY" + sep + only + sep + T("extension unusable", "扩展层不可用");
        if (m_route == LAE_ROUTE_CD) return "LAEY" + sep + only;
        std::string pk; if (!m_packs.empty() || !m_mainName.empty()) { const std::string nm = m_pack ? m_packs[(size_t)m_pack - 1].name : m_mainName;
            pk = sep + (nm.empty() ? std::string(T("Pack ", "扩展包 ")) + std::to_string(m_pack + 1) : nm); }
        if (m_route == LAE_ROUTE_CD24) return "LAEY" + pk + sep + T("Bit-depth layer only", "仅位深层");
        if (m_route == LAE_ROUTE_16) return "LAEY" + pk + sep + T("Frequency layer only", "仅频率层");
        return "LAEY" + pk + sep + T("All layers", "全部层"); }
    // the track, verified by liblae (read through the file object: only the CD layer and the LAE chunk are held, about the file size);
    // on failure the CD layer of the WAV plays (whole track)
    void load(abort_callback& p_abort) {
        char err[512] = "";
        { FpPin pin; if (!lae_selftest()) strcpy_s(err, "decoder self-test failed (floating-point state)");
          else { FileReader r{m_file, p_abort}; m_track = lae_open_reader(&FileReader::read, &r, m_size, err, sizeof err); if (r.ep) std::rethrow_exception(r.ep); } }
        if (m_track && m_pack > 0) { FpPin pin; if (lae_select_pack(m_track, m_pack) != 0) { console::formatter() << T("LAEY: pack ", "LAEY：扩展包 ") << m_pack + 1 << T(" unavailable; playing pack 1", " 不可用；播放扩展包 1"); usePack(0); } }
        m_packNow = m_pack;
        if (m_track) return;
        if (m_flacForm) { console::formatter() << T("LAEY: ", "LAEY：") << err << T("; passed to the FLAC decoder (CD/core layer)", "；交给 FLAC 解码器（CD 层/核心层）"); throw exception_io_unsupported_format(); }   // FLAC form: no CD-layer reader here
        console::formatter() << T("LAEY: ", "LAEY：") << err << T("; playing the CD/core layer", "；播放 CD 层/核心层");
        m_cdOnly = true; m_fallback = err;
        // the CD layer, chosen by liblae's rules (the last 'fmt ' and the last 'data' chunk; sizes clamped to the file; 64-bit positions)
        t_filesize p = 12, dOff = 0, dLen = 0; bool haveFmt = false, haveData = false, pcm16 = false;
        while (m_size - p >= 8) { uint8_t c[8]; m_file->seek(p, p_abort); m_file->read_object(c, 8, p_abort);
            const t_filesize body = p + 8; t_filesize n = le32(c + 4); if (n > m_size - body) n = m_size - body;
            if (!memcmp(c, "fmt ", 4)) { if (n < 16) throw exception_io_data(T("LAEY: invalid fmt chunk", "LAEY：fmt 块无效"));
                uint8_t f[26]; const size_t k = n >= 26 ? 26 : 16; m_file->read_object(f, k, p_abort);
                int fmt = f[0] | f[1] << 8; if (fmt == 0xFFFE && k == 26) fmt = f[24] | f[25] << 8;   // WAVE_FORMAT_EXTENSIBLE: the sub-format
                pcm16 = fmt == 1 && (f[2] | f[3] << 8) == 2 && (f[14] | f[15] << 8) == 16; haveFmt = true; }
            else if (!memcmp(c, "data", 4)) { dOff = body; dLen = n; haveData = true; }
            if (n + (n & 1) >= m_size - body) break; p = body + n + (n & 1); }
        if (!haveFmt || !haveData) throw exception_io_data(T("LAEY: no audio data", "LAEY：没有音频数据"));
        if (!pcm16) throw exception_io_data(T("LAEY: the CD/core layer is not 16-bit stereo PCM", "LAEY：CD 层/核心层不是 16 位立体声 PCM"));
        const t_filesize len = dLen / 4 * 2;
        try { m_cd.resize((size_t)len); } catch (...) { throw exception_io_data(T("LAEY: file too large for this decoder", "LAEY：文件超出本解码器的处理范围")); }
        m_file->seek(dOff, p_abort); m_file->read_object(m_cd.data(), (size_t)len * 2, p_abort);   // little-endian: the bytes are the samples
    }
    // ---- LAES (LAE Lossless)
    // the structure only (for track info and the choice); the worker opens the file again and reads it whole
    void laesOpen(abort_callback& p_abort) {
        char err[512] = "", mg[5]; FileReader r{m_file, p_abort};
        { FpPin pin; m_lq = laes_open_reader(&FileReader::read, &r, m_size, 1, mg, err, sizeof err); }
        if (r.ep) std::rethrow_exception(r.ep);
        if (!m_lq) throw exception_io_unsupported_format();   // a plain FLAC / WAV (or one this reader cannot read): the next decoder takes it
        m_laes = true; laes_file_info fi; laes_get_info(m_lq, &fi); m_base = fi.base_rate; }
    // the choice to play: the one picked for this track in the context menu, if any; else (SPEC 10.3) the first master of the list (the menu
    // order), and of it: "CD layer only" = the core master instead; "CD + depth layer" = its base-rate level; otherwise its highest rate up to
    // the set maximum; a word above the set maximum: that entry's own derived 16-bit version. The limits never move to another master.
    int laesPick() const {
        laes_file_info fi; laes_get_info(m_lq, &fi); if (!fi.payload_ok) return 0;
        { const std::string want = g_choices.get(m_path.get_ptr());
          if (!want.empty()) for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(m_lq, i, &c); if (want == c.name) return i; } }
        if (layers() == 2) return 0;
        const int capR = layers() == 1 ? fi.base_rate : (int)g_maxrate.get(), capB = (int)g_maxbits.get();
        auto capped = [&](int i) { laes_choice_info c; laes_get_choice(m_lq, i, &c);   // word above the maximum: this entry's own derived 16-bit version, if the file offers one
            if (capB > 0 && c.bits > capB) for (int d = 0; d < fi.choices; d++) if (laes_choice_from(m_lq, d) == i) return d;
            return i; };
        // SPEC 10.3 (2026-11-12): what plays without a choice of the user is the file's default entry (set when the file was encoded: a
        // master itself, else the core master), never an entry picked by rate, size or device. The limits work inside that master only.
        int d = laes_default_choice(m_lq); if (d < 0 || d >= fi.choices) d = 0;
        laes_choice_info dc; laes_get_choice(m_lq, d, &dc); if (dc.kind != LAES_KIND_MASTER) return capped(0);
        for (int i = d; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(m_lq, i, &c);
            if (i > d && (c.kind == LAES_KIND_MASTER || c.kind == LAES_KIND_CORE)) break;
            if (c.master != dc.master || c.kind == LAES_KIND_DERIVED16) continue;
            if (capR > 0 && c.rate > capR) continue;
            return capped(i); }
        return capped(0); }


    std::string laesWhat(int i) const { laes_choice_info c; laes_get_choice(m_lq, i, &c); return choiceText(m_lq, c, 0, i); }
    void laesInfo(file_info& p_info) {
        const int i = laesPick(); laes_choice_info c; laes_get_choice(m_lq, i, &c); laes_file_info fi; laes_get_info(m_lq, &fi);
        p_info.set_length((double)c.frames / c.rate); p_info.info_set_int("samplerate", c.rate); p_info.info_set_int("channels", 2); p_info.info_set_int("bitspersample", c.bits);
        p_info.info_set("encoding", "lossless"); p_info.info_set("codec", "LAES");
        p_info.info_set("laes_form", fi.is_flac ? "FLAC" : "WAV"); p_info.info_set("laes_core", (khz(fi.base_rate) + " / " + std::to_string(fi.core_bits) + T("-bit", " 位")).c_str());
        std::string all; for (int k = 0; k < fi.choices; k++) { if (k) all += "; "; all += laesWhat(k); } p_info.info_set("laes_choices", all.c_str());
        p_info.info_set("laes_plays", laesWhat(i).c_str());
        if (!fi.payload_ok) p_info.info_set("laes_note", (std::string(T("extension unusable; playing the core master: ", "扩展层不可用；播放核心母带：")) + laes_file_problem(m_lq)).c_str());
        if (m_wavForm) for (auto& kv : m_wavTags) p_info.meta_add(kv.first.c_str(), kv.second.c_str());   // INFO + id3 under foobar2000's names
        else for (int k = 0; ; k++) { const char* key; const char* val; if (laes_get_tag(m_lq, k, &key, &val) != 0) break; p_info.meta_add(key, val); }
        p_info.info_set_bitrate((t_int64)((m_size * 8) / ((double)c.frames / c.rate) / 1000.0 + 0.5)); }
    // startAt: the frame to begin at (switching, seeking); a derived 16-bit version too (its dither state is moved there, bit-exact; 2026-11-12)
    void laesStart(int choice, abort_callback& p_abort, uint64_t startAt = 0, std::unique_ptr<Prefetched> adopt = nullptr) {
        laes_choice_info c; laes_get_choice(m_lq, choice, &c);
        // masters are independent and may differ in length: a time past the end of this choice ends the track there (no worker), never a
        // start from 0.
        const bool past = startAt >= c.frames;
        dbg("%p start choice %d (%s) at %llu adopt %s [%llu..%llu) m_pos %llu", (void*)this, choice, c.name, (unsigned long long)startAt, adopt ? "yes" : "no", adopt ? (unsigned long long)adopt->b0 : 0ull, adopt ? (unsigned long long)adopt->b1 : 0ull, (unsigned long long)m_pos);
        retirePrefetcher(std::move(m_pf));   // made again for the new choice (laesRun)
        retireWorker(std::move(m_worker)); if (!past) m_worker.reset(new LaesWorker(m_path.get_ptr(), m_size, choice, c.master, c.frames, c.rate, startAt, std::move(adopt)));
        m_laesChoice = choice; m_laesChoiceNow = choice; m_laesFrames = c.frames; m_rate = c.rate; m_bits = c.bits; m_laesKind = c.kind; m_gen = s_layerGen.load();
        m_laesLabel = std::string("LAES \xC2\xB7 ") + laesWhat(choice); if (!m_laesFallback.empty()) m_laesLabel += std::string(" \xC2\xB7 ") + T("extension unusable", "扩展层不可用");
        m_report = true; (void)p_abort; }
    bool laesRun(audio_chunk& p_chunk, abort_callback& p_abort) {
        enum { block = 4096 };
        if (nowMs() - s_menuSeekMs.load() > 1000) laesApplyChange(p_abort);   // a menu switch is made at its seek (the audible time), unless that seek does not come
        if (m_pos >= m_laesFrames) { dbg("%p end: m_pos %llu >= frames %llu", (void*)this, (unsigned long long)m_pos, (unsigned long long)m_laesFrames); return false; }
        const uint64_t n = (std::min)((uint64_t)block, m_laesFrames - m_pos);
        if (!m_worker->waitFor(m_pos, n, p_abort)) {
            dbg("%p waitFor false at %llu: base %llu written %llu failed %d why %s", (void*)this, (unsigned long long)m_pos, (unsigned long long)m_worker->base(), (unsigned long long)m_worker->written(), (int)m_worker->failed(), m_worker->why().c_str());
            if (!m_worker->failed()) return false;
            // nothing of this choice has played yet: the whole track plays as the core instead (spec 1.3: no master that failed its check)
            if (!m_laesPlayed && m_laesChoice != 0 && !m_testing) { m_laesFallback = m_worker->why(); m_laesFellBack = true; console::formatter() << T("LAES: ", "LAES：") << m_laesFallback.c_str() << T("; playing the core master", "；播放核心母带");
                const double t = (double)m_pos / m_rate; laesStart(0, p_abort); m_pos = (size_t)(std::min)(m_laesFrames, (uint64_t)audio_math::time_to_samples(t, m_rate)); return laesRun(p_chunk, p_abort); }
            throw exception_io_data((std::string("LAES: ") + m_worker->why()).c_str()); }   // never switch to the core inside a track
        m_buf.resize((size_t)n * 2); if (!m_worker->read(m_pos, m_buf.data(), n)) throw exception_io_data(T("LAES: cannot read the temporary file", "LAES：无法读取临时文件"));
        const int bps = m_bits / 8; m_bytes.resize((size_t)n * 2 * bps);
        for (size_t i = 0; i < (size_t)n * 2; i++) { const int32_t v = m_buf[i]; for (int k = 0; k < bps; k++) m_bytes[i * bps + k] = (uint8_t)(v >> (8 * k)); }
        p_chunk.set_data_fixedpoint(m_bytes.data(), m_bytes.size(), m_rate, 2, m_bits, audio_chunk::channel_config_stereo);
        if (!m_laesPlayed) dbg("%p first audio at %llu", (void*)this, (unsigned long long)m_pos);
        m_pos += (size_t)n; m_laesPlayed = true; laesPrefetch(); return true; }
    // keeps the other verified masters of the file ready (setting), once this worker plays a verified master
    void laesPrefetch() {
        if (!m_pf && g_prefetch.get() && m_playback && !m_testing && m_laesFallback.empty() && m_worker) { std::shared_ptr<SharedLaes> sh = m_worker->shared();
            if (sh) { laes_file_info fi; laes_get_info(sh->f, &fi); std::vector<std::pair<int, laes_choice_info>> cs;
                for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(sh->f, i, &c); if (c.kind == LAES_KIND_MASTER && i != m_laesChoice && g_verified.get(fi.file_sha256, c.master) > 0) cs.push_back({i, c}); }
                if (!cs.empty()) m_pf.reset(new LaesPrefetcher(sh, cs)); } }
        if (m_pf && m_rate > 0) m_pf->setTime((double)m_pos / m_rate); }
    // the user chose other settings in the menu: a playback decoder continues at the same time position with the new choice
    // returns true when it switched. atSeconds: the time to begin at (a seek); otherwise where decoding has got to
    bool laesApplyChange(abort_callback& p_abort, double atSeconds = -1) {
        if (!m_playback || m_testing || !m_laesFallback.empty() || m_gen == s_layerGen.load()) return false;
        const unsigned gen = s_layerGen.load(); const int want = laesPick(); m_gen = gen; dbg("%p apply: want %d now %d pinned '%s' at %.3f", (void*)this, want, m_laesChoice, g_choices.get(m_path.get_ptr()).c_str(), atSeconds); if (want == m_laesChoice) return false;
        // the new choice begins right at the current time (the frame at its own rate), not at its start
        const double t = atSeconds >= 0 ? atSeconds : m_rate > 0 ? (double)m_pos / m_rate : 0; laes_choice_info c; laes_get_choice(m_lq, want, &c);
        const uint64_t at = (std::min)(c.frames, (uint64_t)audio_math::time_to_samples(t, c.rate));
        std::unique_ptr<Prefetched> pre = m_pf ? m_pf->take(want, at) : nullptr;   // the samples kept ready, when they hold the position
        laesStart(want, p_abort, at, std::move(pre)); m_pos = (size_t)at; return true; }
    bool m_laes = false, m_laesPlayed = false; laes_file* m_lq = nullptr; std::unique_ptr<LaesPrefetcher> m_pf; std::unique_ptr<LaesWorker> m_worker; int m_laesChoice = 0, m_laesKind = 0; std::atomic<int> m_laesChoiceNow{0};
    uint64_t m_laesFrames = 0; std::string m_laesLabel, m_laesFallback; std::atomic<bool> m_laesFellBack{false};   // m_laesFallback set, published for the menu thread

    service_ptr_t<file> m_file; t_filesize m_size = 0; pfc::string8 m_path;
    bool m_flacForm = false;   // LAEY in the FLAC form (m_wavTags then holds its Vorbis comments)
    wavtags::List m_wavTags; bool m_wavForm = false;   // the tags of a WAV-form file (LAEY, LAES WAV), read at open
    int m_master = 0, m_base = 0, m_stages = 0, m_rate = 0, m_bits = 24, m_route = LAE_ROUTE_FULL; uint64_t m_masterFrames = 0;
    // extension packs: the further packs of the file, the pack in use (0 = the main body; m_master, m_stages, m_masterFrames describe it), pack 0's values
    std::vector<PackRow> m_packs; std::string m_mainName; int m_pack = 0, m_master0 = 0, m_stages0 = 0; uint64_t m_frames0 = 0; std::atomic<int> m_packNow{0};
    int packWanted() const { const int k = packOf(m_path.get_ptr()); return k < 0 || (size_t)k > m_packs.size() ? 0 : k; }
    void usePack(int k) { if (k < 1 || (size_t)k > m_packs.size()) k = 0;
        if (k) { const PackRow& q = m_packs[(size_t)k - 1]; if (q.stages < 1 || q.stages > 3 || q.rate != (m_base << q.stages)) k = 0; }
        m_pack = k; m_master = k ? m_packs[(size_t)k - 1].rate : m_master0; m_stages = k ? m_packs[(size_t)k - 1].stages : m_stages0; m_masterFrames = k ? m_packs[(size_t)k - 1].frames : m_frames0; }
    void initPacks(const std::function<bool(uint64_t, void*, size_t)>& rd, uint64_t n) { m_master0 = m_master; m_stages0 = m_stages; m_frames0 = m_masterFrames; m_packs = scanPacks(rd, n, &m_mainName); usePack(packWanted()); }
    std::atomic<int> m_routeNow{LAE_ROUTE_FULL}, m_rateNow{0};   // m_route / m_rate published for the menu thread
    lae_track* m_track = nullptr; std::vector<int16_t> m_cd; size_t m_pos = 0;
    bool m_cdOnly = false;        // the extension could not be verified: the whole track plays from m_cd
    bool m_fallbackNow = false;   // the current decode plays the CD layer (m_cdOnly, or lae_start failed); m_fallback says why
    bool m_report = false, m_reportRate = false, m_playback = false, m_testing = false; unsigned m_gen = 0; std::string m_fallback;
    std::atomic<bool> m_fallbackNowA{false};   // m_fallbackNow published for the menu thread
    uint64_t m_winFrames = 0, m_winBytes = 0; t_int64 m_kbps = 0; bool m_needBase = true;
    std::vector<int32_t> m_buf; std::vector<uint8_t> m_bytes;
};
std::atomic<unsigned> input_lae::s_layerGen{0};
std::atomic<uint64_t> input_lae::s_menuSeekMs{0};
std::mutex input_lae::s_lock;
std::vector<const input_lae*> input_lae::s_live;

static input_singletrack_factory_t<input_lae> g_input_lae_factory;
DECLARE_FILE_TYPE("LAE files", "*.LAE");

namespace {
// ---- menu Playback > LAE: which layers play; applies at once (the playing LAE track switches at the same position)
struct Preset { int layers, maxrate; const char* en; const char* cn; GUID guid; };
const Preset kPresets[] = {
    { 0, 0,      "All layers · master sample rate",            "全部层 · 母带采样率",      { 0x166cde31, 0x21dd, 0x4611, { 0x98, 0x56, 0xb8, 0x64, 0x4a, 0xda, 0x01, 0x9b } } },
    { 0, 192000, "All layers · up to 176.4 / 192 kHz",           "全部层 · 最高 176.4 / 192 kHz",     { 0xed163c2d, 0x153a, 0x4c28, { 0xa4, 0x5a, 0x54, 0x4f, 0x9c, 0xee, 0x60, 0x9f } } },
    { 0, 96000,  "All layers · up to 88.2 / 96 kHz",            "全部层 · 最高 88.2 / 96 kHz",      { 0xbc3a37e8, 0x3753, 0x4e35, { 0xab, 0x9a, 0xbc, 0xfb, 0x0d, 0xde, 0x74, 0xfc } } },
    { 3, 0,      "Frequency layer only · master sample rate", "仅频率层 · 母带采样率", { 0x8b2f61d4, 0x3c7a, 0x4e19, { 0xa2, 0x6d, 0x41, 0x9e, 0x57, 0x0c, 0xb3, 0xf8 } } },
    { 3, 192000, "Frequency layer only · up to 176.4 / 192 kHz", "仅频率层 · 最高 176.4 / 192 kHz", { 0x5e0a97c3, 0x71b4, 0x4d2f, { 0x93, 0x58, 0x6a, 0xd1, 0x2b, 0xe4, 0x80, 0x7c } } },
    { 3, 96000,  "Frequency layer only · up to 88.2 / 96 kHz", "仅频率层 · 最高 88.2 / 96 kHz", { 0xc49d3e76, 0x08f5, 0x4a6b, { 0xb7, 0x1c, 0x95, 0x3a, 0xe2, 0x64, 0x0d, 0x21 } } },
    { 1, 0,      "Bit-depth layer only · 24-bit",     "仅位深层 · 24 位", { 0xaf8688f3, 0x90df, 0x4278, { 0xb6, 0x13, 0xef, 0x14, 0x73, 0x5d, 0xd5, 0xaf } } },
    { 2, 0,      "CD/core layer only · 16-bit",              "仅 CD 层/核心层 · 16 位",     { 0x89ee1614, 0xb19d, 0x4977, { 0x82, 0x8c, 0xde, 0x84, 0xc4, 0xed, 0x26, 0xbb } } },
};
const unsigned kPresetCount = sizeof kPresets / sizeof kPresets[0];
const GUID guid_menu = { 0x52c9c179, 0xeccd, 0x44ec, { 0x89, 0x77, 0x66, 0x05, 0xd9, 0x6e, 0xe1, 0x83 } };
mainmenu_group_popup_factory g_menuGroup(guid_menu, mainmenu_groups::playback, mainmenu_commands::sort_priority_dontcare, "LAE");

int currentPreset() {
    const int l = (int)g_layers.get(), r = (int)g_maxrate.get();
    for (unsigned i = 0; i < kPresetCount; i++) if (kPresets[i].layers == l && ((l != 0 && l != 3) || kPresets[i].maxrate == r)) return (int)i;
    return -1; }

// sets the layers every LAEY track plays with (LAES tracks on "automatic" follow it too); the playing track switches at the same position
// maxrate below 0: the rate setting stays as it is (the layers chosen play at the base rate anyway)
void applyLayers(int layers, int maxrate) {
    g_layers.set(layers); if (maxrate >= 0) g_maxrate.set(maxrate); dbg("menu layers %d maxrate %d", layers, maxrate); input_lae::s_menuSeekMs = nowMs(); input_lae::s_layerGen++;
    // when the audible track's playback decoder would play other layers: a seek to the current position empties the playback buffer
    // and reaches that decoder, which switches there (other LAE decoders switch on their next block; a non-LAE track is left alone)
    auto pc = playback_control::get(); metadb_handle_ptr np;
    if (!pc->get_now_playing(np) || !pc->playback_can_seek()) return;
    // stored codec: "LAEY" ("LAE" in info stored by versions before 2026-11-10) or "LAES"
    bool lae = false; { metadb_info_container::ptr ic; if (np->get_info_ref(ic)) { const char* c = ic->info().info_get("codec"); lae = c && !strncmp(c, "LAE", 3); } }
    if (input_lae::s_wouldChange(np->get_path()) || (lae && input_lae::s_wouldChange(nullptr))) { input_lae::s_menuSeekMs = nowMs(); pc->playback_seek(pc->playback_get_position()); } }
void applyPreset(unsigned i) { applyLayers(kPresets[i].layers, kPresets[i].maxrate); }

class lae_menu : public mainmenu_commands {
public:
    t_uint32 get_command_count() { return kPresetCount; }
    GUID get_command(t_uint32 i) { return kPresets[i].guid; }
    void get_name(t_uint32 i, pfc::string_base& out) { out = T(kPresets[i].en, kPresets[i].cn); }
    bool get_description(t_uint32, pfc::string_base& out) { out = T("LAEY layers (applies immediately)", "LAEY 层（立即生效）"); return true; }
    GUID get_parent() { return guid_menu; }
    t_uint32 get_sort_priority() { return sort_priority_base; }   // the presets keep their order (equal priorities: same group order)
    bool get_display(t_uint32 i, pfc::string_base& text, t_uint32& flags) { get_name(i, text); flags = currentPreset() == (int)i ? flag_radiochecked : 0; return true; }
    void execute(t_uint32 i, service_ptr_t<service_base>) { applyPreset(i); }
};
static mainmenu_commands_factory_t<lae_menu> g_lae_menu;

// ---- decoder order: a decoder ahead of LAE that also takes .wav files would get LAE .wav files first (and play their CD layer only)
std::set<std::string> splitKnown(const char* s) { std::set<std::string> r; std::string cur; for (const char* p = s; ; p++) { if (!*p || *p == ';') { if (!cur.empty()) r.insert(cur); cur.clear(); if (!*p) break; } else cur += *p; } return r; }
class lae_order : public initquit {
public:
    void on_init() {
        fb2k::inMainThread([] {
            // every installed decoder (to tell a newly installed one next time), and the enabled ones in the user's order
            std::set<std::string> installed; { service_enum_t<input_entry> e; input_entry::ptr p;
                while (e.next(p)) { input_entry_v2::ptr v2; if (p->service_query_t(v2)) installed.insert(pfc::print_guid(v2->get_guid()).get_ptr()); } }
            const std::set<std::string> known = splitKnown(g_known.get_ptr());
            { std::string s; for (const std::string& g : installed) { s += g; s += ';'; } g_known = s.c_str(); }
            if (!g_autoOrder.get()) return;
            input_manager_v3::ptr im; if (!input_manager::get()->service_query_t(im)) return;   // foobar2000 before 1.5
            pfc::list_t<input_entry::ptr> list; im->get_enabled_inputs(list);
            // the decoders ahead of LAE that also take .wav files: the first one's name, and whether any was installed since the last start
            const GUID mine = input_lae::g_get_guid(); bool found = false, ahead = false, fresh = false; pfc::string8 who;
            for (size_t i = 0; i < list.get_count(); i++) { input_entry_v2::ptr v2; const bool isV2 = list[i]->service_query_t(v2);
                if (isV2 && v2->get_guid() == mine) { found = true; break; }
                if (!list[i]->is_our_path("C:\\x.wav", "wav") && !list[i]->is_our_path("C:\\x.flac", "flac")) continue;
                ahead = true; if (who.is_empty() && isV2) who = v2->get_name();
                if (isV2 && !known.empty() && !known.count(pfc::print_guid(v2->get_guid()).get_ptr())) fresh = true; }
            if (!found || !ahead) return;   // LAE disabled by the user, or first among the decoders that take .wav
            // move up when LAE looks at this profile's order for the first time (the order foobar2000 made up, or the order before 0.3), or when
            // a decoder installed since the last start was put ahead; a known decoder ahead of LAE later on is the user's arrangement
            if (who.is_empty()) who = T("a decoder that handles .wav files", "一个处理 .wav 的解码器");
            if (!(known.empty() || fresh) || g_slot.current + 1 >= kInputGuids) {
                console::formatter() << T("LAE: another .wav decoder is placed before LAE: ", "LAE：另一个 .wav 解码器排在 LAE 之前：") << who; return; }
            g_slot.next = g_slot.current + 1; g_promoted = true; standard_commands::main_saveconfig();   // saved now: quitting may crash (other components)
            pfc::string8 msg;
            if (zh()) msg << "解码器列表中“" << who << "”排在 LAE 之前并处理 .wav 文件，LAE 的 .wav 文件将只播放 CD 层/核心层。\n\n"
                             "已将 LAE 移到列表最前，重启 foobar2000 后生效。现在重启？";
            else msg << "\"" << who << "\" is placed before LAE in the decoder list and handles .wav files; LAE .wav files would play the CD/core layer only.\n\n"
                        "LAE has been moved to the top of the list. Restart foobar2000 now to apply?";
            if (uMessageBox(core_api::get_main_window(), msg, T("LAE decoder", "LAE 解码器"), MB_YESNO | MB_ICONINFORMATION) == IDYES) standard_commands::main_restart();
        });
    }
};
static initquit_factory_t<lae_order> g_lae_order;
class laes_quit : public initquit { public: void on_quit() { dbg("quit: begin, retiring %d", g_retiring.load()); LaesVerifier::get().quit(); for (int i = 0; i < 1000 && g_retiring.load() > 0; i++) Sleep(10); LaesFileCache::get().clear(); dbg("quit: end, retiring %d", g_retiring.load()); } };   // the verifier stops before foobar2000 closes
static initquit_factory_t<laes_quit> g_laes_quit;

// ---- LAES files added to the media library are checked in the background (setting; on by default)
bool isLaesCandidate(const char* path) { const char* e = strrchr(path, '.'); return e && (!stricmp_utf8(e, ".flac") || !stricmp_utf8(e, ".wav") || !stricmp_utf8(e, ".lae")); }
class laes_library : public library_callback {
public:
    void on_items_added(metadb_handle_list_cref items) { if (!g_verifyOnAdd.get()) return; for (size_t i = 0; i < items.get_count(); i++) if (isLaesCandidate(items[i]->get_path())) LaesVerifier::get().add(items[i]->get_path(), false); }
    void on_items_removed(metadb_handle_list_cref) {}
    void on_items_modified(metadb_handle_list_cref items) { on_items_added(items); }   // a changed file has a new SHA-256: checked again
};
static service_factory_single_t<laes_library> g_laes_library;

// ---- context menu: "Verify LAES" (checks the selected LAES files now, with the progress window; results in the console) and
// "LAES: choose what plays" (a list of the track's choices; the pick is remembered for that track and applies at once)
const GUID guid_ctx_verify = { 0x7a9b31c2, 0x4e5d, 0x4f60, { 0x8a, 0x71, 0x92, 0xb3, 0xc4, 0xd5, 0xe6, 0x07 } };
const GUID guid_ctx_choose = { 0x1d6e8f25, 0x9a3b, 0x4c71, { 0xb4, 0x0e, 0x5f, 0x62, 0x83, 0xa9, 0xc7, 0x1d } };
const GUID guid_ctx_layers = { 0x3f4c2a91, 0x6b7e, 0x4d08, { 0x9c, 0x15, 0x2e, 0x74, 0xa0, 0xb6, 0xd8, 0x53 } };
// which formats the selected tracks are, from the stored codec ("LAEY", "LAE" before 2026-11-10, "LAES"): bit 0 = LAEY, bit 1 = LAES;
// a track whose info is not read yet counts as both, so no command is hidden wrongly
unsigned ctxFormats(metadb_handle_list_cref items) {
    unsigned mask = 0; const size_t count = items.get_count() < 256 ? items.get_count() : 256;
    for (size_t k = 0; k < count && mask != 3; k++) { metadb_info_container::ptr ic; const char* c = items[k]->get_info_ref(ic) ? ic->info().info_get("codec") : nullptr;
        if (!c) mask |= 3; else if (!strncmp(c, "LAES", 4)) mask |= 2; else if (!strncmp(c, "LAE", 3)) mask |= 1; }
    return mask; }
class laes_ctx : public contextmenu_item_simple {
public:
    unsigned get_num_items() { return 3; }
    void get_item_name(unsigned i, pfc::string_base& out) { out = i == 0 ? T("LAES: Verify masters", "LAES：校验母带") : i == 1 ? T("LAES: Select master\xE2\x80\xA6", "LAES：选择母带…") : T("LAEY: Select layers\xE2\x80\xA6", "LAEY：选择层…"); }
    // the LAES commands show on LAES tracks only, the LAEY command on LAEY tracks only
    bool context_get_display(unsigned i, metadb_handle_list_cref items, pfc::string_base& out, unsigned&, const GUID&) {
        if (!(ctxFormats(items) & (i == 2 ? 1u : 2u))) return false; get_item_name(i, out); return true; }
    void context_command(unsigned i, metadb_handle_list_cref items, const GUID&) {
        if (i == 2) {
            // what this track has: its own rates (the 44.1 kHz or the 48 kHz family, master rate down to twice the CD layer's rate), nothing
            // it does not have. The choice sets the settings of the menu Playback > LAE; a rate below the master is stored as its 48 kHz
            // family value, the form that menu uses (192000 = 176.4 or 192 kHz), so tracks of the other family follow it at their own rate.
            // The master and CD-layer rates come from the track's stored info; without them the menu's own list is shown.
            // Extension packs (spec/lossy/SPEC.md, Section 5.6): a file with further packs lists the packs first (number, name, master rate),
            // each with its own layers in a submenu. The pack is the user's choice for this file, by number, kept in the player's own record
            // (laes_choices.txt), never written into the file and never taken over by another file.
            int master = 0, base = 0; std::vector<std::pair<int, std::string>> packRows; std::string mainName;   // the further packs: master rate, name; the main body's name
            const char* path = items.get_count() > 0 ? items[0]->get_path() : "";
            if (items.get_count() > 0) { metadb_info_container::ptr ic; if (items[0]->get_info_ref(ic)) {
                const char* ms = ic->info().info_get("lae_master"); const char* cs = ic->info().info_get("lae_cd_layer"); if (!cs) cs = ic->info().info_get("lae_core_layer"); const char* sl = cs ? strchr(cs, '/') : nullptr;
                if (ms) master = atoi(ms); if (sl) base = atoi(sl + 1);
                const char* pn = ic->info().info_get("lae_packs"); const int np = pn ? atoi(pn) : 0;
                if (const char* p0 = ic->info().info_get("lae_pack_0")) mainName = p0;
                for (int k = 1; k <= np && k <= 8; k++) { const char* pv = ic->info().info_get((std::string("lae_pack_") + std::to_string(k)).c_str()); if (!pv) break;
                    const char* sp = strchr(pv, ' '); packRows.push_back({ atoi(pv), sp ? std::string(sp + 1) : std::string() }); } } }
            struct Pick { int layers, maxrate, pack; };   // maxrate below 0 = unchanged; pack below 0 = unchanged
            HMENU pm = CreatePopupMenu(); std::vector<Pick> picks;
            if (master > 0 && base > 0 && master >= base) {
                const int lay = (int)g_layers.get(), cap = (int)g_maxrate.get(); int curPack = packOf(path); if (curPack < 0 || (size_t)curPack > packRows.size()) curPack = 0;
                auto wide = [](const std::string& s) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(s.c_str()).get_ptr()); };
                auto fill = [&](HMENU menu, int pack, int top, bool withBase) {   // the layers of one pack (top = its master rate)
                    int nowRate = top; while (cap > 0 && nowRate > cap && (nowRate >> 1) > base) nowRate >>= 1;
                    auto add = [&](int layers, int rate, const std::string& text) {
                        const bool high = layers == 0 || layers == 3, on = lay == layers && (!high || rate == nowRate) && (pack < 0 || layers == 2 || pack == curPack);
                        picks.push_back({ layers, !high ? -1 : rate == top ? 0 : base % 11025 == 0 ? rate / 147 * 160 : rate, layers == 2 ? -1 : pack });
                        AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), (UINT)picks.size(), wide(text).c_str()); };
                    for (int pass = 0; pass < 2; pass++) { int rate = top;
                        do { add(pass == 0 ? 0 : 3, rate, std::string(pass == 0 ? T("All layers", "全部层") : T("Frequency layer only", "仅频率层")) + " \xC2\xB7 " + khz(rate) + (pass == 0 ? T(" / 24-bit", " / 24 位") : T(" / 16-bit", " / 16 位")) +   // without the depth layer the precision is 16 bit (user 2026-11-12); the samples still go out in 24-bit words
                                 (rate == top ? std::string(" \xC2\xB7 ") + T("master sample rate", "母带采样率") : std::string())); } while ((rate >>= 1) > base);
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }
                    add(1, base, std::string(T("Bit-depth layer only", "仅位深层")) + " \xC2\xB7 " + khz(base) + T(" / 24-bit", " / 24 位"));
                    if (withBase) add(2, base, std::string(T("", "仅 ")) + baseName(base) + T(" only", "") + " \xC2\xB7 " + khz(base) + T(" / 16-bit", " / 16 位")); };
                if (packRows.empty()) fill(pm, -1, master, true);
                else { for (int k = 0; k <= (int)packRows.size(); k++) { const int top = k ? packRows[(size_t)k - 1].first : master; if (top <= base) continue;
                        HMENU sub = CreatePopupMenu(); fill(sub, k, top, false);
                        // the extension pack's name as the encoder stored it (the main body's in its optional "LNAM" record), with its master
                        // sample rate (LAEY is lossy: no "master" wording); a pack without a name shows its number
                        const std::string nm = k ? packRows[(size_t)k - 1].second : mainName;
                        std::string label = nm.empty() ? std::string(T("Pack ", "扩展包 ")) + std::to_string(k + 1) : cutUtf8(nm, 40);
                        label += " \xC2\xB7 " + khz(top);
                        AppendMenuW(pm, MF_POPUP | (k == curPack && lay != 2 ? MF_CHECKED : 0), (UINT_PTR)sub, wide(label).c_str()); }
                    AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
                    picks.push_back({ 2, -1, -1 }); AppendMenuW(pm, MF_STRING | (lay == 2 ? MF_CHECKED : 0), (UINT)picks.size(), wide(std::string(T("", "仅 ")) + baseName(base) + T(" only", "") + " \xC2\xB7 " + khz(base) + T(" / 16-bit", " / 16 位")).c_str()); }
            } else { const int now = currentPreset();
                for (unsigned k = 0; k < kPresetCount; k++) { picks.push_back({ kPresets[k].layers, kPresets[k].maxrate, -1 });
                    AppendMenuW(pm, MF_STRING | (now == (int)k ? MF_CHECKED : 0), 1 + k, pfc::stringcvt::string_wide_from_utf8(T(kPresets[k].en, kPresets[k].cn))); } }
            POINT at; GetCursorPos(&at); const int pick = (int)TrackPopupMenu(pm, TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, 0, core_api::get_main_window(), nullptr); DestroyMenu(pm);
            if (pick > 0) { const Pick pk = picks[(size_t)pick - 1];
                if (pk.pack >= 0 && *path) { g_choices.set(path, pk.pack ? "laeypack=" + std::to_string(pk.pack) : std::string()); input_lae::s_layerGen++; }
                applyLayers(pk.layers, pk.maxrate); } return; }
        if (i == 0) { unsigned n = 0; for (size_t k = 0; k < items.get_count(); k++) if (isLaesCandidate(items[k]->get_path())) { LaesVerifier::get().add(items[k]->get_path(), true); n++; }
            console::formatter() << T("LAES: verifying ", "LAES：校验 ") << n << T(" file(s)", " 个文件"); return; }
        if (items.get_count() < 1) return; const char* path = items[0]->get_path();
        laes_file* f = nullptr; char err[512] = "", mg[5];
        try { OwnReader rd; abort_callback_dummy ab; filesystem::g_open(rd.f, path, filesystem::open_mode_read, ab); const t_filesize size = rd.f->get_size(ab);
            FpPin pin; f = laes_open_reader(&OwnReader::read, &rd, size, 1, mg, err, sizeof err); } catch (...) { f = nullptr; }
        if (!f) { uMessageBox(core_api::get_main_window(), T("This track is not an LAES file.", "本曲目不是 LAES 文件。"), T("LAE decoder", "LAE 解码器"), MB_OK | MB_ICONINFORMATION); return; }
        laes_file_info fi; laes_get_info(f, &fi); const std::string cur = g_choices.get(path);
        HMENU m = CreatePopupMenu(); AppendMenuW(m, MF_STRING | (cur.empty() ? MF_CHECKED : 0), 1, pfc::stringcvt::string_wide_from_utf8(T("Automatic (Playback > LAE)", "自动（播放 > LAE）")));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr); std::vector<std::string> names;
        // SPEC 10.3: the first level lists the core master and the masters themselves, in the list's order (the menu order); a master's rate
        // levels and derived versions are in its own submenu, on the line below it. A master that failed its SHA-256 check stays, greyed.
        auto wide = [](const std::string& s) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(s.c_str()).get_ptr()); };
        auto flagsOf = [&](const laes_choice_info& c) -> UINT { const bool ok = fi.payload_ok || c.kind == LAES_KIND_CORE; const bool bad = c.master >= 0 && g_failed.has(path, c.master);
            return MF_STRING | (cur == c.name ? MF_CHECKED : 0) | (ok && !bad ? 0 : MF_GRAYED); };
        for (int k = 0; k < fi.choices; k++) { laes_choice_info c; laes_get_choice(f, k, &c); names.push_back(c.name); }
        for (int k = 0; k < fi.choices; ) { laes_choice_info c; laes_get_choice(f, k, &c);   // k = the head of a group: the core master or a master
            const bool bad = c.master >= 0 && g_failed.has(path, c.master);
            AppendMenuW(m, flagsOf(c), 2 + k, wide(choiceText(f, c, 40, k) + (bad ? T(" (unavailable: SHA-256 mismatch)", "（不可用：SHA-256 不符）") : "")).c_str());
            int e = k + 1; HMENU sub = nullptr; bool subChecked = false;
            for (; e < fi.choices; e++) { laes_choice_info d; laes_get_choice(f, e, &d); if (d.kind == LAES_KIND_CORE || d.kind == LAES_KIND_MASTER) break;
                if (!sub) sub = CreatePopupMenu(); AppendMenuW(sub, flagsOf(d), 2 + e, wide(choiceText(f, d, 40, e)).c_str()); subChecked |= cur == d.name; }
            if (sub) AppendMenuW(m, MF_POPUP | (subChecked ? MF_CHECKED : 0), (UINT_PTR)sub,
                wide(std::string("      ") + masterText(f, c.kind == LAES_KIND_CORE ? -1 : c.master, 40) + T(": derived versions", "：派生版本")).c_str());
            k = e; }
        laes_close(f);

        POINT pt; GetCursorPos(&pt); const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, core_api::get_main_window(), nullptr); DestroyMenu(m);
        if (cmd <= 0) return;
        g_choices.set(path, cmd == 1 ? std::string() : names[(size_t)cmd - 2]); dbg("menu choose '%s' for %s", cmd == 1 ? "" : names[(size_t)cmd - 2].c_str(), path); input_lae::s_menuSeekMs = nowMs(); input_lae::s_layerGen++;
        // the playing track switches at the same position (as with the layer menu)
        auto pc = playback_control::get(); metadb_handle_ptr np;
        if (pc->get_now_playing(np) && !strcmp(np->get_path(), path) && pc->playback_can_seek()) { input_lae::s_menuSeekMs = nowMs(); pc->playback_seek(pc->playback_get_position()); }; }
    GUID get_item_guid(unsigned i) { return i == 0 ? guid_ctx_verify : i == 1 ? guid_ctx_choose : guid_ctx_layers; }
    bool get_item_description(unsigned i, pfc::string_base& out) {
        out = i == 0 ? T("Verifies every master of the selected files (SHA-256).", "校验所选文件的全部母带（SHA-256）。")
            : i == 1 ? T("Selects the master, sample-rate level or derived version for this track.", "为本曲目选择母带、采样率级别或派生版本。")
                     : T("Selects the extension pack and layers for this track.", "为本曲目选择扩展包和层。"); return true; }
};
static contextmenu_item_factory_t<laes_ctx> g_laes_ctx;
}
