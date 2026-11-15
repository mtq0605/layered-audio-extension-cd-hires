// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
#pragma once
// Tags and pictures of a release file (2026-11-05).
// Only the standard structures: FLAC VORBIS_COMMENT + PICTURE blocks; RIFF LIST/INFO + an "id3 " chunk holding an ID3v2.4 tag.
//   --tag=KEY=VALUE        Vorbis comment names (TITLE, ARTIST, ALBUM, DATE, COPYRIGHT, COMMENT, ...); a key may repeat
//   --cover=<image>        the front cover (picture type 3)
//   --picture=<type>:<image>   any other picture (the type table shared by ID3 APIC and FLAC PICTURE: 4 back cover, 5 leaflet, 6 medium,
//                          8 artist, ...); any number; JPEG or PNG, stored byte for byte; at most 16 MB - 1 for the FLAC block
// One set describes the core and every master: no scope field, no version, no hash of the tags. Placement: FLAC after
// STREAMINFO and before every "LAE2" block; WAV after "fmt " and before "data". Nothing here touches the samples or the payload.
// Without any of these arguments nothing is written and the file is the same byte for byte as before.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>

namespace laestag {
struct Picture { int type = 3; std::string mime; uint32_t w = 0, h = 0, depth = 0, colors = 0; std::vector<uint8_t> data; };
struct Tags { std::vector<std::pair<std::string, std::string>> kv; std::vector<Picture> pics;
    bool empty() const { return kv.empty() && pics.empty(); } };
inline const char* kEncoder = "laes (LAE Lossless)";   // the vendor string / ISFT / TSSE: fixed, so that the same input gives the same file

// width, height, colour depth (bits per pixel) and palette size from the image header; "" or the reason
inline std::string readImage(const std::string& path, Picture& p) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return "cannot read " + path;
    p.data.clear(); uint8_t b[1 << 16]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) p.data.insert(p.data.end(), b, b + k); fclose(f);
    const std::vector<uint8_t>& d = p.data; const size_t n = d.size();
    auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]; };
    if (n >= 33 && !memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) && !memcmp(&d[12], "IHDR", 4)) {
        p.mime = "image/png"; p.w = be32(16); p.h = be32(20); const int bitDepth = d[24], colorType = d[25];
        const int channels = colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : 1; p.depth = (uint32_t)(bitDepth * channels);
        if (colorType == 3) for (size_t o = 8; o + 12 <= n;) { const uint32_t len = be32(o); if (!memcmp(&d[o + 4], "PLTE", 4)) { p.colors = len / 3; break; } if (len > n - o - 12) break; o += 12 + len; }
        return ""; }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {   // JPEG: the first SOF marker (C0..CF without C4 DHT, C8 JPG, CC DAC) gives size and components
        p.mime = "image/jpeg"; size_t o = 2;
        while (o + 4 <= n) { if (d[o] != 0xFF) return path + ": JPEG marker expected"; const uint8_t m = d[o + 1]; if (m == 0xFF) { o++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { o += 2; continue; }
            const size_t len = (size_t)d[o + 2] << 8 | d[o + 3]; if (len < 2 || o + 2 + len > n) break;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { if (len < 8) break;
                p.h = (uint32_t)d[o + 5] << 8 | d[o + 6]; p.w = (uint32_t)d[o + 7] << 8 | d[o + 8]; p.depth = (uint32_t)d[o + 4] * d[o + 9]; return ""; }
            if (m == 0xDA || m == 0xD9) break; o += 2 + len; }
        return path + ": no JPEG frame header (SOF) found"; }
    return path + ": not a JPEG or PNG image"; }

// well-formed UTF-8: shortest forms, no surrogates, at most U+10FFFF (any character, control characters included)
inline bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) { const uint8_t c = (uint8_t)s[i]; uint32_t cp; size_t k;
        if (c < 0x80) { cp = c; k = 1; } else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 2; } else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 3; } else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 4; } else return false;
        if (i + k > s.size()) return false; for (size_t q = 1; q < k; q++) { if (((uint8_t)s[i + q] & 0xC0) != 0x80) return false; cp = cp << 6 | ((uint8_t)s[i + q] & 0x3F); }
        if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += k; }
    return true; }
