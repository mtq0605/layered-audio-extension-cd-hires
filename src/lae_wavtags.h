// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_wavtags.h - the tags of a WAV-form LAE file (LAEY, and LAES in the WAV form) for the player plugins (2026-11-06).
// Reads the RIFF LIST/INFO chunk and the "id3 " chunk (ID3v2.3 or 2.4) and returns the fields under the common names (TITLE, ARTIST,
// ALBUM, ALBUM ARTIST, DATE, GENRE, TRACKNUMBER, DISCNUMBER, COMPOSER, COMMENT, COPYRIGHT, ENCODER, ...; TXXX under its own description).
// ID3 first: it holds every field (laes and lae_enc write the same values to both); INFO adds the fields ID3 lacks. Text that is not
// UTF-8 is read as Latin-1 (ID3 encoding 0) or, for INFO from older Windows programs, in the system code page. All output is UTF-8.
// Reading only: nothing here writes a file. The decoders do not use any of it (tags never change the audio).
//   read(rd, size): rd(offset, buffer, bytes) -> true when all bytes were read; size: the file size.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#ifdef _WIN32
extern "C" {
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int, unsigned long, const char*, int, wchar_t*, int);
}
#endif

namespace lae_wavtags {
typedef std::vector<std::pair<std::string, std::string>> List;
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) { const uint8_t c = (uint8_t)s[i]; const int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= s.size() + (k ? 0 : 1)) return false;
        for (int j = 1; j <= k; j++) if (((uint8_t)s[i + j] & 0xC0) != 0x80) return false;
        i += (size_t)k + 1; }
    return true; }
inline void putUtf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c; else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | (c >> 6 & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | (c >> 12 & 0x3F)); o += (char)(0x80 | (c >> 6 & 0x3F)); o += (char)(0x80 | (c & 0x3F)); } }
inline std::string fromLatin1(const std::string& s) { std::string o; for (unsigned char c : s) putUtf8(o, c); return o; }
inline std::string fromAnsi(const std::string& s) {   // the system code page (INFO written by older Windows programs); Latin-1 elsewhere
#ifdef _WIN32
    if (s.empty()) return s; const int n = MultiByteToWideChar(0, 0, s.data(), (int)s.size(), nullptr, 0); if (n <= 0) return fromLatin1(s);
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(0, 0, s.data(), (int)s.size(), &w[0], n);
    const int m = WideCharToMultiByte(65001, 0, w.data(), n, nullptr, 0, nullptr, nullptr); std::string o((size_t)(m > 0 ? m : 0), '\0');
    if (m > 0) WideCharToMultiByte(65001, 0, w.data(), n, &o[0], m, nullptr, nullptr); return o;
#else
    return fromLatin1(s);
#endif
}
inline std::string fromUtf16(const uint8_t* p, size_t n, bool be) { std::string o;
    for (size_t i = 0; i + 1 < n; i += 2) { uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8); if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) { const uint32_t d = be ? (uint32_t)(p[i + 2] << 8 | p[i + 3]) : (uint32_t)(p[i + 2] | p[i + 3] << 8);
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; } }
        putUtf8(o, c); }
    return o; }
// one ID3 text in encoding e: its values (ID3v2.4 separates several by NUL)
inline std::vector<std::string> texts(int e, const uint8_t* p, size_t n) { std::vector<std::string> out;
    if (e == 1 || e == 2) { bool be = e == 2; size_t i = 0;
        while (i + 1 < n) { size_t j = i; if (e == 1 && j + 1 < n && ((p[j] == 0xFF && p[j + 1] == 0xFE) || (p[j] == 0xFE && p[j + 1] == 0xFF))) { be = p[j] == 0xFE; j += 2; }
            size_t k = j; while (k + 1 < n && (p[k] || p[k + 1])) k += 2; out.push_back(fromUtf16(p + j, k - j, be)); i = k + 2; } }
    else { const std::string s((const char*)p, n); size_t i = 0;
        while (i <= s.size()) { size_t k = s.find('\0', i); if (k == std::string::npos) k = s.size(); std::string v = s.substr(i, k - i);
            if (e == 0 || !validUtf8(v)) v = fromLatin1(v); out.push_back(v); i = k + 1; if (k == s.size()) break; } }
    while (!out.empty() && out.back().empty()) out.pop_back(); return out; }
