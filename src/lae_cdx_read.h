// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE reader core (spec/cdx/SPEC.md, section 6), shared by the lae_cdx tool and the player plugins:
//   - the disc stream built from the input audio (whole-disc or per-track rips, with or without CUE; single files; LAE streams),
//   - the extension files (a folder of .lax files, or the /LAE/ folder of the data session),
//   - per track: coarse location (TOC position; information extent from lead0/tail0), precise location (sliding S1/S2 over every
//     start in the window), accepted only by the whole-track MD5.
// Positions found here exist only in the in-memory result, for display (R16); nothing here writes files.
// Paths are std::filesystem::path end to end and files are opened through the wide API on Windows, so names outside the ANSI code
// page work (a UTF-8 CUE is decoded as UTF-8).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <climits>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <functional>
#include <filesystem>
#include "lae_cdx.h"
#include "iso9660.h"
namespace cdxr {
namespace fs = std::filesystem;
static const long long SECTOR = 588, PREGAP = 150 * 588, WDEF = 5880;
// samples placed for a FILE of a CUE that cannot be read: more than any search window, so every window that reaches it is unknown
static const long long MISSING_GAP = 100LL * 44100;
inline bool readBytes(const fs::path& p, std::vector<uint8_t>& b) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb");
    auto seek = [](FILE* g, long long o, int w) { return _fseeki64(g, o, w); }; auto tell = [](FILE* g) { return (long long)_ftelli64(g); };
#else
    FILE* f = fopen(p.c_str(), "rb");
    auto seek = [](FILE* g, long long o, int w) { return fseeko(g, o, w); }; auto tell = [](FILE* g) { return (long long)ftello(g); };
#endif
    if (!f) return false; bool ok = seek(f, 0, SEEK_END) == 0; long long n = ok ? tell(f) : -1;
    ok = ok && n >= 0 && (unsigned long long)n <= (size_t)-1 / 2 && seek(f, 0, SEEK_SET) == 0;
    if (ok) { try { b.resize((size_t)n); } catch (...) { ok = false; } }
    ok = ok && (n == 0 || fread(b.data(), 1, b.size(), f) == b.size()); fclose(f); return ok; }
inline std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
inline std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
inline std::string u8(const fs::path& p) { return p.u8string(); }   // for reports
inline std::string extOf(const fs::path& p) { return lower(p.extension().u8string()); }
inline bool lossyExt(const std::string& e) { return e == ".mp3" || e == ".m4a" || e == ".aac" || e == ".ogg" || e == ".opus" || e == ".wma"; }