inline std::string upperKey(const std::string& k) { std::string u = k; for (char& c : u) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return u; }
// a repeated key: one INFO entry / text frame (INFO "; ", ID3v2.4 NUL-separated values)
inline std::vector<std::pair<std::string, std::vector<std::string>>> merged(const Tags& t) {
    std::vector<std::pair<std::string, std::vector<std::string>>> m;
    for (auto& kv : t.kv) { bool found = false; for (auto& e : m) if (e.first == kv.first) { e.second.push_back(kv.second); found = true; }
        if (!found) m.push_back({kv.first, {kv.second}}); }
    return m; }
inline const char* id3Frame(const std::string& k);
// the final lengths of what flacBlocks / wavChunks write, for a vendor string of encLen bytes; "" or the reason.
// FLAC: VORBIS_COMMENT and every PICTURE below 2^24 (24-bit block length); WAV: every ID3 frame and the whole ID3 tag below 2^28
// (syncsafe), the LIST/INFO and "id3 " chunks below 2^32.
inline std::string checkSizes(const Tags& t, size_t encLen, bool flac = true, bool wav = true) {   // flac / wav: which form to check
    const uint64_t kFlac = 1ull << 24, kId3 = 1ull << 28, kRiff = 1ull << 32;
    if (flac && !t.kv.empty()) { uint64_t vc = 8 + encLen; for (auto& kv : t.kv) vc += 4 + kv.first.size() + 1 + kv.second.size();
        if (vc >= kFlac) return "the tags together are too large for a FLAC VORBIS_COMMENT block (16 MB)"; }
    if (flac) for (auto& p : t.pics) if (32 + p.mime.size() + p.data.size() >= kFlac) return "a picture is too large for a FLAC metadata block (16 MB)";
    if (!wav || t.empty()) return "";
    uint64_t fr = 0, info = 4 + 8 + encLen + 1 + 1;   // "INFO" + ISFT
    auto frame = [&](uint64_t body) { fr += 10 + body; return body < kId3; };
    for (auto& m : merged(t)) { uint64_t v = 0, iv = 0;
        for (size_t i = 0; i < m.second.size(); i++) { v += (i ? 1 : 0) + m.second[i].size(); iv += (i ? 2 : 0) + m.second[i].size(); }
        info += 8 + iv + 1 + 1;   // upper bound (an INFO entry only for the known names)
        const uint64_t body = m.first == "COMMENT" ? 1 + 3 + 1 + v : *id3Frame(m.first) ? 1 + v : 1 + m.first.size() + 1 + v;
        if (!frame(body)) return "a tag is too large for an ID3v2 frame (256 MB)"; }
    frame(1 + encLen);   // TSSE
    for (auto& p : t.pics) if (!frame(1 + p.mime.size() + 1 + 1 + 1 + p.data.size())) return "a picture is too large for an ID3v2 frame (256 MB)";
    if (fr >= kId3) return "the tags and pictures together are too large for an ID3v2 tag (256 MB)";
    if (info >= kRiff || 10 + fr >= kRiff) return "the tags together are too large for a RIFF chunk (4 GB)";
    return ""; }
// the final check inside flacBlocks / wavChunks: with err the reason goes there and nothing is returned, else std::length_error
inline bool sizesOk(const Tags& t, const char* enc, std::string* err, bool flac) {
    const std::string e = checkSizes(t, strlen(enc), flac, !flac); if (e.empty()) return true;
    if (err) { *err = e; return false; } throw std::length_error(e); }

