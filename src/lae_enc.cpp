// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// LAE standalone encoder (lossy tier). Every verified module is compiled into this one
// executable inside its own namespace, so the encoder runs exactly the code that the tests verified.
//   input : PCM WAV master, stereo, 16/24/32-bit integer, rate = 44.1k or 48k family x 2, 4 or 8
//   output: WAV whose audio is the CD layer (16 bit at 44.1/48 kHz, plays everywhere) + a "LAE " chunk with
//           the depth layer (L, R) and the frequency layer (M, S), each range-coded with partition levels
// usage: lae_enc <master.wav> <out.wav | out.flac | out.lae> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2 (scheme), default s1] [zs] [cd=<CD.wav>] [energy] [equal]
//                [cdcheck] [cdalign=<offset>] [cdaccept] [cdedge=pad | cdedge=cut] [bits=24] [mem=<GB>] [noise=<marks.laenoise>] [g | g=b | g=c | g=d]
//                [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...] [--form=flac | --form=wav]
//        the output form (2026-11-10): WAV form by default; .flac = FLAC form; .lae = WAV form unless --form=flac (see outputForm below)
//        lae_enc tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]   new tags, no re-encoding
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
//   Sound only the master has, one CD sample or more before the CD starts or after it ends (2026-11-03): told, then the user chooses
//   (a console asks; in scripts the option): cdedge=pad adds digital silence to the CD layer there (the master kept whole, the CD layer
//   longer than the given CD), cdedge=cut cuts the master to the CD (its sound beyond the CD not kept). Without a choice nothing is encoded.
//   bits=24: a 32-bit master is encoded from its top 24 bits (without it the encoder asks on a console, or stops otherwise).
//   mem=<GB>: memory budget for coding two layers at once (default: half the physical memory); above it they are coded one after the
//   other. The output file does not depend on it.
//   noise=<marks.laenoise> (2026-11-04): the cells the user marked (lae_noise / noise_panel.html, on this master, already aligned) are deleted:
//   in the frequency layer the master is taken as zero there, and a cell marked in all its bins is A. Cells below the frequency layer's
//   start (k0 = CD cut-off - 1 kHz) lie in the CD layer: listed, not deleted. g: every A cell (marked or silent) whose G - the up-converted
//   CD layer's part of it, about 120 dB below the music (docs/NOISE.md) - lies above the 24-bit rounding floor codes
//   -G, so the decoded output there is silent down to the 24-bit floor. Neither option: the output is the earlier one, byte for byte.
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
#include <numeric>
#include <chrono>
#include <string>
#include <functional>
#include <type_traits>
#include <memory>
#include <new>
#include <thread>
#include <atomic>
#include <set>
#include <mutex>
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
#include "wav_in.h"      // block-wise master reader (WAV, RF64, W64)
#include "blk_io.h"      // block-wise reads / writes and filter windows (global, shared by the block-wise modules)
#include "enc_io.h"      // checked I/O, shared by every module (global, like part_stream.h)
#include "align_hash.h"
#include "lae_cdx_read.h"   // the publisher's CUE for the independent-CD planner (cd_plan.cpp)
#include "halfband.h"
#include "part_stream.h"
#include "partition.h"   // encoder search (global, like part_stream.h, so every module shares ::part)
#include "wavio.h"
#include "lae_math.h"   // the encoder's log2 / sinpi / cospi / exp2, identical on every platform (2026-11-03)
#include "fft.h"
#include "mdct_tb.h"   // global, shared by the lossy and decoder modules (both namespaces use ::mdct)
#include "frac_delay.h"   // global: the v6 phase delay, shared by the route split, the alignment and the decoder (::lae_fd)
#include "noise_marks.h"   // global: the marking file of noise= (::nmk), read by the encoder and by the layer coder
#include "laes_tags.h"     // tags and pictures (2026-11-06): the same code and layout as laes
#include "laey_flac.h"     // the FLAC form (2026-11-10): reader (also used by the decoder module below)
#include "laes_flac_enc.h" // FLAC frame writer (RFC 9639, the project's own code), the one of the LAES FLAC form
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
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
namespace m_base {   // decimate() for the independent-CD alignment (cd_align.cpp); the CD layer itself is derived by base_blk.cpp
#include "base_layer.cpp"
}
namespace m_padblk {   // the encoder modules (block-wise since 2026-10-29: memory of a few blocks, output identical to the earlier whole-track modules)
#include "pad_blk.cpp"
}
namespace m_baseblk {
#include "base_blk.cpp"
}
namespace m_routesblk {
#include "routes_blk.cpp"
}
namespace m_lossyblk {   // layer coder
LAE_LAYER_MODULE_IO
#include "lossy_blk.cpp"
}
namespace m_partblk {   // range coder of a layer
LAE_LAYER_MODULE_IO
#include "part_blk.cpp"
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
static std::string out, part, tmp; static bool hadOld = false, partMine = false, tmpMine = false;
static std::atomic<bool> cleaned{false}, finished{false};   // atomic: on Windows the Ctrl+C handler runs in a thread of its own while the layer threads work
static bool keepTmp = false;   // resume: the work folder stays when the run does not finish, so the next run can continue from it
static std::string outerTmp; static bool outerMine = false;   // extension packs: the calling run's work folder while a further master runs inside it (tmp is then the inner one)
static void closeAll() {
#ifdef _WIN32
    _fcloseall();   // files a failed module left open would block their deletion on Windows
#endif
}
static void cleanup() { if (cleaned.exchange(true)) return; closeAll(); std::error_code ec;   // the .part only while it is unfinished; the work folder always
    if (partMine) std::filesystem::remove(part, ec); if (tmpMine && !getenv("KEEPTMP") && !keepTmp) std::filesystem::remove_all(tmp, ec); }
static void oldNote() { if (hadOld) printf("note: the existing %s was NOT written by this run\n", out.c_str()); }
static void emergency(const char* why) { if (finished.exchange(true)) return; printf("ENCODER STOPPED: %s; nothing written\n", why); oldNote(); fflush(stdout); cleanup();
    if (outerMine && !getenv("KEEPTMP")) { std::error_code ec; std::filesystem::remove_all(outerTmp, ec); }   // a stop inside a further master: the calling run's work folder goes too
    lae_u8::restoreConsole(); }   // _exit follows: no atexit, so the console is set back here (win_utf8.h)
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
                          "PARTBYTES", "FULLBAND", "MASTERREF", "DEPTHADAPT", "K0FRAC", "OCTSPLIT", "LEAKIN", "LEAKOUT", "CLIPMAP", "NOISE", "NOISEPAD", "NOISEM", "GMODE"}) if (getenv(k)) env(k, ""); }
static std::vector<uint8_t> slurp(const std::string& p) { return encio::readAll<uint8_t>(p.c_str()); }   // a missing or unreadable file stops the encoder
static bool exists(const std::string& p) { std::error_code ec; return std::filesystem::exists(p, ec); }
static long long fileSize(const std::string& p) { std::error_code ec; auto n = std::filesystem::file_size(p, ec); return ec ? -1 : (long long)n; }
// An input WAV / RF64 / W64 whose header gives a data length beyond the end of the file (2026-11-14): the file is read to its end as
// before (wavin::probe, wavio::read, wavio::readCD and the CUE reader all limit the length to the file), and the user is told.
static void longHeaderNote(const char* path, const wavin::Info& in) {
    if (in.hdrLen) printf("NOTE: the header of %s gives a data length of %llu bytes, but the file holds only %lld bytes of it: the file may be cut off (incomplete). It is read to its end and the run continues.\n", path, in.hdrLen, in.dataBytes); }
static void longHeaderNote(const char* path) { wavin::Info in; wavin::probe(path, in); longHeaderNote(path, in); }   // for a file another reader has just read

namespace m_plan {
#include "cd_plan.cpp"
}

// Tags and pictures (2026-11-06), the same as laes: --tag=KEY=VALUE (Vorbis names: TITLE, ARTIST, ALBUM, DATE, ...; a key may repeat),
// --cover=<image> (front cover), --picture=<type>:<image> (other picture types); JPEG or PNG, stored byte for byte. Also accepted without
// the leading "--", like the encoder's other options. One set describes the whole file (CD layer and extension). WAV layout: a LIST/INFO
// chunk and an "id3 " chunk (ID3v2.4, UTF-8) after "fmt " and before "data". The CD audio and the "LAE " chunk do not change, and
// without these options the file is the same, byte for byte.
static const char* kLaeyEncoder = "lae_enc (LAE Lossy)";
static bool isTagOpt(const char* a) { return laestag::isTagArg(a) || !strncmp(a, "tag=", 4) || !strncmp(a, "cover=", 6) || !strncmp(a, "picture=", 8); }
static std::string tagArgs(int argc, char** argv, int from, laestag::Tags& t) {
    std::vector<std::string> v{"lae_enc"};
    for (int i = from; i < argc; i++) if (isTagOpt(argv[i])) v.push_back(argv[i][0] == '-' ? std::string(argv[i]) : "--" + std::string(argv[i]));
    std::vector<char*> pv; for (auto& x : v) pv.push_back(&x[0]); return laestag::parseArgs((int)pv.size(), pv.data(), t); }

// The form of the output file (2026-11-10). WAV is the default form of LAEY; FLAC is a supported one: a name ending in .flac = FLAC form (the CD layer in FLAC audio frames,
// the extension in "LAE1" APPLICATION blocks: laey_flac.h); a name ending in .lae (the format's own name) = WAV form, or FLAC form with
// --form=flac (laes: the other way round); any other name = WAV form (CD layer in 'data', extension in the "LAE " chunk), as before. --form=flac / --form=wav (also
// without the "--") must agree with a .flac / .wav name. Readers go by content, never by the name.
static bool isFormOpt(const char* a) { return !strncmp(a, "--form=", 7) || !strncmp(a, "form=", 5); }
static bool nameEnds(const std::string& s, const char* e) { const size_t n = strlen(e); if (s.size() <= n) return false;
    for (size_t i = 0; i < n; i++) { char c = s[s.size() - n + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != e[i]) return false; } return true; }
static int outputForm(const std::string& out, int argc, char** argv, int from) {   // 0 = WAV form, 1 = FLAC form, -1 = refused (reason printed)
    std::string formArg; for (int i = from; i < argc; i++) if (isFormOpt(argv[i])) formArg = strchr(argv[i], '=') + 1;
    if (!formArg.empty() && formArg != "flac" && formArg != "wav") { printf("--form must be flac or wav\n"); return -1; }
    const bool laeName = nameEnds(out, ".lae"); const bool asFlac = laeName ? formArg == "flac" : nameEnds(out, ".flac");
    if (!formArg.empty() && !laeName && (formArg == "flac") != asFlac) { printf("--form=%s does not match the name %s (use .lae to choose the form freely)\n", formArg.c_str(), out.c_str()); return -1; }
    return asFlac ? 1 : 0; }

