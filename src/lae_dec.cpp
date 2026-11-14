// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE standalone decoder (lossy tier: routes full, 16, cd24), STREAMING: memory does not grow with the track length.
// Reads a file written by lae_enc, WAV form or FLAC form (told apart by content): the audio is the CD layer; the "LAE " chunk (WAV) or
// the "LAE1" APPLICATION blocks (FLAC, laey_flac.h) hold the depth layer (L, R) and the frequency layer (M, S, one stream per octave).
// usage: lae_dec <in.wav | in.flac | in.lae> <out.wav> [full|16|cd24] [output rate]   full = 24-bit route, 16 = CD + frequency layer, cd24 = CD + depth (base rate);
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
#include "laey_flac.h"   // the FLAC form (2026-11-10)
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
// Exports used only by the encoder (leak compensation, limiter map): active only when the encoder compiles this decoder in.
#ifdef LAE_ENCODER_HOOKS
static const char* hookPath(const char* name) { return getenv(name); }
#else
static const char* hookPath(const char*) { return nullptr; }
#endif
#ifndef LAE_ENCODER_HOOKS   // the encoder's own decode check writes into its work folder: no question there
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
// Decision 21 (2026-11-15): an existing output is not replaced silently. On a console (input and output both a console) the person
// is asked twice and only two answers "yes" replace it; without a console it is refused unless --overwrite was given.
static bool onConsole() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(fileno(stdin)) && isatty(fileno(stdout));
#endif
}
static bool answeredYes() { char b[256]; if (!fgets(b, sizeof b, stdin)) return false; std::string s(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t k = 0; while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++; s = s.substr(k);
    for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32); return s == "yes"; }   // only the word yes, in any letter case
static bool mayWrite(const char* out, bool overwrite) {
    std::error_code ec; if (!std::filesystem::exists(std::filesystem::u8path(out), ec) || overwrite) return true;
    if (onConsole()) {
        printf("\"%s\" already exists. Replace it? Type yes to continue: \n\"%s\" %s", out, out, u8"已经存在。要替换吗？输入 yes 继续："); fflush(stdout);
        if (answeredYes()) { printf("Really replace \"%s\"? It cannot be undone. Type yes again: \n%s\"%s\" %s", out, u8"确定要替换 ", out, u8" 吗？替换后无法恢复。再输入一次 yes："); fflush(stdout);
            if (answeredYes()) return true; } }
    printf("\"%s\" already exists; it was not replaced (give --overwrite to replace it).\n\"%s\" %s\n", out, out, u8"已经存在，没有替换（加 --overwrite 才替换）。");
    return false; }
#endif

int main(int argc, char** argv) {
    lae_u8::init(argc, argv);   // UTF-8 arguments and file names on Windows (win_utf8.h)
    int packArg = 0;   // pack=<n> (anywhere after the output name): decode extension pack n instead of the main body; taken out of the arguments here
    for (int i = 3; i < argc; i++) if (!strncmp(argv[i], "pack=", 5)) { const char* v = argv[i] + 5; char* end = nullptr; const long k = strtol(v, &end, 10);
        if (!*v || *end || k < 0 || k > 8) { printf("pack=%s: give the pack's number (0 = the main body)\n", v); return 1; }
        packArg = (int)k; for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1]; argc--; i--; }
    bool overwrite = false;   // --overwrite (anywhere): an existing output is replaced without a question (decision 21)
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--overwrite")) { overwrite = true; for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1]; argc--; i--; }
    if (argc < 3) { printf("usage: lae_dec <in.wav | in.flac | in.lae> <out.wav> [full|16|cd24] [output rate] [pack=<n>] [--overwrite]\n"
                           "  --overwrite: replace an existing <out.wav> without asking (on a console it is asked twice; otherwise it is refused)\n"
                           "  %s\n", u8"--overwrite：不问就替换已有的 <out.wav>（在控制台上会问两次；不在控制台上就拒绝）"); return 1; }
#ifndef LAE_ENCODER_HOOKS
    if (!mayWrite(argv[2], overwrite)) return 1;
#else
    (void)overwrite;