// --tag / --cover / --picture from the command line; "" or the reason. Checked before any work is done.
inline std::string parseArgs(int argc, char** argv, Tags& t) {
    for (int i = 1; i < argc; i++) { const std::string a = argv[i];
        if (!a.compare(0, 6, "--tag=")) { const std::string kv = a.substr(6); const size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) return "--tag needs KEY=VALUE: " + a;
            const std::string key = upperKey(kv.substr(0, eq)); for (char c : key) if (c < 0x20 || c > 0x7D || c == '=') return "--tag: a key is ASCII 0x20..0x7D without '=': " + a;
            if (!validUtf8(kv.substr(eq + 1))) return "--tag: the value is not valid UTF-8: " + a;   // Vorbis comments and ID3 (encoding 3) are UTF-8
            t.kv.push_back({key, kv.substr(eq + 1)}); }
        else if (!a.compare(0, 8, "--cover=") || !a.compare(0, 10, "--picture=")) { Picture p; std::string path;
            if (a[2] == 'c') { p.type = 3; path = a.substr(8); }
            else { const std::string v = a.substr(10); const size_t c = v.find(':'); if (c == std::string::npos || c == 0 || c > 2) return "--picture needs <type>:<image>: " + a;
                for (size_t q = 0; q < c; q++) if (v[q] < '0' || v[q] > '9') return "--picture: the type is a number 0..20: " + a;
                p.type = atoi(v.substr(0, c).c_str()); path = v.substr(c + 1); if (p.type > 20) return "--picture: the type is a number 0..20: " + a; }
            const std::string e = readImage(path, p); if (!e.empty()) return e;
            if (32 + p.mime.size() + p.data.size() >= (1u << 24)) return path + ": too large for a FLAC metadata block (16 MB)";
            t.pics.push_back(std::move(p)); } }
    size_t all = 0; for (auto& p : t.pics) all += p.data.size() + 64; if (all >= (1u << 28) - 4096) return "the pictures together are too large for an ID3v2 tag (256 MB)";
    return checkSizes(t, 64); }   // the text counts too (room for a vendor string of 64 bytes)
// true when the argument is one of ours (the work folder does not depend on it)
inline bool isTagArg(const std::string& a) { return !a.compare(0, 6, "--tag=") || !a.compare(0, 8, "--cover=") || !a.compare(0, 10, "--picture="); }

inline void le32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }
inline void be32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 3; i >= 0; i--) o.push_back((uint8_t)(v >> (8 * i))); }
inline void add(std::vector<uint8_t>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); }

// FLAC: VORBIS_COMMENT (type 4) then one PICTURE (type 6) per picture; lastBlock: the final one carries the "last" flag. Empty for no tags.
inline std::vector<uint8_t> flacBlocks(const Tags& t, bool lastBlock, const char* enc = kEncoder, std::string* err = nullptr) {   // enc: the vendor string (lae_enc names itself)
    if (!sizesOk(t, enc, err, true)) return {};
    std::vector<std::vector<uint8_t>> bl; std::vector<int> types;
    if (!t.kv.empty()) { std::vector<uint8_t> b; le32(b, (uint32_t)strlen(enc)); add(b, enc); le32(b, (uint32_t)t.kv.size());
        for (auto& kv : t.kv) { const std::string s = kv.first + "=" + kv.second; le32(b, (uint32_t)s.size()); add(b, s); }
        bl.push_back(b); types.push_back(4); }
    for (auto& p : t.pics) { std::vector<uint8_t> b; be32(b, (uint32_t)p.type); be32(b, (uint32_t)p.mime.size()); add(b, p.mime); be32(b, 0);   // no description
        be32(b, p.w); be32(b, p.h); be32(b, p.depth); be32(b, p.colors); be32(b, (uint32_t)p.data.size()); b.insert(b.end(), p.data.begin(), p.data.end());
        bl.push_back(b); types.push_back(6); }
    std::vector<uint8_t> o; for (size_t i = 0; i < bl.size(); i++) { const uint32_t L = (uint32_t)bl[i].size();
        o.push_back((uint8_t)((lastBlock && i + 1 == bl.size() ? 0x80 : 0) | types[i])); o.push_back((uint8_t)(L >> 16)); o.push_back((uint8_t)(L >> 8)); o.push_back((uint8_t)L);
        o.insert(o.end(), bl[i].begin(), bl[i].end()); }
    return o; }

// Vorbis name -> RIFF INFO id / ID3v2.4 text frame; "" = none (ID3: TXXX)
static const char* const kInfoIds[][2] = {{"TITLE", "INAM"}, {"ARTIST", "IART"}, {"ALBUM", "IPRD"}, {"DATE", "ICRD"}, {"COPYRIGHT", "ICOP"}, {"COMMENT", "ICMT"},
                                           {"GENRE", "IGNR"}, {"TRACKNUMBER", "ITRK"}, {"ENGINEER", "IENG"}, {"DESCRIPTION", "ISBJ"}};
inline const char* infoId(const std::string& k) {
    for (auto& e : kInfoIds) if (k == e[0]) return e[1]; return ""; }
