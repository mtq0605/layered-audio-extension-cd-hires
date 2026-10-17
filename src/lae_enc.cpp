// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAE standalone encoder (lossy tier). Every verified module is compiled into this one
// executable inside its own namespace, so the encoder runs exactly the code that the tests verified.
//   input : PCM WAV master, stereo, 16/24/32-bit integer, rate = 44.1k or 48k family x 2, 4 or 8
//   output: WAV whose audio is the CD layer (16 bit at 44.1/48 kHz, plays everywhere) + a "LAE " chunk with
//           the depth layer (L, R) and the frequency layer (M, S), each range-coded with partition levels
// usage: lae_enc <master.wav> <out.wav> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2 (scheme), default s1] [zs] [cd=<CD.wav>] [energy] [equal]
//   mode = share of the scanned D (and C) content kept at its tier (see the MODES table in partition_format.h); energy = measure the
//   share by energy instead of cell count; equal = halving cuts only. The frequency layer is stored per octave (independent streams).
// The encoder decodes every layer again and refuses to write the file unless the decoded coefficient planes are identical.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <complex>
#include <map>
#include <chrono>
#include <string>
#include <functional>
#include <type_traits>
#include <memory>
#include <filesystem>
#ifndef _WIN32
#include <cstdlib>   // setenv / unsetenv
#endif
#include "halfband.h"
#include "part_stream.h"
#include "partition.h"   // encoder search (global, like part_stream.h, so every module shares ::part)
#include "wavio.h"
#include "fft.h"
#include "mdct_tb.h"   // global, shared by the lossy and decoder modules (both namespaces use ::mdct)
namespace m_pad {
#include "pad_mirror.cpp"
}
namespace m_base {
#include "base_layer.cpp"
}
namespace m_routes {
#include "routes.cpp"
}
namespace m_lossy {
#include "lossy_v2.cpp"
}
namespace m_part {
#include "part_coder.cpp"
}
#define LAE_ENCODER_HOOKS   // the encoder reads the decoder's leak and limiter exports
namespace m_dec {
#include "lae_dec.cpp"
}

static int call(int (*fn)(int, char**), std::vector<std::string> a) {
    std::vector<char*> v; for (auto& s : a) v.push_back(&s[0]); v.push_back(nullptr); return fn((int)a.size(), v.data()); }