// ---- RIFF/WAVE in memory: 16-bit stereo PCM 'data' and the optional "LAE " chunk (every size checked without wrap-around)
inline std::string parseWav(const uint8_t* b, size_t size, int& rate, std::vector<int16_t>& pcm, std::vector<uint8_t>* lae) {
    if (size < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return "not a WAV file";
    auto u16 = [&](size_t o) { return (int)(b[o] | b[o + 1] << 8); }; auto u32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    size_t p = 12; int fmt = 0, ch = 0, bits = 0; const uint8_t* data = nullptr; size_t dlen = 0; bool haveFmt = false;
    while (size - p >= 8) { size_t body = p + 8, n = u32(p + 4); if (n > size - body) n = size - body;   // a truncated last chunk is tolerated
        if (!memcmp(b + p, "fmt ", 4)) { if (n < 16) return "bad fmt chunk"; fmt = u16(body); ch = u16(body + 2); rate = (int)u32(body + 4); bits = u16(body + 14); if (fmt == 0xFFFE && n >= 26) fmt = u16(body + 24); haveFmt = true; }
        else if (!memcmp(b + p, "data", 4)) { data = b + body; dlen = n; }
        else if (!memcmp(b + p, "LAE ", 4) && lae) lae->assign(b + body, b + body + n);
        if (n + (n & 1) >= size - body) break; p = body + n + (n & 1); }
    if (!haveFmt || !data) return "not a PCM WAV file";
    if (fmt != 1 || ch != 2 || bits != 16) return "not 16-bit stereo PCM";
    pcm.resize(dlen / 4 * 2); for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(data[i * 2] | data[i * 2 + 1] << 8);
    return ""; }
inline std::string parseWav(const std::vector<uint8_t>& b, int& rate, std::vector<int16_t>& pcm, std::vector<uint8_t>* lae) { return parseWav(b.data(), b.size(), rate, pcm, lae); }

// ---- the disc stream (6.2): samples at absolute positions; position 0 = INDEX 01 of the first audio track of the TOC
struct Disc {
    bool toc = false, tocFromOrder = false;    // tocFromOrder: TOC derived from the order and lengths of files (R2, reported prominently)
    std::map<int, long long> p;                // TOC: track number -> INDEX 01 position
    long long base = 0; std::vector<int16_t> s; std::vector<uint8_t> st;   // st: 0 unknown, 1 known, 2 conflict (unknown for good, R5)
    long long conflicts = 0; int rate = 0; std::string name; std::vector<std::string> notes;   // rate of the input audio; notes for the report
    std::set<int> missing;                     // tracks whose INDEX 01 is in a CUE FILE that could not be read
    long long end() const { return base + (long long)st.size(); }
    bool has(long long a) const { long long i = a - base; return i >= 0 && i < (long long)st.size() && st[(size_t)i] == 1; }
    int16_t L(long long a) const { return has(a) ? s[(size_t)(a - base) * 2] : 0; }
    int16_t R(long long a) const { return has(a) ? s[(size_t)(a - base) * 2 + 1] : 0; }
    uint32_t word(long long a) const { return has(a) ? cdx::word(s[(size_t)(a - base) * 2], s[(size_t)(a - base) * 2 + 1]) : 0; }
};
inline void reserve(Disc& d, long long lo, long long hi) {   // make [lo, hi) addressable
    if (hi <= lo) return;
    if (d.st.empty()) { d.base = lo; d.s.assign((size_t)(hi - lo) * 2, 0); d.st.assign((size_t)(hi - lo), 0); return; }
    long long nlo = std::min(lo, d.base), nhi = std::max(hi, d.end()); if (nlo == d.base && nhi == d.end()) return;
    std::vector<int16_t> s2((size_t)(nhi - nlo) * 2, 0); std::vector<uint8_t> t2((size_t)(nhi - nlo), 0); size_t sh = (size_t)(d.base - nlo);
    memcpy(&s2[sh * 2], d.s.data(), d.s.size() * 2); memcpy(&t2[sh], d.st.data(), d.st.size()); d.s.swap(s2); d.st.swap(t2); d.base = nlo; }
// puts a file's samples at absolute position at (R4); a position two files give different values becomes unknown (R5)
inline void place(Disc& d, long long at, const int16_t* pcm, long long n) {
    reserve(d, at, at + n);
    for (long long i = 0; i < n; i++) { size_t j = (size_t)(at + i - d.base); int16_t l = pcm[i * 2], r = pcm[i * 2 + 1];
        if (d.st[j] == 2) continue;
        if (d.st[j] == 1) { if (d.s[j * 2] != l || d.s[j * 2 + 1] != r) { d.st[j] = 2; d.s[j * 2] = d.s[j * 2 + 1] = 0; d.conflicts++; } continue; }
        d.st[j] = 1; d.s[j * 2] = l; d.s[j * 2 + 1] = r; } }

// ---- input audio files: 16-bit stereo PCM WAV, raw little-endian PCM (BINARY), raw big-endian PCM (MOTOROLA).
// A reason starting with "cannot read" means the file is missing or unreadable; any other reason is about its content.
inline std::string loadPcm(const fs::path& path, const std::string& type, std::vector<int16_t>& pcm, int& rate, int wantRate) {
    const std::string ext = extOf(path);
    if (lossyExt(ext)) return "lossy base layer (" + u8(path) + "): not used for the extension (F7)";
    std::vector<uint8_t> raw; if (!readBytes(path, raw)) return "cannot read " + u8(path);
    if (type == "WAVE") { std::string e = parseWav(raw, rate, pcm, nullptr);
        if (!e.empty()) return ext == ".flac" ? "FLAC input is not supported by this reader yet: " + u8(path) : e + ": " + u8(path);
        if (wantRate && rate != wantRate) return "sample rate " + std::to_string(rate) + " Hz, expected " + std::to_string(wantRate) + ": " + u8(path);
        return ""; }
    if (type == "BINARY" || type == "MOTOROLA") { rate = wantRate ? wantRate : 44100;
        pcm.resize(raw.size() / 4 * 2); for (size_t i = 0; i < pcm.size(); i++) pcm[i] = type == "BINARY" ? (int16_t)(raw[i * 2] | raw[i * 2 + 1] << 8) : (int16_t)(raw[i * 2] << 8 | raw[i * 2 + 1]);
        return ""; }
    return "unsupported FILE type " + type; }
// where the PCM of a CUE FILE comes from: the disk, or memory (the self-check reads its own image this way)
using PcmSource = std::function<std::string(const fs::path&, const std::string& type, std::vector<int16_t>&, int& rate)>;
inline PcmSource diskSource() { return [](const fs::path& p, const std::string& t, std::vector<int16_t>& pcm, int& rate) { return loadPcm(p, t, pcm, rate, 44100); }; }

// ---- CUE sheets (R2, R3, R4). Supported: FILE (BINARY, MOTOROLA, WAVE), TRACK (AUDIO; data tracks after the audio tracks are
// skipped when they are in their own FILE), INDEX, PREGAP before an INDEX 01 at the start of a file, POSTGAP at the end of a file,
// and the metadata keywords. Anything else stops the reader (R3). A CUE gives absolute positions only through its files following
// each other (CUE convention): PREGAP/POSTGAP add positions no file covers. An independent drive TOC is not read (R2, first source).
struct CueSheet {
    struct File { fs::path path, alt; std::string name, type; int audio = -1; long long maxIndex = 0; };   // audio: -1 no index yet, 0 data, 1 audio; alt: the ANSI reading of an ambiguous name
    struct Index { int num = 0, file = -1; long long pos = 0; };   // pos: in the file, in samples
    struct Track { int num = 0; bool audio = true; std::string mode; int idx1File = -1; long long idx1 = -1; int firstFile = -1; long long pregap = 0, postgap = 0;
                   std::vector<Index> idx; std::string isrc, flags; };   // idx: every INDEX in sheet order; flags: upper case, space separated
    std::vector<File> files; std::vector<Track> tracks; std::string catalog; };
// the layout of a sheet in absolute positions (track 1 INDEX 01 = 0): what an independent-CD image must keep (spec/cdx/SPEC.md L14).
// end: the end of the last audio file. The reader itself does not need it.
struct Layout { struct T { int num = 0; std::vector<std::pair<int, long long>> idx; std::string isrc, flags; }; std::vector<T> tracks; std::string catalog; long long end = 0; };
// strict UTF-8 (RFC 3629): no C0/C1 or F5-FF lead bytes, no overlong forms, no surrogates, nothing above U+10FFFF
inline bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) { const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        int n; unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) n = 1; else if (c == 0xE0) { n = 2; lo = 0xA0; } else if (c == 0xED) { n = 2; hi = 0x9F; } else if (c >= 0xE1 && c <= 0xEF) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; } else if (c == 0xF4) { n = 3; hi = 0x8F; } else if (c >= 0xF1 && c <= 0xF3) n = 3; else return false;
        if (i + (size_t)n >= s.size()) return false;   // the continuation bytes must exist
        for (int k = 1; k <= n; k++) { const unsigned char b = (unsigned char)s[i + (size_t)k]; if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) return false; }
        i += (size_t)n + 1; }
    return true; }
