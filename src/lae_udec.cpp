// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_udec: one decoder for both formats, LAEY (lossy tier, extension LAE1) and LAES (lossless, extension LAE2), 2026-11-10.
// The format is recognised by the file's content, never by its name: an LAEY file and an LAES file may both be called .lae, .wav or
// .flac. Everything is done through the lae_any_* functions of lae_api.h (lae_any.cpp + lae_api.cpp + laes_api.cpp); lae_dec and laes
// stay as they are and give the same samples.
//   lae_udec <file>                     the same as info
//   lae_udec info <file>                the format, every choice the file offers, and the checks of that format
//   lae_udec dec <file> <out.wav> [--pick=<n> | --pick=<name> | --pick=<rate>:<bits> | --pick=master:<k> | --pick=name:<text> | --pick=pack:<k>] [--verify-all] [--threads=<n>]
//                                       decodes one choice (default: LAEY the route full at the master rate, LAES the default entry the file names, else the core master)
//   lae_udec verify <file> [--pick=..]  every check of the format, nothing written (LAES: every master's SHA-256 in one run; LAEY:
//                                       every extension pack decoded to its end on the route full at its master rate, one line per pack)
// An existing output is replaced only after two answers "yes" on a console, or with --overwrite (decision 21, 2026-11-15).
// A choice: LAEY cd | cd24 (core | core24 in a 48 kHz file) | 16_<rate> | full_<rate> (also 16, full = at the master rate); LAES core | master_<k>_<rate>_<bits> |
// master_<k>_level_<rate>_<bits> | derived16_<rate>_from_<name>. <rate>:<bits> picks LAEY the fullest route at that rate and bit depth (16 = route 16, which has no depth layer); LAES (SPEC 10.3): <rate>:<bits>,
// master:<k> (k = the number shown) and name:<text> (the publisher's name, exactly) fit masters themselves only; when several masters fit,
// a tick list is shown (number, rate, word length, name; numbers and ranges can be typed below it), outside a console they are
// listed and nothing is written. Several ticked: each goes to its own file, <out name>_<choice name><out suffix>.
// Output: WAV, 16 / 24 / 32-bit integer PCM (WAVE_FORMAT_EXTENSIBLE above 16 bits; RF64 beyond 4 GB). It is written as <out>.part and
// renamed after the last check passed; when a check fails nothing is left.
// The player's seek record (lae_api.h, lae_seek) is not used by this tool: LAE_INDEX_DIR is set to "off" unless it is already set,
// so an LAEY file's entropy streams are checked at every run and nothing is written outside the output file.
// Exit code: 0 done and every check passed; 1 usage, or the file has no LAE extension; 2 a check failed; 3 the file cannot be
// read or decoded; 4 this build fails the floating-point self-test.
// Build (Windows): cl /O2 /EHsc /std:c++17 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DLAE_STATIC /bigobj
//   src\lae_udec.cpp src\lae_any.cpp src\lae_api.cpp src\laes_api.cpp /Fe:lae_udec.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src\win_longpath.manifest
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include "lae_api.h"
#include "laes_pick.h"   // the tick list for several masters that fit one --pick
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
bool stderrIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}
std::string optOf(int argc, char** argv, const char* key) { const std::string k = std::string(key) + "="; for (int i = 1; i < argc; i++) if (!strncmp(argv[i], k.c_str(), k.size())) return argv[i] + k.size(); return ""; }
bool flagOf(int argc, char** argv, const char* key) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], key)) return true; return false; }
// Decision 21 (2026-11-15): an existing output is not replaced silently. On a console (input and output both a console, as the tick
// list) the person is asked twice and only two answers "yes" replace it; without a console it is refused unless --overwrite was given.
bool answeredYes() { char b[256]; if (!fgets(b, sizeof b, stdin)) return false; std::string s(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t k = 0; while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++; return s.substr(k) == "yes"; }
bool mayWrite(const std::string& out, bool overwrite) {
    std::error_code ec; if (!fs::exists(fs::u8path(out), ec) || overwrite) return true; const char* o = out.c_str();
    if (laespick::interactive()) {
        printf("\"%s\" already exists. Replace it? Type yes to continue: \n\"%s\" %s", o, o, u8"已经存在。要替换吗？输入 yes 继续："); fflush(stdout);
        if (answeredYes()) { printf("Really replace \"%s\"? It cannot be undone. Type yes again: \n%s\"%s\" %s", o, u8"确定要替换 ", o, u8" 吗？替换后无法恢复。再输入一次 yes："); fflush(stdout);
            if (answeredYes()) return true; } }
    printf("\"%s\" already exists; it was not replaced (give --overwrite to replace it).\n\"%s\" %s\n", o, o, u8"已经存在，没有替换（加 --overwrite 才替换）。");
    return false; }
const char* resultText(int r) { return r == 1 ? "passed" : r == 0 ? "FAILED" : "  --  "; }

void printFormat(const lae_probe_info& pi, const char* path, bool damaged) {
    const std::string suffix = fs::u8path(path).extension().u8string();
    const char* cont = pi.container == LAE_CONTAINER_FLAC ? "carried in a FLAC file" : pi.container == LAE_CONTAINER_WAV ? "carried in a WAV file" : "neither a WAV nor a FLAC file";
    printf("file: %s\n", path);
    if (pi.format == LAE_FORMAT_LAEY) printf("format: LAEY (lossy tier, extension LAE1)  %s\n", u8"\u683c\u5f0f\uff1aLAEY\uff08\u6709\u635f\u6863\uff0c\u6269\u5c55 LAE1\uff09");
    else if (pi.format == LAE_FORMAT_LAES) printf("format: LAES (lossless, extension LAE2)  %s\n", u8"\u683c\u5f0f\uff1aLAES\uff08\u65e0\u635f\uff0c\u6269\u5c55 LAE2\uff09");
    else if (damaged) printf("format: cannot be recognised (the file cannot be read or its structure is damaged)\n");
    else printf("format: no LAE extension (%s)\n", pi.container == LAE_CONTAINER_NONE ? "not a WAV or FLAC file" : "a plain WAV / FLAC file");
    printf("recognised by content (the name's suffix \"%s\" decides nothing; %s)  %s\n", suffix.c_str(), cont, u8"\u6309\u5185\u5bb9\u8bc6\u522b\uff0c\u4e0d\u770b\u6587\u4ef6\u540d"); }

// the bit depth shown for a choice: route 16 (no depth layer) is 16-bit; its samples come in 24-bit words, which is only the carrier
static int shownBits(const lae_any_choice& c) { return c.route == LAE_ROUTE_16 ? 16 : c.bits; }

void printChoices(const lae_any* a, const lae_any_info& in, int picked) {
    printf("choices (* = default%s):\n", picked >= 0 ? ", > = picked" : "");
    for (int i = 0; i < in.choices; i++) { lae_any_choice c; if (lae_any_get_choice(a, i, &c)) break;
        printf(" %c%c%2d  %-34s %6d Hz %2d-bit  %12llu frames  %s\n", i == in.default_choice ? '*' : ' ', i == picked ? '>' : ' ', i + 1, c.name, c.rate, shownBits(c), (unsigned long long)c.frames, c.what); } }

// returns the number of failed checks; open = false: every check, true: only those already made
int printChecks(const lae_any* a, const lae_any_info& in, const char* when) {
    int failed = 0; printf("checks of %s (%s):\n", lae_format_name(in.format), when);
    for (int i = 0; i < in.checks; i++) { lae_any_check c; if (lae_any_get_check(a, i, &c)) break; if (c.result == 0) failed++;
        printf("  [%s] %-26s %s%s%s\n", resultText(c.result), c.method, c.object, c.detail[0] ? " | " : "", c.detail); }
    return failed; }

// the publisher's name of an LAES master, from the list text 'master <n> "<name>" (lossless)' ("" when it has none)
std::string masterNameOf(const lae_any_choice& c) { const std::string w = c.what; const size_t q = w.find('"'); const std::string tail = "\" (lossless)";
    if (q == std::string::npos || w.size() < q + 1 + tail.size() || w.compare(w.size() - tail.size(), tail.size(), tail)) return ""; return w.substr(q + 1, w.size() - tail.size() - q - 1); }
// the choice, -1 when nothing fits, -2 when several masters fit (LAES: the same rate and word length, or the same name; they are in `several`)
int pickChoice(const lae_any* a, const lae_any_info& in, const std::string& pk, std::vector<int>& several) {
    if (pk.empty()) return in.default_choice;
    { const int byName = lae_any_find_choice(a, pk.c_str()); if (byName >= 0) return byName; }   // a name first: LAEY "16" is the route, not choice 16
    if (pk.find_first_not_of("0123456789") == std::string::npos) { const int n = atoi(pk.c_str()); return n >= 1 && n <= in.choices ? n - 1 : -1; }
    const size_t col = pk.find(':');
    if (in.format == LAE_FORMAT_LAEY && pk.rfind("pack:", 0) == 0) {   // pack:<k> = that extension pack on the route full at its master rate (its last choice)
        const std::string arg = pk.substr(5); if (arg.empty() || arg.find_first_not_of("0123456789") != std::string::npos) return -1; const int k = atoi(arg.c_str()); int hit = -1;
        for (int i = 0; i < in.choices; i++) { lae_any_choice c; lae_any_get_choice(a, i, &c); if (lae_any_choice_pack(a, i) == k && c.route == LAE_ROUTE_FULL) hit = i; }
        return hit; }
    if (in.format == LAE_FORMAT_LAES && col != std::string::npos) {   // SPEC 10.3: master:<k>, name:<text> and <rate>:<bits> fit masters themselves only
        const bool byNo = pk.rfind("master:", 0) == 0, byText = pk.rfind("name:", 0) == 0, byFmt = pk.find_first_not_of("0123456789:") == std::string::npos;
        if (!byNo && !byText && !byFmt) return -1;
        const std::string arg = pk.substr(col + 1); const int r = atoi(pk.c_str()), b = atoi(arg.c_str()); std::vector<int> hit;
        for (int i = 0; i < in.choices; i++) { lae_any_choice c; lae_any_get_choice(a, i, &c); if (c.laes_kind != LAES_KIND_MASTER) continue;
            const bool fits = byNo ? std::string(c.name).rfind("master_" + arg + "_", 0) == 0 && arg.find_first_not_of("0123456789") == std::string::npos && !arg.empty()
                            : byText ? !arg.empty() && masterNameOf(c) == arg : c.rate == r && c.bits == b;
            if (fits) hit.push_back(i); }
        if (hit.size() > 1) { several = hit; return -2; }
        return hit.empty() ? -1 : hit[0]; }
    if (col != std::string::npos && pk.find_first_not_of("0123456789:") == std::string::npos) { const int r = atoi(pk.c_str()), b = atoi(pk.c_str() + col + 1); int hit = -1;
        for (int i = 0; i < in.choices; i++) { lae_any_choice c; lae_any_get_choice(a, i, &c); if (lae_any_choice_pack(a, i) <= 0 && c.rate == r && shownBits(c) == b) hit = i; }   // LAEY: the fullest route at that rate and bit depth (16 = route 16), in the main body (pack 0) only; another pack only by pack:<k> or its name (SPEC 5.6)
        return hit; }
    return -1; }


std::vector<uint8_t> wavHeader(int rate, int bits, uint64_t frames) {
    std::vector<uint8_t> h; const int by = bits / 8; const uint64_t db = frames * 2 * (uint64_t)by; const uint32_t fl = bits > 16 ? 40 : 16; const uint64_t riff = 4 + 8 + fl + 8 + db + (db & 1);
    auto u16 = [&](uint32_t v) { h.push_back((uint8_t)v); h.push_back((uint8_t)(v >> 8)); }; auto u32 = [&](uint32_t v) { for (int q = 0; q < 4; q++) h.push_back((uint8_t)(v >> (8 * q))); };
    auto u64 = [&](uint64_t v) { for (int q = 0; q < 8; q++) h.push_back((uint8_t)(v >> (8 * q))); }; const bool big = riff > 0xFFFFFFFFull;
    if (big) { h.insert(h.end(), {'R', 'F', '6', '4'}); u32(0xFFFFFFFFu); h.insert(h.end(), {'W', 'A', 'V', 'E', 'd', 's', '6', '4'}); u32(28); u64(riff + 36); u64(db); u64(frames); u32(0); }
    else { h.insert(h.end(), {'R', 'I', 'F', 'F'}); u32((uint32_t)riff); h.insert(h.end(), {'W', 'A', 'V', 'E'}); }
    h.insert(h.end(), {'f', 'm', 't', ' '}); u32(fl); u16(fl == 40 ? 0xFFFE : 1); u16(2); u32((uint32_t)rate); u32((uint32_t)rate * 2 * (uint32_t)by); u16(2 * (uint32_t)by); u16((uint32_t)bits);
    if (fl == 40) { u16(22); u16((uint32_t)bits); u32(3); const uint8_t g[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}; h.insert(h.end(), g, g + 16); }
    h.insert(h.end(), {'d', 'a', 't', 'a'}); u32(big ? 0xFFFFFFFFu : (uint32_t)db); return h; }

void usage() {
    printf("lae_udec: one decoder for LAEY (extension LAE1) and LAES (extension LAE2); the format is recognised by content\n"
           "  lae_udec info <file>\n"
           "  lae_udec dec <file> <out.wav> [--pick=<n> | --pick=<name> | --pick=<rate>:<bits> | --pick=master:<k> | --pick=name:<text> | --pick=pack:<k>] [--verify-all] [--threads=<n>] [--overwrite]\n"
           "  lae_udec verify <file> [--pick=..]\n"
           "  --overwrite: replace an existing output without asking (on a console it is asked twice; otherwise it is refused)\n"
           "  %s\n", u8"--overwrite：不问就替换已有的输出文件（在控制台上会问两次；不在控制台上就拒绝）"); }
}