// lae_enc tag <in.wav> <out.wav> [tags] (2026-11-06): new tags without re-encoding. A new file is written ("fmt ", the new tags, then
// every other chunk in its order; former LIST/INFO and "id3 " chunks are left out; no tags given = none); the input is not changed. The
// new file's CD audio ("data") and "LAE " chunk are then compared byte for byte with the input's. First the input must pass the
// decoder's open check (2026-11-15, decoderOpens below): a damaged file is refused, nothing written.
namespace m_tag {
struct Ck { char id[4]; long long body; uint32_t len; };
static bool walk(FILE* f, std::vector<Ck>& cks, std::string& why) {
    uint8_t h[12]; if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { why = "not a RIFF WAVE file"; return false; }
    if (_fseeki64(f, 0, SEEK_END)) { why = "cannot seek"; return false; } const long long end = _ftelli64(f); long long p = 12;
    while (p + 8 <= end) { uint8_t c[8]; if (_fseeki64(f, p, SEEK_SET) || fread(c, 1, 8, f) != 8) { why = "cannot read a chunk header"; return false; }
        Ck k; memcpy(k.id, c, 4); k.len = (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24; k.body = p + 8;
        if (k.body + k.len > end) { why = std::string("chunk '") + std::string(k.id, 4) + "' runs past the end of the file (the file is cut off: a decoder plays such a file up to its end, but lae_enc tag copies only complete files)"; return false; }   // stricter than the decoders (spec 3.1) on purpose: a new file is written
        cks.push_back(k); p = k.body + k.len + (k.len & 1); }
    return true; }
static const Ck* find(const std::vector<Ck>& cks, const char* id) { for (auto& k : cks) if (!memcmp(k.id, id, 4)) return &k; return nullptr; }
static bool isOldTag(FILE* f, const Ck& k) {   // LIST of type INFO, or an id3 chunk
    if (!memcmp(k.id, "id3 ", 4) || !memcmp(k.id, "ID3 ", 4)) return true;
    if (memcmp(k.id, "LIST", 4) || k.len < 4) return false; char t[4]; return !_fseeki64(f, k.body, SEEK_SET) && fread(t, 1, 4, f) == 4 && !memcmp(t, "INFO", 4); }
static bool copyBody(FILE* in, const Ck& k, FILE* out) {   // header, body, pad byte
    uint8_t h[8]; memcpy(h, k.id, 4); for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(k.len >> (8 * i)); if (fwrite(h, 1, 8, out) != 8) return false;
    if (_fseeki64(in, k.body, SEEK_SET)) return false; std::vector<uint8_t> b(1 << 20); unsigned long long left = k.len;
    while (left) { const size_t n = (size_t)std::min<unsigned long long>(left, b.size()); if (fread(b.data(), 1, n, in) != n || fwrite(b.data(), 1, n, out) != n) return false; left -= n; }
    const uint8_t z = 0; return !(k.len & 1) || fwrite(&z, 1, 1, out) == 1; }
static bool sameBody(FILE* a, const Ck& ka, FILE* b, const Ck& kb) {
    if (ka.len != kb.len || _fseeki64(a, ka.body, SEEK_SET) || _fseeki64(b, kb.body, SEEK_SET)) return false;
    std::vector<uint8_t> x(1 << 20), y(1 << 20); unsigned long long left = ka.len;
    while (left) { const size_t n = (size_t)std::min<unsigned long long>(left, x.size()); if (fread(x.data(), 1, n, a) != n || fread(y.data(), 1, n, b) != n || memcmp(x.data(), y.data(), n)) return false; left -= n; }
    return true; }
// FLAC form (2026-11-10): "fLaC" | STREAMINFO | the new tags | every other block in its order (former VORBIS_COMMENT, PICTURE and
// PADDING blocks are left out) | the audio frames, copied. Then the new file's "LAE1" blocks and audio frames are compared byte for
// byte with the input's.
struct Bk { int type; long long body; uint32_t len; bool lae1; };
static bool walkFlac(FILE* f, std::vector<Bk>& bks, long long& audioOff, long long& end, std::string& why) {
    uint8_t h[4]; if (_fseeki64(f, 0, SEEK_END)) { why = "cannot seek"; return false; } end = _ftelli64(f);
    if (_fseeki64(f, 0, SEEK_SET) || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) { why = "not a FLAC file"; return false; }
    long long p = 4; bool last = false;
    while (!last) { uint8_t b[4]; if (p + 4 > end || _fseeki64(f, p, SEEK_SET) || fread(b, 1, 4, f) != 4) { why = "FLAC metadata truncated"; return false; }
        Bk k; last = (b[0] & 0x80) != 0; k.type = b[0] & 0x7F; k.len = (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; k.body = p + 4; k.lae1 = false;
        if (k.body + k.len > end) { why = "FLAC metadata truncated"; return false; }
        if (k.type == 2 && k.len >= 4) { char id[4]; if (fread(id, 1, 4, f) != 4) { why = "FLAC metadata truncated"; return false; } k.lae1 = !memcmp(id, "LAE1", 4); }
        bks.push_back(k); p = k.body + k.len; }
    audioOff = p; return true; }
static bool copyRange(FILE* in, long long off, unsigned long long len, FILE* out) {
    if (_fseeki64(in, off, SEEK_SET)) return false; std::vector<uint8_t> b(1 << 20);
    while (len) { const size_t n = (size_t)std::min<unsigned long long>(len, b.size()); if (fread(b.data(), 1, n, in) != n || fwrite(b.data(), 1, n, out) != n) return false; len -= n; }
    return true; }
static bool sameRange(FILE* a, long long offA, FILE* b, long long offB, unsigned long long len) {
    if (_fseeki64(a, offA, SEEK_SET) || _fseeki64(b, offB, SEEK_SET)) return false; std::vector<uint8_t> x(1 << 20), y(1 << 20);
    while (len) { const size_t n = (size_t)std::min<unsigned long long>(len, x.size()); if (fread(x.data(), 1, n, a) != n || fread(y.data(), 1, n, b) != n || memcmp(x.data(), y.data(), n)) return false; len -= n; }
    return true; }
// the decoder's open check (2026-11-15), before anything is written: the WAV form's "LAE " chunk (its CRC-32, the header and every
// stream header) or the FLAC form's frames, MD5 and "LAE1" blocks, exactly as lae_dec opens the file. A damaged file is refused.
static bool decoderOpens(const std::string& in, bool inFlac, std::string& why) {
    wavio::Wav w; int flacRate = 0;
    if (inFlac) { bool notLaey = true; why = laeyflac::readFile(in.c_str(), w.rate, w.pcm16, w.lae, notLaey); if (!why.empty()) return false; flacRate = w.rate; }
    else if (!wavio::readCD(in.c_str(), w) || w.bits != 16 || w.ch != 2) { why = "not a 16-bit stereo WAV"; return false; }
    m_dec::Decoder D; int code = 0; why = D.open(std::move(w.pcm16), std::move(w.lae), "full", 0, &code); if (!why.empty()) return false;
    if (flacRate && flacRate != D.base) { why = "FLAC STREAMINFO gives " + std::to_string(flacRate) + " Hz, the extension's base rate is " + std::to_string(D.base) + " Hz"; return false; }
    return true; }
static bool openCheckFails(const std::string& in, bool inFlac) { std::string why; if (decoderOpens(in, inFlac, why)) return false;
    printf("%s does not pass the decoder's open check (%s): lae_enc tag copies only undamaged files; nothing written\n", in.c_str(), why.c_str()); return true; }
static int retagFlac(FILE* fi, const std::string& in, const std::string& out, const std::string& part, const laestag::Tags& tags) {
    std::error_code ec; std::vector<Bk> bks; long long audioOff = 0, end = 0; std::string why;
    if (!walkFlac(fi, bks, audioOff, end, why)) { fclose(fi); printf("%s: %s; nothing written\n", in.c_str(), why.c_str()); return 1; }
    bool haveLae = false; for (auto& k : bks) haveLae = haveLae || k.lae1;
    if (bks.empty() || bks[0].type != 0 || bks[0].len != 34 || !haveLae) { fclose(fi); printf("%s is not an LAE file in the FLAC form (it needs STREAMINFO and \"LAE1\" APPLICATION blocks); nothing written\n", in.c_str()); return 1; }
    if (openCheckFails(in, true)) { fclose(fi); return 1; }
    std::vector<Bk> keep; for (size_t i = 1; i < bks.size(); i++) { const int t = bks[i].type; if (t != 0 && t != 1 && t != 4 && t != 6) keep.push_back(bks[i]); }
    const std::vector<uint8_t> tb = laestag::flacBlocks(tags, false, kLaeyEncoder);
    FILE* fo = fopen(part.c_str(), "wb"); bool ok = fo != nullptr;
    if (ok) { const uint8_t h[8] = {'f', 'L', 'a', 'C', 0, 0, 0, 34}; ok = fwrite(h, 1, 8, fo) == 8 && copyRange(fi, bks[0].body, 34, fo) && (tb.empty() || fwrite(tb.data(), 1, tb.size(), fo) == tb.size()); }
    for (size_t i = 0; ok && i < keep.size(); i++) { const Bk& k = keep[i]; const uint8_t bh[4] = {(uint8_t)((i + 1 == keep.size() ? 0x80 : 0) | k.type), (uint8_t)(k.len >> 16), (uint8_t)(k.len >> 8), (uint8_t)k.len};
        ok = fwrite(bh, 1, 4, fo) == 4 && copyRange(fi, k.body, k.len, fo); }
    ok = ok && copyRange(fi, audioOff, (unsigned long long)(end - audioOff), fo);
    if (fo) ok = fclose(fo) == 0 && ok;
    if (ok) { FILE* fc = fopen(part.c_str(), "rb"); std::vector<Bk> b2; long long a2 = 0, e2 = 0; std::string w2;
        ok = fc && walkFlac(fc, b2, a2, e2, w2) && e2 - a2 == end - audioOff && sameRange(fi, audioOff, fc, a2, (unsigned long long)(end - audioOff));
        std::vector<Bk> l1, l2; for (auto& k : bks) if (k.lae1) l1.push_back(k); for (auto& k : b2) if (k.lae1) l2.push_back(k);
        ok = ok && l1.size() == l2.size(); for (size_t i = 0; ok && i < l1.size(); i++) ok = l1[i].len == l2[i].len && sameRange(fi, l1[i].body, fc, l2[i].body, l1[i].len);
        if (fc) fclose(fc);
        if (!ok) printf("check FAILED: the new file's CD audio frames or LAE1 blocks differ from the input's\n"); }
    else printf("cannot write %s\n", part.c_str());
    fclose(fi);
    if (ok) { std::filesystem::rename(std::filesystem::u8path(part), std::filesystem::u8path(out), ec); ok = !ec; if (!ok) printf("cannot rename %s to %s\n", part.c_str(), out.c_str()); }
    if (!ok) { std::filesystem::remove(std::filesystem::u8path(part), ec); printf("nothing written\n"); return 2; }
    printf("written: %s (FLAC form, %zu tag(s), %zu picture(s)); CD audio frames and LAE1 blocks byte-identical to %s\n", out.c_str(), tags.kv.size(), tags.pics.size(), in.c_str()); return 0; }
static int retag(int argc, char** argv) {
    if (argc < 4) { printf("usage: lae_enc tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]   (WAV or FLAC form, the output keeps the input's form; no tags given: the tags are removed)\n"); return 1; }
    const std::string in = argv[2], out = argv[3], part = out + ".part";
    for (int i = 4; i < argc; i++) if (!isTagOpt(argv[i])) { printf("unknown option '%s': nothing written (options: --tag=KEY=VALUE --cover=<image> --picture=<type>:<image>)\n", argv[i]); return 1; }
    std::error_code ec; if (std::filesystem::exists(std::filesystem::u8path(out), ec)) { printf("%s already exists: choose another output name (the input is never changed); nothing written\n", out.c_str()); return 1; }
    if (std::filesystem::exists(std::filesystem::u8path(part), ec)) { printf("%s already exists: remove it or choose another output name; nothing written\n", part.c_str()); return 1; }
    laestag::Tags tags; { const std::string e = tagArgs(argc, argv, 4, tags); if (!e.empty()) { printf("%s: nothing written\n", e.c_str()); return 1; } }
    const std::vector<uint8_t> tc = laestag::wavChunks(tags, kLaeyEncoder);
    FILE* fi = fopen(in.c_str(), "rb"); if (!fi) { printf("cannot read %s\n", in.c_str()); return 1; }
    { uint8_t h4[4] = {0}; const bool inFlac = fread(h4, 1, 4, fi) == 4 && !memcmp(h4, "fLaC", 4);   // the output keeps the input's form; its name must not say the other one
      if (inFlac ? nameEnds(out, ".wav") : nameEnds(out, ".flac")) { fclose(fi); printf("%s is in the %s form: give the output a %s name (lae_enc tag does not change the form); nothing written\n", in.c_str(), inFlac ? "FLAC" : "WAV", inFlac ? ".flac or .lae" : ".wav or .lae"); return 1; }
      if (inFlac) return retagFlac(fi, in, out, part, tags); }
    rewind(fi);   // the form check above read 4 bytes; walk() reads the RIFF header from the start
    std::vector<Ck> cks; std::string why; if (!walk(fi, cks, why)) { fclose(fi); printf("%s: %s; nothing written\n", in.c_str(), why.c_str()); return 1; }
    const Ck* fmt = find(cks, "fmt "); const Ck* dat = find(cks, "data"); const Ck* lae = find(cks, "LAE ");
    if (!fmt || !dat || !lae) { fclose(fi); printf("%s is not an LAE file (it needs \"fmt \", \"data\" and \"LAE \" chunks); nothing written\n", in.c_str()); return 1; }
    if (openCheckFails(in, false)) { fclose(fi); return 1; }
    std::vector<Ck> keep; keep.push_back(*fmt); for (auto& k : cks) if (&k != fmt && !isOldTag(fi, k)) keep.push_back(k);
    unsigned long long riff = 4 + tc.size(); for (auto& k : keep) riff += 8ULL + k.len + (k.len & 1);
    if (riff > 0xFFFFFFFFull) { fclose(fi); printf("the new file would exceed the 4 GB limit of WAV; nothing written\n"); return 1; }
    FILE* fo = fopen(part.c_str(), "wb"); bool ok = fo != nullptr;
    if (ok) { uint8_t h[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}; for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(riff >> (8 * i)); ok = fwrite(h, 1, 12, fo) == 12; }
    for (size_t i = 0; ok && i < keep.size(); i++) { ok = copyBody(fi, keep[i], fo); if (ok && i == 0 && !tc.empty()) ok = fwrite(tc.data(), 1, tc.size(), fo) == tc.size(); }
    if (fo) ok = fclose(fo) == 0 && ok;
    if (ok) { FILE* fc = fopen(part.c_str(), "rb"); std::vector<Ck> c2; std::string w2;
        ok = fc && walk(fc, c2, w2) && find(c2, "data") && find(c2, "LAE ") && sameBody(fi, *dat, fc, *find(c2, "data")) && sameBody(fi, *lae, fc, *find(c2, "LAE ")); if (fc) fclose(fc);
        if (!ok) printf("check FAILED: the new file's CD audio or LAE chunk differs from the input's\n"); }
    else printf("cannot write %s\n", part.c_str());
    fclose(fi);
    if (ok) { std::filesystem::rename(std::filesystem::u8path(part), std::filesystem::u8path(out), ec); ok = !ec; if (!ok) printf("cannot rename %s to %s\n", part.c_str(), out.c_str()); }
    if (!ok) { std::filesystem::remove(std::filesystem::u8path(part), ec); printf("nothing written\n"); return 2; }
    printf("written: %s (%zu tag(s), %zu picture(s)); CD audio and LAE chunk byte-identical to %s\n", out.c_str(), tags.kv.size(), tags.pics.size(), in.c_str()); return 0; }
}  // namespace m_tag

// Extension packs (2026-11-10, spec/lossy/SPEC.md, Section 5.6; one core and several mutually independent masters):
// master2=<master.wav> .. master9= each add one further extension to the file, computed as a whole from that
// master against the same core (the CD layer / core layer) by this encoder's own cd= path - its alignment (master<n>align=<offset>, as
// cdalign=; without it both searches run and the encoder stops), its own cut-off, depth layer with its own leak compensation, and
// frequency layer. master<n>accept = cdaccept and master<n>edge=cut = cdedge=cut for that master; master<n>name= is the name shown where
// a pack is chosen (master1name= names the main body, the first master: one optional "LNAM" record after the last pack record). A pack is exactly the "LAE " payload that "lae_enc <master n> cd=<the core>" writes; it goes into one record
// after the main body's last stream (lae_decoder.h), where v5 decoders do not read, so they play the core and the main body as before.
// Nothing is shared between packs and nothing in one depends on another; the core must come out of every pack's
// alignment unchanged, sample for sample. Without these options nothing here runs and the output is the earlier one, byte for byte.
struct PackOpt { std::string path, align, name, edge; bool accept = false, any = false; };
static bool packOptArg(const char* a) { return !strncmp(a, "master", 6) && a[6] >= '2' && a[6] <= '9' && (a[7] == '=' || !strncmp(a + 7, "align=", 6) || !strncmp(a + 7, "name=", 5) || !strncmp(a + 7, "edge=", 5) || !strcmp(a + 7, "accept")); }
static std::string packOptParse(const char* a, PackOpt* pk) {   // pk[0] = master2 .. pk[7] = master9; "" or the reason
    PackOpt& q = pk[a[6] - '2']; const char* r = a + 7; const std::string key = std::string(a, 7); q.any = true;
    if (*r == '=') { if (!r[1]) return key + "=: give the master's file"; q.path = r + 1; return ""; }
    if (!strncmp(r, "align=", 6)) { if (!r[6]) return key + "align=: give the offset, as for cdalign="; q.align = r + 6; return ""; }
    if (!strncmp(r, "name=", 5)) { if (strlen(r + 5) > 255) return key + "name=: a name has at most 255 bytes"; q.name = r + 5; return ""; }
    if (!strncmp(r, "edge=", 5)) { if (strcmp(r + 5, "cut") && strcmp(r + 5, "pad")) return key + "edge=: pad or cut, as for cdedge="; q.edge = r + 5; return ""; }
    q.accept = true; return ""; }

static int encode(int argc, char** argv) {
    if (argc < 3) { printf("usage: lae_enc <master.wav> <out.wav | out.flac | out.lae> [mode: max | 1 | 3 | 5 | keep | keep20, default 3] [s1 | s2, default s1] [zs] [cd=<independent CD.wav>] [energy] [equal] [cdcheck] [cdalign=<offset>] [cdaccept] [cdedge=pad | cdedge=cut] [bits=24] [mem=<GB>] [noise=<marks.laenoise>] [g | g=b | g=c | g=d] [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]\n"); return 1; }
    auto T0 = std::chrono::steady_clock::now();
    const std::string outPath = argv[2], partPath = outPath + ".part", tmp = outPath + ".tmp";
    encrun::out = outPath; encrun::part = partPath; encrun::tmp = tmp; encrun::hadOld = exists(outPath);
    auto oldNote = []() { encrun::oldNote(); };
    if (!mdct::selftestOK() || !alignhash::md5SelfTest() || encio::testFail("selftest")) { printf("encoder self-test FAILED: this build does not compute the IMDCT in the IEEE-754 order the format requires, or its MD5 is wrong (see docs/BUILD.md); nothing encoded\n"); return 4; }
    // the mode is the third argument and may be left out (default 3): a third argument that is no mode name is read as the first option
    const int opt0 = argc > 3 && part::modeIndex(argv[3]) < 0 ? 3 : 4; const char* modeName = opt0 == 4 && argc > 3 ? argv[3] : "3"; const int mi = part::modeIndex(modeName);
    const char* noisePath = nullptr; bool gOpt = false; char gLevel = 'd';   // noise=<marks.laenoise>, g = g=d / g=c / g=b (2026-11-04/05)
    bool equal = false, zsOpt = false, cdCheck = false, cdAccept = false, bits24 = false, manual = false, resume = false; int wm = 0, scheme = 1, cdEdge = -1; const char* cdPath = nullptr; long long manualT = 0; unsigned long long memBudget = 0;   // mem=<GB>: memory budget for coding two layers at once (default: half the physical memory); cdEdge: cdedge=pad / cut (m_align::EDGE_*), -1 not given
    std::string manualText; int manualSg = 1; long long manualI = 0, manualF = 0, manualFs = 1, manualPlace = 0, manualPhi = 0;   // cdalign=<o>: o = sg * (I + F / Fs) master samples
    double manualMove = 0;   // the fraction (master samples, |f| <= 1/2) the master is moved by when it is placed at manualT
    PackOpt pk[8]; int nPacks = 0;   // extension packs: master2= .. master9=
    std::string mainName;   // master1name=<text>: the name of the main body (the first master), "" = none stored
    // internal steps of the extension packs (each a run of this encoder inside the calling run): alignout=<file> = align this master to
    // cd=, hand the aligned master to the calling run and encode nothing; rawin=<rate> = argument 1 is such an aligned master
    const char* alignOut = nullptr; int rawRate = 0; long cdAddLead = 0, cdAddTail = 0;   // cdAdd*: the digital-silence frames the alignment added before / after the given CD
    laestag::Tags tags; std::vector<uint8_t> tagChunks;   // --tag / --cover / --picture: read and checked before any work
    const int formSel = outputForm(outPath, argc, argv, opt0); if (formSel < 0) { printf("nothing encoded\n"); return 1; } const bool asFlac = formSel == 1;
    { const std::string e = tagArgs(argc, argv, opt0, tags); if (!e.empty()) { printf("%s: nothing encoded\n", e.c_str()); return 1; }
      tagChunks = asFlac ? laestag::flacBlocks(tags, false, kLaeyEncoder) : laestag::wavChunks(tags, kLaeyEncoder); }   // FLAC form: VORBIS_COMMENT + PICTURE blocks, before the "LAE1" blocks
    for (int i = opt0; i < argc; i++) { const char* a = argv[i]; if (isTagOpt(a) || isFormOpt(a)) continue;
        if (!strncmp(a, "master1name=", 12)) { if (strlen(a + 12) > 255) { printf("master1name=: a name has at most 255 bytes\n"); return 1; } mainName = a + 12; continue; }   // the main body's name, shown where a master is chosen
        if (!strcmp(a, "equal")) equal = true; else if (!strcmp(a, "energy")) wm = 1; else if (!strcmp(a, "s2")) scheme = 2; else if (!strcmp(a, "s1")) scheme = 1;
        else if (!strcmp(a, "zs")) zsOpt = true; else if (!strncmp(a, "cd=", 3) && a[3]) cdPath = a + 3; else if (!strcmp(a, "cdcheck")) cdCheck = true;
        else if (!strcmp(a, "cdaccept")) cdAccept = true; else if (!strcmp(a, "cdedge=pad")) cdEdge = m_align::EDGE_PAD; else if (!strcmp(a, "cdedge=cut")) cdEdge = m_align::EDGE_CUT; else if (!strcmp(a, "bits=24")) bits24 = true; else if (!strcmp(a, "resume")) resume = true;
        else if (!strncmp(a, "noise=", 6) && a[6]) noisePath = a + 6; else if (!strcmp(a, "g") || !strcmp(a, "g=d")) { gOpt = true; gLevel = 'd'; } else if (!strcmp(a, "g=c")) { gOpt = true; gLevel = 'c'; } else if (!strcmp(a, "g=b")) { gOpt = true; gLevel = 'b'; }
        else if (!strncmp(a, "alignout=", 9) && a[9]) alignOut = a + 9; else if (!strncmp(a, "rawin=", 6) && atoi(a + 6) > 0) rawRate = atoi(a + 6);
        else if (packOptArg(a)) { const std::string e = packOptParse(a, pk); if (!e.empty()) { printf("%s: nothing encoded\n", e.c_str()); return 1; } }   // extension packs (2026-11-10)
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
        else if (i == 3) { printf("mode must be max, 1, 3, 5, keep or keep20\n"); return 1; }   // the mode slot: neither a mode nor an option
        else { printf("unknown option '%s': nothing encoded (options: s1 s2 zs energy equal cd=<CD.wav> cdcheck cdalign=<offset> cdaccept cdedge=pad cdedge=cut bits=24 mem=<GB> noise=<marks.laenoise> g g=b g=c g=d master2=<master.wav> .. master9= master<n>align= master<n>name= (n from 1) master<n>accept master<n>edge= --tag=KEY=VALUE --cover=<image> --picture=<type>:<image> resume)\n", a); return 1; } }
    encrun::keepTmp = resume;
    if ((cdCheck || cdAccept || manual || cdEdge >= 0) && !cdPath) { printf("cdcheck, cdalign, cdaccept and cdedge= need cd=<CD.wav>\n"); return 1; }
    for (int k = 0; k < 8; k++) { if (!pk[k].any) continue;
        if (pk[k].path.empty()) { printf("master%d...: master%d=<master.wav> is missing: nothing encoded\n", k + 2, k + 2); return 1; }
        if (k != nPacks) { printf("master%d=: the further masters are numbered without a gap (master%d= is missing): nothing encoded\n", k + 2, nPacks + 2); return 1; }
        nPacks++; }
    if (alignOut && (!cdPath || nPacks || resume || cdCheck)) { printf("alignout= is an internal step of the extension packs: nothing encoded\n"); return 1; }
    if (nPacks && (noisePath || resume || cdCheck)) { printf("further masters (master2= ...) cannot be combined with noise=, resume or cdcheck in this version: nothing encoded\n"); return 1; }
    if (mi >= part::MODE_KEEP) wm = 2; if (mi == 0) wm = 1;
    const bool partLeft = exists(partPath);   // with resume, decided once the work folder's notes are read (below)
    if (partLeft && !resume) { printf("%s already exists and was not created by this run: remove it or choose another output name; nothing encoded\n", partPath.c_str()); return 1; }
    const std::string marker = (std::filesystem::path(tmp) / "lae_enc.tmp").string();
    if (exists(tmp) && !exists(marker)) { printf("%s already exists and is not an lae_enc work folder: remove it or choose another output name; nothing encoded\n", tmp.c_str()); return 1; }
    clearModuleEnv();   // opt-in diagnostics excepted
    env("ZSIGN", zsOpt ? "1" : "");   // opt-in zero level with true signs (partition.h)
    env("RULE_F", scheme == 2 ? "2" : "1"); env("RULE_D", mi == part::MODE_KEEP20 ? "3" : "1");   // precision rules (partition.h); recorded in every stream header
    if (mi == part::MODE_KEEP20 && !cdCheck) printf("NOTE keep20: the depth layer is coded at 20 bit instead of 18 bit. The CD band gets about 13 dB closer to the master\n"
                                        "  (about -124 instead of -111 dBFS error), which is very unlikely to be audible at normal listening levels; the file grows by about\n"
                                        "  84 MB per hour of music (44.1 kHz family; about 94 MB for 48 kHz), i.e. about 9 minutes less music per CD-Extra disc.\n");   // as the layer module forces it (keep: cells AND energy; max: energy), so the header and the report say what was used
    // only the header is read here; the samples go straight to m0.raw once the work folder exists (the master is never held whole in memory)
    wavin::Info wi;
    wavio::Wav w;
    if (rawRate > 0) { const long long rb = fileSize(argv[1]); if (rb < 8) { printf("rawin=: the aligned master is missing\n"); return 2; }   // internal: an aligned master of this run (stereo int32, 24 bit left-aligned)
        wi.rate = rawRate; wi.ch = 2; wi.bits = 24; wi.dataBytes = rb / 8 * 6; }
    else if (!(wavin::probe(argv[1], wi) && wi.ch == 2)) {
        if (wi.badLen) printf("the header of %s gives a data length of 2^63 bytes or more: the file is damaged or not a valid %s file; nothing encoded\n", argv[1], wi.kind.c_str());
        else printf("need a stereo integer PCM WAV\n");
        oldNote(); return 1; }
    longHeaderNote(argv[1], wi);   // rawin=: nothing (hdrLen is 0)
    w.rate = wi.rate; w.ch = wi.ch; w.bits = wi.bits;
    std::string note32;
    if (w.bits == 32) {   // LAE v5 carries at most 24 bit: say what reducing the master would drop, then end or continue as the user chooses
        long long lost = wavin::low8(argv[1], wi);
        if (lost == 0) note32 = "input 32 bit with 24-bit content (nothing dropped)";
        else {
            printf("NOTE: the master is a 32-bit file. LAE v5 carries at most 24 bit, so the lowest 8 bits would be dropped (%s).\n",
                   lost > 0 ? (std::to_string(lost) + " samples carry information there").c_str() : "their content could not be checked");
            if (!bits24) {
                if (!interactive()) { printf("  nothing encoded. To encode with 24-bit output, add the option bits=24.\n"); oldNote(); return 1; }
                printf("  1 = end the task (nothing is written)\n  2 = encode with 24-bit output (the top 24 bits are kept)\n  choose 1 or 2: "); fflush(stdout);
                char line[32] = {0}; if (!fgets(line, sizeof line, stdin) || line[0] != '2') { printf("task ended: nothing encoded\n"); oldNote(); return 1; } }
            note32 = "input 32 bit, encoded as 24 bit"; } }
    const double base = fmod((double)w.rate, 44100.0) == 0 ? 44100.0 : 48000.0; int stages = (int)lround(lmath::log2(w.rate / base));
    if (stages < 1 || stages > 3 || fabs(base * (1 << stages) - w.rate) > 0.5) { printf("rate %d is not 2/4/8 x 44.1 or 48 kHz\n", w.rate); oldNote(); return 1; }
    if (manual) {   // cdalign=<o>: parsed as o = manualPlace - manualPhi / U (U units per master sample), then the nearest whole offset and the fraction
        const long long Ux = lae_fd::unitsPerMaster(stages);
        if (manualF == 0) { manualPlace = manualSg * manualI; manualPhi = 0; }
        else { const long long rest = manualSg > 0 ? manualFs - manualF : manualF; manualPlace = manualSg > 0 ? manualI + 1 : -manualI; manualPhi = (rest * Ux + manualFs / 2) / manualFs;
               if (manualPhi >= Ux) { manualPlace--; manualPhi = 0; } }
        manualT = manualPhi * 2 > Ux ? manualPlace - 1 : manualPlace;   // the whole offset nearest to o: where the first check runs and the master is placed
        manualMove = (double)(manualT - manualPlace) + (double)manualPhi / (double)Ux; }   // and the fraction it is moved by: o = manualT - manualMove
    const int P = 4096;   // mirror padding at the master rate (decoder trims it)
    long long Lm = wi.dataBytes / (wi.bits / 8) / 2;   // changes only when the master is placed on an independent CD's time line
    auto lengthOk = [&](const char* what) {   // length limit: the v5 header (decoder check: Lm <= 2,000,000,000 - 2P); every module indexes the master with 64-bit positions
        if (Lm < (long long)w.rate) { printf("%s is shorter than 1 second: not supported\n", what); oldNote(); return false; }
        long long maxFrames = 2000000000LL - 2LL * P; const char* who = "the v5 format"; const char* hint = "";
        if (const char* t = getenv("LAE_TESTMAXFRAMES")) { long long v = atoll(t); if (v > 0) maxFrames = std::min(maxFrames, v); }   // test hook: can only lower the limit
        if (Lm > maxFrames) { printf("%s is %.2f minutes long; at %d Hz %s allows at most %.2f minutes%s: nothing encoded\n", what, Lm / (60.0 * w.rate), w.rate, who, maxFrames / (60.0 * w.rate), hint); oldNote(); return false; }
        return true; };
    if (!lengthOk("the master")) return 1;
    nmk::Marks noiseMk; const long long LmIn = Lm;   // noise=: the marking file belongs to this master (SHA-256, length, rate) and lies inside its grid
    if (noisePath) {
        { std::string why; if (!nmk::readMarks(noisePath, noiseMk, &why)) { printf("cannot read the marking file %s (%s): nothing encoded\n", noisePath, why.c_str()); oldNote(); return 1; } }
        if (noiseMk.frames != Lm || fabs(noiseMk.rate - w.rate) > 0.5) { printf("the marking file %s is for another master (length or rate differ): nothing encoded\n", noisePath); oldNote(); return 1; }
        WavStream nws; if (!nws.open(argv[1])) { printf("cannot read %s for the SHA-256 check of noise=: nothing encoded\n", argv[1]); oldNote(); return 1; }
        if (nmk::masterSha(nws) != noiseMk.sha) { printf("the marking file %s is for another master (SHA-256 differs): nothing encoded\n", noisePath); oldNote(); return 1; }
        const long Kn = mdct::frames((long)Lm);
        for (size_t i = 0; i < noiseMk.pos.size(); i++) { const nmk::Pos& p = noiseMk.pos[i];
            if (p.m0 < 0 || p.m1 > Kn || p.m0 >= p.m1 || p.c0 < 0 || p.c1 > nmk::NCOL || p.c0 >= p.c1) { printf("the marking file %s: position %zu lies outside the grid: nothing encoded\n", noisePath, i + 1); oldNote(); return 1; } }
        printf("noise: %zu position(s) from %s (the master's SHA-256 matches); the marked cells are deleted in the frequency layer\n", noiseMk.pos.size(), noisePath); }
    if (gOpt) printf("g: on, strength %c (%s) - deleted or silent A cells code -G where G lies above the 24-bit rounding floor (docs/NOISE.md)\n",
                     gLevel, gLevel == 'd' ? "step 1, down to the 24-bit floor" : gLevel == 'c' ? "C steps: block peak - 4 bit" : "B steps: block peak - 2 bit");
    auto fitsLength = [&](long long n) { long long mf = 2000000000LL - 2LL * P;   // the limits of lengthOk, without its messages
        if (const char* e = getenv("LAE_TESTMAXFRAMES")) { long long v = atoll(e); if (v > 0) mf = std::min(mf, v); }
        return n >= (long long)w.rate && n <= mf; };
    { std::error_code ec; bool made = std::filesystem::create_directories(tmp, ec); if (ec || !exists(tmp)) { printf("cannot create the work folder %s: nothing encoded\n", tmp.c_str()); oldNote(); return 2; }
      if (made || exists(marker)) encrun::tmpMine = true; encio::writeText(marker.c_str(), "lae_enc work folder (removed at the end of each run)\n"); }
    auto T = [&](const std::string& n) { return (std::filesystem::path(tmp) / n).string(); };
    struct Guard { ~Guard() { encrun::cleanup(); } } guard;   // removes the work folder always, the .part on failure
    // disk (2026-11-05): the work folder's largest size, sampled every 0.25 s while the encoder runs and printed at the end (joined before the
    // work folder is removed: declared after the guard)
    struct DiskPoll { std::atomic<bool> stop{false}; std::atomic<unsigned long long> peak{0}; std::thread th;
        void start(const std::string& dir) { th = std::thread([this, dir]() { while (!stop) { unsigned long long s = 0; std::error_code ec;
            for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) { std::error_code e2; const auto n = it->file_size(e2); if (!e2) s += n; }
            if (s > peak) peak = s; std::this_thread::sleep_for(std::chrono::milliseconds(250)); } }); }
        ~DiskPoll() { stop = true; if (th.joinable()) th.join(); } } diskPoll;
    try { diskPoll.start(tmp); } catch (...) {}   // no thread: no measurement, the encoding is the same
    auto spaceOk = [&](unsigned long long reuse = 0) {   // free space for the intermediates + the output; reuse = work files about to be overwritten
        // bytes per padded master frame (2026-11-05, measured with the work folder sampled while encoding: 28 plain, 29 with cd=, 32 with g on 15-s excerpts, 37 on
        // Grand (10 min, two layers at once); about 20 for cdcheck); the margins cover the alignment's copies of the master with cd= and the blocks of two layers coded at once
        std::error_code ec; auto sp = std::filesystem::space(tmp, ec); const double perFrame = (cdCheck ? 45.0 : (cdPath ? 55.0 : 45.0)) + (gOpt && !cdCheck ? 12.0 : 0.0);
        if (!ec) sp.available += reuse;
        unsigned long long needB = (unsigned long long)((Lm + 2 * P) * perFrame) + (64ull << 20), hardB = (unsigned long long)((Lm + 2 * P) * perFrame * 0.85);   // stop below the measured peak
        if ((!ec && sp.available < hardB) || encio::testFail("diskspace")) { printf("not enough free disk space next to %s: about %.0f MB needed, %.0f MB free: nothing encoded\n", outPath.c_str(), needB / 1e6, ec ? 0.0 : sp.available / 1e6); oldNote(); return false; }
        if (!ec && sp.available < needB) printf("WARNING: free disk space is tight (%.0f MB free, about %.0f MB needed); every write is checked, so a full disk stops the run cleanly\n", sp.available / 1e6, needB / 1e6);
        return true; };
    if (!spaceOk()) return 2;
    std::string masterMd5;   // MD5 of the master's data bytes, computed once while reading
    if (rawRate > 0) { std::error_code ec; std::filesystem::copy_file(argv[1], T("m0.raw"), std::filesystem::copy_options::overwrite_existing, ec); if (ec) encio::fail(std::string("cannot copy ") + argv[1] + ": " + ec.message()); }
    else { long long fr = 0, l8 = 0; wavin::toRaw(argv[1], wi, T("m0.raw").c_str(), fr, l8, masterMd5);
        if (fr != Lm) encio::fail("internal error: master length"); }
    // resume (block-wise, without cd=): every finished step is noted in resume.txt in the work folder, together with a key made of this
    // encoder build, the options, the block length and the master's MD5. A run with the same key continues after the last finished step;
    // any other key starts from the beginning. The notes are read only by this encoder and never go into the LAE file.
    // With cd= the alignment's result belongs to the key too (2026-11-14): the pad / cut choice can be answered at the console, so it is
    // not among the options. Once the alignment has ended, the placed master's length, its position and that choice are compared with
    // the line "align ..." the earlier run noted; if they differ, no finished step is reused (see below, after the alignment).
    std::set<std::string> doneSteps; std::mutex stepMu; const std::string manifest = T("resume.txt"); std::string resumeHead, oldAlign;
    if (resume) { std::string key = std::string("lae_enc " __DATE__ " " __TIME__ " | options");   // not the block length: it does not change any result
        for (int i = 3; i < argc; i++) if (strcmp(argv[i], "resume") && !isTagOpt(argv[i]) && !isFormOpt(argv[i]) && strncmp(argv[i], "mem=", 4)) key += std::string(" ") + argv[i];   // tags and the form: written with the file only; mem=: only the block length, which changes no result (FU07-9)
        key += " | master " + masterMd5;
        if (noisePath) { const std::string t = encio::readText(noisePath); uint64_t h = 1469598103934665603ull; for (unsigned char ch : t) { h ^= ch; h *= 1099511628211ull; }   // FNV-1a of the marking file
            char hs[32]; snprintf(hs, sizeof hs, "%016llx", (unsigned long long)h); key += std::string(" | noise ") + hs; }
        if (cdPath) { wavio::Wav cw; key += " | CD " + (wavio::readCD(cdPath, cw) ? alignhash::cdMd5(cw.pcm16.data(), cw.pcm16.size() / 2) : std::string("unreadable")); }   // cd=: the alignment runs again every time; the steps after it continue
        std::string old; if (exists(manifest)) old = encio::readText(manifest.c_str());
        const std::string head = "key " + key + "\n"; resumeHead = head;
        if (!old.empty() && old.compare(0, head.size(), head) == 0) { size_t p = head.size();
            while (p < old.size()) { size_t e = old.find('\n', p); if (e == std::string::npos) break; if (old.compare(p, 5, "done ") == 0) doneSteps.insert(old.substr(p + 5, e - p - 5));
                else if (old.compare(p, 6, "align ") == 0) oldAlign = old.substr(p + 6, e - p - 6); p = e + 1; }
            printf("resume: continuing the earlier run (%zu step(s) already done)\n", doneSteps.size()); }
        else { if (!old.empty()) printf("resume: the work folder belongs to another run (other master, options or encoder build): starting from the beginning\n");
            encio::writeText(manifest.c_str(), head); } }
    if (partLeft) {   // resume (2026-11-05): a run killed after writing its .part leaves it; the run with the same key writes it again
        if (resume && doneSteps.count("part")) { std::error_code ec; std::filesystem::remove(partPath, ec);
            if (ec || exists(partPath)) { printf("cannot remove the unfinished %s of the earlier run: nothing encoded\n", partPath.c_str()); return 1; }
            printf("resume: the unfinished %s of the earlier run is written again\n", partPath.c_str()); }
        else { printf("%s already exists and was not created by this run: remove it or choose another output name; nothing encoded\n", partPath.c_str()); return 1; } }
    auto isDone = [&](const std::string& s) { std::lock_guard<std::mutex> g(stepMu); return doneSteps.count(s) > 0; };
    auto markDone = [&](const std::string& s) { if (!resume) return; std::lock_guard<std::mutex> g(stepMu); doneSteps.insert(s);
        FILE* f = fopen(manifest.c_str(), "ab"); bool okw = f && fprintf(f, "done %s\n", s.c_str()) > 0; if (f && fclose(f)) okw = false;
        if (!okw) encio::fail("cannot write " + manifest); };
    auto derive = [&]() {   // padded master and the derived CD layer (16 bit), 24-bit base and dither flags, all from m0.raw
        if (call(m_padblk::main, {"pad", T("m0.raw"), T("m.raw"), std::to_string(P)})) { printf("padding failed\n"); oldNote(); return false; }
        env("DITHERFLAGS", T("dither.flags")); env("BASE24", T("b24.s32"));
        if (call(m_baseblk::main, {"base", T("m.raw"), std::to_string(w.rate), std::to_string((int)base), T("b16.s16")})) { printf("CD layer derivation failed\n"); oldNote(); return false; } env("BASE24", "");
        return true; };
    auto stamp = [&](const char* w) { if (getenv("TIMING")) printf("[enc %.1f s] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count(), w); };
    if (!cdPath && isDone("derive")) { printf("resume: CD layer derivation already done\n"); env("DITHERFLAGS", T("dither.flags")); }   // the second route split reads the dither flags derive() names (fixed 2026-11-05: a resumed run gave another depth layer)
    else { if (!derive()) return 2; if (!cdPath) markDone("derive"); }   // cd=: the alignment may place and move the master and derive again
    stamp("padding + CD layer derived");
    // independent CD: the fraction of a master sample is measured in units of 2^-22 CD sample (U1 per master sample) and applied to the master
    // itself when it is placed (2026-10-23); the file stays v5 and the decoder is unchanged
    const int64_t U1 = lae_fd::unitsPerMaster(stages);
    std::string alignNote, alignReport;   // alignReport: the text of <out>.align.txt (the actual move), written only beside the finished file
    std::string alignKey;   // resume with cd=: what the alignment ended with (the placed master's length, the position, pad / cut)
    long long placeT = 0;   // the whole offset the master was placed at (noise= needs 0: the marking was made on the aligned master)
    if (cdPath) {   // independent CD: it becomes the CD layer bit for bit; the extension codes master - CD. No dither of ours to regenerate.
        wavio::Wav c; if (!wavio::readCD(cdPath, c) || c.bits != 16 || c.ch != 2 || c.rate != (int)base) { printf("independent CD must be a 16-bit stereo WAV at %d Hz\n", (int)base); oldNote(); return 1; }
        longHeaderNote(cdPath);
        std::vector<uint8_t> b = slurp(T("b16.s16")); long nb0 = (long)b.size() / 4, pb0 = P >> stages, need = nb0 - 2 * pb0, have = (long)c.pcm16.size() / 2;
        auto lengthMsg = [&]() { printf("independent CD has %ld samples, the master needs %ld at %d Hz: the lengths do not match; nothing encoded\n", have, need, (int)base); };
        {   // alignment (design D and A, 2026-10-22 / 10-23): with cdalign=<offset> the position is the user's: check in three blocks, place the
            // master where needed, check again, and the remaining time difference decides whether the encoder goes on, asks, or stops. Without it
            // (2026-10-23) the search runs whitened AND plain; each result goes through the same
            // phase measurement, placement and re-check as a hand-given offset would (nothing is encoded), both are reported, and the encoder stops:
            // the user gives the position by hand. The two results are never compared or chosen between
            using m_align::detail::fmt;
            std::vector<int32_t> b24 = encio::readAll<int32_t>(T("b24.s32").c_str());
            // the master is m0.raw in the work folder, read through blkio::View windows; every step that changes it (placement, the digital
            // silence around the move, the move, the cut of that silence) writes m0.new piece by piece, which then replaces m0.raw
            const std::string mFile = T("m0.raw"), mNew = T("m0.new");
            auto masterReplace = [&]() { std::error_code ec; std::filesystem::rename(mNew, mFile, ec); if (ec) encio::fail("cannot replace " + mFile + ": " + ec.message()); };
            auto masterRange = [&](long long a, long long b, long long padA, long long padB) {   // the new master: padA zero frames, frames [a, b) of the master, padB zero frames
                { const blkio::View m(mFile); blkio::Writer wr(mNew.c_str()); std::vector<int32_t> z((size_t)1 << 19, 0), x; const long long BS = 1LL << 18;
                  for (long long k = 0; k < padA; k += BS) wr.put(z.data(), (size_t)(std::min(BS, padA - k) * 2));
                  for (long long i = a; i < b; i += BS) { const long long e = std::min(b, i + BS); x.resize((size_t)(e - i) * 2); for (long long j = i; j < e; j++) { x[(size_t)(j - i) * 2] = m[(size_t)j * 2]; x[(size_t)(j - i) * 2 + 1] = m[(size_t)j * 2 + 1]; } wr.put(x); }
                  for (long long k = 0; k < padB; k += BS) wr.put(z.data(), (size_t)(std::min(BS, padB - k) * 2));
                  wr.finish(); }
                masterReplace(); };
            auto offText = [&](long long tp, double mv) { return fmt("%+.7f", (double)tp - mv); };   // o = whole offset - the master's move (7 decimals: finer than 1/524288)
            // one attempt at one position (the checks of the hand-given path). sch: "" by hand, else the search's name (output prefix, nothing kept).
            // kind: 0 aligned, 1 asks (-40..-33 dB or beyond), 2 stops (the reason in why), 3 fatal (derivation, disk); rep / placed: the final check
            struct Att { int kind = 2; std::string why; };
            m_align::Report rep; std::string placed; long padP = 0, cutL = 0, cutT = 0; long long rpT = 0; double rpMv = 0;   // the last attempt, for the report
            m_align::Place edgeQ; bool edgeAsk = false;   // the last placement, and whether the master has sound beyond the CD (needs cdedge=pad / cut; 2026-11-03)
            // a master whose speed drifts against the CD (tape wow and flutter): never continued on its own; with the position given by
            // hand AND cdaccept it goes on like a difference beyond -33 dB (the extension codes the whole difference, the precision stays the same)
            auto driftConfirmed = [&](m_align::Report& r) { if (manual && !r.found && (r.tag == "drift" || r.tag == "edit")) { r.found = true;   /* "edit": some check blocks do not match at all; no block matching ("nomatch") is never continued */ r.band = m_align::BAND_CONFIRM_LARGE;   // then asked like every difference beyond -33 dB
                // the text names which of the two the check found (2026-11-14); what happens next is the same for both
                printf(r.tag == "edit" ? "align: WARNING: some check blocks of the master do not match the CD at this position at all (an edit, another take or another version of the recording): one position cannot fit the whole piece. Continuing needs your confirmation (cdaccept, or the answer at the console)\n"
                                       : "align: WARNING: the master's speed drifts against the CD (for example tape wow and flutter): one position cannot fit the whole piece. Continuing needs your confirmation (cdaccept, or the answer at the console)\n"); } };
            auto attempt = [&](const m_align::Report& rep1, const std::string& sch, long long tPlace, double mv, int edge) -> Att {
                Att A; const std::string pre = sch.empty() ? "align: " : "align (" + sch + "): ", key = sch.empty() ? "" : " search=" + sch;
                rep = rep1; placed.clear(); padP = cutL = cutT = 0; rpT = tPlace; rpMv = mv; edgeQ = m_align::Place(); edgeAsk = false;
                if (!rep1.found) { A.why = rep1.why + (rep1.hint.empty() ? "" : "; " + rep1.hint); return A; }
                printf("PHASE t=%lld phi=0 move=%+.7f offset=%s%s\n", tPlace, mv, offText(tPlace, mv).c_str(), key.c_str());
                if (mv != 0) printf("%sthe master is placed at %+lld and moved by %+.7f master samples (its band-limited waveform taken %+.7f samples later: interpolated once, at alignment; the file stays v5)\n",
                                    pre.c_str(), tPlace, mv, mv);
                else printf("%sthe CD lies on a whole master sample: the master is not moved by a fraction\n", pre.c_str());
                const bool placeNeeded = tPlace != 0 || have != need;
                if (placeNeeded || mv != 0) {   // the CD's own samples are never changed: the master moves by whole master samples (a silent CD frame may go in front)
                    const long long t = tPlace; long nLead = 0, nTail = 0;
                    m_align::Place q; { const blkio::View m(mFile); q = m_align::place(m, Lm, have, stages, t, w.rate, edge); } edgeQ = q; edgeAsk = q.onlyLead || q.onlyTail;
                    auto placeLine = [&](const char* result) { printf("PLACE mode=%s t=%lld drop_lead=%lld drop_tail=%lld pad_lead=%lld pad_tail=%lld cd_zero_lead=%d cd_only_lead=%ld cd_only_tail=%ld lm=%lld lm2=%lld result=%s%s\n",
                                                                      sch.empty() ? "manual" : "auto", q.t, q.dropLead, q.dropTail, q.padLead, q.padTail, q.cdLead, nLead, nTail, q.Lm, q.Lm2, result, key.c_str()); };
                    const bool tentative = edgeAsk && cdEdge < 0;   // the search goes on as with pad, but nothing is chosen yet
                    for (auto& l : q.lines) printf("%s%s\n", pre.c_str(), (tentative && l.rfind("cdedge=pad: ", 0) == 0 ? "with cdedge=pad (not chosen yet): " + l.substr(12) : l).c_str());
                    std::string bad = q.ok ? std::string() : q.why; const bool moved = q.ok && (q.shift != 0 || q.Lm2 != Lm);
                    if (bad.empty() && q.cdLead) { c.pcm16.insert(c.pcm16.begin(), 2 * (size_t)q.cdLead, (int16_t)0); have = (long)c.pcm16.size() / 2; cdAddLead += q.cdLead; }
                    if (bad.empty() && q.cdTail) { c.pcm16.insert(c.pcm16.end(), 2 * (size_t)q.cdTail, (int16_t)0); have = (long)c.pcm16.size() / 2; cdAddTail += q.cdTail; }   // cdedge=pad at the end
                    if (bad.empty() && moved) {   // cheap stops first, before anything is resized, written or derived
                        const std::string far = m_align::cdOnlyFar(c.pcm16, have, q, stages, (int)base, pb0);
                        if (!far.empty()) bad = far; else if (!fitsLength(q.Lm2)) bad = fmt("the placed master (%lld frames) is outside this encoder's length limits", q.Lm2); }
                    if (!bad.empty()) { placeLine("stop"); A.why = bad; return A; }   // not possible here; no other offset is tried in its place
                    if (moved || mv != 0) {
                        unsigned long long reuse = 0; for (const char* f : {"m0.raw", "m.raw", "b16.s16", "b24.s32", "dither.flags"}) { long long s = fileSize(T(f)); if (s > 0) reuse += (unsigned long long)s; }   // derive() overwrites these
                        if (moved) { { const blkio::View m(mFile); m_align::applyPlaceFile(m, q, mNew); } masterReplace(); Lm = q.Lm2; }   // M' replaces the master; the CD layer, base and dither flags are derived again from it
                        if (mv != 0) {   // the fraction (2026-10-23): digital silence at both ends first, so that what the move carries past the master's first /
                            // last sample is kept; afterwards only what is still digital silence is removed again: at most the added frames, never a frame of
                            // the given CD. Nothing about it is stored anywhere; decoders never discard a sample.
                            const long Pcd = 128; const long long up1 = 1LL << stages, Pm = (long long)Pcd * up1;   // Pcd: CD frames of digital silence at each end (not the mirror padding P)
                            c.pcm16.insert(c.pcm16.begin(), 2 * (size_t)Pcd, (int16_t)0); c.pcm16.insert(c.pcm16.end(), 2 * (size_t)Pcd, (int16_t)0); have = (long)c.pcm16.size() / 2;
                            masterRange(0, Lm, Pm, Pm); Lm += 2 * Pm;
                            long nclip = 0; { const blkio::View m(mFile); m_align::moveMasterFile(m, Lm, mv, mNew, nclip); } masterReplace();   // then the fraction: the placed master's waveform taken mv later
                            if (nclip) printf("%sWARNING: %ld samples of the moved master exceed 24-bit full scale and are hard-clipped\n", pre.c_str(), nclip);
                            long kL = 0, kT = 0;
                            { const blkio::View m(mFile); auto silent = [&](long long a) { for (long long i = a; i < a + up1; i++) if (m[(size_t)i * 2] || m[(size_t)i * 2 + 1]) return false; return true; };
                              while (kL < Pcd && silent((long long)kL * up1)) kL++; while (kT < Pcd && silent(Lm - (long long)(kT + 1) * up1)) kT++; }
                            c.pcm16.erase(c.pcm16.end() - 2 * (size_t)kT, c.pcm16.end()); c.pcm16.erase(c.pcm16.begin(), c.pcm16.begin() + 2 * (size_t)kL); have = (long)c.pcm16.size() / 2;
                            if (kL || kT) masterRange((long long)kL * up1, Lm - (long long)kT * up1, 0, 0); Lm -= (long long)(kL + kT) * up1;
                            padP = Pcd; cutL = kL; cutT = kT; cdAddLead += Pcd - kL; cdAddTail += Pcd - kT;
                            printf("%sdigital silence: %ld CD frames added at each end before the move; removed again where still digital silence: start %ld, end %ld (kept: start %ld, end %ld)\n",
                                   pre.c_str(), Pcd, kL, kT, Pcd - kL, Pcd - kT); }
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
                    m_align::Report rep2; { const blkio::View m(mFile); rep2 = m_align::run(c.pcm16, have, b24, nb0, pb0, m, (long)Lm, (int)base, stages, t, "", 0, -1, &zero); } driftConfirmed(rep2);
                    for (auto& l : rep2.lines) printf("%sre-check: %s\n", pre.c_str(), l.c_str());
                    printf("ALIGN_RECHECK%s%s\n", rep2.alignLine.substr(5).c_str(), key.c_str());
                    if (mv != 0 && rep2.hint.rfind("still shifted", 0) == 0)   // the suggestion keeps the fraction
                        rep2.hint = fmt("still shifted by %+ld master samples: try cdalign=%s", rep2.bestNear, offText(t + rep2.bestNear, mv).c_str());
                    if (!rep2.found) { A.why = fmt("the placement at %+lld master samples%s does not align: %s", t, mv != 0 ? fmt(" with the master moved by %+.7f", mv).c_str() : "", rep2.why.c_str()) + (rep2.hint.empty() ? "" : "; " + rep2.hint); return A; }
                    const std::string phs = mv != 0 ? fmt("moved by %+.7f master samples", mv) : std::string();
                    std::string layer = !(q.cdLead || q.cdTail) ? std::string() : (q.cdLead > 1 || q.cdTail
                        ? fmt("; CD layer = %d frame(s) of digital silence + the given CD + %ld frame(s) of digital silence", q.cdLead, q.cdTail) : std::string("; CD layer = one digital-silence frame + the given CD"));
                    if (q.cutLead || q.cutTail) layer += "; master CUT to the CD: " + (q.cutLead ? m_align::detail::dur(q.cutLead / (double)w.rate) + " of its sound at the start" : std::string())
                                                   + (q.cutLead && q.cutTail ? " and " : "") + (q.cutTail ? m_align::detail::dur(q.cutTail / (double)w.rate) + " at the end" : std::string()) + " not kept";
                    placed = placeNeeded ? fmt(" (master placed %s at %+lld master samples%s, then checked again%s)", sch.empty() ? "by hand" : ("by the " + sch + " search").c_str(), t, mv != 0 ? (", " + phs).c_str() : "", layer.c_str())
                                         : fmt(" (master %s, then checked again)", phs.c_str());
                    rep = rep2; }
                if (rep.band == m_align::BAND_STOP) { A.why = rep.why + (rep.hint.empty() ? "" : "; " + rep.hint); return A; }
                A.kind = rep.band == m_align::BAND_OK ? 0 : 1; A.why = rep.why; return A; };
            auto edgeText = [&]() {   // the two choices, said plainly (the user, 2026-11-03: both must be told clearly)
                const m_align::Place& q = edgeQ; const double r = (double)w.rate; std::string s = "the master has sound that the CD does not have:";
                if (q.onlyLead) s += fmt(" %s before the CD starts;", m_align::detail::dur(q.onlyLead / r).c_str());
                if (q.onlyTail) s += fmt(" %s after the CD ends;", m_align::detail::dur(q.onlyTail / r).c_str());
                return s + "\n    cdedge=pad: digital silence is added to the CD layer there, so the master's sound is kept whole; the CD's own samples are unchanged,"
                           " but the CD layer in the LAE file (and on a disc made from it) is that much longer than the given CD"
                           "\n    cdedge=cut: the master is cut where the CD starts and ends; its sound beyond the CD is NOT kept in the LAE file (where it still"
                           " sounds there, the high-resolution playback ends abruptly); the CD layer is exactly the given CD"; };
            if (!manual) {
                { std::error_code ec; std::filesystem::copy_file(mFile, T("m0.orig"), std::filesystem::copy_options::overwrite_existing, ec); if (ec) encio::fail("cannot copy " + mFile + ": " + ec.message()); }   // the given master, for the second search
                const long long Lm0 = Lm; const std::vector<int16_t> cd0 = c.pcm16; const long have0 = have;
                std::string res[2], off[2]; const char* nm[2] = {"whitened", "plain"}; bool edgeAny = false;
                for (int k = 0; k < 2; k++) {
                    if (k) { { std::error_code ec; std::filesystem::copy_file(T("m0.orig"), mFile, std::filesystem::copy_options::overwrite_existing, ec); if (ec) encio::fail("cannot restore " + mFile + ": " + ec.message()); }
                             Lm = Lm0; c.pcm16 = cd0; have = have0; if (!derive()) return 2;   // the second search starts from the given master again
                             b = slurp(T("b16.s16")); nb0 = (long)b.size() / 4; need = nb0 - 2 * pb0; b24 = encio::readAll<int32_t>(T("b24.s32").c_str()); }
                    m_align::Report r; { const blkio::View m(mFile); r = m_align::run(c.pcm16, have, b24, nb0, pb0, m, (long)Lm, (int)base, stages, 0, "", 0, -1, nullptr, k == 0); }
                    for (auto& l : r.lines) printf("align (%s): %s\n", nm[k], l.c_str()); printf("%s search=%s\n", r.alignLine.c_str(), nm[k]);
                    long long tP = r.placeAt; double mvv = 0;   // the whole offset, and the fraction the master is moved by (|mvv| <= 1/2)
                    if (r.found) { std::string phLine; int64_t adv = 0; { const blkio::View m(mFile); adv = m_align::measurePhase(c.pcm16, m, Lm, stages, w.rate, r.placeAt, r, phLine); }
                        printf("align (%s): %s\n", nm[k], phLine.c_str()); mvv = (double)adv / (double)U1; }
                    const Att A = attempt(r, nm[k], tP, mvv, cdEdge >= 0 ? cdEdge : m_align::EDGE_PAD); if (A.kind == 3) return 2;   // without cdedge= the search goes on as with pad, and says that a choice is needed
                    off[k] = r.found ? offText(tP, mvv) : std::string("-");
                    res[k] = A.kind == 0 ? fmt("aligned at cdalign=%s%s", off[k].c_str(), placed.c_str())
                           : A.kind == 1 ? fmt("not aligned within the limit at cdalign=%s: %s", off[k].c_str(), A.why.c_str())
                           : r.found ? fmt("alignment failed at cdalign=%s: %s", off[k].c_str(), A.why.c_str()) : fmt("alignment failed: %s", A.why.c_str());
                    if (edgeAsk && cdEdge < 0) { res[k] += "; and it needs your choice: cdedge=pad or cdedge=cut"; edgeAny = true; }
                    printf("align: %s search: %s\n", nm[k], res[k].c_str()); }
                auto st = [&](int k) { return res[k].rfind("aligned", 0) == 0 ? "aligned" : (res[k].rfind("not aligned", 0) == 0 ? "not_aligned" : "failed"); };
                printf("ALIGN_SUGGEST whitened=%s:%s plain=%s:%s%s\n", st(0), off[0].c_str(), st(1), off[1].c_str(), edgeAny ? " edge=needed" : "");
                if (edgeAny) printf("independent CD: needs your choice: %s\n", edgeText().c_str());
                printf("independent CD: %s; give the position by hand: cdalign=<offset> (whitened search: %s; plain search: %s)\n",
                       cdCheck ? "cdcheck: both searches checked, nothing encoded" : "nothing encoded", res[0].c_str(), res[1].c_str());
                if (cdCheck) return 0; oldNote(); return 1; }
            // by hand
            {   m_align::Report r; { const blkio::View m(mFile); r = m_align::run(c.pcm16, have, b24, nb0, pb0, m, (long)Lm, (int)base, stages, 0, "; the placement given by hand follows", 0, -1, &manualT); } driftConfirmed(r);
                for (auto& l : r.lines) printf("align: %s\n", l.c_str()); printf("%s\n", r.alignLine.c_str());
                printf("align: offset given by hand: cdalign=%s (checked first at the nearest whole offset %+lld)\n", manualText.c_str(), manualT);
                printf("align: fraction given by hand: cdalign=%s is the whole offset %+lld with the master moved by %+.7f master samples; not measured\n",
                       manualText.c_str(), manualT, manualMove);
                Att A = attempt(r, "", manualT, manualMove, cdEdge >= 0 ? cdEdge : m_align::EDGE_STOP); if (A.kind == 3) return 2;
                if (A.kind == 2 && edgeAsk && cdEdge < 0) {   // the master has sound beyond the CD: tell, then the user chooses (2026-11-03)
                    printf("independent CD: needs your choice: %s\n", edgeText().c_str());
                    if (cdCheck) { printf("cdcheck: it would ask: cdedge=pad or cdedge=cut; nothing encoded\n"); return 0; }
                    if (!interactive()) { printf("  nothing encoded. Add cdedge=pad or cdedge=cut to choose.\n"); oldNote(); return 1; }
                    printf("  1 = end the task (nothing is written)\n  2 = pad: add digital silence to the CD layer, keep the master whole\n  3 = cut: cut the master to the CD, its sound beyond the CD is not kept\n  choose 1, 2 or 3: "); fflush(stdout);
                    char line[32] = {0}; if (!fgets(line, sizeof line, stdin) || (line[0] != '2' && line[0] != '3')) { printf("task ended: nothing encoded\n"); oldNote(); return 1; }
                    cdEdge = line[0] == '2' ? m_align::EDGE_PAD : m_align::EDGE_CUT; printf("  chosen: cdedge=%s\n", cdEdge == m_align::EDGE_PAD ? "pad" : "cut");
                    A = attempt(r, "", manualT, manualMove, cdEdge); if (A.kind == 3) return 2; }
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
                placeT = rpT;
                alignKey = fmt("lm=%lld t=%lld move=%+.7f edge=%s cd=%ld lead=%ld tail=%ld", Lm, rpT, rpMv, !edgeAsk ? "none" : (cdEdge == m_align::EDGE_CUT ? "cut" : "pad"), have, cdAddLead, cdAddTail);
                alignReport =fmt("LAE alignment report (independent CD)\nmaster: %s\nCD: %s\noutput: %s\n", argv[1], cdPath, outPath.c_str())
                    + fmt("whole offset: %+lld master samples\n", rpT)
                    + (rpMv != 0 ? fmt("fraction moved: %+.7f master samples (%+.4f microseconds): the master's waveform taken that much later\n", rpMv, rpMv * us) : std::string("fraction moved: none\n"))
                    + fmt("total offset (cdalign): %s master samples (%+.3f microseconds): the CD starts %s than the master\n", offText(rpT, rpMv).c_str(), off * us,
                          off > 0 ? "later" : (off < 0 ? "earlier" : "at the same time"))
                    + (padP ? fmt("digital silence added before the move: %ld CD frames at each end; removed after the move where still digital silence: start %ld, end %ld; kept: start %ld, end %ld CD frames\n",
                                  padP, cutL, cutT, padP - cutL, padP - cutT) : std::string())
                    + (placed.empty() ? std::string() : "placement:" + placed + "\n")
                    + (edgeAsk ? (cdEdge == m_align::EDGE_CUT
                        ? "sound only the master has: CUT (cdedge=cut): " + (edgeQ.cutLead ? m_align::detail::dur(edgeQ.cutLead / (double)w.rate) + " at the start" : std::string()) + (edgeQ.cutLead && edgeQ.cutTail ? " and " : "")
                          + (edgeQ.cutTail ? m_align::detail::dur(edgeQ.cutTail / (double)w.rate) + " at the end" : std::string()) + " of the master's sound are not in this file\n"
                        : fmt("sound only the master has: PADDED (cdedge=pad): the CD layer has %d CD frames of digital silence before and %ld after the given CD; the master is whole\n", edgeQ.cdLead, edgeQ.cdTail)) : std::string())
                    + "check after the move: " + rep.why + "\n" + rep.alignLine + "\n"; } }
        if (have != need) { lengthMsg(); oldNote(); return 1; }
        int16_t* s = (int16_t*)b.data(); for (long i = 0; i < need * 2; i++) s[pb0 * 2 + i] = c.pcm16[i];
        encio::writeVec(T("b16.s16").c_str(), b);
        std::vector<uint8_t> df = slurp(T("dither.flags")); std::fill(df.begin(), df.end(), 0); encio::writeVec(T("dither.flags").c_str(), df);
        printf("independent CD: %s (%ld samples) used as the CD layer\n", cdPath, have); }
    if (resume && cdPath) {   // the alignment's result is part of the resume key: steps finished for another placed master are not reused
        const std::string line = "align " + alignKey + "\n";
        if (oldAlign.empty() && doneSteps.empty()) { FILE* f = fopen(manifest.c_str(), "ab"); bool okw = f && fputs(line.c_str(), f) >= 0; if (f && fclose(f)) okw = false; if (!okw) encio::fail("cannot write " + manifest); }
        else if (oldAlign != alignKey) {
            if (!doneSteps.empty()) printf("resume: the alignment ended with another result than in the earlier run (the placed master's length, its position, or the pad / cut choice): starting from the beginning\n");
            doneSteps.clear(); encio::writeText(manifest.c_str(), resumeHead + line); } }
    if (alignOut) {   // internal step of the extension packs: this master now lies on the given core's time line; hand it to the calling run
        std::error_code ec; std::filesystem::rename(T("m0.raw"), alignOut, ec); if (ec) encio::fail(std::string("cannot write ") + alignOut + ": " + ec.message());
        encio::writeText((std::string(alignOut) + ".report.txt").c_str(), alignReport.empty() ? std::string("\n") : alignReport);
        encio::writeText((std::string(alignOut) + ".txt").c_str(), std::to_string(Lm) + " " + std::to_string(cdAddLead) + " " + std::to_string(cdAddTail) + " " + std::to_string(w.rate) + "\n");
        printf("further master aligned: %lld frames on the core's time line (digital silence added to the core: %ld frame(s) before, %ld after)\n", Lm, cdAddLead, cdAddTail); return 0; }
    // extension packs. First every further master is aligned to the core by the master's own alignment (cd= path: the user's position,
    // placement, the move by the fraction, the checks), one after the other. Where an alignment adds digital silence to the core (sound
    // only that master has with master<n>edge=pad, or what the fractional move carries past a hard-cut edge), the master and the
    // masters aligned before get the same silence, so all lie on one time line and the core is the same for every pack. Then each
    // aligned master is encoded against the final core as a run of its own with nothing shared (position 0, nothing moved again), and
    // its "LAE " payload is kept for the file. All of it before the main body's layers, so that a master not placed yet stops early.
    std::vector<std::vector<uint8_t>> packRecs; std::string packReport; std::vector<uint8_t> mainRec;   // mainRec: the optional "LNAM" record (master1name=)
    if (nPacks) {
        const long pbA = P >> stages; std::vector<int16_t> core; { std::vector<int16_t> b = encio::readAll<int16_t>(T("b16.s16").c_str()); core.assign(b.begin() + 2 * (size_t)pbA, b.end() - 2 * (size_t)pbA); }
        const std::string coreW = T("core.wav");
        auto writeCore = [&]() { std::vector<uint8_t> o; wavio::hdr(o, (int)base, 2, 16, core.size() * 2, 0); o.insert(o.end(), (const uint8_t*)core.data(), (const uint8_t*)core.data() + core.size() * 2); encio::writeVec(coreW.c_str(), o); };
        writeCore();
        auto padRawFile = [&](const std::string& f, long long a, long long b) {   // a zero frames + the file + b zero frames
            const std::string nf = f + ".new"; { const long long n = fileSize(f) / 8; const blkio::View m(f); blkio::Writer wr(nf.c_str()); std::vector<int32_t> z((size_t)1 << 19, 0), x; const long long BS = 1LL << 18;
              for (long long k = 0; k < a; k += BS) wr.put(z.data(), (size_t)(std::min(BS, a - k) * 2));
              for (long long i = 0; i < n; i += BS) { const long long e = std::min(n, i + BS); x.resize((size_t)(e - i) * 2); for (long long j = i; j < e; j++) { x[(size_t)(j - i) * 2] = m[(size_t)j * 2]; x[(size_t)(j - i) * 2 + 1] = m[(size_t)j * 2 + 1]; } wr.put(x); }
              for (long long k = 0; k < b; k += BS) wr.put(z.data(), (size_t)(std::min(BS, b - k) * 2));
              wr.finish(); }
            std::error_code ec; std::filesystem::rename(nf, f, ec); if (ec) encio::fail("cannot replace " + f + ": " + ec.message()); };
        struct Aligned { std::string raw; int rate = 0; long long up = 1; long lead = 0, tail = 0; }; std::vector<Aligned> al((size_t)nPacks);
        std::vector<std::string> common = {modeName, scheme == 2 ? "s2" : "s1"};   // the coding options are the same for every pack
        for (int i = opt0; i < argc; i++) { const char* a = argv[i]; if (!strcmp(a, "zs") || !strcmp(a, "energy") || !strcmp(a, "equal") || !strcmp(a, "bits=24") || !strncmp(a, "mem=", 4) || !strcmp(a, "g") || !strncmp(a, "g=", 2)) common.push_back(a); }   // g: every pack on its own master (2026-11-11)
        const bool hadUnits = getenv("LAE_BLOCKUNITS") != nullptr; bool stopA = false;
        auto nested = [&](std::vector<std::string> a) {   // one further master: this encoder, run as a step of this run with its own work folder
            const std::string so = encrun::out, sp = encrun::part, st = encrun::tmp; const bool b1 = encrun::hadOld, b2 = encrun::partMine, b3 = encrun::tmpMine, b4 = encrun::cleaned, b5 = encrun::keepTmp;
            std::vector<char*> v; for (auto& s : a) v.push_back(&s[0]); v.push_back(nullptr); int rc;
            encrun::outerTmp = st; encrun::outerMine = b3;   // a Ctrl+C or a fatal error inside it removes this run's work folder too (emergency)
            try { rc = encode((int)a.size(), v.data()); }
            catch (const encio::Fail& e) { printf("further master: stopped: %s\n", e.what()); rc = 2; }
            catch (const std::bad_alloc&) { printf("further master: stopped: out of memory\n"); rc = 2; }
            catch (const std::exception& e) { printf("further master: stopped: %s\n", e.what()); rc = 2; }   // any other error: this run stops cleanly and removes its own work folder
            catch (...) { printf("further master: stopped: internal error\n"); rc = 2; }
            encrun::out = so; encrun::part = sp; encrun::tmp = st; encrun::hadOld = b1; encrun::partMine = b2; encrun::tmpMine = b3; encrun::cleaned = b4; encrun::keepTmp = b5; encrun::outerTmp.clear(); encrun::outerMine = false;
            clearModuleEnv(); env("ZSIGN", zsOpt ? "1" : ""); env("RULE_F", scheme == 2 ? "2" : "1"); env("RULE_D", mi == part::MODE_KEEP20 ? "3" : "1"); env("DITHERFLAGS", T("dither.flags"));   // this run's own module settings again
            if (!hadUnits) env("LAE_BLOCKUNITS", "");
            return rc; };
        const long long upM = 1LL << stages; const bool bits24c = std::find(common.begin(), common.end(), std::string("bits=24")) != common.end();
        for (int k = 0; k < nPacks; k++) { const PackOpt& q = pk[k]; const std::string num = std::to_string(k + 2), pre = "master" + num + " (pack " + std::to_string(k + 1) + "): ", raw = T("al" + num + ".raw");
            printf("%saligning %s to the core (%zu frames) with the master's alignment. In its lines below, \"the master\" is this master, \"the CD\" / \"independent CD\" is the core, cdalign= stands for master%salign=, cdaccept for master%saccept, cdedge= for master%sedge=\n",
                   pre.c_str(), q.path.c_str(), core.size() / 2, num.c_str(), num.c_str(), num.c_str());
            std::vector<std::string> a = {"lae_enc", q.path, T("al" + num + ".wav"), "3", "cd=" + coreW, "alignout=" + raw};
            if (!q.align.empty()) a.push_back("cdalign=" + q.align); if (q.accept) a.push_back("cdaccept"); if (!q.edge.empty()) a.push_back("cdedge=" + q.edge); if (bits24c) a.push_back("bits=24");
            const int rc = nested(a);
            if (rc != 0 || !exists(raw + ".txt")) { stopA = true;
                printf(q.align.empty() ? "%snot placed yet: choose its position from the two searches above and give it as master%salign=<offset>\n" : "%snot aligned at master%salign=%s (see the lines above)\n", pre.c_str(), num.c_str(), q.align.c_str()); continue; }
            long long Lx = 0; long aL = 0, aT = 0; int rx = 0;
            if (sscanf(encio::readText((raw + ".txt").c_str()).c_str(), "%lld %ld %ld %d", &Lx, &aL, &aT, &rx) != 4 || aL < 0 || aT < 0 || rx <= 0) encio::fail("internal error: alignment result of master" + num);
            packReport += "pack " + std::to_string(k + 1) + " (master" + num + "): " + q.path + "\n" + encio::readText((raw + ".report.txt").c_str());
            { std::error_code ec; std::filesystem::remove(raw + ".txt", ec); std::filesystem::remove(raw + ".report.txt", ec); }
            const long long upX = (long long)(rx / (int)base);
            if (upX < 2 || Lx / upX != (long long)(core.size() / 2) + aL + aT) { printf("%sinternal error: the aligned master has %lld frames at %d Hz, the core %zu + %ld + %ld; nothing encoded\n", pre.c_str(), Lx, rx, core.size() / 2, aL, aT); oldNote(); return 2; }
            if (aL || aT) printf("%sits alignment needs %ld frame(s) of digital silence before the core and %ld after\n", pre.c_str(), aL, aT);
            al[(size_t)k].raw = raw; al[(size_t)k].rate = rx; al[(size_t)k].up = upX; al[(size_t)k].lead = aL; al[(size_t)k].tail = aT; }
        if (stopA) { printf("further masters: nothing encoded\n"); oldNote(); return 1; }
        long maxL = 0, maxT = 0; for (auto& x : al) { maxL = std::max(maxL, x.lead); maxT = std::max(maxT, x.tail); }
        if (maxL || maxT) {   // the largest need on each side: the core gets that digital silence once, and the master and every further master the same
            printf("further masters: the core gets %ld frame(s) of digital silence before and %ld after (the largest need of any master); the master and every further master get the same\n", maxL, maxT);
            core.insert(core.begin(), 2 * (size_t)maxL, (int16_t)0); core.insert(core.end(), 2 * (size_t)maxT, (int16_t)0);
            padRawFile(T("m0.raw"), maxL * upM, maxT * upM); Lm += (long long)(maxL + maxT) * upM;
            if (!fitsLength(Lm)) { printf("further masters: the time line (%lld frames) is outside this encoder's length limits: nothing encoded\n", Lm); oldNote(); return 1; }
            if (!derive()) return 2;
            if (cdPath) {   // the given CD with its digital silence is the CD layer again, as after the master's own alignment
                std::vector<uint8_t> b = slurp(T("b16.s16")); if (b.size() / 4 != core.size() / 2 + 2 * (size_t)pbA) encio::fail("internal error: core length after padding");
                memcpy(b.data() + 4 * (size_t)pbA, core.data(), core.size() * 2); encio::writeVec(T("b16.s16").c_str(), b);
                std::vector<uint8_t> df = slurp(T("dither.flags")); std::fill(df.begin(), df.end(), 0); encio::writeVec(T("dither.flags").c_str(), df); }
            else { std::vector<int16_t> b = encio::readAll<int16_t>(T("b16.s16").c_str()); const size_t want = core.size(); core.assign(b.begin() + 2 * (size_t)pbA, b.end() - 2 * (size_t)pbA);   // derived core: derived again from the padded master
                if (core.size() != want) encio::fail("internal error: derived core length after padding"); }
            for (auto& x : al) if (maxL - x.lead || maxT - x.tail) padRawFile(x.raw, (maxL - x.lead) * x.up, (maxT - x.tail) * x.up);
            writeCore(); }
        for (int k = 0; k < nPacks; k++) { const PackOpt& q = pk[k]; const std::string num = std::to_string(k + 2), pre = "master" + num + " (pack " + std::to_string(k + 1) + "): ", outK = T("pk" + num + ".wav");
            printf("%sencoding the aligned master against the core (%zu frames) as an extension of its own (position 0: nothing is moved again)\n", pre.c_str(), core.size() / 2);
            std::vector<std::string> a = {"lae_enc", al[(size_t)k].raw, outK}; a.insert(a.end(), common.begin(), common.end()); a.push_back("cd=" + coreW); a.push_back("cdalign=0"); a.push_back("rawin=" + std::to_string(al[(size_t)k].rate));
            if (q.accept) a.push_back("cdaccept");
            const int rc = nested(a);
            if (rc != 0 || !exists(outK)) { stopA = true; printf("%snot encoded (see the lines above)\n", pre.c_str()); continue; }
            wavio::Wav pw; if (!wavio::readCD(outK.c_str(), pw) || pw.lae.size() < 60) encio::fail("internal error: the pack of master" + num + " could not be read back");
            if (pw.pcm16 != core) { printf("%sinternal error: the pack's core differs from the file's core (%zu frames instead of %zu, or other samples); nothing encoded\n", pre.c_str(), pw.pcm16.size() / 2, core.size() / 2); oldNote(); return 2; }
            { std::error_code ec; std::filesystem::remove(al[(size_t)k].raw, ec); }
            std::vector<uint8_t> r = {'L', 'P', 'A', 'K'}; const uint32_t n = (uint32_t)(1 + q.name.size() + pw.lae.size() - 4); for (int i = 0; i < 4; i++) r.push_back((uint8_t)(n >> (8 * i)));
            r.push_back((uint8_t)q.name.size()); r.insert(r.end(), q.name.begin(), q.name.end()); r.insert(r.end(), pw.lae.begin(), pw.lae.end() - 4);   // the pack's payload without its CRC: one CRC covers the whole chunk
            printf("%sencoded: %zu B\n", pre.c_str(), r.size()); packRecs.push_back(std::move(r));
            { std::error_code ec; std::filesystem::remove(outK, ec); std::filesystem::remove(outK + ".align.txt", ec); } }
        { std::error_code ec; std::filesystem::remove(coreW, ec); }
        if (stopA) { printf("further masters: nothing encoded\n"); oldNote(); return 1; } }
    { std::error_code ec; std::filesystem::remove(T("m0.raw"), ec); std::filesystem::remove(T("m0.orig"), ec); }   // disk (2026-11-05): every later step reads the padded master m.raw
    if (noisePath && (placeT != 0 || Lm != LmIn)) {   // the user (2026-11-04): alignment and its confirmation come first, the marking is made on the aligned master
        printf("noise=: the master was placed on the CD's time line here (whole offset %+lld master samples, length %lld -> %lld frames), so the marked cells no longer lie on the grid they were marked on.\n"
               "  Make the marking on the aligned master (lae_enc plan writes it) and encode that one with cdalign=0; nothing encoded\n", placeT, LmIn, Lm); oldNote(); return 1; }
    env("CDEXT", cdPath ? "1" : "");   // cut-off detection by coherence for an independent CD (routes.cpp)
    env("CUTOFFOUT", T("cutoff.txt"));
    if (isDone("routes")) printf("resume: route split already done\n");
    else { if (call(m_routesblk::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), T("F.s32")})) { printf("route split failed\n"); oldNote(); return 2; } markDone("routes"); }
    env("CUTOFFOUT", "");
    stamp("route split"); double fc = atof(encio::readText(T("cutoff.txt").c_str()).c_str());
    env("PART", modeName); env("PARTEQ", equal ? "1" : ""); env("PARTW", wm ? "1" : "0");
    // one layer = one encoder run (possibly several octave segments) + a self-check of every segment's stream
    std::atomic<bool> ok(true);   // shared by the two layer threads of a pair
    int depthPass = 1;   // resume: the depth layers are coded twice (before and after leak compensation), each pass is its own step
    auto layer = [&](const char* in, int rate, const char* ch, const std::string& tag) {
        if (encio::testFail(("layerthrow:" + tag).c_str())) encio::fail("forced by the test hook in layer " + tag);   // test: an exception outside the modules
        const std::string step = "layer " + tag + (tag[0] == 'd' ? std::to_string(depthPass) : std::string());
        if (isDone(step)) { std::vector<std::vector<uint8_t>> out; int nseg = atoi(encio::readText(T(tag + ".seg").c_str()).c_str());
            if (nseg < 1 || nseg > 8) { ok = false; encenv::print("layer %s: bad segment count\n", tag.c_str()); return out; }
            for (int i = 0; i < nseg; i++) out.push_back(slurp(T(tag + ".bin" + (nseg > 1 ? "." + std::to_string(i) : std::string()))));
            encenv::print("resume: %s already coded\n", step.c_str()); return out; }
        env("CHSEL", ch); env("DUMP", T(tag + ".lae")); env("PLANE", T(tag + ".plane")); env("SEGCOUNT", T(tag + ".seg"));
        int rc = call(m_lossyblk::main, {"lossy", T(in), std::to_string(rate)}); env("SEGCOUNT", ""); std::vector<std::vector<uint8_t>> out;
        if (rc) { ok = false; encenv::print("layer %s: coding failed (code %d)\n", tag.c_str(), rc); return out; }
        int nseg = atoi(encio::readText(T(tag + ".seg").c_str()).c_str()); if (nseg < 1 || nseg > 8) { ok = false; encenv::print("layer %s: bad segment count\n", tag.c_str()); return out; }
        for (int i = 0; i < nseg; i++) { std::string suf = nseg > 1 ? "." + std::to_string(i) : "";
            env("PARTBYTES", T(tag + ".bin" + suf));
            if (call(m_partblk::main, {"part", T(tag + ".lae" + suf), T(tag + ".plane" + suf)})) ok = false;
            { std::error_code ec; std::filesystem::remove(T(tag + ".plane" + suf), ec); }   // disk (2026-11-05): the plane is only for this check
            env("PARTBYTES", ""); if (!ok) return out; out.push_back(slurp(T(tag + ".bin" + suf))); }
        markDone(step); return out; };
    typedef std::vector<std::vector<uint8_t>> Streams;
    // two layers of the same kind at once (L and R, M and S): the second runs in its own thread with its own module variables and printed text.
    // Same arithmetic as one after the other, so the streams are byte-identical.
    // memory plan (2026-10-29 / 30): made once at the start from the track length and the physical memory read then; the whole encode follows it.
    // Budget: mem=<GB>, or by default min(1/2, max(1/4, all - 28 GB)) of the physical memory (16 GB: 4, 32 GB: 8, 48 GB: 20, 64 GB: 32), so
    // that the system and other programs keep room. A layer needs about 1 byte per layer sample (the cell records) + 20 bytes per sample
    // of one block (measured 2026-10-30: about 17) + 128 MB. The block length (whole floor segments and root rows, a memory unit only) is the largest that fits the budget
    // with two layers at once, at most 4 units; if not even one unit fits two at once, the layers run one after the other. Never two at once
    // on 32-bit builds (address space) or with PARTTHREADS=1. LAE_BLOCKUNITS sets the block length by hand (tests). The output depends on
    // neither: every block length and both orders give byte-identical files.
    struct PairPlan { bool both = false; unsigned long long need = 0; };
    unsigned long long memTotal = 0, memAvail = 0; physMemory(memTotal, memAvail);
    const unsigned long long G28 = 28ull << 30, memRule = std::min(memTotal / 2, std::max(memTotal / 4, memTotal > G28 ? memTotal - G28 : 0ull));
    const unsigned long long memUse = memBudget > 0 ? memBudget : memRule;
    const char* ptEnv = getenv("PARTTHREADS"); const bool oneThread = ptEnv && atoi(ptEnv) == 1;
    auto unitSamples = [&](int rate) { long seg = std::max(1L, (long)(rate / mdct::N)); long u = seg; while (u % 8) u += seg; return (unsigned long long)u * mdct::N; };   // as lossy_blk.cpp
    const unsigned long long sF = (unsigned long long)(Lm + 2 * P), sD = sF >> stages, uF = unitSamples(w.rate), uD = unitSamples((int)base), M128 = 128ull << 20;
    auto layerNeed = [&](unsigned long long samples, unsigned long long unitS, long long units) { return samples + 20ull * std::min(samples, unitS * (unsigned long long)units) + M128; };
    const bool mayPair = sizeof(void*) >= 8 && !oneThread && !getenv("LAE_TESTSEQUENTIAL");
    long long units = 0; const char* buEnv = getenv("LAE_BLOCKUNITS");
    if (buEnv) units = atoll(buEnv);
    else { const unsigned long long lim = std::min(memUse, memAvail ? memAvail : memUse);
        auto fit = [&](unsigned long long room) { return room > sF + M128 ? (long long)((room - sF - M128) / (20ull * uF)) : 0LL; };
        units = mayPair ? fit(lim / 2) : 0; if (units < 1) units = fit(lim); units = std::max(1LL, std::min({4LL, units, (long long)((sF + uF - 1) / uF)}));   // at most 4 units (about 32 s): larger blocks are not faster (measured 2026-10-30: 8 / 32 / 64 s, 52-54 s) and only take memory; never longer than the track
        env("LAE_BLOCKUNITS", std::to_string(units)); }
    auto planFor = [&](unsigned long long samples, unsigned long long unitS) { PairPlan p; const long long u = units > 0 ? units : (long long)((samples + unitS - 1) / unitS);
        p.need = 2 * layerNeed(samples, unitS, u); p.both = mayPair && p.need <= memUse && (!memAvail || p.need <= memAvail); return p; };
    const PairPlan planD = planFor(sD, uD), planF = planFor(sF, uF);
    printf("memory plan: %.2f min at %d Hz; blocks of %lld unit(s) (%.0f s); depth layers %s (two at once about %.0f MB), frequency layers %s (two at once about %.0f MB); budget %.0f MB%s, available %.0f MB\n",
           Lm / (60.0 * w.rate), w.rate, units, units > 0 ? (double)(uF * (unsigned long long)units) / w.rate : Lm / (double)w.rate,
           planD.both ? "two at once" : "coded one after the other", planD.need / 1e6, planF.both ? "two at once" : "coded one after the other", planF.need / 1e6,
           memUse / 1e6, memBudget > 0 ? " (mem=)" : "", memAvail / 1e6);
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
    // resume: once the leak step is done, the first depth pass is not needed any more (its only use is the file the leak is decoded from)
    const bool skipD1 = isDone("leak"); Streams dL, dR; if (!skipD1) layerPair("T.s32", (int)base, "L", "dL", "R", "dR", dL, dR, planD.both); stamp("depth layers");
    env("FULLBAND", ""); env("MASTERREF", ""); env("DEPTHADAPT", "");
    char kf[32]; snprintf(kf, sizeof kf, "%.6f", (fc - 1000.0) / (base / 2)); env("K0FRAC", kf); env("OCTSPLIT", "1");
    if (noisePath) {   // cells below k0 lie in the CD layer: listed, not deleted (the user, 2026-11-04: the default stays as before)
        const double k0f = atof(kf); int k0b = (int)floor(k0f * (base / 2) / (w.rate / 2.0 / mdct::N)); k0b -= k0b % nmk::CW; long below = 0;   // as lossy_blk.cpp computes k0 with noise=
        for (size_t i = 0; i < noiseMk.pos.size(); i++) { const nmk::Pos& p = noiseMk.pos[i]; if (p.c0 * nmk::CW >= k0b) continue; below++; const bool whole = p.c1 * nmk::CW <= k0b;
            printf("noise: position %zu (%.3f-%.3f s, %.1f-%.1f Hz) lies %s below %.1f Hz, the frequency layer's start: in the CD layer, not deleted%s\n", i + 1,
                   nmk::frameT0(p.m0, w.rate), nmk::frameT0(p.m1, w.rate), nmk::colHz(p.c0, w.rate), nmk::colHz(p.c1, w.rate), whole ? "wholly" : "partly", k0b * (w.rate / 2.0 / mdct::N), whole ? "" : " there (deleted above it)"); }
        if (below) printf("noise: %ld position(s) reach below the frequency layer's start; deleting there would change the CD layer (not done)\n", below);
        env("NOISE", noisePath); env("NOISEPAD", std::to_string(P)); }
    if (noisePath || gOpt) env("NOISEM", T("m.raw")); if (gOpt) env("GMODE", std::string(1, gLevel));
    Streams fM, fS; if (ok) layerPair("F.s32", w.rate, "M", "fM", "S", "fS", fM, fS, planF.both); env("K0FRAC", ""); env("OCTSPLIT", ""); stamp("frequency layers");
    env("NOISE", ""); env("NOISEPAD", ""); env("NOISEM", ""); env("GMODE", "");
    if (ok) { std::error_code ec; std::filesystem::remove(T("F.s32"), ec); }   // disk (2026-11-05): the frequency layers are coded (and noted as done); nothing reads F.s32 again
    if (gOpt && ok && fM.size() == fS.size() && fM.size() % 2 == 0) {   // the user is told what g costs (2026-11-04): the G streams are the second half of each plane's streams
        unsigned long long all = 0, gb = 0; const size_t half = fM.size() / 2;
        for (size_t i = 0; i < fM.size(); i++) { const unsigned long long b = fM[i].size() + fS[i].size(); all += b; if (i >= half) gb += b; }
        printf("g: the G streams take %.2f MB of the frequency layer's %.2f MB (%.1f %%); without g the file would be about that much smaller\n", gb / 1e6, all / 1e6, all ? 100.0 * gb / all : 0.0); }
    if (!ok || (!skipD1 && (dL.size() != 1 || dR.size() != 1)) || fM.empty() || fM.size() != fS.size()) { printf("ENCODER SELF-CHECK FAILED: a decoded layer differs from the encoder's plane or a layer could not be coded; nothing written\n"); oldNote(); return 2; }
    // CD layer: padded base trimmed by P >> stages at each end; the trimmed edges go into the LAE chunk (the decoder needs them).
    // The CD samples are not held in memory: they are copied from b16.s16 piece by piece when the file is written and checked.
    long nb, pb = P >> stages; std::vector<int16_t> edge(4 * pb);
    { blkio::Reader<int16_t, 2> r(T("b16.s16").c_str()); nb = r.frames; std::vector<int16_t> x;
      r.read(0, pb, x); std::copy(x.begin(), x.end(), edge.begin()); r.read(nb - pb, nb, x); std::copy(x.begin(), x.end(), edge.begin() + 2 * pb); }
    const unsigned long long cdBytes = (unsigned long long)(nb - 2 * pb) * 4;
    std::vector<uint8_t> dfl = slurp(T("dither.flags"));
    // LAE chunk v5 (v5 = v4 + precision rule per stream, reconstruction offset 0.20, 6 modes; v4 = v3 + 16-bit backstop): header, base edges, dither flags, depth L, depth R, number of frequency segments, M segments, S segments
    // v6 = v5 + the phase number after pb (an independent CD whose position has a fraction of a master sample); phase number 0 is written as v5
    // The chunk is a list of pieces pointing at the coded streams (no second copy); its CRC-32 is added up piece by piece.
    typedef std::pair<const uint8_t*, size_t> Piece;
    auto chunkPieces = [&](std::vector<std::vector<uint8_t>>& small, std::vector<Piece>& pc) {
        small.clear(); pc.clear(); small.reserve(64);
        auto put32 = [&](int32_t v) { small.emplace_back((uint8_t*)&v, (uint8_t*)&v + 4); pc.push_back({small.back().data(), 4}); };
        auto putb = [&](const void* p, size_t n) { pc.push_back({(const uint8_t*)p, n}); };
        auto putS = [&](const std::vector<uint8_t>& s) { put32((int32_t)s.size()); putb(s.data(), s.size()); };
        put32(0x3145414C); put32(5); put32(w.rate); put32((int32_t)base); put32(stages); put32((int32_t)Lm); put32(P);
        put32((int32_t)fc); put32(mi); put32(equal ? 1 : 0); put32(wm); put32((int32_t)nb); put32((int32_t)pb);
        putb(edge.data(), edge.size() * 2); put32((int32_t)dfl.size()); putb(dfl.data(), dfl.size());
        putS(dL[0]); putS(dR[0]); put32((int32_t)fM.size()); for (auto& s : fM) putS(s); for (auto& s : fS) putS(s);
        for (auto& r : packRecs) putb(r.data(), r.size());   // extension packs: one record each, after the main body's last stream and inside the one CRC
        // the main body's name (2026-11-12, spec/lossy/SPEC.md, Section 5.6): one optional record after the last pack record,
        // "LNAM" | 1 byte length | the name (UTF-8). Display only; a decoder that does not know it ends the pack list there.
        if (!mainName.empty()) { mainRec.assign({ 'L', 'N', 'A', 'M', (uint8_t)mainName.size() }); mainRec.insert(mainRec.end(), mainName.begin(), mainName.end()); putb(mainRec.data(), mainRec.size()); }
        uint32_t c = 0xFFFFFFFFu; static const struct Tab { uint32_t t[256]; Tab() { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; } } } tab;
        for (auto& q : pc) for (size_t i = 0; i < q.second; i++) c = tab.t[(c ^ q.first[i]) & 0xFF] ^ (c >> 8);   // = crc32() of part_stream.h over the whole chunk
        put32((int32_t)(c ^ 0xFFFFFFFFu));   // v3: CRC-32 of everything before it (integrity check, whole chunk)
        size_t n = 0; for (auto& q : pc) n += q.second; return n; };
    // FLAC form (2026-11-10): the CD layer coded once into FLAC frames (block size 4096, two independent channels, 16 bits: the frame
    // writer of the LAES FLAC form), kept in the work folder as cd.flacframes; STREAMINFO needs the smallest and the largest frame and
    // the MD5 of the samples. The frames are coded in batches (memory of one batch); each frame depends only on its own samples, so
    // the bytes do not depend on the number of threads. A resumed run codes them again (seconds).
    struct CdFlac { bool made = false; uint32_t fmin = 0, fmax = 0; uint8_t md5[16] = {0}; unsigned long long bytes = 0; } cdf;
    const std::string cdFlacPath = T("cd.flacframes");
    auto makeCdFlac = [&]() -> bool { if (cdf.made) return true;
        const long n = nb - 2 * pb, B = 4096, nf = (n + B - 1) / B; if (n < 16) { printf("the CD layer is too short for the FLAC form (fewer than 16 samples): nothing written\n"); return false; }
        blkio::Reader<int16_t, 2> r(T("b16.s16").c_str()); alignhash::Md5 md; FILE* f = fopen(cdFlacPath.c_str(), "wb"); if (!f) { printf("cannot write %s\n", cdFlacPath.c_str()); return false; }
        const long batch = 512; uint32_t fmin = 0xFFFFFF, fmax = 0; unsigned long long bytes = 0; bool okw = true; std::vector<int16_t> x; std::vector<int32_t> ch[2];
        const int th = (int)std::min(16u, std::max(1u, std::thread::hardware_concurrency()));
        for (long k0 = 0; k0 < nf && okw; k0 += batch) { const long k1 = std::min(nf, k0 + batch), a = k0 * B, b = std::min(n, k1 * B), cnt = k1 - k0;
            r.read(pb + a, pb + b, x); md.update((const uint8_t*)x.data(), x.size() * 2);   // 16-bit little-endian, interleaved: the bytes STREAMINFO's MD5 is defined on
            for (int c = 0; c < 2; c++) { ch[c].resize((size_t)(b - a)); for (long i = 0; i < b - a; i++) ch[c][(size_t)i] = x[(size_t)i * 2 + c]; }
            std::vector<std::vector<uint8_t>> fr((size_t)cnt); std::atomic<long> next{0}; std::atomic<bool> bad{false};
            auto work = [&]() { try { for (long k; (k = next.fetch_add(1)) < cnt;) fr[(size_t)k] = laesflac::encodeFrame(ch, (int64_t)k * B, (int64_t)std::min<long>(B, (b - a) - k * B), (uint64_t)(k0 + k), (int)base, 16); } catch (...) { bad = true; } };
            { std::vector<std::thread> pool; for (int t = 1; t < th && t < cnt; t++) { try { pool.emplace_back(work); } catch (...) { break; } } work(); for (auto& p : pool) p.join(); }
            if (bad) { fclose(f); encio::fail("the CD layer could not be coded as FLAC (out of memory?)"); }
            for (auto& q : fr) { fmin = std::min<uint32_t>(fmin, (uint32_t)q.size()); fmax = std::max<uint32_t>(fmax, (uint32_t)q.size()); bytes += q.size(); okw = okw && fwrite(q.data(), 1, q.size(), f) == q.size(); } }
        okw = fclose(f) == 0 && okw; if (!okw) { printf("cannot write %s\n", cdFlacPath.c_str()); return false; }
        const std::string h = md.hex(); for (int i = 0; i < 16; i++) cdf.md5[i] = (uint8_t)std::stoul(h.substr((size_t)i * 2, 2), nullptr, 16);
        cdf.fmin = fmin; cdf.fmax = fmax; cdf.bytes = bytes; cdf.made = true; return true; };
    // the bytes of the FLAC form: "fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (the tags, if any) | the chunk body in "LAE1" APPLICATION
    // blocks of at most 2^24 - 5 bytes each (the last one carries the "last metadata block" flag) | the audio frames. No SEEKTABLE,
    // no CUESHEET, no padding, nothing between the pieces.
    size_t flacPieces = 0;
    auto fileBytesFlac = [&](const std::vector<Piece>& pc, size_t laeN, const std::function<bool(const uint8_t*, size_t)>& f) {
        const long n = nb - 2 * pb; const uint32_t bs = n >= 4096 ? 4096u : (uint32_t)std::max<long>(16, n);
        std::vector<uint8_t> o = {'f', 'L', 'a', 'C'}; laesflac::putBlockHeader(o, false, 0, 34);
        laes::BitW si; si.put(bs, 16); si.put(bs, 16); si.put(cdf.fmin, 24); si.put(cdf.fmax, 24); si.put((uint64_t)(int)base, 20); si.put(1, 3); si.put(15, 5); si.put((uint64_t)n, 36);
        o.insert(o.end(), si.b.begin(), si.b.end()); o.insert(o.end(), cdf.md5, cdf.md5 + 16); o.insert(o.end(), tagChunks.begin(), tagChunks.end());
        if (!f(o.data(), o.size())) return false;
        size_t left = laeN, room = 0, blocks = 0; size_t pieceMax = laeyflac::kPiece;
        if (const char* tp = std::getenv("LAEY_TEST_PIECE")) { const long long v = atoll(tp); if (v >= 1 && (unsigned long long)v < laeyflac::kPiece) pieceMax = (size_t)v; }   // test only: smaller pieces, to exercise the split
        for (auto& q : pc) { const uint8_t* p = q.first; size_t k = q.second;
            while (k) { if (!room) { room = std::min(pieceMax, left); std::vector<uint8_t> bh; laesflac::putBlockHeader(bh, left == room, 2, (uint32_t)(room + 4));
                    bh.insert(bh.end(), {'L', 'A', 'E', '1'}); blocks++; if (!f(bh.data(), bh.size())) return false; }
                const size_t t = std::min(k, room); if (!f(p, t)) return false; p += t; k -= t; room -= t; left -= t; } }
        if (left || room) return false; flacPieces = blocks;
        FILE* ff = fopen(cdFlacPath.c_str(), "rb"); if (!ff) return false; std::vector<uint8_t> buf((size_t)1 << 20); unsigned long long total = 0; bool okc = true;
        for (size_t k; okc && (k = fread(buf.data(), 1, buf.size(), ff)) > 0;) { total += k; okc = f(buf.data(), k); }
        fclose(ff); return okc && total == cdf.bytes; };
    // the bytes of the output file (as wavio::writeCD writes them), handed to f piece by piece
    auto fileBytesWav = [&](const std::vector<Piece>& pc, size_t laeN, const std::function<bool(const uint8_t*, size_t)>& f) {
        std::vector<uint8_t> o; const size_t extra = laeN == 0 ? 0 : 8 + laeN + (laeN & 1); wavio::hdr(o, (int)base, 2, 16, (size_t)cdBytes, extra + tagChunks.size());
        if (!tagChunks.empty()) o.insert(o.begin() + 36, tagChunks.begin(), tagChunks.end());   // tags after "fmt " (RIFF 12 + fmt 24 bytes), before "data"
        if (!f(o.data(), o.size())) return false;
        blkio::Reader<int16_t, 2> r(T("b16.s16").c_str()); std::vector<int16_t> x; const long BS = 1L << 18;
        for (long a = pb; a < nb - pb; a += BS) { r.read(a, std::min(nb - pb, a + BS), x); if (!f((const uint8_t*)x.data(), x.size() * 2)) return false; }
        o.clear(); if (cdBytes & 1) o.push_back(0);
        if (laeN) { o.insert(o.end(), {'L', 'A', 'E', ' '}); uint32_t n = (uint32_t)laeN; for (int i = 0; i < 4; i++) o.push_back((uint8_t)(n >> (8 * i))); }
        if (!f(o.data(), o.size())) return false;
        for (auto& q : pc) if (q.second && !f(q.first, q.second)) return false;
        const uint8_t z = 0; return !(laeN & 1) || f(&z, 1); };
    // written to <out>.part and read back: the file on disk must hold exactly this CD layer and this chunk
    auto fileBytes = [&](const std::vector<Piece>& pc, size_t laeN, const std::function<bool(const uint8_t*, size_t)>& f) { return asFlac ? fileBytesFlac(pc, laeN, f) : fileBytesWav(pc, laeN, f); };
    auto writeFile = [&]() { std::vector<std::vector<uint8_t>> small; std::vector<Piece> pc; const size_t laeN = chunkPieces(small, pc);
        if (asFlac) { if (laeN == 0 || laeN > 0x7FFFFFFFull || !makeCdFlac()) { if (laeN > 0x7FFFFFFFull) printf("the extension would exceed 2 GB: nothing written\n"); return (size_t)0; } }
        else if (!wavio::fits32(cdBytes, 8ULL + laeN + 1 + tagChunks.size())) { printf("output would exceed the 4 GB limit of WAV: nothing written\n"); return (size_t)0; }
        encrun::partMine = true; markDone("part");   // resume: this run owns the .part from here on
        bool okw = false; { FILE* f = fopen(partPath.c_str(), "wb"); if (f) { okw = fileBytes(pc, laeN, [&](const uint8_t* p, size_t n) { return fwrite(p, 1, n, f) == n; }); okw = fclose(f) == 0 && okw; } }
        if (!okw || encio::testFail("partwrite")) { printf("cannot write %s\n", partPath.c_str()); return (size_t)0; }
        bool okr = false; { FILE* f = fopen(partPath.c_str(), "rb"); if (f) { std::vector<uint8_t> g; unsigned long long total = 0;
            okr = fileBytes(pc, laeN, [&](const uint8_t* p, size_t n) { g.resize(n); total += n; return fread(g.data(), 1, n, f) == n && !memcmp(g.data(), p, n); });
            okr = okr && encio::size(f) == (long long)total; fclose(f); } }
        if (!okr || encio::testFail("partverify")) { printf("read-back check FAILED: %s does not hold what was written (disk full or failing?)\n", partPath.c_str()); return (size_t)0; }
        return laeN; };
    size_t chunk = 0; if (!skipD1) { chunk = writeFile(); if (!chunk) { oldNote(); return 2; } stamp("file written"); }
    long nclip = -1;
    {   // leak compensation (v5, always on): decode the final frequency layer, let the depth layer cancel its leak into the CD band
      if (skipD1) printf("resume: leak decode already done\n");
      else {
        g_streamErr = false; std::error_code ec; std::filesystem::remove(T("leak.s32"), ec);
        env("LEAKOUT", T("leak.s32")); int rc = call(m_dec::main, {"dec", partPath, T("check.wav"), "full"}); env("LEAKOUT", "");
        long long lsz = fileSize(T("leak.s32")); const bool forced = encio::testFail("leakdecode");
        if (rc || lsz != (long long)nb * 8 || g_streamErr || forced) { printf("leak compensation FAILED (decoder code %d, leak file %lld bytes, expected %lld%s%s): nothing written\n", rc, lsz, (long long)nb * 8,
                                                                                g_streamErr ? ", decoded values out of range" : "", forced ? ", forced by the test hook" : ""); oldNote(); return 2; }
        markDone("leak"); }
        if (isDone("routes2")) printf("resume: second route split already done\n");
        else { char fcs[32]; snprintf(fcs, sizeof fcs, "%.0f", fc); env("LEAKIN", T("leak.s32"));
          int rc = call(m_routesblk::main, {"routes", T("m.raw"), std::to_string(w.rate), T("b16.s16"), T("b24.s32"), T("T.s32"), "-", fcs}); env("LEAKIN", "");
          stamp("leak decoded + route split again");
          if (rc || encio::testFail("routes2")) { printf("leak compensation FAILED in the route split: nothing written\n"); oldNote(); return 2; }
          markDone("routes2"); }
        env("FULLBAND", "1"); env("MASTERREF", T("b24.s32")); env("DEPTHADAPT", "1");
        depthPass = 2; dL.clear(); dR.clear(); layerPair("T.s32", (int)base, "L", "dL", "R", "dR", dL, dR, planD.both);
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
      const long long wantWav = 44 + Lm * 6 + ((Lm * 6) & 1), clipB = fileSize(T("clip.bin")), haveWav = fileSize(T("check.wav"));
      if (haveWav != wantWav) { printf("end-to-end check FAILED: the decoded file has %lld bytes, expected %lld: nothing written\n", haveWav, wantWav); oldNote(); return 2; }   // the decoder said OK but did not write the whole track
      if (clipB >= 0 && !encio::testFail("clipdecode")) nclip = (long)(clipB / 4);
      if (nclip >= 0) printf("samples beyond full scale before the decoder's hard limiter: %ld\n", nclip);
      else printf("samples beyond full scale before the decoder's hard limiter: not measured (the check files are incomplete)\n"); }
    { std::error_code ec; std::filesystem::remove(T("check.wav"), ec); }
    {   // only a complete, verified file gets the output name; a briefly locked target (scanner, indexer, player) is retried
        std::error_code ec; for (int k = 0; k < 20; k++) { ec.clear(); std::filesystem::rename(partPath, outPath, ec); if (!ec) break; std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
        if (ec) { printf("cannot rename %s to %s: %s (the output may be open in another program)\n", partPath.c_str(), outPath.c_str(), ec.message().c_str()); oldNote(); return 2; }
        encrun::partMine = false; encrun::keepTmp = false; }   // finished: the work folder goes, also after a resumed run
    if (nPacks) {   // the file's packs: printed, and their alignment reports go into the report beside the file
        size_t all = 0; for (auto& r : packRecs) all += r.size();
        printf("extension packs: %d further pack(s), %zu B in all (pack 0 = the main body, from %s)\n", nPacks, all, argv[1]);
        for (int k = 0; k < nPacks; k++) printf("  pack %d: %s%s%s%s, %zu B\n", k + 1, pk[k].path.c_str(), pk[k].name.empty() ? "" : " (\"", pk[k].name.c_str(), pk[k].name.empty() ? "" : "\")", packRecs[(size_t)k].size());
        if (!packReport.empty()) { if (alignReport.empty()) alignReport = std::string("LAE alignment report (further masters)\noutput: ") + outPath + "\n"; alignReport += packReport; } }
    if (!alignReport.empty()) { const std::string rp = outPath + ".align.txt"; FILE* f = fopen(rp.c_str(), "wb");
        bool ok = f && fwrite(alignReport.data(), 1, alignReport.size(), f) == alignReport.size(); if (f && fclose(f)) ok = false;
        printf(ok ? "alignment report: %s\n" : "WARNING: the alignment report %s could not be written (the LAE file itself is complete)\n", rp.c_str()); }
    if (diskPoll.peak > 0) printf("work folder: largest %.2f GB (%.0f bytes per master frame; sampled every 0.25 s)\n", diskPoll.peak / 1e9, diskPoll.peak / (double)(Lm + 2 * P));
    size_t sd = dL[0].size() + dR[0].size(), sf = 0; for (auto& s : fM) sf += s.size(); for (auto& s : fS) sf += s.size();
    std::string extra; if (!note32.empty()) extra += " | " + note32; if (!alignNote.empty()) extra += " | " + alignNote;
    if (asFlac) printf("FLAC form: CD layer %llu B as FLAC frames (%.1f %% of its %llu B of PCM), extension in %zu LAE1 block(s)\n", cdf.bytes, cdBytes ? 100.0 * cdf.bytes / cdBytes : 0.0, cdBytes, flacPieces);
    printf("LAE encode OK: %s | CD %d Hz cut-off %.0f Hz | scheme %d, mode %s (share by %s) %s | CD PCM %zu B, depth %zu B, frequency %zu B in %zu octave segment(s), chunk %zu B | beyond full scale before limiter %s | %.1f s%s\n",
           argv[2], (int)base, fc, scheme, part::MODES[mi].name, wm == 2 ? "cells+energy" : (wm ? "energy" : "cells"), equal ? "equal" : "unequal", (size_t)cdBytes, sd, sf, fM.size(), chunk,
           nclip >= 0 ? std::to_string(nclip).c_str() : "not measured", std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count(), extra.c_str());
    return 0;
}

// Test switches (2026-11-15): the environment variables that only tests use act only with the master switch LAE_TEST_MODE=1. Without it
// they are removed at the start, so no inherited variable changes what this program does; with it every run says which are active.
// Outside the master switch stay the harmless diagnostics that change no output byte: TIMING (times printed), KEEPTMP (the work
// folder is kept), TIERSTAT (statistics printed). The encoder's own module variables (clearModuleEnv) are not test switches.
static void testSwitches() {
    static const char* const kTest[] = {"LAEY_TEST_PIECE", "LAE_TESTMAXFRAMES", "LAE_TESTFAIL", "LAE_BLOCKUNITS", "LAE_TESTSEQUENTIAL", "PARTTHREADS"};
    const char* tm = getenv("LAE_TEST_MODE"); const bool on = tm && !strcmp(tm, "1"); std::string act;
    for (const char* k : kTest) { const char* v = getenv(k); if (!v) continue; if (on) act += std::string(act.empty() ? "" : ", ") + k + "=" + v; else env(k, ""); }
    if (on) { printf("TEST MODE (LAE_TEST_MODE=1): active test switches: %s. Unset LAE_TEST_MODE to return to normal behaviour.\n", act.empty() ? "none" : act.c_str()); fflush(stdout); } }

int main(int argc, char** argv) {
    std::set_terminate(encrun::onTerminate); std::atexit(encrun::onExit);
    std::signal(SIGINT, encrun::onSignal); std::signal(SIGTERM, encrun::onSignal);
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // no error-report dialog that would block a scripted run
    std::signal(SIGBREAK, encrun::onSignal);
#endif
    lae_u8::init(argc, argv);   // UTF-8 arguments and file names on Windows (win_utf8.h); after signal(), so Ctrl+C sets the console back first
    testSwitches();
    int rc;
    try { rc = argc > 1 && !strcmp(argv[1], "plan") ? m_plan::planMain(argc, argv) : argc > 1 && !strcmp(argv[1], "tag") ? m_tag::retag(argc, argv) : encode(argc, argv); }
    catch (const encio::Fail& e) { encrun::emergency(e.what()); return 2; }
    catch (const std::bad_alloc&) { encrun::emergency("out of memory"); return 2; }
    catch (const std::exception& e) { encrun::emergency(e.what()); return 2; }
    catch (...) { encrun::emergency("internal error"); return 2; }
    encrun::finished = true; return rc; }
