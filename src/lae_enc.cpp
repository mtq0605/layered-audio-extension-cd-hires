// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAE standalone encoder (lossy tier). Every verified module is compiled into this one
// executable inside its own namespace, so the encoder runs exactly the code that the tests verified.
//   input : PCM WAV master, stereo, 16/24/32-bit integer, rate = 44.1k or 48k family x 2, 4 or 8
//   output: WAV whose audio is the CD layer (16 bit at 44.1/48 kHz, plays everywhere) + a "LAE " chunk with
//           the depth layer (L, R) and the frequency layer (M, S), each range-coded with partition levels
// usage: lae_enc <master.wav> <out.wav> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2 (scheme), default s1] [zs] [cd=<CD.wav>] [energy] [equal]
//                [cdcheck] [cdalign=<offset>] [cdaccept] [bits=24] [mem=<GB>]
//        lae_enc plan <master.wav> [<master2.wav> ...] <publisher.cue> <outdir> [cdaccept]   the CD's split markers onto the master (cd_plan.cpp)
//   mode = share of the scanned D (and C) content kept at its tier (see the MODES table in partition_format.h); energy = measure the
//   share by energy instead of cell count; equal = halving cuts only. The frequency layer is stored per octave (independent streams).
//   cd=<CD.wav>: independent CD. The position is always confirmed by the user (2026-10-23): without cdalign= the
//   encoder searches where the CD lies in the master by waveform twice, whitened and plain (cd_align.cpp), measures each result's fraction
//   of a master sample, tries each like a hand-given offset (placement, check in three blocks, check again) and reports both - aligned at
//   cdalign=<offset>, not aligned, or alignment failed - then stops: nothing is encoded. With cdalign=<offset> (master samples, > 0: the
//   CD starts later; whole or with a fraction, 3.4999828 = whole offset 3 and the master moved by the fraction) the master is placed on the CD's time line by
//   whole master samples (only across digital silence), checked again, and the remaining time difference decides:
//   <= -40 dB encodes, anything larger stops and asks (cdaccept continues; a console asks); a CD that does not fit is never encoded.
//   cdcheck = the same without encoding (rc 0). cdaccept = continue after a remaining difference beyond -40 dB.
//   bits=24: a 32-bit master is encoded from its top 24 bits (without it the encoder asks on a console, or stops otherwise).
//   mem=<GB>: memory budget for coding two layers at once (default: half the physical memory); above it they are coded one after the
//   other. The output file does not depend on it.
// The encoder decodes every layer again and refuses to write the file unless the decoded coefficient planes are identical.
// Fail-safe: every file operation is checked, every module result is checked, the output is first written as <out>.part, read back
// and verified, decoded end to end, and only then renamed to <out>. On any failure, including an internal error, an out-of-memory
// condition in a worker thread, an exit() inside a module or Ctrl+C, nothing is written and the temporary folder is removed.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <csignal>
#include <exception>
#include <vector>
#include <algorithm>
#include <complex>
#include <map>
#include <chrono>
#include <string>
#include <functional>
#include <type_traits>
#include <memory>
#include <new>
#include <thread>
#include <atomic>
#include <climits>
#include <filesystem>
#ifdef _WIN32
#include <io.h>      // _isatty
extern "C" __declspec(dllimport) void* __stdcall GetStdHandle(unsigned long);   // kernel32, declared here to keep <windows.h> macros out of the modules
extern "C" __declspec(dllimport) int __stdcall GetConsoleMode(void*, unsigned long*);
struct LaeMemStatus { unsigned long len, load; unsigned long long totalPhys, availPhys, totalPage, availPage, totalVirt, availVirt, availExt; };   // MEMORYSTATUSEX
extern "C" __declspec(dllimport) int __stdcall GlobalMemoryStatusEx(LaeMemStatus*);
#else
#include <cstdlib>   // setenv / unsetenv
#include <unistd.h>  // isatty, _exit
#endif
#include "enc_io.h"      // checked I/O, shared by every module (global, like part_stream.h)
#include "align_hash.h"
#include "lae_cdx_read.h"   // the publisher's CUE for the independent-CD planner (cd_plan.cpp)
#include "halfband.h"
#include "part_stream.h"
#include "partition.h"   // encoder search (global, like part_stream.h, so every module shares ::part)
#include "wavio.h"
#include "fft.h"
#include "mdct_tb.h"   // global, shared by the lossy and decoder modules (both namespaces use ::mdct)
#include "frac_delay.h"   // global: the v6 phase delay, shared by the route split, the alignment and the decoder (::lae_fd)
// ---- per-thread module settings and output: two layers (L and R, M and S) are coded at once, each in its own thread. A layer thread
// keeps its own copy of the variables a layer sets (env() below) and its own printed text, which is shown after the pair in the usual
// order, so the log reads as if the layers ran one after the other. Outside a layer thread both go to the process as before.
namespace encenv {
static thread_local std::map<std::string, std::string>* over = nullptr;
static thread_local std::string* out = nullptr;
static const char* get(const char* k) { if (over) { auto it = over->find(k); if (it != over->end()) return it->second.empty() ? nullptr : it->second.c_str(); } return std::getenv(k); }
static int print(const char* f, ...) { va_list a; va_start(a, f); int n;
    if (out) { va_list b; va_copy(b, a); n = vsnprintf(nullptr, 0, f, b); va_end(b); if (n > 0) { std::string s((size_t)n + 1, '\0'); vsnprintf(&s[0], s.size(), f, a); s.resize((size_t)n); *out += s; } }
    else n = vprintf(f, a);
    va_end(a); return n; }
}
#define LAE_LAYER_MODULE_IO static const char* getenv(const char* k) { return encenv::get(k); } \
    template <class... A> static int printf(const char* f, A... a) { return encenv::print(f, a...); }
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
LAE_LAYER_MODULE_IO
#include "lossy_v2.cpp"
}
namespace m_part {
LAE_LAYER_MODULE_IO
#include "part_coder.cpp"
}
namespace m_align {
#include "cd_align.cpp"
}
#define LAE_ENCODER_HOOKS   // the encoder reads the decoder's leak and limiter exports
namespace m_dec {
#include "lae_dec.cpp"
}

// ---- run state for the emergency cleanup (terminate, exit() inside a module, Ctrl+C): what this run created and must remove
namespace encrun {
static std::string out, part, tmp; static bool hadOld = false, partMine = false, tmpMine = false, cleaned = false, finished = false;
static void closeAll() {
#ifdef _WIN32
    _fcloseall();   // files a failed module left open would block their deletion on Windows
#endif
}
static void cleanup() { if (cleaned) return; cleaned = true; closeAll(); std::error_code ec;   // the .part only while it is unfinished; the work folder always
    if (partMine) std::filesystem::remove(part, ec); if (tmpMine && !getenv("KEEPTMP")) std::filesystem::remove_all(tmp, ec); }
static void oldNote() { if (hadOld) printf("note: the existing %s was NOT written by this run\n", out.c_str()); }
static void emergency(const char* why) { if (finished) return; finished = true; printf("ENCODER STOPPED: %s; nothing written\n", why); oldNote(); fflush(stdout); cleanup(); }
static void onTerminate() { emergency("internal error or out of memory (in a worker thread)"); fflush(stdout); _exit(2); }
static void onExit() { if (!finished) emergency("a module ended the program"); }
static void onSignal(int) { emergency("interrupted"); fflush(stdout); _exit(130); }
}