#endif
    std::string mode = argc > 3 ? argv[3] : "full";
    if (!mdct::selftestOK()) { printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the format requires (see docs/BUILD.md); refusing to decode\n"); return 4; }
    wavio::Wav w; int flacRate = 0;   // FU11-9: the STREAMINFO rate of the FLAC form
    if (laeyflac::isFlacFile(argv[1])) {   // FLAC form (2026-11-10): the CD layer in the FLAC frames, the extension in "LAE1" APPLICATION blocks
        bool notLaey = true; const std::string fe = laeyflac::readFile(argv[1], w.rate, w.pcm16, w.lae, notLaey);
        if (!fe.empty()) { printf("%s\n", fe.c_str()); return notLaey ? 1 : 3; } w.bits = 16; w.ch = 2; flacRate = w.rate; }
    else if (!wavio::readCD(argv[1], w) || w.bits != 16 || w.ch != 2) { printf("not a 16-bit stereo WAV\n"); return 1; }
    Decoder D; int code = 0; int orate = argc > 4 ? (atoi(argv[4]) > 0 ? atoi(argv[4]) : -1) : 0;
    D.pack = packArg;   // extension pack (2026-11-10): 0 = the main body, as always
    std::string e = D.open(std::move(w.pcm16), std::move(w.lae), mode, orate, &code); if (!e.empty()) { printf("%s\n", e.c_str()); return code; }
    if (flacRate && flacRate != D.base) { printf("FLAC STREAMINFO gives %d Hz, the extension's base rate is %d Hz: refusing to decode; the CD audio itself still plays\n", flacRate, D.base); return 3; }   // FU11-9: spec/lossy 3.4
    D.noSeekMarks();   // decision 34 (F05-4): this tool never seeks, so no checkpoints are kept (memory no longer grows with the track)
    if (!hookPath("LEAKOUT") && !hookPath("CLIPMAP")) {
        // decision 10 (FU34-9 / FU30-2, spec/lossy 6.7): every entropy stream of the decoded pack must end exactly at its last byte, as the
        // player's one-time check requires; it is the same check (every stream decoded from start to end, route full at the master rate,
        // in parallel) before any output is written. The encoder's own decode runs (LEAKOUT, CLIPMAP) skip it: they decode its own new streams.
        Decoder chk; chk.pack = D.pack; int c2 = 0; std::string e2 = chk.openRef(D.cdOwn, D.laeOwn, "full", 0, &c2);
        if (!e2.empty()) { printf("%s\n", e2.c_str()); return c2 ? c2 : 3; }
        std::vector<std::vector<SavedMark>> unused; e2 = chk.checkAll(unused, chk.seekThreads);
        if (!e2.empty()) { printf("%s\n", e2.c_str()); return 3; } }
    if (!D.packs.empty() && !hookPath("LEAKOUT") && !hookPath("CLIPMAP")) {   // the file's extension packs; only the caller chooses one (pack=<n>)
        printf("extension packs: %zu (decoding pack %d)\n  pack 0: the main body%s%s%s\n", D.packs.size() + 1, D.pack, D.mainName.empty() ? "" : ", \"", D.mainName.c_str(), D.mainName.empty() ? "" : "\"");
        for (size_t k = 0; k < D.packs.size(); k++) printf("  pack %zu: %d Hz%s%s%s\n", k + 1, D.packs[k].rate, D.packs[k].name.empty() ? "" : ", \"", D.packs[k].name.c_str(), D.packs[k].name.empty() ? "" : "\""); }
    if (hookPath("LEAKOUT") && D.haveF) {   // leak compensation (encoder only): the decoded frequency layer brought down to the base rate (int32, 1/16 LSB24, L/R interleaved)
        FILE* fl = fopen(hookPath("LEAKOUT"), "wb"); if (!fl) { printf("cannot write %s\n", hookPath("LEAKOUT")); return 1; }   // FU21-3: the reason is said (as the Java LaeDec)
        long nlk = D.writeLeak(fl);
        fclose(fl); printf("leak written: %ld base-rate samples/ch\n", nlk); return 0; }
    if (!wavio::fits32((unsigned long long)D.Lo * 6, 0)) { printf("output would exceed the 4 GB limit of WAV: choose a lower output rate\n"); return 1; }
    FILE* fo = fopen(argv[2], "wb"); if (!fo) { printf("cannot write %s\n", argv[2]); return 1; }
    bool wok = true;   // F05-1 (2026-11-14): every write and the close are checked; on a failure (disk full) the output is removed
    { std::vector<uint8_t> hdr; wavio::hdr(hdr, D.wrate, 2, 24, (size_t)D.Lo * 6, 0); wok = fwrite(hdr.data(), 1, hdr.size(), fo) == hdr.size(); }
    bool wantMap = hookPath("CLIPMAP") && D.d == 0 && mode != "cd24";
    std::vector<uint8_t> buf; buf.reserve(1 << 16); std::vector<int32_t> clipPos[2], blk(2 * 4096);
    for (long got; wok && (got = D.read(blk.data(), 4096, wantMap ? clipPos : nullptr)) > 0;) {
        for (long k = 0; k < got * 2; k++) { int32_t v = blk[(size_t)k]; buf.push_back((uint8_t)v); buf.push_back((uint8_t)(v >> 8)); buf.push_back((uint8_t)(v >> 16)); }
        if (buf.size() >= 60000) { wok = fwrite(buf.data(), 1, buf.size(), fo) == buf.size(); buf.clear(); } }
    if (wok) wok = fwrite(buf.data(), 1, buf.size(), fo) == buf.size();
    if (wok && ((D.Lo * 6) & 1)) wok = fputc(0, fo) != EOF;
    if (fclose(fo) != 0) wok = false;
    if (!wok) { remove(argv[2]); printf("cannot write %s (disk full?): the output was removed\n", argv[2]); return 1; }
    if (g_streamErr) { remove(argv[2]); printf("corrupt stream (values out of range): refusing to decode\n"); return 3; }   // FU21-3: before the clip map, so no map is left beside a removed output (as the Java LaeDec)
    if (wantMap) { FILE* fm = fopen(hookPath("CLIPMAP"), "wb"); if (fm) { for (int c = 0; c < 2; c++) fwrite(clipPos[c].data(), 4, clipPos[c].size(), fm); fclose(fm); } }   // channel 0 first, as before
    printf("LAE decode OK: %s -> %s | %s, %d Hz %s, %ld samples/ch | mode %s (share by %s) %s, CD / core layer cut-off %d Hz | frequency segments used %d of %d | hard-limited samples %ld | format LAEY (extension LAE1)\n",
           argv[1], argv[2], mode == "16" ? "16-bit route" : (mode == "cd24" ? "CD / core layer + depth" : "24-bit route"), D.wrate, mode == "16" ? "16 bit (in 24-bit words)" : "24 bit", D.Lo, part::MODES[D.mi].name, D.wm == 2 ? "cells+energy" : (D.wm ? "energy" : "cells"), D.eq ? "equal" : "unequal", D.fc, D.nused, D.nseg, D.clip);
    return 0; }