inline bool asciiOnly(const std::string& s) { for (char c : s) if ((unsigned char)c >= 0x80) return false; return true; }
inline std::string parseCueText(std::string txt, const fs::path& dir, CueSheet& c) {
    const std::string R3 = " (R3: the reader stops and does not guess)";
    bool bom = txt.size() >= 3 && (uint8_t)txt[0] == 0xEF && (uint8_t)txt[1] == 0xBB && (uint8_t)txt[2] == 0xBF; if (bom) txt = txt.substr(3);
    const bool utf8 = bom || validUtf8(txt); size_t pos = 0; int lineNo = 0;   // without a BOM a valid UTF-8 sheet is read as UTF-8, with the ANSI reading kept as a fallback name
    auto msf = [](const std::string& t, long long& v) { long long mm; int ss, ff; char x; if (sscanf(t.c_str(), "%lld:%d:%d%c", &mm, &ss, &ff, &x) != 3 || mm < 0 || mm > 99 || ss < 0 || ss > 59 || ff < 0 || ff > 74) return false;
        v = ((mm * 60 + ss) * 75 + ff) * SECTOR; return true; };
    while (pos < txt.size()) {
        size_t e = txt.find('\n', pos); std::string line = txt.substr(pos, e == std::string::npos ? std::string::npos : e - pos); pos = e == std::string::npos ? txt.size() : e + 1; lineNo++;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t a = line.find_first_not_of(" \t"); if (a == std::string::npos) continue; line = line.substr(a);
        std::vector<std::string> tok; { size_t i = 0; while (i < line.size()) { if (line[i] == ' ' || line[i] == '\t') { i++; continue; }
            if (line[i] == '"') { size_t q = line.find('"', i + 1); if (q == std::string::npos) return "CUE line " + std::to_string(lineNo) + ": unterminated quote" + R3; tok.push_back(line.substr(i + 1, q - i - 1)); i = q + 1; }
            else { size_t q = line.find_first_of(" \t", i); if (q == std::string::npos) q = line.size(); tok.push_back(line.substr(i, q - i)); i = q; } } }
        std::string kw = upper(tok[0]); std::string where = "CUE line " + std::to_string(lineNo) + ": ";
        if (kw == "CATALOG" && tok.size() >= 2) { c.catalog = tok[1]; continue; }   // kept for an independent-CD image (L14); otherwise metadata
        if (kw == "ISRC" && tok.size() >= 2 && !c.tracks.empty()) { c.tracks.back().isrc = tok[1]; continue; }
        if (kw == "FLAGS" && !c.tracks.empty()) { std::string f; for (size_t i = 1; i < tok.size(); i++) f += (i > 1 ? " " : "") + upper(tok[i]); c.tracks.back().flags = f; continue; }
        static const std::set<std::string> meta = {"REM", "TITLE", "PERFORMER", "SONGWRITER", "COMPOSER", "ARRANGER", "MESSAGE", "CATALOG", "ISRC", "CDTEXTFILE", "FLAGS"};
        if (meta.count(kw)) continue;
        if (kw == "FILE") { if (tok.size() != 3) return where + "FILE needs a name and a type" + R3;
            std::string type = upper(tok[2]); fs::path p, alt;
            try { p = dir / (utf8 ? fs::u8path(tok[1]) : fs::path(tok[1])); } catch (...) { return where + "file name cannot be decoded" + R3; }
            if (utf8 && !bom && !asciiOnly(tok[1])) { try { alt = dir / fs::path(tok[1]); } catch (...) { alt.clear(); } }
            if (type == "MP3" || lossyExt(extOf(p))) return "lossy base layer (" + u8(p.filename()) + "): not used for the extension (F7)";
            if (type != "BINARY" && type != "WAVE" && type != "MOTOROLA") return where + "unsupported FILE type " + tok[2] + R3;
            CueSheet::File f; f.path = p; f.alt = alt; f.name = u8(p.filename()); f.type = type; c.files.push_back(f); continue; }
        if (kw == "TRACK") { if (tok.size() != 3 || c.files.empty()) return where + "TRACK needs a number, a type and a preceding FILE" + R3;
            int n = atoi(tok[1].c_str()); if (n < 1 || n > 99) return where + "track number out of range" + R3;
            std::string type = upper(tok[2]); bool audio = type == "AUDIO";
            static const std::set<std::string> data = {"MODE1/2048", "MODE1/2352", "MODE2/2048", "MODE2/2324", "MODE2/2336", "MODE2/2352", "CDI/2336", "CDI/2352"};
            if (!audio && !data.count(type)) return where + "unsupported TRACK type " + tok[2] + R3;
            if (!c.tracks.empty() && n != c.tracks.back().num + 1) return where + "track numbers are not consecutive" + R3;
            if (audio && !c.tracks.empty() && !c.tracks.back().audio) return where + "audio track after a data track (not a CD-Extra layout)" + R3;
            CueSheet::Track t; t.num = n; t.audio = audio; t.mode = type; c.tracks.push_back(t); continue; }
        if (kw == "INDEX") { if (tok.size() != 3 || c.tracks.empty()) return where + "INDEX needs a number, a time and a preceding TRACK" + R3;
            int in = atoi(tok[1].c_str()); long long v; if (in < 0 || in > 99 || !msf(tok[2], v)) return where + "bad INDEX" + R3;
            CueSheet::Track& t = c.tracks.back(); int f = (int)c.files.size() - 1; int kind = t.audio ? 1 : 0;
            if (c.files[(size_t)f].audio >= 0 && c.files[(size_t)f].audio != kind) return where + "one FILE holds both audio and data tracks" + R3;
            c.files[(size_t)f].audio = kind; c.files[(size_t)f].maxIndex = std::max(c.files[(size_t)f].maxIndex, v); if (t.firstFile < 0) t.firstFile = f;
            if (in == 1) { if (t.idx1 >= 0) return where + "second INDEX 01" + R3; t.idx1 = v; t.idx1File = f; }
            CueSheet::Index ix; ix.num = in; ix.file = f; ix.pos = v; t.idx.push_back(ix); continue; }
        if (kw == "PREGAP" || kw == "POSTGAP") { if (tok.size() != 2 || c.tracks.empty()) return where + kw + " needs a time and a preceding TRACK" + R3; long long v; if (!msf(tok[1], v)) return where + "bad " + kw + R3;
            (kw == "PREGAP" ? c.tracks.back().pregap : c.tracks.back().postgap) = v; continue; }
        return where + "unknown keyword " + tok[0] + R3; }
    if (c.tracks.empty() || !c.tracks[0].audio) return "CUE has no audio track first" + R3;
    for (auto& f : c.files) if (f.audio < 0) return "CUE: FILE " + f.name + " has no INDEX line (not supported)" + R3;
    for (auto& t : c.tracks) if (t.audio && t.idx1 < 0) return "CUE track " + std::to_string(t.num) + " has no INDEX 01" + R3;
    for (size_t k = 0; k < c.tracks.size(); k++) { const auto& t = c.tracks[k]; if (!t.audio) continue;
        if (t.pregap && (t.idx1 != 0 || t.firstFile != t.idx1File)) return "CUE track " + std::to_string(t.num) + ": PREGAP that does not start a file is not supported" + R3;
        if (t.postgap) { bool ok = true; for (size_t j = k + 1; j < c.tracks.size(); j++) if (c.tracks[j].firstFile == t.idx1File) ok = false;
            if (!ok) return "CUE track " + std::to_string(t.num) + ": POSTGAP that does not end a file is not supported" + R3; } }
    return ""; }