inline const char* id3Name(const char* id) {
    static const char* m[][2] = {{"TIT2", "TITLE"}, {"TPE1", "ARTIST"}, {"TALB", "ALBUM"}, {"TPE2", "ALBUM ARTIST"}, {"TDRC", "DATE"}, {"TYER", "DATE"}, {"TCOP", "COPYRIGHT"},
                                 {"TCON", "GENRE"}, {"TRCK", "TRACKNUMBER"}, {"TPOS", "DISCNUMBER"}, {"TCOM", "COMPOSER"}, {"TEXT", "LYRICIST"}, {"TSRC", "ISRC"}, {"TIT3", "SUBTITLE"},
                                 {"TPE3", "CONDUCTOR"}, {"TPUB", "PUBLISHER"}, {"TBPM", "BPM"}, {"TSSE", "ENCODER"}};
    for (auto& e : m) if (!memcmp(id, e[0], 4)) return e[1]; return nullptr; }
inline void id3(const std::vector<uint8_t>& t, List& out) {
    if (t.size() < 10 || memcmp(t.data(), "ID3", 3)) return; const int ver = t[3]; if (ver != 3 && ver != 4) return;
    const uint8_t flags = t[5]; const size_t want = (size_t)10 + ((size_t)(t[6] & 0x7F) << 21 | (size_t)(t[7] & 0x7F) << 14 | (size_t)(t[8] & 0x7F) << 7 | (t[9] & 0x7F));
    std::vector<uint8_t> b(t.begin(), t.begin() + (want < t.size() ? want : t.size())); size_t p = 10;
    if (ver == 3 && (flags & 0x80)) { std::vector<uint8_t> u(b.begin(), b.begin() + 10); for (size_t i = 10; i < b.size(); i++) { u.push_back(b[i]); if (b[i] == 0xFF && i + 1 < b.size() && b[i + 1] == 0) i++; } b.swap(u); }
    if (flags & 0x40) { if (p + 4 > b.size()) return;
        p += ver == 4 ? ((size_t)(b[p] & 0x7F) << 21 | (size_t)(b[p + 1] & 0x7F) << 14 | (size_t)(b[p + 2] & 0x7F) << 7 | (b[p + 3] & 0x7F)) : ((size_t)b[p] << 24 | (size_t)b[p + 1] << 16 | (size_t)b[p + 2] << 8 | b[p + 3]) + 4; }
    while (p + 10 <= b.size() && b[p]) { char id[5] = {(char)b[p], (char)b[p + 1], (char)b[p + 2], (char)b[p + 3], 0};
        const size_t n = ver == 4 ? ((size_t)(b[p + 4] & 0x7F) << 21 | (size_t)(b[p + 5] & 0x7F) << 14 | (size_t)(b[p + 6] & 0x7F) << 7 | (b[p + 7] & 0x7F))
                                  : ((size_t)b[p + 4] << 24 | (size_t)b[p + 5] << 16 | (size_t)b[p + 6] << 8 | b[p + 7]);
        const uint16_t ff = (uint16_t)(b[p + 8] << 8 | b[p + 9]); p += 10; if (n > b.size() - p) break;
        std::vector<uint8_t> d(b.begin() + p, b.begin() + p + n); p += n;
        if ((ver == 4 ? (ff & 0x000C) : (ff & 0x00C0)) || d.empty()) continue;   // compressed or encrypted: not read
        if (ver == 4 && (ff & 0x0002)) { std::vector<uint8_t> u; for (size_t i = 0; i < d.size(); i++) { u.push_back(d[i]); if (d[i] == 0xFF && i + 1 < d.size() && d[i + 1] == 0) i++; } d.swap(u); }
        if (ver == 4 && (ff & 0x0001)) { if (d.size() < 4) continue; d.erase(d.begin(), d.begin() + 4); }
        if (d.empty()) continue; const int e = d[0];
        if (!memcmp(id, "TXXX", 4)) { std::vector<std::string> v = texts(e, d.data() + 1, d.size() - 1); if (v.size() >= 2 && !v[0].empty()) for (size_t i = 1; i < v.size(); i++) out.push_back({v[0], v[i]}); }
        else if (!memcmp(id, "COMM", 4)) { if (d.size() < 4) continue; std::vector<std::string> v = texts(e, d.data() + 4, d.size() - 4); if (v.size() >= 2) out.push_back({v[0].empty() || v[0] == "XXX" ? "COMMENT" : v[0], v[1]}); }
        else if (id[0] == 'T') { const char* nm = id3Name(id); if (!nm) continue; for (auto& v : texts(e, d.data() + 1, d.size() - 1)) out.push_back({nm, v}); } } }
