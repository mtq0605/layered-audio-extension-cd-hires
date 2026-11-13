// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// laey_flac.h - reader of the LAEY FLAC form (2026-11-10): the structure (lae_flacmeta.h: STREAMINFO, the "LAE1" APPLICATION
// blocks, the tags) plus the decoding of the audio frames into the CD layer. The pair (CD layer, extension) is then decoded exactly
// as the pair ('data', "LAE ") of the WAV form (lae_decoder.h). The FLAC frames are read with laes_flac.h (RFC 9639, the project's own
// code; two independent channels; frames with inter-channel decorrelation are reported, not decoded).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <functional>
#include "lae_flacmeta.h"
#include "laes_flac.h"
#include "align_hash.h"
namespace laeyflac {
inline laesflac::Info infoOf(const Layout& L) { laesflac::Info si; si.rate = L.rate; si.channels = L.channels; si.bps = L.bps; si.minBs = L.minBs; si.maxBs = L.maxBs; si.total = L.total; memcpy(si.md5, L.md5, 16); return si; }

// the audio frames (all of them, n bytes) decoded into pcm (interleaved L/R). Frames of a fixed block size are decoded in several
// threads when the stream is long; the result is the one of decoding them in order. Returns "" or the reason.
inline std::string decodeAudio(const uint8_t* a, size_t n, const laesflac::Info& si, std::vector<int16_t>& pcm, int threads = 0) {
    pcm.assign((size_t)si.total * 2, 0);
    auto run = [&](size_t p, size_t end, uint64_t frameNo, uint64_t have, uint64_t& haveOut) -> std::string {
        laes::V ch[2];
        while (p < end) { ch[0].clear(); ch[1].clear(); size_t used = 0;
            const std::string e = laesflac::decodeFrame(a + p, end - p, si, frameNo, have, ch, used); if (!e.empty()) return e;
            const size_t k = ch[0].size(); if (have + k > si.total) return "more samples than STREAMINFO gives";
            int16_t* o = pcm.data() + (size_t)have * 2; for (size_t i = 0; i < k; i++) { o[i * 2] = (int16_t)ch[0][i]; o[i * 2 + 1] = (int16_t)ch[1][i]; }
            have += k; p += used; frameNo++; }
        haveOut = have; return ""; };
    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency()); threads = std::min(threads, 16);
    bool done = false;
    if (threads > 1 && si.minBs == si.maxBs && si.maxBs > 0 && n >= ((size_t)4 << 20)) {   // parts that start at a frame header
        std::vector<size_t> at{0}; std::vector<uint64_t> num{0}; bool okSplit = true; uint64_t k0 = 1;
        if (!laesflac::frameHeaderAt(a, n, si, k0) || k0 != 0) okSplit = false;
        for (int t = 1; okSplit && t < threads; t++) { size_t p = std::max(at.back() + 1, n / (size_t)threads * (size_t)t); uint64_t k = 0; bool found = false;
            for (; p + 16 < n; p++) if (a[p] == 0xFF && (a[p + 1] & 0xFE) == 0xF8 && laesflac::frameHeaderAt(a + p, n - p, si, k) && k > num.back()) { found = true; break; }
            if (!found) break; at.push_back(p); num.push_back(k); }
        if (okSplit && at.size() > 1) { const size_t parts = at.size(); std::vector<std::string> err(parts); std::vector<uint64_t> endHave(parts, 0); std::vector<std::thread> pool; std::vector<char> thrown(parts, 0);
            auto job = [&](size_t s) { try { err[s] = run(at[s], s + 1 < parts ? at[s + 1] : n, num[s], num[s] * (uint64_t)si.maxBs, endHave[s]); } catch (...) { thrown[s] = 1; } };
            for (size_t s = 1; s < parts; s++) { try { pool.emplace_back(job, s); } catch (...) { thrown[s] = 1; } }
            job(0); for (auto& th : pool) th.join();
            done = true; for (size_t s = 0; s < parts; s++) { if (thrown[s] || !err[s].empty()) done = false;
                if (endHave[s] != (s + 1 < parts ? num[s + 1] * (uint64_t)si.maxBs : si.total)) done = false; } } }   // a part that did not end where the next begins: decode in order instead
    if (!done) { uint64_t have = 0; const std::string e = run(0, n, 0, 0, have); if (!e.empty()) return "FLAC CD layer: " + e;
        if (have != si.total) return "FLAC CD layer: sample count differs from STREAMINFO"; }
    return ""; }