// places every audio file of the sheet into the disc stream; positions follow the sheet. A FILE that cannot be read does not stop
// the reading (R5): it becomes unknown positions (all of its INDEX positions plus MISSING_GAP, so no later file can fall inside
// them); the tracks whose INDEX 01 is in it are marked missing (they fail without any CD output), and the windows of the other
// tracks that reach into it see unknown samples. When no audio FILE of the sheet can be read at all, the reader stops.
inline std::string loadCueText(const std::string& text, const fs::path& dir, Disc& d, const PcmSource& src, Layout* lay = nullptr) {
    CueSheet c; std::string e = parseCueText(text, dir, c); if (!e.empty()) return e;
    std::vector<std::vector<int16_t>> pcm(c.files.size()); std::vector<long long> start(c.files.size(), 0); std::vector<bool> missing(c.files.size(), false); long long T = 0;
    std::string firstMissing; size_t placed = 0;
    for (size_t f = 0; f < c.files.size(); f++) { if (c.files[f].audio != 1) continue;
        int rate = 0; std::string le = src(c.files[f].path, c.files[f].type, pcm[f], rate);
        if (le.rfind("cannot read", 0) == 0 && !c.files[f].alt.empty()) { pcm[f].clear(); le = src(c.files[f].alt, c.files[f].type, pcm[f], rate); }   // the ANSI reading of the name
        if (!le.empty() && le.rfind("cannot read", 0) != 0) return le;
        for (auto& t : c.tracks) if (t.audio && t.idx1File == (int)f && t.pregap) T += t.pregap;
        start[f] = T;
        if (!le.empty()) { missing[f] = true; pcm[f].clear(); T += c.files[f].maxIndex + MISSING_GAP; if (firstMissing.empty()) firstMissing = c.files[f].name;
            d.notes.push_back("file " + c.files[f].name + " not found: its tracks cannot be read, nearby windows see unknown samples (R5)"); }
        else { T += (long long)pcm[f].size() / 2; placed++; }
        for (auto& t : c.tracks) if (t.audio && t.idx1File == (int)f && t.postgap) T += t.postgap;
        if (T > (1LL << 28)) return "the sheet spans more than a CD holds (R3: the reader stops and does not guess)"; }
    if (!placed) return "cannot read " + firstMissing + " (no audio file of the sheet could be read)";
    long long abs0 = start[(size_t)c.tracks[0].idx1File] + c.tracks[0].idx1;
    for (auto& t : c.tracks) if (t.audio) { if (!missing[(size_t)t.idx1File] && t.idx1 > (long long)pcm[(size_t)t.idx1File].size() / 2) return "CUE track " + std::to_string(t.num) + ": INDEX 01 beyond the end of its file (R3: the reader stops and does not guess)";
        d.p[t.num] = start[(size_t)t.idx1File] + t.idx1 - abs0; if (missing[(size_t)t.idx1File]) d.missing.insert(t.num); }
    long long lo = LLONG_MAX, hi = LLONG_MIN; for (size_t f = 0; f < c.files.size(); f++) if (c.files[f].audio == 1 && !pcm[f].empty()) { lo = std::min(lo, start[f] - abs0); hi = std::max(hi, start[f] - abs0 + (long long)pcm[f].size() / 2); }
    if (lay) { *lay = Layout(); lay->catalog = c.catalog; lay->end = hi > LLONG_MIN ? hi : 0;
        for (auto& t : c.tracks) if (t.audio) { Layout::T lt; lt.num = t.num; lt.isrc = t.isrc; lt.flags = t.flags; bool has0 = false; for (auto& ix : t.idx) if (ix.num == 0) has0 = true;
            if (t.pregap && !has0) lt.idx.push_back({0, start[(size_t)t.idx1File] + t.idx1 - t.pregap - abs0});   // a PREGAP is the INDEX 00 of that length (burned as digital silence)
            for (auto& ix : t.idx) lt.idx.push_back({ix.num, start[(size_t)ix.file] + ix.pos - abs0}); lay->tracks.push_back(lt); } }
    if (lo < hi) reserve(d, lo, hi);
    for (size_t f = 0; f < c.files.size(); f++) if (c.files[f].audio == 1) { place(d, start[f] - abs0, pcm[f].data(), (long long)pcm[f].size() / 2); std::vector<int16_t>().swap(pcm[f]); }   // freed once placed
    d.toc = true; d.rate = 44100; return ""; }