// runs one module; a checked-I/O failure, an exhausted memory or any other exception inside it becomes a non-zero result with the reason printed
static int call(int (*fn)(int, char**), std::vector<std::string> a) {
    std::vector<char*> v; for (auto& s : a) v.push_back(&s[0]); v.push_back(nullptr);
    try { return fn((int)a.size(), v.data()); }   // the messages go to the layer's own text when the module runs in a layer thread
    catch (const encio::Fail& e) { encenv::print("ENCODER STOPPED in module %s: %s\n", a[0].c_str(), e.what()); return 90; }
    catch (const std::bad_alloc&) { encenv::print("ENCODER STOPPED in module %s: out of memory\n", a[0].c_str()); return 91; }
    catch (const std::exception& e) { encenv::print("ENCODER STOPPED in module %s: %s\n", a[0].c_str(), e.what()); return 92; }
    catch (...) { encenv::print("ENCODER STOPPED in module %s: internal error\n", a[0].c_str()); return 93; } }
// sets a variable for the next module; an empty value removes it (modules test getenv() != nullptr)
#ifdef _WIN32
static void env(const char* k, const std::string& v) { if (encenv::over) (*encenv::over)[k] = v; else _putenv_s(k, v.c_str()); }
static bool interactive() {   // a person at a console on both ends: input from the console and output to it (not a pipe, a file or NUL)
    unsigned long m; return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout)) && GetConsoleMode(GetStdHandle((unsigned long)-10), &m) && GetConsoleMode(GetStdHandle((unsigned long)-11), &m); }
#else
static void env(const char* k, const std::string& v) { if (encenv::over) (*encenv::over)[k] = v; else if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1); }
static bool interactive() { return isatty(fileno(stdin)) && isatty(fileno(stdout)); }
#endif
// physical memory: total and available in bytes (0 if unknown: the layers then run one after the other)
#ifdef _WIN32
static void physMemory(unsigned long long& total, unsigned long long& avail) { LaeMemStatus m{}; m.len = sizeof m; if (GlobalMemoryStatusEx(&m)) { total = m.totalPhys; avail = m.availPhys; } else total = avail = 0; }
#else
static void physMemory(unsigned long long& total, unsigned long long& avail) {   // Linux: MemTotal / MemAvailable (page cache counted); other systems: unknown
    total = avail = 0; FILE* f = fopen("/proc/meminfo", "r"); if (!f) return; char line[256];
    while (fgets(line, sizeof line, f)) { unsigned long long kb = 0;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = kb * 1024; else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = kb * 1024; }
    fclose(f); }
#endif
// module variables come from this program only: anything inherited from the calling environment is removed (the encoder and the planner)
static void clearModuleEnv() {
    for (const char* k : {"ZSIGN", "RULE_F", "RULE_D", "DITHERFLAGS", "BASE24", "CDEXT", "CUTOFFOUT", "PART", "PARTEQ", "PARTW", "CHSEL", "DUMP", "PLANE", "SEGCOUNT",
                          "PARTBYTES", "FULLBAND", "MASTERREF", "DEPTHADAPT", "K0FRAC", "OCTSPLIT", "LEAKIN", "LEAKOUT", "CLIPMAP"}) if (getenv(k)) env(k, ""); }
static std::vector<uint8_t> slurp(const std::string& p) { return encio::readAll<uint8_t>(p.c_str()); }   // a missing or unreadable file stops the encoder
static bool exists(const std::string& p) { std::error_code ec; return std::filesystem::exists(p, ec); }
static long long fileSize(const std::string& p) { std::error_code ec; auto n = std::filesystem::file_size(p, ec); return ec ? -1 : (long long)n; }
// number of samples of a 32-bit PCM WAV whose lowest byte is not zero (what reducing it to 24 bit would drop); -1 if unreadable
static long long low8Count(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return -1; long long n = encio::size(f); if (n < 12) { fclose(f); return -1; }
    std::vector<uint8_t> b; try { b.resize((size_t)n); } catch (...) { fclose(f); return -1; }
    bool ok = fread(b.data(), 1, b.size(), f) == b.size(); fclose(f); if (!ok) return -1;
    size_t p = 12; long long cnt = 0;
    while (p + 8 <= b.size()) { uint32_t len = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24; size_t body = p + 8; if (body + len > b.size()) len = (uint32_t)(b.size() - body);
        if (!memcmp(&b[p], "data", 4)) { for (size_t i = body; i + 4 <= body + len; i += 4) if (b[i]) cnt++; return cnt; }
        p = body + len + (len & 1); }
    return -1; }

namespace m_plan {
#include "cd_plan.cpp"
}