inline const char* infoName(const char* id) {
    static const char* m[][2] = {{"INAM", "TITLE"}, {"IART", "ARTIST"}, {"IPRD", "ALBUM"}, {"ICRD", "DATE"}, {"ICOP", "COPYRIGHT"}, {"ICMT", "COMMENT"}, {"IGNR", "GENRE"},
                                 {"ITRK", "TRACKNUMBER"}, {"IENG", "ENGINEER"}, {"ISBJ", "DESCRIPTION"}, {"ISFT", "ENCODER"}};
    for (auto& e : m) if (!memcmp(id, e[0], 4)) return e[1]; return nullptr; }
inline bool sameName(const std::string& a, const std::string& b) { if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) { char x = a[i], y = b[i]; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return false; } return true; }
// every tag of a RIFF / RF64 / BW64 WAVE file (empty for anything else)
template <class R> List read(R&& rd, uint64_t size) {
    List fromId3, fromInfo; uint8_t h[12]; if (size < 12 || !rd(0, h, 12) || memcmp(h + 8, "WAVE", 4) || (memcmp(h, "RIFF", 4) && memcmp(h, "RF64", 4) && memcmp(h, "BW64", 4))) return List();
    for (uint64_t pos = 12; pos + 8 <= size;) { uint8_t c[8]; if (!rd(pos, c, 8)) break; uint64_t n = le32(c + 4);
        if (!memcmp(c, "data", 4) && n == 0xFFFFFFFFu) break;   // RF64 data: the tags of our files come before it
        if (n > size - pos - 8) n = size - pos - 8;
        if ((!memcmp(c, "id3 ", 4) || !memcmp(c, "ID3 ", 4)) && n <= (16u << 20)) { std::vector<uint8_t> t((size_t)n); if (n && rd(pos + 8, t.data(), t.size())) id3(t, fromId3); }
        else if (!memcmp(c, "LIST", 4) && n >= 4 && n <= (1u << 20)) { std::vector<uint8_t> t((size_t)n);
            if (rd(pos + 8, t.data(), t.size()) && !memcmp(t.data(), "INFO", 4))
                for (size_t p = 4; p + 8 <= t.size();) { const size_t m = le32(&t[p + 4]); if (m > t.size() - p - 8) break;
                    const char* nm = infoName((const char*)&t[p]); std::string v((const char*)&t[p + 8], m); while (!v.empty() && v.back() == '\0') v.pop_back();
                    if (!validUtf8(v)) v = fromAnsi(v); if (nm && !v.empty()) fromInfo.push_back({nm, v}); p += 8 + m + (m & 1); } }
        pos += 8 + n + (n & 1); }
    List out = fromId3;
    for (auto& kv : fromInfo) { bool have = false; for (auto& x : fromId3) if (sameName(x.first, kv.first)) { have = true; break; } if (!have) out.push_back(kv); }
    return out; }
}  // namespace lae_wavtags