inline std::string loadCue(const fs::path& cuePath, Disc& d, const PcmSource& src = diskSource(), Layout* lay = nullptr) {
    std::vector<uint8_t> b; if (!readBytes(cuePath, b)) return "cannot read " + u8(cuePath);
    d.name = u8(cuePath); return loadCueText(std::string(b.begin(), b.end()), cuePath.parent_path(), d, src, lay); }

// ---- extension files (R13, R14, X7). cd: the file comes from a CD (track 1-98, CD layer at 44100 Hz)
struct Ext { std::string name; std::vector<uint8_t> bytes; cdx::Trailer t; size_t blockLen = 0; std::string bad; };
inline void check(Ext& x, bool cd = false) { x.bad = cdx::parse(x.bytes, x.t, x.blockLen, cd); if (!x.bad.empty()) return;
    uint32_t stored; memcpy(&stored, &x.bytes[x.blockLen - 4], 4); if (cdx::crc32(x.bytes.data(), x.blockLen - 4) != stored) x.bad = "extension layer damaged"; }
inline std::string loadExtDir(const fs::path& dir, std::vector<Ext>& out, bool cd = false) {
    std::error_code ec; std::vector<fs::path> names;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && extOf(it->path()) == ".lax") names.push_back(it->path());
    if (ec) return "cannot list " + u8(dir); std::sort(names.begin(), names.end());
    for (auto& n : names) { Ext x; x.name = u8(n.filename()); if (!readBytes(n, x.bytes)) return "cannot read " + u8(n); check(x, cd); out.push_back(std::move(x)); }
    return ""; }