// sets a variable for the next module; an empty value removes it (modules test getenv() != nullptr)
#ifdef _WIN32
static void env(const char* k, const std::string& v) { _putenv_s(k, v.c_str()); }
#else
static void env(const char* k, const std::string& v) { if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1); }
#endif
static std::vector<uint8_t> slurp(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); std::vector<uint8_t> b; if (!f) return b; fseek(f, 0, SEEK_END); b.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(b.data(), 1, b.size(), f); fclose(f); return b; }

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: lae_enc <master.wav> <out.wav> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2, default s1] [zs] [cd=<independent CD.wav>] [energy] [equal]\n"); return 1; }
    auto T0 = std::chrono::steady_clock::now();
    const char* modeName = argc > 3 ? argv[3] : "3"; int mi = part::modeIndex(modeName); if (mi < 0) { printf("mode must be max, 1, 3, 5, keep or keep20\n"); return 1; }
    bool equal = false; int wm = 0; for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "equal")) equal = true; if (!strcmp(argv[i], "energy")) wm = 1; }
    if (mi >= part::MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
    const char* cdPath = nullptr; for (int i = 4; i < argc; i++) if (!strncmp(argv[i], "cd=", 3)) cdPath = argv[i] + 3;   // independent CD (already aligned)
    int scheme = 1; bool zsOpt = false; for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "s2")) scheme = 2; else if (!strcmp(argv[i], "s1")) scheme = 1; else if (!strcmp(argv[i], "zs")) zsOpt = true; }
    env("ZSIGN", zsOpt ? "1" : "");   // opt-in zero level with true signs (partition.h)
    env("RULE_F", scheme == 2 ? "2" : "1"); env("RULE_D", mi == part::MODE_KEEP20 ? "3" : "1");   // precision rules (partition.h); recorded in every stream header
    if (mi == part::MODE_KEEP20) printf("NOTE keep20: the depth layer is coded at 20 bit instead of 18 bit. The CD band gets about 13 dB closer to the master\n"
                                        "  (about -124 instead of -111 dBFS error), which is very unlikely to be audible at normal listening levels; the file grows by about\n"
                                        "  84 MB per hour of music (44.1 kHz family; about 94 MB for 48 kHz), i.e. about 9 minutes less music per CD-Extra disc.\n");   // as the layer module forces it (keep: cells AND energy; max: energy), so the header and the report say what was used
    wavio::Wav w; if (!wavio::read(argv[1], w) || w.ch != 2) { printf("need a stereo integer PCM WAV\n"); return 1; }
    const double base = fmod((double)w.rate, 44100.0) == 0 ? 44100.0 : 48000.0; int stages = (int)lround(log2(w.rate / base));
    if (fabs(base * (1 << stages) - w.rate) > 0.5 || stages < 1 || stages > 3) { printf("rate %d is not 2/4/8 x 44.1 or 48 kHz\n", w.rate); return 1; }
    std::string tmp = std::string(argv[2]) + ".tmp"; { std::error_code ec; std::filesystem::create_directories(tmp, ec); } auto T = [&](const std::string& n) { return (std::filesystem::path(tmp) / n).string(); };
    struct TmpGuard { std::string p; ~TmpGuard() { if (!getenv("KEEPTMP")) { std::error_code ec; std::filesystem::remove_all(p, ec); } } } tmpGuard{tmp};   // intermediates (hundreds of MB) never outlive the run
    const int P = 4096;   // mirror padding at the master rate (decoder trims it)
    { FILE* f = fopen(T("m0.raw").c_str(), "wb"); fwrite(w.s.data(), 4, w.s.size(), f); fclose(f); }
    if ((long)(w.s.size() / 2) < (long)w.rate) { printf("the master is shorter than 1 second: not supported\n"); return 1; }
    if (call(m_pad::main, {"pad", T("m0.raw"), T("m.raw"), std::to_string(P)})) { printf("padding failed\n"); return 2; }
    env("DITHERFLAGS", T("dither.flags")); env("BASE24", T("b24.s32"));
    if (call(m_base::main, {"base", T("m.raw"), std::to_string(w.rate), std::to_string((int)base), T("b16.s16")})) { printf("CD layer derivation failed\n"); return 2; } env("BASE24", "");
    if (cdPath) {   // independent CD: it becomes the CD layer bit for bit; the extension codes master - CD. No dither of ours to regenerate.
        wavio::Wav c; if (!wavio::read(cdPath, c) || c.bits != 16 || c.ch != 2 || c.rate != (int)base) { printf("independent CD must be a 16-bit stereo WAV at %d Hz\n", (int)base); return 1; }
        std::vector<uint8_t> b = slurp(T("b16.s16")); long nb0 = (long)b.size() / 4, pb0 = P >> stages, need = nb0 - 2 * pb0, have = (long)c.pcm16.size() / 2;
        if (have != need) { printf("independent CD has %ld samples, the master needs %ld at %d Hz: align it first (automatic alignment is not implemented yet)\n", have, need, (int)base); return 1; }
        int16_t* s = (int16_t*)b.data(); for (long i = 0; i < need * 2; i++) s[pb0 * 2 + i] = c.pcm16[i];
        { FILE* fo = fopen(T("b16.s16").c_str(), "wb"); fwrite(b.data(), 1, b.size(), fo); fclose(fo); }
        std::vector<uint8_t> df = slurp(T("dither.flags")); std::fill(df.begin(), df.end(), 0); { FILE* fo = fopen(T("dither.flags").c_str(), "wb"); fwrite(df.data(), 1, df.size(), fo); fclose(fo); }
        printf("independent CD: %s (%ld samples) used as the CD layer\n", cdPath, have); }
    env("CDEXT", cdPath ? "1" : "");   // cut-off detection by coherence for an independent CD (routes.cpp)
    env("CUTOFFOUT", T("cutoff.txt"));
    if (call(m_routes::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), T("F.s32")})) { printf("route split failed\n"); return 2; } env("CUTOFFOUT", "");
    double fc = atof((const char*)slurp(T("cutoff.txt")).data());
    env("PART", modeName); env("PARTEQ", equal ? "1" : ""); env("PARTW", wm ? "1" : "0");
    // one layer = one encoder run (possibly several octave segments) + a self-check of every segment's stream
    bool ok = true;
    auto layer = [&](const char* in, int rate, const char* ch, const std::string& tag) {
        env("CHSEL", ch); env("DUMP", T(tag + ".lae")); env("PLANE", T(tag + ".plane")); env("SEGCOUNT", T(tag + ".seg"));
        if (call(m_lossy::main, {"lossy", T(in), std::to_string(rate)})) ok = false; env("SEGCOUNT", "");
        int nseg = atoi((const char*)slurp(T(tag + ".seg")).data()); std::vector<std::vector<uint8_t>> out;
        for (int i = 0; i < nseg; i++) { std::string suf = nseg > 1 ? "." + std::to_string(i) : "";
            env("PARTBYTES", T(tag + ".bin" + suf));
            if (call(m_part::main, {"part", T(tag + ".lae" + suf), T(tag + ".plane" + suf)})) ok = false;
            env("PARTBYTES", ""); out.push_back(slurp(T(tag + ".bin" + suf))); }
        return out; };
    env("FULLBAND", "1"); env("MASTERREF", T("b24.s32")); env("DEPTHADAPT", "1");
    auto dL = layer("T.s32", (int)base, "L", "dL"), dR = layer("T.s32", (int)base, "R", "dR");
    env("FULLBAND", ""); env("MASTERREF", ""); env("DEPTHADAPT", "");
    char kf[32]; snprintf(kf, sizeof kf, "%.6f", (fc - 1000.0) / (base / 2)); env("K0FRAC", kf); env("OCTSPLIT", "1");
    auto fM = layer("F.s32", w.rate, "M", "fM"), fS = layer("F.s32", w.rate, "S", "fS"); env("K0FRAC", ""); env("OCTSPLIT", "");
    if (!ok || fM.size() != fS.size()) { printf("ENCODER SELF-CHECK FAILED: a decoded layer differs from the encoder's plane; nothing written\n"); return 2; }
    // CD layer: padded base trimmed by P >> stages at each end; the trimmed edges go into the LAE chunk (the decoder needs them)
    std::vector<uint8_t> b16 = slurp(T("b16.s16")); long nb = (long)b16.size() / 4, pb = P >> stages;
    std::vector<int16_t> cd((nb - 2 * pb) * 2), edge(4 * pb); const int16_t* s16 = (const int16_t*)b16.data();
    for (long i = 0; i < nb - 2 * pb; i++) { cd[i * 2] = s16[(i + pb) * 2]; cd[i * 2 + 1] = s16[(i + pb) * 2 + 1]; }
    for (long i = 0; i < 2 * pb; i++) { edge[i] = s16[i]; edge[2 * pb + i] = s16[(nb - pb) * 2 + i]; }
    std::vector<uint8_t> dfl = slurp(T("dither.flags"));
    // LAE chunk v5 (v5 = v4 + precision rule per stream, reconstruction offset 0.20, 6 modes; v4 = v3 + 16-bit backstop): header, base edges, dither flags, depth L, depth R, number of frequency segments, M segments, S segments
    auto writeFile = [&]() { std::vector<uint8_t> lae; auto put32 = [&](int32_t v) { lae.insert(lae.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        auto putb = [&](const void* p, size_t n) { lae.insert(lae.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        auto putS = [&](const std::vector<uint8_t>& s) { put32((int32_t)s.size()); putb(s.data(), s.size()); };
        put32(0x3145414C); put32(5); put32(w.rate); put32((int32_t)base); put32(stages); put32((int32_t)(w.s.size() / 2)); put32(P);
        put32((int32_t)fc); put32(mi); put32(equal ? 1 : 0); put32(wm); put32((int32_t)nb); put32((int32_t)pb);
        putb(edge.data(), edge.size() * 2); put32((int32_t)dfl.size()); putb(dfl.data(), dfl.size());
        putS(dL[0]); putS(dR[0]); put32((int32_t)fM.size()); for (auto& s : fM) putS(s); for (auto& s : fS) putS(s);
        put32((int32_t)crc32(lae.data(), lae.size()));   // v3: CRC-32 of everything before it (integrity check, whole chunk)
        return wavio::writeCD(argv[2], (int)base, cd, lae) ? lae.size() : (size_t)0; };
    size_t chunk = writeFile(); if (!chunk) { printf("cannot write %s\n", argv[2]); return 1; }
    long nclip = 0;
    {   // leak compensation (v5, always on): decode the final frequency layer, let the depth layer cancel its leak into the CD band
        env("LEAKOUT", T("leak.s32")); call(m_dec::main, {"dec", argv[2], T("check.wav"), "full"}); env("LEAKOUT", "");
        char fcs[32]; snprintf(fcs, sizeof fcs, "%.0f", fc); env("LEAKIN", T("leak.s32"));
        call(m_routes::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), T("F2.s32"), fcs}); env("LEAKIN", "");
        env("FULLBAND", "1"); env("MASTERREF", T("b24.s32")); env("DEPTHADAPT", "1");
        dL = layer("T.s32", (int)base, "L", "dL"); dR = layer("T.s32", (int)base, "R", "dR");
        env("FULLBAND", ""); env("MASTERREF", ""); env("DEPTHADAPT", "");
        if (!ok) { printf("ENCODER SELF-CHECK FAILED after leak compensation; nothing written\n"); remove(argv[2]); return 2; }
        chunk = writeFile(); printf("leak compensation: depth layer re-encoded against the decoded frequency layer\n"); }
    // no clip control: an overshoot beyond full scale is bounded by the coding error (16-bit backstop; peak error
    // <= -76.6 dBFS measured on hard-clipped +12 dB masters) and the decoder's hard limiter returns it to the master's
    // own full-scale value. One decode reports how many samples the limiter touches (diagnostic only).
    { env("CLIPMAP", T("clip.bin")); call(m_dec::main, {"dec", argv[2], T("check.wav"), "full"}); env("CLIPMAP", "");
      nclip = (long)slurp(T("clip.bin")).size() / 4; printf("samples beyond full scale before the decoder's hard limiter: %ld\n", nclip); }
    remove(T("check.wav").c_str());
    size_t sd = dL[0].size() + dR[0].size(), sf = 0; for (auto& s : fM) sf += s.size(); for (auto& s : fS) sf += s.size();
    printf("LAE encode OK: %s | CD %d Hz cut-off %.0f Hz | scheme %d, mode %s (share by %s) %s | CD PCM %zu B, depth %zu B, frequency %zu B in %zu octave segment(s), chunk %zu B | beyond full scale before limiter %ld | %.1f s\n",
           argv[2], (int)base, fc, scheme, part::MODES[mi].name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), equal ? "equal" : "unequal", cd.size() * 2, sd, sf, fM.size(), chunk, nclip,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count());
    return 0;
}
