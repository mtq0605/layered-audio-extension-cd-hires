// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_flacmeta.h - the structure of an LAEY file in the FLAC form (2026-11-10), without any decoding: for the decoder (laey_flac.h)
// and for the player plugins (recognising the file, its length and rates, its tags). Only the standard library; nothing here writes.
//   "fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (optional) | APPLICATION blocks with ID "LAE1" (k >= 1) | audio frames = the CD layer
//   * the CD layer: 2 channels, 16 bits, 44.1 or 48 kHz, losslessly coded by FLAC; STREAMINFO holds its length and its MD5;
//   * the extension: the body of the WAV form's "LAE " chunk, byte for byte (it begins with "LAE1" and ends with its CRC-32), split
//     into pieces of at most 2^24 - 5 bytes; each piece is the data of one APPLICATION block with ID "LAE1", in order;
//   * no SEEKTABLE, no CUESHEET, nothing interleaved with the audio frames.
// A reader joins the data of every "LAE1" block in file order.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <functional>
namespace laeyflac {
typedef std::function<bool(uint64_t off, void* buf, size_t n)> ReadFn;   // true when all n bytes were read
struct Piece { uint64_t off = 0, len = 0; };
struct Layout {
    int rate = 0, channels = 0, bps = 0, minBs = 0, maxBs = 0; uint64_t total = 0; uint8_t md5[16] = {0};   // STREAMINFO
    uint64_t audioOff = 0, audioLen = 0;      // the audio frames: from the end of the metadata to the end of the file
    std::vector<Piece> lae; uint64_t laeLen = 0;   // the data of the "LAE1" blocks (after the ID), in file order, and their total
    bool lae2 = false;                        // an "LAE2" block was met (an LAES file)
    uint64_t vcOff = 0, vcLen = 0; };         // the first VORBIS_COMMENT block (0: none)
const size_t kPiece = ((size_t)1 << 24) - 1 - 4;   // the largest piece: a metadata block holds 2^24 - 1 bytes, 4 of them the ID

// the metadata blocks of a FLAC file. Returns "" or the reason. Nothing is decoded.
inline std::string layout(const ReadFn& rd, uint64_t size, Layout& L) {
    uint8_t h[4]; if (size < 8 || !rd(0, h, 4) || memcmp(h, "fLaC", 4)) return "not a FLAC file";
    uint64_t p = 4; bool last = false, haveSi = false; L = Layout();
    while (!last) { uint8_t bh[4]; if (size - p < 4 || !rd(p, bh, 4)) return "FLAC metadata truncated";
        last = (bh[0] & 0x80) != 0; const int type = bh[0] & 0x7F; const uint64_t len = (uint64_t)bh[1] << 16 | (uint64_t)bh[2] << 8 | bh[3]; p += 4;
        if (len > size - p) return "FLAC metadata truncated";
        if (!haveSi && type != 0) return "STREAMINFO is not the first metadata block";
        if (type == 0) { uint8_t s[34]; if (haveSi || len != 34 || !rd(p, s, 34)) return "bad STREAMINFO";
            L.minBs = s[0] << 8 | s[1]; L.maxBs = s[2] << 8 | s[3]; uint64_t v = 0; for (int i = 10; i < 18; i++) v = v << 8 | s[i];
            L.rate = (int)(v >> 44); L.channels = (int)((v >> 41) & 7) + 1; L.bps = (int)((v >> 36) & 31) + 1; L.total = v & (((uint64_t)1 << 36) - 1);
            memcpy(L.md5, s + 18, 16); haveSi = true; }
        else if (type == 2 && len >= 4) { uint8_t id[4]; if (!rd(p, id, 4)) return "FLAC metadata truncated";
            if (!memcmp(id, "LAE1", 4)) { if (len > 4) { Piece q; q.off = p + 4; q.len = len - 4; L.lae.push_back(q); L.laeLen += len - 4; } }
            else if (!memcmp(id, "LAE2", 4)) L.lae2 = true; }
        else if (type == 4 && !L.vcLen) { L.vcOff = p; L.vcLen = len; }
        p += len; }
    L.audioOff = p; L.audioLen = size - p; return ""; }
// what an LAEY reader needs from the layout; "" or the reason. notLaey: not an LAEY FLAC file at all (a plain FLAC, an LAES file);
// otherwise the reason names a damaged file.
inline std::string check(const Layout& L, bool& notLaey) {
    notLaey = true;
    if (L.lae.empty()) return L.lae2 ? "this is an LAES file (lossless, extension LAE2): decode it with laes; this decoder reads LAEY (extension LAE1)" : "no LAE1 block: plain FLAC audio";
    if (L.channels != 2 || L.bps != 16) return "the FLAC audio is not 16-bit stereo: not the CD layer of an LAEY file";
    notLaey = false;
    if (L.total == 0) return "STREAMINFO gives no sample count (the CD layer of an LAEY file always has one)";
    if (L.maxBs < 16) return "bad STREAMINFO block size";
    if (L.total > (uint64_t)0x7FFFFFFF || L.laeLen > (uint64_t)0x7FFFFFFF) return "the file is too large for this decoder";
    return ""; }
// the first n bytes of the extension (n <= 64; the v5 header's first 24 bytes: magic, version, master rate, base rate, stages, master
// frames), across pieces; false when the extension is shorter or cannot be read
inline bool head(const ReadFn& rd, const Layout& L, uint8_t* out, size_t n) {
    size_t got = 0; for (size_t i = 0; i < L.lae.size() && got < n; i++) { const size_t k = (size_t)(L.lae[i].len < (uint64_t)(n - got) ? L.lae[i].len : (uint64_t)(n - got));
        if (!rd(L.lae[i].off, out + got, k)) return false; got += k; }
    return got == n; }
// the Vorbis comments (KEY=value, UTF-8; keys as written) of the first VORBIS_COMMENT block; empty when there is none or it is damaged
inline std::vector<std::pair<std::string, std::string>> comments(const ReadFn& rd, const Layout& L) {
    std::vector<std::pair<std::string, std::string>> out; if (L.vcLen < 8 || L.vcLen >= ((uint64_t)1 << 24)) return out;
    std::vector<uint8_t> b((size_t)L.vcLen); if (!rd(L.vcOff, b.data(), b.size())) return out; size_t q = 0;
    auto u32 = [&](uint32_t& v) { if (b.size() - q < 4) return false; v = (uint32_t)b[q] | (uint32_t)b[q + 1] << 8 | (uint32_t)b[q + 2] << 16 | (uint32_t)b[q + 3] << 24; q += 4; return true; };
    uint32_t vl = 0, cnt = 0; if (!u32(vl) || vl > b.size() - q) return out; q += vl; if (!u32(cnt)) return out;
    for (uint32_t i = 0; i < cnt; i++) { uint32_t l = 0; if (!u32(l) || l > b.size() - q) break; const std::string kv((const char*)b.data() + q, l); q += l;
        const size_t eq = kv.find('='); if (eq != std::string::npos && eq > 0) out.push_back(std::make_pair(kv.substr(0, eq), kv.substr(eq + 1))); }
    return out; }
}  // namespace laeyflac