inline std::string loadExtIso(const std::vector<uint8_t>& img, uint32_t lba, std::vector<Ext>& out) {
    std::vector<iso::Entry> es; std::string err; if (!iso::list(img, lba, true, es, err) && !iso::list(img, lba, false, es, err)) return "data session unreadable: " + err;
    for (auto& e : es) { std::string up = upper(e.path); if (e.dir || up.rfind("LAE/", 0) != 0 || up.size() < 4 || up.compare(up.size() - 4, 4, ".LAX") != 0) continue;
        Ext x; x.name = e.path; if (!iso::readFile(img, lba, e, x.bytes)) return "cannot read " + e.path + " from the data session"; check(x, true); out.push_back(std::move(x)); }
    return ""; }

// ---- per-track location and verification (6.3, 6.4)
struct Track {
    int track = 0; enum State { PASS, FAIL, NOEXT } state = NOEXT; std::string reason;
    long long start = 0;        // position of the CD layer in the disc stream (display only, R16)
    const Ext* ext = nullptr; std::vector<int16_t> cd;   // PASS: the verified CD layer (ncd frames) and its extension file
};
struct Report { std::vector<Track> tracks; std::vector<std::string> rejected, unused; bool otherDisc = false; };
inline bool md5Window(const Disc& d, long long t, const cdx::Trailer& tr, std::vector<int16_t>* cdOut) {
    alignhash::Md5 m; std::vector<int16_t> cd((size_t)tr.ncd * 2); std::vector<uint8_t> by((size_t)tr.ncd * 4);
    for (uint32_t i = 0; i < tr.ncd; i++) { int16_t l = d.L(t + i), r = d.R(t + i); cd[i * 2] = l; cd[i * 2 + 1] = r;
        by[i * 4] = (uint8_t)l; by[i * 4 + 1] = (uint8_t)((uint16_t)l >> 8); by[i * 4 + 2] = (uint8_t)r; by[i * 4 + 3] = (uint8_t)((uint16_t)r >> 8); }
    m.update(by.data(), by.size()); if (m.hex() != tr.md5hex()) return false; if (cdOut) cdOut->swap(cd); return true; }
