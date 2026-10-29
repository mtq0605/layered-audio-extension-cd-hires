// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE standalone decoder (lossy tier: routes full, 16, cd24), STREAMING: memory does not grow with the track length.
// Reads a WAV written by lae_enc: the audio is the CD layer, the "LAE " chunk holds the depth layer (L, R) and the frequency layer
// (M, S, one stream per octave).
// usage: lae_dec <in.wav> <out.wav> [full|16|cd24] [output rate]   full = 24-bit route, 16 = CD + frequency layer, cd24 = CD + depth (base rate);
//        output rate: master rate (default) or any octave below it; frequency segments above that octave are skipped, not decoded
// The decoding itself is in lae_decoder.h (deterministic, streaming).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "wavio.h"
#include "lae_decoder.h"
// Exports used only by the encoder (leak compensation, limiter map): active only when the encoder compiles this decoder in.
#ifdef LAE_ENCODER_HOOKS
static const char* hookPath(const char* name) { return getenv(name); }
#else
static const char* hookPath(const char*) { return nullptr; }
#endif

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: lae_dec <in.wav> <out.wav> [full|16|cd24] [output rate]\n"); return 1; }
    std::string mode = argc > 3 ? argv[3] : "full";
    if (!mdct::selftestOK()) { printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the format requires (see docs/BUILD.md); refusing to decode\n"); return 4; }
    wavio::Wav w; if (!wavio::read(argv[1], w) || w.bits != 16 || w.ch != 2) { printf("not a 16-bit stereo WAV\n"); return 1; }
    Decoder D; int code = 0; int orate = argc > 4 ? (atoi(argv[4]) > 0 ? atoi(argv[4]) : -1) : 0;
    std::string e = D.open(std::move(w.pcm16), std::move(w.lae), mode, orate, &code); if (!e.empty()) { printf("%s\n", e.c_str()); return code; }
    if (hookPath("LEAKOUT") && D.haveF) {   // leak compensation (encoder only): the decoded frequency layer brought down to the base rate (int32, 1/16 LSB24, L/R interleaved)
        FILE* fl = fopen(hookPath("LEAKOUT"), "wb"); if (!fl) return 1; long nlk = D.writeLeak(fl);
        fclose(fl); printf("leak written: %ld base-rate samples/ch\n", nlk); return 0; }
    if (!wavio::fits32((unsigned long long)D.Lo * 6, 0)) { printf("output would exceed the 4 GB limit of WAV: choose a lower output rate\n"); return 1; }
    FILE* fo = fopen(argv[2], "wb"); if (!fo) { printf("cannot write %s\n", argv[2]); return 1; }
    { std::vector<uint8_t> hdr; wavio::hdr(hdr, D.wrate, 2, 24, (size_t)D.Lo * 6, 0); fwrite(hdr.data(), 1, hdr.size(), fo); }
    bool wantMap = hookPath("CLIPMAP") && D.d == 0 && mode != "cd24";
    std::vector<uint8_t> buf; buf.reserve(1 << 16); std::vector<int32_t> clipPos[2], blk(2 * 4096);
    for (long got; (got = D.read(blk.data(), 4096, wantMap ? clipPos : nullptr)) > 0;) {
        for (long k = 0; k < got * 2; k++) { int32_t v = blk[(size_t)k]; buf.push_back((uint8_t)v); buf.push_back((uint8_t)(v >> 8)); buf.push_back((uint8_t)(v >> 16)); }
        if (buf.size() >= 60000) { fwrite(buf.data(), 1, buf.size(), fo); buf.clear(); } }
    fwrite(buf.data(), 1, buf.size(), fo); if ((D.Lo * 6) & 1) fputc(0, fo); fclose(fo);
    if (wantMap) { FILE* fm = fopen(hookPath("CLIPMAP"), "wb"); if (fm) { for (int c = 0; c < 2; c++) fwrite(clipPos[c].data(), 4, clipPos[c].size(), fm); fclose(fm); } }   // channel 0 first, as before
    if (g_streamErr) { remove(argv[2]); printf("corrupt stream (values out of range): refusing to decode\n"); return 3; }
    printf("LAE decode OK: %s -> %s | %s, %d Hz 24 bit, %ld samples/ch | mode %s (share by %s) %s, CD cut-off %d Hz | frequency segments used %d of %d | hard-limited samples %ld | format LAEY (extension LAE1)\n",
           argv[1], argv[2], mode == "16" ? "16-bit route" : (mode == "cd24" ? "CD + depth" : "24-bit route"), D.wrate, D.Lo, part::MODES[D.mi].name, D.wm == 2 ? "cells+energy" : (D.wm ? "energy" : "cells"), D.eq ? "equal" : "unequal", D.fc, D.nused, D.nseg, D.clip);
    return 0; }