int main(int argc, char** argv) {
    lae_u8::init(argc, argv);
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1]; int at = 2;
    if (cmd != "info" && cmd != "dec" && cmd != "verify") { cmd = "info"; at = 1; }   // lae_udec <file>
    std::vector<const char*> pos; for (int i = at; i < argc; i++) if (strncmp(argv[i], "--", 2)) pos.push_back(argv[i]);
    if (pos.empty() || (cmd == "dec" && pos.size() < 2)) { usage(); return 1; }
    const char* in = pos[0];
#ifdef _WIN32
    if (!getenv("LAE_INDEX_DIR")) _putenv_s("LAE_INDEX_DIR", "off");   // no player's record: every stream is checked at this run
#else
    setenv("LAE_INDEX_DIR", "off", 0);
#endif
    if (lae_selftest() != 1) { printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the formats require (see docs/BUILD.md); refusing to decode\n"); return 4; }

    lae_probe_info pi; char err[512] = {0};
    lae_any* a = lae_any_open_file(in, &pi, err, (int)sizeof err);
    printFormat(pi, in, !a && pi.format == LAE_FORMAT_NONE && strncmp(err, "no LAE", 6) != 0);
    if (!a) { printf("%s\n", err); return pi.format == LAE_FORMAT_NONE && !strncmp(err, "no LAE", 6) ? 1 : 3; }
    lae_any_info info; lae_any_get_info(a, &info);
    printf("%s\n", info.identity);
    if (!info.usable) printf("the extension failed a check: only the core master is offered\n");

    if (cmd == "info") { printChoices(a, info, -1); const int failed = printChecks(a, info, "made at open; -- = made by decoding a choice to its end"); lae_any_close(a); return failed ? 2 : 0; }

    std::string pk = optOf(argc, argv, "--pick"); if (pk.empty() && cmd == "dec" && pos.size() > 2) pk = pos[2];   // lae_udec dec in out full
    std::vector<int> several; const int choice = pickChoice(a, info, pk, several);
    if (choice == -1) { printf("--pick: '%s' is not one of the choices\n", pk.c_str()); printChoices(a, info, -1); lae_any_close(a); return 1; }
    // several masters fit (the same rate and word length, or the same name): a tick list, each line with the list number,
    // rate, word length and name (laes_pick.h, the list `laes dec` shows); numbers and ranges can be typed below it. Outside a console
    // the candidates are listed and nothing is written. Every ticked one is decoded; with more than one, each goes to its own file,
    // <out name>_<choice name><out suffix>.
    std::vector<int> todo; if (choice >= 0) todo.push_back(choice);
    else { std::vector<laespick::Item> items; for (int i : several) { lae_any_choice c; lae_any_get_choice(a, i, &c); char lb[48]; snprintf(lb, sizeof lb, "%6d Hz %2d-bit  ", c.rate, c.bits);
               std::string w = c.what; const std::string tail = " (lossless)"; if (w.size() > tail.size() && !w.compare(w.size() - tail.size(), tail.size(), tail)) w.resize(w.size() - tail.size());
               items.push_back({ (long)i + 1, lb + w }); }
        char tl[160]; snprintf(tl, sizeof tl, "--pick: '%.60s' fits %d masters. Tick the ones to decode:", pk.c_str(), (int)several.size());
        if (!laespick::interactive()) { printf("%s\n", tl); for (const auto& x : items) printf("  %3ld  %s\n", x.no, x.line.c_str());
            printf("not in a console: nothing written. Give the numbers: --pick=<n>\n"); lae_any_close(a); return 1; }
        const std::vector<long> got = laespick::tick(tl, items, "e.g. " + std::to_string(items.front().no) + "," + std::to_string(items.back().no) + " or " + std::to_string(items.front().no) + "-" + std::to_string(items.back().no));
        for (long n : got) for (int i : several) if ((long)i + 1 == n) todo.push_back(i);
        if (todo.empty()) { printf("nothing ticked: nothing written\n"); lae_any_close(a); return 1; } }
    const bool all = cmd == "verify" || flagOf(argc, argv, "--verify-all"); const int threads = atoi(optOf(argc, argv, "--threads").c_str());
    const bool overwrite = flagOf(argc, argv, "--overwrite");
    // decision 3 (2026-11-15, F06-5 (2)): LAEY verify decodes every extension pack completely (the main body and each further pack, route
    // full at its master rate, to its last frame), one result line per pack; a pack that failed its check at open is a failed line.
    // The pack's checks at open are the list entries "every entropy stream of the extension" (pack 0) and "... of pack <k> ..." (pack k).
    const bool packLines = cmd == "verify" && info.format == LAE_FORMAT_LAEY && info.usable; std::vector<std::string> packLabel{"pack 0 (the main body)"};
    int worst = 0, packsFailed = 0;
    if (packLines) {
        for (int i = 0; i < info.checks; i++) { lae_any_check c; if (lae_any_get_check(a, i, &c)) break; const std::string ob = c.object, lead = "every entropy stream of ";
            if (ob.rfind(lead + "pack ", 0) != 0) continue; packLabel.push_back(ob.substr(lead.size()));
            if (c.result == 0) { printf("%s: FAILED (not decoded: its entropy streams failed the check at open)\n", packLabel.back().c_str()); packsFailed++; worst = 2; } }
        for (int k = 0; k < (int)packLabel.size(); k++) { int hit = -1;   // the pack's route full at its master rate: its last full choice
            for (int i = 0; i < info.choices; i++) { lae_any_choice c; lae_any_get_choice(a, i, &c); if (lae_any_choice_pack(a, i) == k && c.route == LAE_ROUTE_FULL) hit = i; }
            if (hit >= 0 && std::find(todo.begin(), todo.end(), hit) == todo.end()) todo.push_back(hit); } }
    for (const int one : todo) {
    lae_any_choice C; lae_any_get_choice(a, one, &C);
    std::string out = cmd == "dec" ? pos[1] : "";
    if (cmd == "dec" && todo.size() > 1) { const fs::path p = fs::u8path(out); out = (p.parent_path() / fs::u8path(p.stem().u8string() + "_" + C.name + p.extension().u8string())).u8string(); }
    if (cmd == "dec" && !mayWrite(out, overwrite)) { printf("nothing written for %s\n", C.name); if (worst < 1) worst = 1; continue; }   // decision 21
    if (lae_any_start(a, one, all ? LAE_ANY_VERIFY_ALL : 0, threads, err, (int)sizeof err)) { printf("cannot start the choice: %s\n", err); lae_any_close(a); return 3; }
    if (!packLines || one == todo.front()) printChoices(a, info, one);

    const std::string part = out + ".part"; FILE* fo = nullptr; const int by = C.bits / 8;
    if (cmd == "dec") { std::error_code ec;
        if (fs::exists(fs::u8path(part), ec)) { printf("%s exists and was not made by this run: remove it or choose another name; nothing written\n", part.c_str()); lae_any_close(a); return 3; }
        fo = fopen(part.c_str(), "wb"); if (!fo) { printf("cannot write %s\n", part.c_str()); lae_any_close(a); return 3; }
        const std::vector<uint8_t> h = wavHeader(C.rate, C.bits, C.frames); fwrite(h.data(), 1, h.size(), fo); }
    std::vector<int32_t> blk(2 * 16384); std::vector<uint8_t> bytes; uint64_t got = 0; bool wrote = true; int shown = -1; const bool tty = stderrIsTty();
    for (int64_t n; (n = lae_any_read(a, blk.data(), 16384)) > 0;) { got += (uint64_t)n;
        if (fo) { bytes.resize((size_t)n * 2 * (size_t)by); size_t q = 0;
            for (int64_t i = 0; i < n * 2; i++) { const uint32_t v = (uint32_t)blk[(size_t)i]; for (int k = 0; k < by; k++) bytes[q++] = (uint8_t)(v >> (8 * k)); }
            wrote = wrote && fwrite(bytes.data(), 1, bytes.size(), fo) == bytes.size(); }
        if (tty && C.frames) { const int pc = (int)(got * 100 / C.frames); if (pc / 5 != shown) { shown = pc / 5; fprintf(stderr, "\r%3d%%", pc); fflush(stderr); } } }
    if (tty) fprintf(stderr, "\r     \r");
    if (fo) { if ((C.frames * 2 * (uint64_t)by) & 1) fputc(0, fo); wrote = wrote && fflush(fo) == 0; wrote = fclose(fo) == 0 && wrote; }

    const int failed = printChecks(a, info, info.format == LAE_FORMAT_LAES ? (all ? "every master in this run" : "those this choice needs") : "all");
    const int ver = lae_any_verified(a); const std::string problem = lae_any_error(a); int code = 0;
    if (got != C.frames || ver != 1 || failed) { code = failed || ver < 0 ? 2 : 3; if (!problem.empty()) printf("stopped: %s\n", problem.c_str()); if (got != C.frames) { printf("decoded %llu of %llu frames\n", (unsigned long long)got, (unsigned long long)C.frames); if (code == 2 && !failed) code = 3; } }
    if (cmd == "dec") { std::error_code ec;
        if (code == 0 && !wrote) { printf("cannot write %s (disk full?)\n", part.c_str()); code = 3; }
        if (code == 0) { fs::rename(fs::u8path(part), fs::u8path(out), ec); if (ec) { printf("cannot rename %s to %s\n", part.c_str(), out.c_str()); code = 3; } }
        if (code != 0) { fs::remove(fs::u8path(part), ec); printf("nothing written%s\n", failed ? " (a check failed; the file's own audio is still read by any WAV / FLAC tool)" : ""); } }
    if (code == 0) { if (cmd == "dec") printf("%s decode OK: %s -> %s | %s, %d Hz %d-bit%s, %llu frames/ch | every check passed\n", lae_format_name(info.format), in, out.c_str(), C.name, C.rate, shownBits(C), C.route == LAE_ROUTE_16 ? " (in 24-bit words)" : "", (unsigned long long)got);
        else if (!packLines) printf("%s verify OK: %s | %s decoded to its end, every check passed\n", lae_format_name(info.format), in, C.name); }
    else if (cmd == "verify" && !packLines) printf("%s verify FAILED: %s\n", lae_format_name(info.format), in);
    if (packLines) { const int k = lae_any_choice_pack(a, one); if (code) packsFailed++;
        printf("%s: %s decoded to its end, %llu frames: %s\n", k >= 0 && k < (int)packLabel.size() ? packLabel[(size_t)k].c_str() : "pack ?", C.name, (unsigned long long)got, code ? "FAILED" : "passed"); }
    if (code > worst) worst = code; }
    if (packLines) { if (worst == 0) printf("LAEY verify OK: %s | every extension pack decoded to its end (%zu pack%s), every check passed\n", in, packLabel.size(), packLabel.size() == 1 ? "" : "s");
        else printf("LAEY verify FAILED: %s | %d result line%s FAILED\n", in, packsFailed, packsFailed == 1 ? "" : "s"); }
    lae_any_close(a); return worst; }