// information extent (6.3 step 1): first and last non-zero sample in [lo, hi) give two coarse starts
inline void extentStarts(const Disc& d, long long lo, long long hi, const cdx::Trailer& t, std::vector<long long>& c) {
    if (t.lead0 >= t.ncd) return; long long f = LLONG_MIN, l = LLONG_MIN;
    for (long long a = std::max(lo, d.base); a < std::min(hi, d.end()); a++) if (d.L(a) || d.R(a)) { f = a; break; }
    if (f == LLONG_MIN) return;
    for (long long a = std::min(hi, d.end()) - 1; a >= f; a--) if (d.L(a) || d.R(a)) { l = a; break; }
    c.push_back(f - t.lead0); c.push_back(l + 1 + t.tail0 - (long long)t.ncd); }
// searches the windows [t, t + ncd) for t in the given ranges (inclusive); first window whose S1/S2 and whole-track MD5 match
inline bool search(const Disc& d, const cdx::Trailer& t, std::vector<std::pair<long long, long long>> ranges, long long& at, std::vector<int16_t>& cd) {
    std::sort(ranges.begin(), ranges.end()); std::vector<std::pair<long long, long long>> m;
    for (auto& g : ranges) { if (g.second < g.first) continue; if (!m.empty() && g.first <= m.back().second + 1) m.back().second = std::max(m.back().second, g.second); else m.push_back(g); }
    bool found = false; long long len = d.end() - d.base;
    for (auto& g : m) { if (found) break;
        cdx::slide([&](long long j) { return d.word(d.base + j); }, len, t.ncd, g.first - d.base, g.second - d.base, t.s1, t.s2,
                   [&](long long tt) { if (!found && md5Window(d, d.base + tt, t, &cd)) { found = true; at = d.base + tt; } }); }
    return found; }