static int encode(int argc, char** argv) {
    if (argc < 3) { printf("usage: lae_enc <master.wav> <out.wav> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2, default s1] [zs] [cd=<independent CD.wav>] [energy] [equal] [cdcheck] [cdalign=<offset>] [cdaccept] [bits=24] [mem=<GB>]\n"); return 1; }
    auto T0 = std::chrono::steady_clock::now();
    const std::string outPath = argv[2], partPath = outPath + ".part", tmp = outPath + ".tmp";
    encrun::out = outPath; encrun::part = partPath; encrun::tmp = tmp; encrun::hadOld = exists(outPath);
    auto oldNote = []() { encrun::oldNote(); };
    if (!mdct::selftestOK() || !alignhash::md5SelfTest() || encio::testFail("selftest")) { printf("encoder self-test FAILED: this build does not compute the IMDCT in the IEEE-754 order the format requires, or its MD5 is wrong (see docs/BUILD.md); nothing encoded\n"); return 4; }
    const char* modeName = argc > 3 ? argv[3] : "3"; int mi = part::modeIndex(modeName); if (mi < 0) { printf("mode must be max, 1, 3, 5, keep or keep20\n"); return 1; }
    bool equal = false, zsOpt = false, cdCheck = false, cdAccept = false, bits24 = false, manual = false; int wm = 0, scheme = 1; const char* cdPath = nullptr; long long manualT = 0; unsigned long long memBudget = 0;   // mem=<GB>: memory budget for coding two layers at once (default: half the physical memory)
    std::string manualText; int manualSg = 1; long long manualI = 0, manualF = 0, manualFs = 1, manualPlace = 0, manualPhi = 0;   // cdalign=<o>: o = sg * (I + F / Fs) master samples
    double manualMove = 0;   // the fraction (master samples, |f| <= 1/2) the master is moved by when it is placed at manualT
    for (int i = 4; i < argc; i++) { const char* a = argv[i];
        if (!strcmp(a, "equal")) equal = true; else if (!strcmp(a, "energy")) wm = 1; else if (!strcmp(a, "s2")) scheme = 2; else if (!strcmp(a, "s1")) scheme = 1;
        else if (!strcmp(a, "zs")) zsOpt = true; else if (!strncmp(a, "cd=", 3) && a[3]) cdPath = a + 3; else if (!strcmp(a, "cdcheck")) cdCheck = true;
        else if (!strcmp(a, "cdaccept")) cdAccept = true; else if (!strcmp(a, "bits=24")) bits24 = true;
        else if (!strncmp(a, "mem=", 4)) { const double g = atof(a + 4); if (!(g > 0) || g > 1e6) { printf("mem=%s: give the memory budget in GB (for example mem=8)\n", a + 4); return 1; } memBudget = (unsigned long long)(g * 1073741824.0); }
        else if (!strcmp(a, "cdalign=none")) { printf("cdalign=none was removed (2026-10-22): every CD is checked. A small remaining difference can be accepted with cdaccept, a known offset given with cdalign=<offset>; nothing encoded\n"); return 1; }
        else if (!strncmp(a, "cdalign=", 8)) {   // master samples, whole or with a fraction (v6): digits, optionally '.' and up to 9 digits
            const char* v = a + 8; const char* p = v; int sg = 1; if (*p == '-' || *p == '+') { sg = *p == '-' ? -1 : 1; p++; }
            long long I = 0, F = 0, Fs = 1; int ni = 0, nf = 0; bool ok = true;
            while (*p >= '0' && *p <= '9' && ni < 11) { I = I * 10 + (*p++ - '0'); ni++; }
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9' && nf < 9) { F = F * 10 + (*p++ - '0'); Fs *= 10; nf++; } ok = nf > 0; }
            if (!ni || !ok || *p || I > 2000000000LL) { printf(strchr(v, ',') ? "cdalign=%s: write the fraction with a point (for example cdalign=3.5)\n"
                                                                              : "cdalign=%s: give the offset in master samples, a whole number or with a fraction of up to 9 digits (for example cdalign=-3 or cdalign=3.4999828)\n", v); return 1; }
            manual = true; manualText = v; manualSg = sg; manualI = I; manualF = F; manualFs = Fs; }
        else { printf("unknown option '%s': nothing encoded (options: s1 s2 zs energy equal cd=<CD.wav> cdcheck cdalign=<offset> cdaccept bits=24 mem=<GB>)\n", a); return 1; } }
    if ((cdCheck || cdAccept || manual) && !cdPath) { printf("cdcheck, cdalign and cdaccept need cd=<CD.wav>\n"); return 1; }
    if (mi >= part::MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
    if (exists(partPath)) { printf("%s already exists and was not created by this run: remove it or choose another output name; nothing encoded\n", partPath.c_str()); return 1; }
    const std::string marker = (std::filesystem::path(tmp) / "lae_enc.tmp").string();
    if (exists(tmp) && !exists(marker)) { printf("%s already exists and is not an lae_enc work folder: remove it or choose another output name; nothing encoded\n", tmp.c_str()); return 1; }
    clearModuleEnv();   // opt-in diagnostics excepted
    env("ZSIGN", zsOpt ? "1" : "");   // opt-in zero level with true signs (partition.h)
    env("RULE_F", scheme == 2 ? "2" : "1"); env("RULE_D", mi == part::MODE_KEEP20 ? "3" : "1");   // precision rules (partition.h); recorded in every stream header
    if (mi == part::MODE_KEEP20 && !cdCheck) printf("NOTE keep20: the depth layer is coded at 20 bit instead of 18 bit. The CD band gets about 13 dB closer to the master\n"
                                        "  (about -124 instead of -111 dBFS error), which is very unlikely to be audible at normal listening levels; the file grows by about\n"
                                        "  84 MB per hour of music (44.1 kHz family; about 94 MB for 48 kHz), i.e. about 9 minutes less music per CD-Extra disc.\n");   // as the layer module forces it (keep: cells AND energy; max: energy), so the header and the report say what was used
    wavio::Wav w; if (!wavio::read(argv[1], w) || w.ch != 2) { printf("need a stereo integer PCM WAV\n"); oldNote(); return 1; }
    std::string note32;
    if (w.bits == 32) {   // LAE v5 carries at most 24 bit: say what reducing the master would drop, then end or continue as the user chooses
        long long lost = low8Count(argv[1]);
        if (lost == 0) note32 = "input 32 bit with 24-bit content (nothing dropped)";
        else {
            printf("NOTE: the master is a 32-bit file. LAE v5 carries at most 24 bit, so the lowest 8 bits would be dropped (%s).\n",
                   lost > 0 ? (std::to_string(lost) + " samples carry information there").c_str() : "their content could not be checked");
            if (!bits24) {
                if (!interactive()) { printf("  nothing encoded. To encode with 24-bit output, add the option bits=24.\n"); oldNote(); return 1; }
                printf("  1 = end the task (nothing is written)\n  2 = encode with 24-bit output (the top 24 bits are kept)\n  choose 1 or 2: "); fflush(stdout);
                char line[32] = {0}; if (!fgets(line, sizeof line, stdin) || line[0] != '2') { printf("task ended: nothing encoded\n"); oldNote(); return 1; } }
            note32 = "input 32 bit, encoded as 24 bit"; } }
    const double base = fmod((double)w.rate, 44100.0) == 0 ? 44100.0 : 48000.0; int stages = (int)lround(log2(w.rate / base));
    if (fabs(base * (1 << stages) - w.rate) > 0.5 || stages < 1 || stages > 3) { printf("rate %d is not 2/4/8 x 44.1 or 48 kHz\n", w.rate); oldNote(); return 1; }
    if (manual) {   // cdalign=<o>: parsed as o = manualPlace - manualPhi / U (U units per master sample), then the nearest whole offset and the fraction
        const long long Ux = lae_fd::unitsPerMaster(stages);
        if (manualF == 0) { manualPlace = manualSg * manualI; manualPhi = 0; }
        else { const long long rest = manualSg > 0 ? manualFs - manualF : manualF; manualPlace = manualSg > 0 ? manualI + 1 : -manualI; manualPhi = (rest * Ux + manualFs / 2) / manualFs;
               if (manualPhi >= Ux) { manualPlace--; manualPhi = 0; } }
        manualT = manualPhi * 2 > Ux ? manualPlace - 1 : manualPlace;   // the whole offset nearest to o: where the first check runs and the master is placed
        manualMove = (double)(manualT - manualPlace) + (double)manualPhi / (double)Ux; }   // and the fraction it is moved by: o = manualT - manualMove
    const int P = 4096;   // mirror padding at the master rate (decoder trims it)
    long long Lm = (long long)(w.s.size() / 2);   // changes only when the master is placed on an independent CD's time line
    auto lengthOk = [&](const char* what) {   // length limits: the v5 header (decoder check: Lm <= 2,000,000,000 - 2P) and, on builds with a 32-bit 'long', the index range of the modules
        if (Lm < (long long)w.rate) { printf("%s is shorter than 1 second: not supported\n", what); oldNote(); return false; }
        long long maxFrames = 2000000000LL - 2LL * P; const char* who = "the v5 format"; const char* hint = "";
        if (sizeof(long) < 8) { long long m32 = 0x7FFFFFFFLL / 8 - 2LL * P; if (m32 < maxFrames) { maxFrames = m32; who = "this encoder build (Windows)"; hint = " (longer tracks need the planned streaming encoder)"; } }
        if (const char* t = getenv("LAE_TESTMAXFRAMES")) { long long v = atoll(t); if (v > 0) maxFrames = std::min(maxFrames, v); }   // test hook: can only lower the limit
        if (Lm > maxFrames) { printf("%s is %.2f minutes long; at %d Hz %s allows at most %.2f minutes%s: nothing encoded\n", what, Lm / (60.0 * w.rate), w.rate, who, maxFrames / (60.0 * w.rate), hint); oldNote(); return false; }
        return true; };
    if (!lengthOk("the master")) return 1;
    auto fitsLength = [&](long long n) { long long mf = 2000000000LL - 2LL * P;   // the limits of lengthOk, without its messages
        if (sizeof(long) < 8) mf = std::min(mf, 0x7FFFFFFFLL / 8 - 2LL * P); if (const char* e = getenv("LAE_TESTMAXFRAMES")) { long long v = atoll(e); if (v > 0) mf = std::min(mf, v); }
        return n >= (long long)w.rate && n <= mf; };
    { std::error_code ec; bool made = std::filesystem::create_directories(tmp, ec); if (ec || !exists(tmp)) { printf("cannot create the work folder %s: nothing encoded\n", tmp.c_str()); oldNote(); return 2; }
      if (made || exists(marker)) encrun::tmpMine = true; encio::writeText(marker.c_str(), "lae_enc work folder (removed at the end of each run)\n"); }
    auto T = [&](const std::string& n) { return (std::filesystem::path(tmp) / n).string(); };
    struct Guard { ~Guard() { encrun::cleanup(); } } guard;   // removes the work folder always, the .part on failure
    auto spaceOk = [&](unsigned long long reuse = 0) {   // free space for the intermediates (about 65-85 bytes per padded master frame, measured; about 20 for cdcheck) + the output; reuse = work files about to be overwritten
        std::error_code ec; auto sp = std::filesystem::space(tmp, ec); const double perFrame = cdCheck ? 20.0 : 85.0; if (!ec) sp.available += reuse;
        unsigned long long needB = (unsigned long long)((Lm + 2 * P) * perFrame) + (64ull << 20), hardB = (unsigned long long)((Lm + 2 * P) * perFrame * 0.7);
        if ((!ec && sp.available < hardB) || encio::testFail("diskspace")) { printf("not enough free disk space next to %s: about %.0f MB needed, %.0f MB free: nothing encoded\n", outPath.c_str(), needB / 1e6, ec ? 0.0 : sp.available / 1e6); oldNote(); return false; }
        if (!ec && sp.available < needB) printf("WARNING: free disk space is tight (%.0f MB free, about %.0f MB needed); every write is checked, so a full disk stops the run cleanly\n", sp.available / 1e6, needB / 1e6);
        return true; };
    if (!spaceOk()) return 2;
    auto derive = [&]() {   // padded master and the derived CD layer (16 bit), 24-bit base and dither flags, all from w.s
        encio::writeVec(T("m0.raw").c_str(), w.s);
        if (call(m_pad::main, {"pad", T("m0.raw"), T("m.raw"), std::to_string(P)})) { printf("padding failed\n"); oldNote(); return false; }
        env("DITHERFLAGS", T("dither.flags")); env("BASE24", T("b24.s32"));
        if (call(m_base::main, {"base", T("m.raw"), std::to_string(w.rate), std::to_string((int)base), T("b16.s16")})) { printf("CD layer derivation failed\n"); oldNote(); return false; } env("BASE24", "");
        return true; };
    auto stamp = [&](const char* w) { if (getenv("TIMING")) printf("[enc %.1f s] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count(), w); };
    if (!derive()) return 2;
    stamp("padding + CD layer derived");
    // independent CD: the fraction of a master sample is measured in units of 2^-22 CD sample (U1 per master sample) and applied to the master
    // itself when it is placed (2026-10-23); the file stays v5 and the decoder is unchanged
    const int64_t U1 = lae_fd::unitsPerMaster(stages);
    std::string alignNote, alignReport;   // alignReport: the text of <out>.align.txt (the actual move), written only beside the finished file
    if (cdPath) {   // independent CD: it becomes the CD layer bit for bit; the extension codes master - CD. No dither of ours to regenerate.
        wavio::Wav c; if (!wavio::read(cdPath, c) || c.bits != 16 || c.ch != 2 || c.rate != (int)base) { printf("independent CD must be a 16-bit stereo WAV at %d Hz\n", (int)base); oldNote(); return 1; }
        std::vector<uint8_t> b = slurp(T("b16.s16")); long nb0 = (long)b.size() / 4, pb0 = P >> stages, need = nb0 - 2 * pb0, have = (long)c.pcm16.size() / 2;
        auto lengthMsg = [&]() { printf("independent CD has %ld samples, the master needs %ld at %d Hz: the lengths do not match; nothing encoded\n", have, need, (int)base); };
        {   // alignment (design D and A, 2026-10-22 / 10-23): with cdalign=<offset> the position is the user's: check in three blocks, place the
            // master where needed, check again, and the remaining time difference decides whether the encoder goes on, asks, or stops. Without it
            // (2026-10-23) the search runs whitened AND plain; each result goes through the same
            // phase measurement, placement and re-check as a hand-given offset would (nothing is encoded), both are reported, and the encoder stops:
            // the user gives the position by hand. The two results are never compared or chosen between
            using m_align::detail::fmt;
            std::vector<int32_t> b24 = encio::readAll<int32_t>(T("b24.s32").c_str());
            auto offText = [&](long long tp, double mv) { return fmt("%+.7f", (double)tp - mv); };   // o = whole offset - the master's move (7 decimals: finer than 1/524288)
            // one attempt at one position (the checks of the hand-given path). sch: "" by hand, else the search's name (output prefix, nothing kept).
            // kind: 0 aligned, 1 asks (-40..-33 dB or beyond), 2 stops (the reason in why), 3 fatal (derivation, disk); rep / placed: the final check
            struct Att { int kind = 2; std::string why; };
            m_align::Report rep; std::string placed; long padP = 0, cutL = 0, cutT = 0; long long rpT = 0; double rpMv = 0;   // the last attempt, for the report
            auto attempt = [&](const m_align::Report& rep1, const std::string& sch, long long tPlace, double mv) -> Att {
                Att A; const std::string pre = sch.empty() ? "align: " : "align (" + sch + "): ", key = sch.empty() ? "" : " search=" + sch;
                rep = rep1; placed.clear(); padP = cutL = cutT = 0; rpT = tPlace; rpMv = mv;
                if (!rep1.found) { A.why = rep1.why + (rep1.hint.empty() ? "" : "; " + rep1.hint); return A; }
                printf("PHASE t=%lld phi=0 move=%+.7f offset=%s%s\n", tPlace, mv, offText(tPlace, mv).c_str(), key.c_str());
                if (mv != 0) printf("%sthe master is placed at %+lld and moved by %+.7f master samples (its band-limited waveform taken %+.7f samples later: interpolated once, at alignment; the file stays v5)\n",
                                    pre.c_str(), tPlace, mv, mv);
                else printf("%sthe CD lies on a whole master sample: the master is not moved by a fraction\n", pre.c_str());
                const bool placeNeeded = tPlace != 0 || have != need;
                if (placeNeeded || mv != 0) {   // the CD's own samples are never changed: the master moves by whole master samples (a silent CD frame may go in front)
                    const long long t = tPlace; long nLead = 0, nTail = 0;
                    m_align::Place q = m_align::place(w.s, Lm, have, stages, t, w.rate);
                    auto placeLine = [&](const char* result) { printf("PLACE mode=%s t=%lld drop_lead=%lld drop_tail=%lld pad_lead=%lld pad_tail=%lld cd_zero_lead=%d cd_only_lead=%ld cd_only_tail=%ld lm=%lld lm2=%lld result=%s%s\n",
                                                                      sch.empty() ? "manual" : "auto", q.t, q.dropLead, q.dropTail, q.padLead, q.padTail, q.cdLead, nLead, nTail, q.Lm, q.Lm2, result, key.c_str()); };
                    for (auto& l : q.lines) printf("%s%s\n", pre.c_str(), l.c_str());
                    std::string bad = q.ok ? std::string() : q.why; const bool moved = q.ok && (q.shift != 0 || q.Lm2 != Lm);
                    if (bad.empty() && q.cdLead) { c.pcm16.insert(c.pcm16.begin(), 2 * (size_t)q.cdLead, (int16_t)0); have = (long)c.pcm16.size() / 2; }
                    if (bad.empty() && moved) {   // cheap stops first, before anything is resized, written or derived
                        const std::string far = m_align::cdOnlyFar(c.pcm16, have, q, stages, (int)base, pb0);
                        if (!far.empty()) bad = far; else if (!fitsLength(q.Lm2)) bad = fmt("the placed master (%lld frames) is outside this encoder's length limits", q.Lm2); }
                    if (!bad.empty()) { placeLine("stop"); A.why = bad; return A; }   // not possible here; no other offset is tried in its place
                    if (moved || mv != 0) {
                        unsigned long long reuse = 0; for (const char* f : {"m0.raw", "m.raw", "b16.s16", "b24.s32", "dither.flags"}) { long long s = fileSize(T(f)); if (s > 0) reuse += (unsigned long long)s; }   // derive() overwrites these
                        if (moved) { m_align::applyPlace(w.s, q); Lm = q.Lm2; }   // M' replaces the master; the CD layer, base and dither flags are derived again from it
                        if (mv != 0) {   // the fraction (2026-10-23): digital silence at both ends first, so that what the move carries past the master's first /
                            // last sample is kept; afterwards only what is still digital silence is removed again: at most the added frames, never a frame of
                            // the given CD. Nothing about it is stored anywhere; decoders never discard a sample.
                            const long P = 128; const long long up1 = 1LL << stages, Pm = (long long)P * up1;
                            c.pcm16.insert(c.pcm16.begin(), 2 * (size_t)P, (int16_t)0); c.pcm16.insert(c.pcm16.end(), 2 * (size_t)P, (int16_t)0); have = (long)c.pcm16.size() / 2;
                            { std::vector<int32_t> z((size_t)(Lm + 2 * Pm) * 2, 0); std::copy(w.s.begin(), w.s.begin() + (size_t)Lm * 2, z.begin() + (size_t)Pm * 2); w.s.swap(z); Lm += 2 * Pm; }
                            long nclip = 0; w.s = m_align::moveMaster(w.s, Lm, mv, nclip);   // then the fraction: the placed master's waveform taken mv later
                            if (nclip) printf("%sWARNING: %ld samples of the moved master exceed 24-bit full scale and are hard-clipped\n", pre.c_str(), nclip);
                            auto silent = [&](long long a) { for (long long i = a; i < a + up1; i++) if (w.s[(size_t)i * 2] || w.s[(size_t)i * 2 + 1]) return false; return true; };
                            long kL = 0, kT = 0; while (kL < P && silent((long long)kL * up1)) kL++; while (kT < P && silent(Lm - (long long)(kT + 1) * up1)) kT++;
                            c.pcm16.erase(c.pcm16.end() - 2 * (size_t)kT, c.pcm16.end()); c.pcm16.erase(c.pcm16.begin(), c.pcm16.begin() + 2 * (size_t)kL); have = (long)c.pcm16.size() / 2;
                            w.s.resize((size_t)(Lm - (long long)kT * up1) * 2); w.s.erase(w.s.begin(), w.s.begin() + (size_t)((long long)kL * up1) * 2); Lm -= (long long)(kL + kT) * up1;
                            padP = P; cutL = kL; cutT = kT;
                            printf("%sdigital silence: %ld CD frames added at each end before the move; removed again where still digital silence: start %ld, end %ld (kept: start %ld, end %ld)\n",
                                   pre.c_str(), P, kL, kT, P - kL, P - kT); }
                        if (!spaceOk(reuse) || !derive()) { A.kind = 3; return A; }
                        b = slurp(T("b16.s16")); nb0 = (long)b.size() / 4; need = nb0 - 2 * pb0; b24 = encio::readAll<int32_t>(T("b24.s32").c_str()); }
                    m_align::Place qc = q;   // the layout cdOnly sees: the frames of added silence that the move filled are zeros added to the master too
                    if (padP) { const long long up1 = 1LL << stages; qc.padLead += (long long)(padP - cutL) * up1; qc.padTail += (long long)(padP - cutT) * up1; qc.Lm2 = Lm; }
                    const std::string co = m_align::cdOnly(c.pcm16, have, b24, pb0, qc, stages, (int)base, nLead, nTail);
                    if (nLead || nTail) printf("%sCD frames that meet only zeros added to the master: start %ld, end %ld (%s)\n", pre.c_str(), nLead, nTail, co.empty() ? "digital silence where the derived base is zero" : "NOT digital silence");
                    if (placeNeeded || !co.empty()) placeLine(co.empty() ? "placed" : "stop");
                    if (!co.empty()) { A.why = co; return A; }
                    if (have != need) { A.why = fmt("the CD has %ld samples, the placed master needs %ld at %d Hz: the lengths do not match", have, need, (int)base); return A; }
                    const long long zero = 0;   // the placed (and moved) master, checked again at offset 0 (the same three blocks, through its own derivation)
                    m_align::Report rep2 = m_align::run(c.pcm16, have, b24, nb0, pb0, w.s, (long)Lm, (int)base, stages, t, "", 0, -1, &zero);
                    for (auto& l : rep2.lines) printf("%sre-check: %s\n", pre.c_str(), l.c_str());
                    printf("ALIGN_RECHECK%s%s\n", rep2.alignLine.substr(5).c_str(), key.c_str());
                    if (mv != 0 && rep2.hint.rfind("still shifted", 0) == 0)   // the suggestion keeps the fraction
                        rep2.hint = fmt("still shifted by %+ld master samples: try cdalign=%s", rep2.bestNear, offText(t + rep2.bestNear, mv).c_str());
                    if (!rep2.found) { A.why = fmt("the placement at %+lld master samples%s does not align: %s", t, mv != 0 ? fmt(" with the master moved by %+.7f", mv).c_str() : "", rep2.why.c_str()) + (rep2.hint.empty() ? "" : "; " + rep2.hint); return A; }
                    const std::string phs = mv != 0 ? fmt("moved by %+.7f master samples", mv) : std::string();
                    placed = placeNeeded ? fmt(" (master placed %s at %+lld master samples%s, then checked again%s)", sch.empty() ? "by hand" : ("by the " + sch + " search").c_str(), t, mv != 0 ? (", " + phs).c_str() : "", q.cdLead ? "; CD layer = one digital-silence frame + the given CD" : "")
                                         : fmt(" (master %s, then checked again)", phs.c_str());
                    rep = rep2; }
                if (rep.band == m_align::BAND_STOP) { A.why = rep.why + (rep.hint.empty() ? "" : "; " + rep.hint); return A; }
                A.kind = rep.band == m_align::BAND_OK ? 0 : 1; A.why = rep.why; return A; };
            if (!manual) {
                const std::vector<int32_t> w0 = w.s; const long long Lm0 = Lm; const std::vector<int16_t> cd0 = c.pcm16; const long have0 = have;
                std::string res[2], off[2]; const char* nm[2] = {"whitened", "plain"};
                for (int k = 0; k < 2; k++) {
                    if (k) { w.s = w0; Lm = Lm0; c.pcm16 = cd0; have = have0; if (!derive()) return 2;   // the second search starts from the given master again
                             b = slurp(T("b16.s16")); nb0 = (long)b.size() / 4; need = nb0 - 2 * pb0; b24 = encio::readAll<int32_t>(T("b24.s32").c_str()); }
                    m_align::Report r = m_align::run(c.pcm16, have, b24, nb0, pb0, w.s, (long)Lm, (int)base, stages, 0, "", 0, -1, nullptr, k == 0);
                    for (auto& l : r.lines) printf("align (%s): %s\n", nm[k], l.c_str()); printf("%s search=%s\n", r.alignLine.c_str(), nm[k]);
                    long long tP = r.placeAt; double mvv = 0;   // the whole offset, and the fraction the master is moved by (|mvv| <= 1/2)
                    if (r.found) { std::string phLine; const int64_t adv = m_align::measurePhase(c.pcm16, w.s, Lm, stages, w.rate, r.placeAt, r, phLine);
                        printf("align (%s): %s\n", nm[k], phLine.c_str()); mvv = (double)adv / (double)U1; }
                    const Att A = attempt(r, nm[k], tP, mvv); if (A.kind == 3) return 2;
                    off[k] = r.found ? offText(tP, mvv) : std::string("-");
                    res[k] = A.kind == 0 ? fmt("aligned at cdalign=%s%s", off[k].c_str(), placed.c_str())
                           : A.kind == 1 ? fmt("not aligned within the limit at cdalign=%s: %s", off[k].c_str(), A.why.c_str())
                           : r.found ? fmt("alignment failed at cdalign=%s: %s", off[k].c_str(), A.why.c_str()) : fmt("alignment failed: %s", A.why.c_str());
                    printf("align: %s search: %s\n", nm[k], res[k].c_str()); }
                auto st = [&](int k) { return res[k].rfind("aligned", 0) == 0 ? "aligned" : (res[k].rfind("not aligned", 0) == 0 ? "not_aligned" : "failed"); };
                printf("ALIGN_SUGGEST whitened=%s:%s plain=%s:%s\n", st(0), off[0].c_str(), st(1), off[1].c_str());
                printf("independent CD: %s; give the position by hand: cdalign=<offset> (whitened search: %s; plain search: %s)\n",
                       cdCheck ? "cdcheck: both searches checked, nothing encoded" : "nothing encoded", res[0].c_str(), res[1].c_str());
                if (cdCheck) return 0; oldNote(); return 1; }
            // by hand
            {   m_align::Report r = m_align::run(c.pcm16, have, b24, nb0, pb0, w.s, (long)Lm, (int)base, stages, 0, "; the placement given by hand follows", 0, -1, &manualT);
                for (auto& l : r.lines) printf("align: %s\n", l.c_str()); printf("%s\n", r.alignLine.c_str());
                printf("align: offset given by hand: cdalign=%s (checked first at the nearest whole offset %+lld)\n", manualText.c_str(), manualT);
                printf("align: fraction given by hand: cdalign=%s is the whole offset %+lld with the master moved by %+.7f master samples; not measured\n",
                       manualText.c_str(), manualT, manualMove);
                const Att A = attempt(r, "", manualT, manualMove); if (A.kind == 3) return 2;
                if (A.kind == 2) { printf("independent CD rejected: %s%s; nothing encoded\n", A.why.c_str(), r.found ? "" : fmt(" (at the offset given by hand, cdalign=%s: check the value, or run without cdalign= to see both searches' positions)", manualText.c_str()).c_str());
                    if (cdCheck) return 0; oldNote(); return 1; } }
            // the user's bands (2026-10-22): <= -40 dB on, -40..-33 dB and beyond -33 dB only after a confirmation, no fit at all never
            auto rejected = [&](const m_align::Report& r, const std::string& what) { printf("independent CD rejected: %s%s; nothing encoded\n", what.c_str(), r.hint.empty() ? "" : ("; " + r.hint).c_str());
                if (cdCheck) return 0; oldNote(); return 1; };
            const bool large = rep.band == m_align::BAND_CONFIRM_LARGE, ask = rep.band == m_align::BAND_CONFIRM || large || rep.mapDiff;   // a channel swap or inverted polarity also needs your confirmation (2026-10-24)
            if (cdCheck) { if (ask) printf("cdcheck: it would ask: %s%s\n", rep.why.c_str(), rep.hint.rfind("still shifted", 0) == 0 ? ("; " + rep.hint).c_str() : "");   // the hint helps stepping by hand
                printf("cdcheck: alignment checked%s, nothing encoded (%s)\n", placed.c_str(), rep.band == m_align::BAND_OK ? "would encode" : (ask ? "would ask for confirmation" : "would not encode")); return 0; }
            if (rep.band == m_align::BAND_STOP) return rejected(rep, rep.why);
            if (ask) {
                printf("independent CD: needs your confirmation: %s%s\n", rep.why.c_str(), placed.c_str());
                if (rep.mapDiff) printf("  note: the channel order or polarity also differs from the master: the extension codes that difference too\n");
                if (large) printf("  note: continuing is possible (the extension codes the whole difference and the precision stays the same), but the file may get much larger\n");
                if (!cdAccept) {
                    if (!interactive()) { printf("  nothing encoded. If this is the right CD and it cannot be aligned better%s, add the option cdaccept.\n", rep.hint.rfind("still shifted", 0) == 0 ? (" (" + rep.hint + ")").c_str() : ""); oldNote(); return 1; }
                    printf("  1 = end the task (nothing is written)\n  2 = continue%s\n  choose 1 or 2: ", large ? " (the file may get much larger)" : ""); fflush(stdout);
                    char line[32] = {0}; if (!fgets(line, sizeof line, stdin) || line[0] != '2') { printf("task ended: nothing encoded\n"); oldNote(); return 1; } }
                else printf("  continuing: cdaccept given%s\n", large ? " (the file may get much larger)" : "");
                alignNote = "independent CD: CONTINUED AFTER CONFIRMATION, " + rep.why + (rep.mapDiff ? ", and the channel order or polarity differs" : "") + placed; }
            else alignNote = (rep.verdict == 1 ? "independent CD: WARNING, " + rep.why : std::string("independent CD: alignment confirmed")) + placed;
            {   // the movement report (a separate text file; plain text for the user's records, read by no program)
                const double off = (double)rpT - rpMv, us = 1e6 / (double)w.rate;
                alignReport = fmt("LAE alignment report (independent CD)\nmaster: %s\nCD: %s\noutput: %s\n", argv[1], cdPath, outPath.c_str())
                    + fmt("whole offset: %+lld master samples\n", rpT)
                    + (rpMv != 0 ? fmt("fraction moved: %+.7f master samples (%+.4f microseconds): the master's waveform taken that much later\n", rpMv, rpMv * us) : std::string("fraction moved: none\n"))
                    + fmt("total offset (cdalign): %s master samples (%+.3f microseconds): the CD starts %s than the master\n", offText(rpT, rpMv).c_str(), off * us,
                          off > 0 ? "later" : (off < 0 ? "earlier" : "at the same time"))
                    + (padP ? fmt("digital silence added before the move: %ld CD frames at each end; removed after the move where still digital silence: start %ld, end %ld; kept: start %ld, end %ld CD frames\n",
                                  padP, cutL, cutT, padP - cutL, padP - cutT) : std::string())
                    + (placed.empty() ? std::string() : "placement:" + placed + "\n")
                    + "check after the move: " + rep.why + "\n" + rep.alignLine + "\n"; } }
        if (have != need) { lengthMsg(); oldNote(); return 1; }
        int16_t* s = (int16_t*)b.data(); for (long i = 0; i < need * 2; i++) s[pb0 * 2 + i] = c.pcm16[i];
        encio::writeVec(T("b16.s16").c_str(), b);
        std::vector<uint8_t> df = slurp(T("dither.flags")); std::fill(df.begin(), df.end(), 0); encio::writeVec(T("dither.flags").c_str(), df);
        printf("independent CD: %s (%ld samples) used as the CD layer\n", cdPath, have); }
    // the master is final here (m0.raw / m.raw hold it): its samples are not needed in memory any more, only its length (memory)
    if ((long long)(w.s.size() / 2) != Lm) encio::fail("internal error: master length");
    std::vector<int32_t>().swap(w.s);
    env("CDEXT", cdPath ? "1" : "");   // cut-off detection by coherence for an independent CD (routes.cpp)
    env("CUTOFFOUT", T("cutoff.txt"));
    if (call(m_routes::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), T("F.s32")})) { printf("route split failed\n"); oldNote(); return 2; } env("CUTOFFOUT", "");
    stamp("route split"); double fc = atof(encio::readText(T("cutoff.txt").c_str()).c_str());
    env("PART", modeName); env("PARTEQ", equal ? "1" : ""); env("PARTW", wm ? "1" : "0");
    // one layer = one encoder run (possibly several octave segments) + a self-check of every segment's stream
    std::atomic<bool> ok(true);   // shared by the two layer threads of a pair
    auto layer = [&](const char* in, int rate, const char* ch, const std::string& tag) {
        if (encio::testFail(("layerthrow:" + tag).c_str())) encio::fail("forced by the test hook in layer " + tag);   // test: an exception outside the modules
        env("CHSEL", ch); env("DUMP", T(tag + ".lae")); env("PLANE", T(tag + ".plane")); env("SEGCOUNT", T(tag + ".seg"));
        int rc = call(m_lossy::main, {"lossy", T(in), std::to_string(rate)}); env("SEGCOUNT", ""); std::vector<std::vector<uint8_t>> out;
        if (rc) { ok = false; encenv::print("layer %s: coding failed (code %d)\n", tag.c_str(), rc); return out; }
        int nseg = atoi(encio::readText(T(tag + ".seg").c_str()).c_str()); if (nseg < 1 || nseg > 8) { ok = false; encenv::print("layer %s: bad segment count\n", tag.c_str()); return out; }
        for (int i = 0; i < nseg; i++) { std::string suf = nseg > 1 ? "." + std::to_string(i) : "";
            env("PARTBYTES", T(tag + ".bin" + suf));
            if (call(m_part::main, {"part", T(tag + ".lae" + suf), T(tag + ".plane" + suf)})) ok = false;
            env("PARTBYTES", ""); if (!ok) return out; out.push_back(slurp(T(tag + ".bin" + suf))); }
        return out; };
    typedef std::vector<std::vector<uint8_t>> Streams;
    // two layers of the same kind at once (L and R, M and S): the second runs in its own thread with its own module variables and printed text.
    // Same arithmetic as one after the other, so the streams are byte-identical.
    // memory plan (2026-10-29): made once at the start from the track length and the physical memory read then; the whole encode follows it.
    // Two layers of a pair at once need about 2 x (80 bytes per layer sample + 64 MB) (measured: about 70 bytes); budget = mem=<GB>, default half
    // the physical memory; never two at once on 32-bit builds (address space) or with PARTTHREADS=1. The output does not depend on the plan.
    struct PairPlan { bool both = false; unsigned long long need = 0; };
    unsigned long long memTotal = 0, memAvail = 0; physMemory(memTotal, memAvail);
    const unsigned long long memUse = memBudget > 0 ? memBudget : memTotal / 2;
    const char* ptEnv = getenv("PARTTHREADS"); const bool oneThread = ptEnv && atoi(ptEnv) == 1;
    auto planFor = [&](long long samples) { PairPlan p; p.need = 2 * (80ull * (unsigned long long)samples + (64ull << 20));
        p.both = sizeof(void*) >= 8 && !oneThread && !getenv("LAE_TESTSEQUENTIAL") && p.need <= memUse && p.need <= memAvail; return p; };
    const PairPlan planD = planFor((Lm + 2 * P) >> stages), planF = planFor(Lm + 2 * P);
    printf("memory plan: %.2f min at %d Hz; depth layers %s (two at once about %.0f MB), frequency layers %s (two at once about %.0f MB); budget %.0f MB, available %.0f MB\n",
           Lm / (60.0 * w.rate), w.rate, planD.both ? "two at once" : "coded one after the other", planD.need / 1e6, planF.both ? "two at once" : "coded one after the other", planF.need / 1e6,
           memUse / 1e6, memAvail / 1e6);
    auto layerPair = [&](const char* in, int rate, const char* c1, const std::string& t1, const char* c2, const std::string& t2, Streams& o1, Streams& o2, bool both) {
        auto sequential = [&]() { o1 = layer(in, rate, c1, t1); if (ok) o2 = layer(in, rate, c2, t2); };
        if (!both) { sequential(); return; }
        std::map<std::string, std::string> e1, e2; std::string s1, s2; std::exception_ptr x1, x2;
        // one layer on this thread; an exception is kept and re-thrown on the main thread after both have finished, so the encoder stops
        // exactly as it does when the layers run one after the other (nothing written, work folder removed)
        auto one = [&](std::map<std::string, std::string>& e, std::string& s, const char* c, const std::string& t, Streams& o, std::exception_ptr& x) {
            struct Reset { ~Reset() { encenv::over = nullptr; encenv::out = nullptr; } } reset;
            encenv::over = &e; encenv::out = &s;
            try { o = layer(in, rate, c, t); } catch (...) { x = std::current_exception(); ok = false; } };
        std::thread th;
        try { th = std::thread([&]() { one(e2, s2, c2, t2, o2, x2); }); }
        catch (...) { sequential(); return; }   // no thread could be started: one after the other
        one(e1, s1, c1, t1, o1, x1); th.join();
        fputs(s1.c_str(), stdout); fputs(s2.c_str(), stdout);
        if (x1) std::rethrow_exception(x1); if (x2) std::rethrow_exception(x2); };
    env("FULLBAND", "1"); env("MASTERREF", T("b24.s32")); env("DEPTHADAPT", "1");
    Streams dL, dR; layerPair("T.s32", (int)base, "L", "dL", "R", "dR", dL, dR, planD.both); stamp("depth layers");
    env("FULLBAND", ""); env("MASTERREF", ""); env("DEPTHADAPT", "");
    char kf[32]; snprintf(kf, sizeof kf, "%.6f", (fc - 1000.0) / (base / 2)); env("K0FRAC", kf); env("OCTSPLIT", "1");
    Streams fM, fS; if (ok) layerPair("F.s32", w.rate, "M", "fM", "S", "fS", fM, fS, planF.both); env("K0FRAC", ""); env("OCTSPLIT", ""); stamp("frequency layers");
    if (!ok || dL.size() != 1 || dR.size() != 1 || fM.empty() || fM.size() != fS.size()) { printf("ENCODER SELF-CHECK FAILED: a decoded layer differs from the encoder's plane or a layer could not be coded; nothing written\n"); oldNote(); return 2; }
    // CD layer: padded base trimmed by P >> stages at each end; the trimmed edges go into the LAE chunk (the decoder needs them)
    std::vector<uint8_t> b16 = slurp(T("b16.s16")); long nb = (long)b16.size() / 4, pb = P >> stages;
    std::vector<int16_t> cd((nb - 2 * pb) * 2), edge(4 * pb); const int16_t* s16 = (const int16_t*)b16.data();
    for (long i = 0; i < nb - 2 * pb; i++) { cd[i * 2] = s16[(i + pb) * 2]; cd[i * 2 + 1] = s16[(i + pb) * 2 + 1]; }
    for (long i = 0; i < 2 * pb; i++) { edge[i] = s16[i]; edge[2 * pb + i] = s16[(nb - pb) * 2 + i]; }
    std::vector<uint8_t> dfl = slurp(T("dither.flags"));
    // LAE chunk v5 (v5 = v4 + precision rule per stream, reconstruction offset 0.20, 6 modes; v4 = v3 + 16-bit backstop): header, base edges, dither flags, depth L, depth R, number of frequency segments, M segments, S segments
    // v6 = v5 + the phase number after pb (an independent CD whose position has a fraction of a master sample); phase number 0 is written as v5
    // written to <out>.part and read back: the file on disk must hold exactly this CD layer and this chunk
    auto writeFile = [&]() { std::vector<uint8_t> lae; auto put32 = [&](int32_t v) { lae.insert(lae.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        auto putb = [&](const void* p, size_t n) { lae.insert(lae.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        auto putS = [&](const std::vector<uint8_t>& s) { put32((int32_t)s.size()); putb(s.data(), s.size()); };
        put32(0x3145414C); put32(5); put32(w.rate); put32((int32_t)base); put32(stages); put32((int32_t)Lm); put32(P);
        put32((int32_t)fc); put32(mi); put32(equal ? 1 : 0); put32(wm); put32((int32_t)nb); put32((int32_t)pb);
        putb(edge.data(), edge.size() * 2); put32((int32_t)dfl.size()); putb(dfl.data(), dfl.size());
        putS(dL[0]); putS(dR[0]); put32((int32_t)fM.size()); for (auto& s : fM) putS(s); for (auto& s : fS) putS(s);
        put32((int32_t)crc32(lae.data(), lae.size()));   // v3: CRC-32 of everything before it (integrity check, whole chunk)
        if (!wavio::fits32((unsigned long long)cd.size() * 2, 8ULL + lae.size() + 1)) { printf("output would exceed the 4 GB limit of WAV: nothing written\n"); return (size_t)0; }
        encrun::partMine = true;
        if (!wavio::writeCD(partPath.c_str(), (int)base, cd, lae) || encio::testFail("partwrite")) { printf("cannot write %s\n", partPath.c_str()); return (size_t)0; }
        wavio::Wav v; if (!wavio::read(partPath.c_str(), v) || v.rate != (int)base || v.bits != 16 || v.ch != 2 || v.pcm16 != cd || v.lae != lae || encio::testFail("partverify")) {
            printf("read-back check FAILED: %s does not hold what was written (disk full or failing?)\n", partPath.c_str()); return (size_t)0; }
        return lae.size(); };
    size_t chunk = writeFile(); if (!chunk) { oldNote(); return 2; } stamp("file written");
    long nclip = -1;
    {   // leak compensation (v5, always on): decode the final frequency layer, let the depth layer cancel its leak into the CD band
        g_streamErr = false; std::error_code ec; std::filesystem::remove(T("leak.s32"), ec);
        env("LEAKOUT", T("leak.s32")); int rc = call(m_dec::main, {"dec", partPath, T("check.wav"), "full"}); env("LEAKOUT", "");
        long long lsz = fileSize(T("leak.s32")); const bool forced = encio::testFail("leakdecode");
        if (rc || lsz != (long long)nb * 8 || g_streamErr || forced) { printf("leak compensation FAILED (decoder code %d, leak file %lld bytes, expected %lld%s%s): nothing written\n", rc, lsz, (long long)nb * 8,
                                                                                g_streamErr ? ", decoded values out of range" : "", forced ? ", forced by the test hook" : ""); oldNote(); return 2; }
        char fcs[32]; snprintf(fcs, sizeof fcs, "%.0f", fc); env("LEAKIN", T("leak.s32"));
        rc = call(m_routes::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), T("F2.s32"), fcs}); env("LEAKIN", "");
        stamp("leak decoded + route split again");
        if (rc || encio::testFail("routes2")) { printf("leak compensation FAILED in the route split: nothing written\n"); oldNote(); return 2; }
        env("FULLBAND", "1"); env("MASTERREF", T("b24.s32")); env("DEPTHADAPT", "1");
        dL.clear(); dR.clear(); layerPair("T.s32", (int)base, "L", "dL", "R", "dR", dL, dR, planD.both);
        env("FULLBAND", ""); env("MASTERREF", ""); env("DEPTHADAPT", "");
        if (!ok || dL.size() != 1 || dR.size() != 1) { printf("ENCODER SELF-CHECK FAILED after leak compensation; nothing written\n"); oldNote(); return 2; }
        chunk = writeFile(); if (!chunk) { oldNote(); return 2; }
        printf("leak compensation: depth layer re-encoded against the decoded frequency layer\n"); }
    // no clip control: an overshoot beyond full scale is bounded by the coding error (16-bit backstop; peak error
    // <= -76.6 dBFS measured on hard-clipped +12 dB masters) and the decoder's hard limiter returns it to the master's
    // own full-scale value. This decode of the final file is also the end-to-end check: if the decoder refuses it, nothing is written.
    { g_streamErr = false; std::error_code ec; std::filesystem::remove(T("clip.bin"), ec);
      env("CLIPMAP", T("clip.bin")); int rc = call(m_dec::main, {"dec", partPath, T("check.wav"), "full"}); env("CLIPMAP", "");
      if (rc || encio::testFail("finaldecode")) { printf("end-to-end check FAILED: the decoder could not decode the final file (code %d): nothing written\n", rc); oldNote(); return 2; }
      const long long wantWav = 44 + Lm * 6 + ((Lm * 6) & 1), clipB = fileSize(T("clip.bin"));
      if (fileSize(T("check.wav")) == wantWav && clipB >= 0 && !encio::testFail("clipdecode")) nclip = (long)(clipB / 4);
      if (nclip >= 0) printf("samples beyond full scale before the decoder's hard limiter: %ld\n", nclip);
      else printf("samples beyond full scale before the decoder's hard limiter: not measured (the check files are incomplete)\n"); }
    { std::error_code ec; std::filesystem::remove(T("check.wav"), ec); }
    {   // only a complete, verified file gets the output name; a briefly locked target (scanner, indexer, player) is retried
        std::error_code ec; for (int k = 0; k < 20; k++) { ec.clear(); std::filesystem::rename(partPath, outPath, ec); if (!ec) break; std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
        if (ec) { printf("cannot rename %s to %s: %s (the output may be open in another program)\n", partPath.c_str(), outPath.c_str(), ec.message().c_str()); oldNote(); return 2; }
        encrun::partMine = false; }
    if (!alignReport.empty()) { const std::string rp = outPath + ".align.txt"; FILE* f = fopen(rp.c_str(), "wb");
        bool ok = f && fwrite(alignReport.data(), 1, alignReport.size(), f) == alignReport.size(); if (f && fclose(f)) ok = false;
        printf(ok ? "alignment report: %s\n" : "WARNING: the alignment report %s could not be written (the LAE file itself is complete)\n", rp.c_str()); }
    size_t sd = dL[0].size() + dR[0].size(), sf = 0; for (auto& s : fM) sf += s.size(); for (auto& s : fS) sf += s.size();
    std::string extra; if (!note32.empty()) extra += " | " + note32; if (!alignNote.empty()) extra += " | " + alignNote;
    printf("LAE encode OK: %s | CD %d Hz cut-off %.0f Hz | scheme %d, mode %s (share by %s) %s | CD PCM %zu B, depth %zu B, frequency %zu B in %zu octave segment(s), chunk %zu B | beyond full scale before limiter %s | %.1f s%s\n",
           argv[2], (int)base, fc, scheme, part::MODES[mi].name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), equal ? "equal" : "unequal", cd.size() * 2, sd, sf, fM.size(), chunk,
           nclip >= 0 ? std::to_string(nclip).c_str() : "not measured", std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count(), extra.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::set_terminate(encrun::onTerminate); std::atexit(encrun::onExit);
    std::signal(SIGINT, encrun::onSignal); std::signal(SIGTERM, encrun::onSignal);
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // no error-report dialog that would block a scripted run
    std::signal(SIGBREAK, encrun::onSignal);
#endif
    int rc;
    try { rc = argc > 1 && !strcmp(argv[1], "plan") ? m_plan::planMain(argc, argv) : encode(argc, argv); }
    catch (const encio::Fail& e) { encrun::emergency(e.what()); return 2; }
    catch (const std::bad_alloc&) { encrun::emergency("out of memory"); return 2; }
    catch (const std::exception& e) { encrun::emergency(e.what()); return 2; }
    catch (...) { encrun::emergency("internal error"); return 2; }
    encrun::finished = true; return rc; }