// the other way, RIFF INFO id -> Vorbis name (laes_export writes FLAC tags from a WAV file's LIST/INFO); ISFT (the
// encoder) -> ENCODER; "" = not one of these (the id is kept as it is)
inline const char* vorbisName(const std::string& id) {
    for (auto& e : kInfoIds) if (id == e[1]) return e[0]; return id == "ISFT" ? "ENCODER" : ""; }
inline const char* id3Frame(const std::string& k) {
    static const char* m[][2] = {{"TITLE", "TIT2"}, {"ARTIST", "TPE1"}, {"ALBUM", "TALB"}, {"ALBUMARTIST", "TPE2"}, {"DATE", "TDRC"}, {"COPYRIGHT", "TCOP"},
                                 {"GENRE", "TCON"}, {"TRACKNUMBER", "TRCK"}, {"DISCNUMBER", "TPOS"}, {"COMPOSER", "TCOM"}, {"LYRICIST", "TEXT"}, {"ISRC", "TSRC"}};
    for (auto& e : m) if (k == e[0]) return e[1]; return ""; }
inline void syncsafe(std::vector<uint8_t>& o, uint32_t v) { for (int i = 3; i >= 0; i--) o.push_back((uint8_t)((v >> (7 * i)) & 0x7F)); }

// WAV: a LIST/INFO chunk (UTF-8 text, NUL-terminated, even sizes) and an "id3 " chunk (ID3v2.4, UTF-8). Empty for no tags.
inline std::vector<uint8_t> wavChunks(const Tags& t, const char* enc = kEncoder, std::string* err = nullptr) {   // enc: the encoder named in ISFT / TSSE (lae_enc names itself)
    std::vector<uint8_t> o; if (t.empty()) return o;
    if (!sizesOk(t, enc, err, false)) return o;
    const auto merged = laestag::merged(t);
    auto join = [](const std::vector<std::string>& v, const std::string& sep) { std::string s; for (size_t i = 0; i < v.size(); i++) s += (i ? sep : std::string()) + v[i]; return s; };
    {   std::vector<uint8_t> info; add(info, "INFO");
        auto put = [&](const char* id, const std::string& v) { add(info, id); le32(info, (uint32_t)v.size() + 1); add(info, v); info.push_back(0); if ((v.size() + 1) & 1) info.push_back(0); };
        for (auto& m : merged) if (*infoId(m.first)) put(infoId(m.first), join(m.second, "; "));
        put("ISFT", enc);
        add(o, "LIST"); le32(o, (uint32_t)info.size()); o.insert(o.end(), info.begin(), info.end()); }
    {   std::vector<uint8_t> fr;
        auto frame = [&](const char* id, const std::vector<uint8_t>& body) { add(fr, id); syncsafe(fr, (uint32_t)body.size()); fr.push_back(0); fr.push_back(0); fr.insert(fr.end(), body.begin(), body.end()); };
        for (auto& m : merged) { std::vector<uint8_t> b; b.push_back(3);   // 3 = UTF-8
            const std::string v = join(m.second, std::string(1, '\0'));
            if (m.first == "COMMENT") { add(b, "XXX"); b.push_back(0); add(b, join(m.second, "\n")); frame("COMM", b); }   // COMM holds one text
            else if (*id3Frame(m.first)) { add(b, v); frame(id3Frame(m.first), b); }
            else { add(b, m.first); b.push_back(0); add(b, v); frame("TXXX", b); } }
        { std::vector<uint8_t> b; b.push_back(3); add(b, enc); frame("TSSE", b); }
        for (auto& p : t.pics) { std::vector<uint8_t> b; b.push_back(3); add(b, p.mime); b.push_back(0); b.push_back((uint8_t)p.type); b.push_back(0); b.insert(b.end(), p.data.begin(), p.data.end()); frame("APIC", b); }
        std::vector<uint8_t> tag; add(tag, "ID3"); tag.push_back(4); tag.push_back(0); tag.push_back(0); syncsafe(tag, (uint32_t)fr.size()); tag.insert(tag.end(), fr.begin(), fr.end());
        add(o, "id3 "); le32(o, (uint32_t)tag.size()); o.insert(o.end(), tag.begin(), tag.end()); if (tag.size() & 1) o.push_back(0); }
    return o; }
}  // namespace laestag