inline Track locate(const Disc& d, const Ext& x, long long W) {
    Track r; r.track = x.t.track; r.ext = &x; const cdx::Trailer& t = x.t; const long long n = t.ncd; long long at = 0;
    if (d.toc && d.missing.count(t.track)) { r.state = Track::FAIL; r.reason = "unknown samples (file missing)"; return r; }   // its INDEX 01 lies in a FILE that could not be read
    if (d.toc) { long long q = d.p.at(t.track); std::vector<long long> c = {q}; extentStarts(d, q - W, q + n + W, t, c);
        extentStarts(d, q, q + n + W, t, c);   // also from the TOC position on: a silent gap at the start of this track's file (TOC from the file order) must not hand the coarse start to the previous track's last sound
        std::vector<std::pair<long long, long long>> rg; for (long long v : c) rg.push_back({v - W, v + W});
        if (search(d, t, rg, at, r.cd)) { r.state = Track::PASS; r.start = at; return r; }
        if (d.tocFromOrder) {   // TOC from the file order only: this track's file may begin with its INDEX 00 gap (of any content), so its INDEX 01 lies
            auto nx = d.p.upper_bound(t.track);   // anywhere from the file start to the next file: search all of it (only after the coarse ranges failed)
            const long long e = nx == d.p.end() ? d.end() - n : nx->second;
            if (search(d, t, {{q - W, std::max(q, e) + W}}, at, r.cd)) { r.state = Track::PASS; r.start = at; return r; } }
        long long unk = 0; for (long long a = q - W; a < q + n + W; a++) if (!d.has(a)) unk++;
        r.state = Track::FAIL; r.reason = unk ? "unknown samples (missing or cut file): " + std::to_string(unk) : "no match"; r.cd.clear(); return r; }
    // no TOC: every start from -W to len - ncd + W (positions outside the file count as 0, R5, R7); coarse starts are searched first
    const long long len = d.end() - d.base; std::vector<long long> c; extentStarts(d, d.base, d.end(), t, c); std::vector<std::pair<long long, long long>> rg;
    for (long long v : c) rg.push_back({std::max(v - W, d.base - W), std::min(v + W, d.end() - n + W)});
    if (search(d, t, rg, at, r.cd) || search(d, t, {{d.base - W, d.end() - n + W}}, at, r.cd)) { r.state = Track::PASS; r.start = at; return r; }
    r.state = Track::FAIL; r.reason = len < n ? "file too short" : "no match"; r.cd.clear(); return r; }
// the whole reading: every extension file checked (X7), grouped by trailer track number (R13), each track located on its own (R7, R10)
inline Report read(const Disc& d, const std::vector<Ext>& exts, long long W) {
    Report S; std::map<int, std::vector<const Ext*>> by;
    for (auto& x : exts) { if (!x.bad.empty()) { S.rejected.push_back(x.name + ": " + x.bad); continue; }
        if (d.toc && !d.p.count(x.t.track)) { S.rejected.push_back(x.name + ": track number not in the TOC"); continue; }
        if (d.rate > 0 && x.t.rate != (uint32_t)d.rate) { S.rejected.push_back(x.name + ": its CD layer is " + std::to_string(x.t.rate) + " Hz, the audio " + std::to_string(d.rate) + " Hz"); continue; }
        by[x.t.track].push_back(&x); }
    std::map<int, Track> res; bool anyMatch = false;
    for (auto& kv : by) { std::vector<Track> pass; Track fail; fail.state = Track::FAIL; std::vector<std::string> unmatched;
        for (const Ext* x : kv.second) { Track r = locate(d, *x, W);
            if (r.state == Track::PASS) pass.push_back(std::move(r));
            else { if (fail.reason.empty() || r.reason != "no match") fail = r; unmatched.push_back(x->name + " (" + r.reason + ")"); } }
        if (!pass.empty()) anyMatch = true;
        if (pass.size() > 1) for (auto& p : pass) if (p.ext->blockLen != pass[0].ext->blockLen || memcmp(p.ext->bytes.data(), pass[0].ext->bytes.data(), p.ext->blockLen)) {
            fail = Track(); fail.state = Track::FAIL; fail.reason = "several extension files pass with different extension blocks"; pass.clear(); break; }   // R13: refused, reported
        Track out = pass.empty() ? std::move(fail) : std::move(pass[0]); out.track = kv.first;
        if (!d.toc) { for (auto& u : unmatched) S.unused.push_back(u);   // R13: a file found nowhere is unused, not a failure
            if (out.state == Track::FAIL && out.reason.rfind("several", 0) != 0) continue; }
        res[kv.first] = std::move(out); }
    if (d.toc) for (auto& kv : d.p) if (!res.count(kv.first)) { Track r; r.track = kv.first; r.state = Track::NOEXT; res[kv.first] = std::move(r); }
    int pass = 0, tried = 0; for (auto& kv : res) { if (kv.second.state != Track::NOEXT) tried++; if (kv.second.state == Track::PASS) pass++; S.tracks.push_back(std::move(kv.second)); }
    S.otherDisc = pass == 0 && (d.toc ? tried > 0 : !anyMatch && !S.unused.empty()); return S; }   // R12
}