// STREAMINFO's MD5 of the decoded CD layer (16-bit little-endian, interleaved). It must be present and right: an all-zero MD5 ("not
// given") fails too, as in the LAES FLAC form - the extension belongs to exactly this CD layer
inline bool md5ok(const std::vector<int16_t>& pcm, const laesflac::Info& si) {
    bool zero = true; for (int i = 0; i < 16; i++) zero = zero && si.md5[i] == 0; if (zero) return false;
    alignhash::Md5 m; uint8_t by[16384]; for (size_t i = 0; i < pcm.size();) { const size_t k = std::min(pcm.size() - i, sizeof by / 2);
        for (size_t j = 0; j < k; j++) { by[j * 2] = (uint8_t)pcm[i + j]; by[j * 2 + 1] = (uint8_t)((uint16_t)pcm[i + j] >> 8); } m.update(by, k * 2); i += k; }
    const std::string h = m.hex(); static const char* d = "0123456789abcdef"; for (int i = 0; i < 16; i++) if (h[(size_t)i * 2] != d[si.md5[i] >> 4] || h[(size_t)i * 2 + 1] != d[si.md5[i] & 15]) return false;
    return true; }
// a whole file through a reader: the CD layer and the extension, checked (frame CRCs, sample count, MD5). Returns "" or the reason;
// notLaey: the file is not an LAEY FLAC file at all (a plain FLAC, an LAES file, not FLAC).
inline std::string read(const ReadFn& rd, uint64_t size, int& rate, std::vector<int16_t>& pcm, std::vector<uint8_t>& lae, bool& notLaey) {
    Layout L; notLaey = true; std::string e = layout(rd, size, L); if (!e.empty()) return e;
    e = check(L, notLaey); if (!e.empty()) return e;
    if (L.audioLen > (size_t)-1 / 2) return "the file is too large for this decoder";
    lae.resize((size_t)L.laeLen); { size_t at = 0; for (const Piece& q : L.lae) { if (!rd(q.off, lae.data() + at, (size_t)q.len)) return "cannot read the file"; at += (size_t)q.len; } }
    std::vector<uint8_t> au((size_t)L.audioLen); const size_t step = (size_t)4 << 20;
    for (size_t at = 0; at < au.size(); at += step) if (!rd(L.audioOff + at, au.data() + at, std::min(step, au.size() - at))) return "cannot read the file";
    const laesflac::Info si = infoOf(L); e = decodeAudio(au.data(), au.size(), si, pcm); if (!e.empty()) return e;
    if (!md5ok(pcm, si)) return "FLAC CD layer: MD5 missing or wrong (the audio is not the one this file was written with): refusing to decode; the CD audio itself still plays";
    rate = L.rate; return ""; }
inline bool isFlacFile(const char* path) { FILE* f = fopen(path, "rb"); if (!f) return false; uint8_t h[4]; const bool y = fread(h, 1, 4, f) == 4 && !memcmp(h, "fLaC", 4); fclose(f); return y; }
inline std::string readFile(const char* path, int& rate, std::vector<int16_t>& pcm, std::vector<uint8_t>& lae, bool& notLaey) {
    notLaey = true; FILE* f = fopen(path, "rb"); if (!f) return "cannot read the file";
#ifdef _WIN32
    auto seekTo = [&](uint64_t p) { return _fseeki64(f, (long long)p, SEEK_SET) == 0; }; _fseeki64(f, 0, SEEK_END); const long long sz = _ftelli64(f);
#else
    auto seekTo = [&](uint64_t p) { return fseeko(f, (off_t)p, SEEK_SET) == 0; }; fseeko(f, 0, SEEK_END); const long long sz = (long long)ftello(f);
#endif
    std::string e = sz < 0 ? std::string("cannot read the file") : read([&](uint64_t off, void* buf, size_t k) { return seekTo(off) && fread(buf, 1, k, f) == k; }, (uint64_t)sz, rate, pcm, lae, notLaey);
    fclose(f); return e; }
}  // namespace laeyflac
