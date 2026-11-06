// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// laes: LAE Lossless tool, WAV form and FLAC form (draft v1, 2026-10-28 / 29; format in laes_file.h, FLAC in laes_flac.h).
//   laes enc <out.wav | out.flac> (--core=<core.wav> | --derive-core) [--core-bits=16|24|32] <master.wav>[@T] ...
//       a master is a 16/24/32-bit stereo WAV (read where it is) or FLAC (2026-11-05: decoded once into the work folder, checked by its MD5)
//       --timing: prints the time of each stage and the largest size of the work folder at the end (the file written does not change)
//       disk (2026-11-05): before the work folder is made, the disk it needs is estimated; more than is free -> stopped, nothing written
//       (--force-disk runs anyway). While it runs, below 2 GB free on that disk: it waits (nothing removed) and goes on once there is room
//       again; --disk-stop: it stops instead, the work folder kept; --disk-stop-remove: it stops and the work folder is removed (the work
//       done so far is lost). A run that stopped (disk full, closed) keeps its work folder;
//       the same command run again continues from it (a different job, encoder build or changed master starts again)
//       --small-disk: for small disks; a FLAC master is not decoded into the work folder and the largest rate level of each master is not
//       kept: both are made again when read (slower encoding, the same file)
//       T = time of the master on the core's time line, in base-rate samples (decimal, e.g. -0.37); default 0.
//       out.flac = FLAC form: the core in the FLAC audio frames, the LAES block in "LAE2" APPLICATION blocks.
//       --derive-core: the core is a master's base level at --core-bits; default 16 for WAV (CD / DVD), for FLAC the highest master
//       word length up to 24 (a 32-bit master gives a 24-bit core; --core-bits=32 keeps 32). A master with exactly that word length is
//       the source (its base level, no requantisation), otherwise the longest one, TPDF-dithered down.
//       out.lae (the format's own name, 2026-11-05): --form=flac (default) or --form=wav chooses the form; readers go by content.
//       tags and pictures (2026-11-05, laes_tags.h; none given: nothing is written, the file is the same byte for byte):
//       --tag=KEY=VALUE (Vorbis names: TITLE, ARTIST, ALBUM, DATE, COPYRIGHT, COMMENT, ...; may repeat), --cover=<image> (front cover),
//       --picture=<type>:<image> (other pictures, ID3 / FLAC picture types, any number); JPEG or PNG stored byte for byte.
//       FLAC: VORBIS_COMMENT + PICTURE before the "LAE2" blocks; WAV: LIST/INFO + "id3 " (ID3v2.4) between "fmt " and "data". UTF-8.
//   laes tag <in> <out> [--tag=... --cover=... --picture=...]   a NEW file with exactly these tags (none: no tags); audio bytes and
//       payload copied byte for byte, then every master checked against its SHA-256; the old file is kept. Refuses an existing <out>.
//   laes parts <file>                                  blocks / chunks, SHA-256 of the audio bytes and of the payload, the tags as read
//   laes dec <in.wav | in.flac> <out dir> [--levels]   writes core.wav and master_<k>_<rate>_<bits>.wav (and every rate level with --levels)
//   laes dec <in> --list                              every choice: core, masters, rate levels (lossless) and derived 16-bit versions
//   laes dec <in> <out dir> --pick=<n>[,<n>...] | --pick=<rate>:<bits>   writes only the chosen ones. A derived 16-bit version exists for each
//       rate without a 16-bit choice of its own: made from the longest-word choice there with a fixed TPDF rule; it is a derived
//       master: lossless relative to that rule (bit-identical on every decoder), not lossless relative to its source
//   laes verify <in.wav | in.flac> <master.wav> ...    decodes and compares every master bit for bit with the given files
//   laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]   LAEY's automatic search (lae_enc cdcheck) -> T exactly as measured
//       (fraction included); alignment error within -60 dB passes, otherwise it is shown and passes only when chosen by hand. Writes nothing.
//   enc: master.wav@auto runs the same alignment against --core; not accepted -> nothing written (--accept accepts without asking)
//   laes selftest                                   regression test with synthetic signals (laes_selftest.h); exit code 0 = all passed
//   laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]   test helper: 16 = TPDF to 16 bit, 24 = as is, 32 = random low byte
// Masters: 16/24/32-bit integer PCM WAV, stereo, base rate x 1/2/4/8. Each master's base level is stored as the difference to the
// adjacent lower tier (of the core and the earlier masters with fewer bits, the one with the most bits not above the master's; a tie goes to the
// latest master; none: the core) or in full - whichever is smaller.
// WAV input is read with full precision (32-bit samples keep all 32 bits); output > 16 bit is written as WAVE_FORMAT_EXTENSIBLE.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include "laes_file_enc.h"
#include "laes_flac_enc.h"
#include "laes_stream_enc.h"   // block-wise enc / dec / verify / align (2026-10-30)
#include "laes_play.h"   // the playback decoder, for the self-check of the written file (2026-11-03)
#include "laes_tags.h"   // tags and pictures (2026-11-05)
#ifdef _WIN32
static const char* kLaeEnc = "lae_enc.exe"; static const char* kQ = "\"";   // cmd.exe needs the whole command line quoted once more
#else   // POSIX names of the same calls
#include <strings.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _stricmp strcasecmp
#define _popen popen
#define _pclose pclose
static const char* kLaeEnc = "lae_enc"; static const char* kQ = "";
#endif
using namespace laes;
namespace fs = std::filesystem;
static bool readAll(const std::string& p, std::vector<uint8_t>& b) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return false; _fseeki64(f, 0, SEEK_END); long long s = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    b.resize((size_t)s); bool ok = fread(b.data(), 1, b.size(), f) == b.size(); fclose(f); return ok; }
static bool writeAll(const std::string& p, const std::vector<uint8_t>& b) { FILE* f = fopen(p.c_str(), "wb"); if (!f) return false; bool ok = fwrite(b.data(), 1, b.size(), f) == b.size(); return fclose(f) == 0 && ok; }
// ---- WAV read (16/24/32-bit integer PCM, stereo; PCM or EXTENSIBLE with the PCM sub-format) + the "LAE " chunk body
static std::string readWav(const std::string& path, Pcm& p, std::vector<uint8_t>* lae = nullptr) {
    std::vector<uint8_t> b; if (!readAll(path, b)) return "cannot read " + path; if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(&b[8], "WAVE", 4)) return path + ": not RIFF/WAVE";
    size_t pos = 12; int tag = 0, ch = 0, rate = 0, bits = 0; const uint8_t* data = nullptr; size_t dlen = 0; if (lae) lae->clear();
    auto u16 = [&](size_t o) { return (int)(b[o] | b[o + 1] << 8); }; auto u32 = [&](size_t o) { return (uint32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24); };
    while (pos + 8 <= b.size()) { uint32_t n = u32(pos + 4); size_t body = pos + 8; if (n > b.size() - body) n = (uint32_t)(b.size() - body);
        if (!memcmp(&b[pos], "fmt ", 4) && n >= 16) { tag = u16(body); ch = u16(body + 2); rate = (int)u32(body + 4); bits = u16(body + 14); if (tag == 0xFFFE && n >= 26) tag = u16(body + 24); }
        else if (!memcmp(&b[pos], "data", 4)) { data = &b[body]; dlen = n; }
        else if (!memcmp(&b[pos], "LAE ", 4) && lae) lae->assign(b.begin() + body, b.begin() + body + n);
        pos = body + n + (n & 1); }
    if (tag != 1 || ch != 2 || (bits != 16 && bits != 24 && bits != 32) || !data) return path + ": need 16/24/32-bit integer PCM, 2 channels";
    const int by = bits / 8; const int64_t n = (int64_t)(dlen / (2 * by)); p.rate = rate; p.bits = bits;
    for (int c = 0; c < 2; c++) { p.c[c].resize(n); for (int64_t i = 0; i < n; i++) { const uint8_t* q = data + (i * 2 + c) * by; uint32_t v = 0; for (int k = 0; k < by; k++) v |= (uint32_t)q[k] << (8 * k);
            if (bits < 32 && (v >> (bits - 1) & 1)) v |= ~0u << bits; p.c[c][i] = (int64_t)(int32_t)v; } }
    return ""; }
// ---- WAV write: 16-bit PCM (for the core) or EXTENSIBLE for 24/32 bit; optional "LAE " chunk
static std::vector<uint8_t> wavBytes(const Pcm& p, const std::vector<uint8_t>* lae) {
    const int by = p.bits / 8; const int64_t n = p.frames(); const size_t db = (size_t)n * 2 * by; const bool ext = p.bits > 16; const uint32_t fmtLen = ext ? 40 : 16;
    const size_t extra = lae && !lae->empty() ? 8 + lae->size() + (lae->size() & 1) : 0;
    std::vector<uint8_t> o; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }; auto p16 = [&](int v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); };
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); p32((uint32_t)(4 + 8 + fmtLen + 8 + db + (db & 1) + extra)); o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); p32(fmtLen);
    p16(ext ? 0xFFFE : 1); p16(2); p32((uint32_t)p.rate); p32((uint32_t)(p.rate * 2 * by)); p16(2 * by); p16(p.bits);
    if (ext) { p16(22); p16(p.bits); p32(3); const uint8_t guid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}; o.insert(o.end(), guid, guid + 16); }
    o.insert(o.end(), {'d', 'a', 't', 'a'}); p32((uint32_t)db); for (int64_t i = 0; i < n; i++) for (int c = 0; c < 2; c++) { uint32_t v = (uint32_t)(int32_t)p.c[c][i]; for (int k = 0; k < by; k++) o.push_back((uint8_t)(v >> (8 * k))); }
    if (db & 1) o.push_back(0);
    if (extra) { o.insert(o.end(), {'L', 'A', 'E', ' '}); p32((uint32_t)lae->size()); o.insert(o.end(), lae->begin(), lae->end()); if (lae->size() & 1) o.push_back(0); }
    return o; }
struct Rng { uint64_t s; explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 88172645463325252ull) {} uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } double uni() { return (double)(next() >> 11) / 9007199254740992.0 - 0.5; } };
static std::string arg(int argc, char** argv, const char* key) { std::string k = std::string(key) + "="; for (int i = 1; i < argc; i++) if (!strncmp(argv[i], k.c_str(), k.size())) return argv[i] + k.size(); return ""; }
static bool flag(int argc, char** argv, const char* key) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], key)) return true; return false; }
// core = a master's base level at coreBits (encoder choice, not normative: the core is stored). A master with exactly coreBits is the
// source (its base level as it is), otherwise the one with the most bits, TPDF-dithered down
static Pcm deriveCore(const std::vector<MasterIn>& ms, int base, int coreBits, size_t& src, bool& dithered) {
    src = 0; for (size_t i = 0; i < ms.size(); i++) if (ms[i].m.bits > ms[src].m.bits) src = i;
    for (size_t i = 0; i < ms.size(); i++) if (ms[i].m.bits == coreBits) { src = i; break; }
    const Pcm& m = ms[src].m; int w = unusedBits(m), st = 0; while ((base << st) < m.rate) st++; Rng r(20261028); Pcm core; core.rate = base; core.bits = coreBits;
    const int sig = m.bits - w, drop = sig - coreBits; const int64_t mx = (1LL << (coreBits - 1)) - 1, mn = -mx - 1; dithered = drop > 0;
    for (int c = 0; c < 2; c++) { V x(m.frames()); for (int64_t i = 0; i < m.frames(); i++) x[i] = m.c[c][i] >> w; for (int k = st; k > 0; k--) { V lo, d; laesfast::fwd(x, lo, d); x = lo; }
        core.c[c].resize(x.size()); for (size_t i = 0; i < x.size(); i++) { int64_t v;
            if (drop <= 0) v = x[i] << (-drop);   // exact (the core keeps every bit of the base level)
            else { const double u1 = r.uni(), u2 = r.uni(); v = (int64_t)std::floor((double)x[i] / std::ldexp(1.0, drop) + u1 + u2 + 0.5); }   // TPDF dither; u1 first (the order is fixed: SPEC 11.1)
            core.c[c][i] = std::max(mn, std::min(mx, v)); } }   // a base level beyond full scale is limited; the layers keep the difference
    return core; }
// T (base samples, decimal) -> whole + fraction in 2^-32 base samples
static void setT(MasterIn& m, double T) { m.whole = (int64_t)std::floor(T); m.frac = (uint32_t)std::llround((T - std::floor(T)) * 4294967296.0); if (m.frac == 0 && T != std::floor(T)) m.whole += 1; }
// derived 16-bit version (decoder rule): y = floor((v + r1 - r2 + 2^(s-1)) / 2^s), s = bits - 16, r1, r2 uniform in [0, 2^s) from
// xorshift64 (seed 20261029, one stream per channel): TPDF dither of +-1 LSB at 16 bits; deterministic. A new master, lossy against src.
static Pcm derive16(const Pcm& sc) { Pcm dv; dv.rate = sc.rate; dv.bits = 16; const int s = sc.bits - 16;
    for (int c = 0; c < 2; c++) { uint64_t st = 20261029ull + 0x9E3779B97F4A7C15ull * (uint64_t)(c + 1); auto nx = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st >> (64 - s); };
        dv.c[c].resize(sc.c[c].size()); for (size_t i = 0; i < sc.c[c].size(); i++) { const int64_t r1 = (int64_t)nx(), r2 = (int64_t)nx();
            dv.c[c][i] = std::max<int64_t>(-32768, std::min<int64_t>(32767, (sc.c[c][i] + r1 - r2 + (1LL << (s - 1))) >> s)); } }
    return dv; }
static std::string laeEncPath(int argc, char** argv) { std::string e = arg(argc, argv, "--lae-enc"); return e.empty() ? (fs::absolute(fs::path(argv[0])).parent_path() / kLaeEnc).string() : e; }
// ---- automatic alignment: LAEY's search (lae_enc cdcheck, design D). LAEY moves the master by its offset (master samples); LAES never
// moves the master and stores T = offset / 2^stages (base samples, same sign) exactly as measured, fraction included.
// err = LAEY's pooled residual after the fraction (its phase line), the alignment error the -60 / -54 dB bands apply to.
struct AutoT { bool found = false; double T = 0, err = 0, resid = 0; std::string search, notes, why; };
static AutoT autoAlign(const std::string& cdPath, const Pcm& cd, const std::string& mPath, const laess::Prep& m, const std::string& enc) {   // m: read and its levels made (block-wise, 2026-10-30)
    AutoT A; int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++;
    if ((cd.rate << st) != m.rate || st > 3) { A.why = "master rate must be the core master rate times 1, 2, 4 or 8"; return A; }
    if (st == 0) { A.why = "LAEY's search needs a master at 2, 4 or 8 times the core master rate; give T by hand (master.wav@T)"; return A; }
    if (!fs::exists(enc)) { A.why = "lae_enc not found: " + enc + " (give --lae-enc=<path>)"; return A; }
    const std::string dummy = (fs::temp_directory_path() / "laes_align_unused.lae").string();   // cdcheck encodes nothing
    const std::string cl = std::string(kQ) + "\"" + enc + "\" \"" + mPath + "\" \"" + dummy + "\" 3 \"cd=" + cdPath + "\" cdcheck" + (m.bits == 32 ? " bits=24" : "") + kQ;
    FILE* pp = _popen(cl.c_str(), "r"); if (!pp) { A.why = "cannot run lae_enc"; return A; }
    const char* nm[2] = {"whitened", "plain"}; std::string sug, why[2]; double pooled[2] = {1, 1}; char ln[8192];
    while (fgets(ln, sizeof ln, pp)) { std::string s = ln; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.rfind("ALIGN_SUGGEST ", 0)) sug = s;
        for (int k = 0; k < 2; k++) { const std::string pre = std::string("align: ") + nm[k] + " search: ", ph = std::string("align (") + nm[k] + "): phase: "; size_t p;
            if (!s.rfind(pre, 0)) why[k] = s.substr(pre.size()); else if (!s.rfind(ph, 0) && (p = s.find("pooled residual ")) != std::string::npos) pooled[k] = atof(s.c_str() + p + 16); } }
    _pclose(pp); if (sug.empty()) { A.why = "lae_enc gave no ALIGN_SUGGEST line (old lae_enc? design D is needed)"; return A; }
    bool have[2] = {false, false}; double T[2] = {0, 0};
    for (int k = 0; k < 2; k++) { size_t p = sug.find(std::string(nm[k]) + "="); if (p == std::string::npos) continue; size_t c = sug.find(':', p), sp = sug.find(' ', p);
        std::string v = sug.substr(c + 1, sp == std::string::npos ? std::string::npos : sp - c - 1); if (v != "-") { have[k] = true; T[k] = atof(v.c_str()) / (double)(1 << st); } }
    A.notes = std::string("LAEY's own placement checks (LAEY rules, not LAES): whitened: ") + why[0] + "; plain: " + why[1];
    if (!have[0] && !have[1]) { A.why = "LAEY's search found no position; give T by hand. " + A.notes; return A; }
    const int k = have[0] && have[1] ? (pooled[1] < pooled[0] ? 1 : 0) : (have[0] ? 0 : 1); A.found = true; A.T = T[k]; A.search = nm[k]; A.err = pooled[k] <= 0 ? pooled[k] : 0;
    // own check: the master's base level against the core moved by T, worst of three blocks (head / middle / tail, up to 5 s each)
    const int w = m.w, up = (m.bits - 16) - w; std::unique_ptr<laess::Sig> base[2] = {laess::levelOf(m, 0, 0), laess::levelOf(m, 1, 0)};   // the base level (samples >> w)
    const int64_t nb = base[0]->n, bl = std::max<int64_t>(1, std::min<int64_t>(nb / 3, 5 * (int64_t)cd.rate)), b0[3] = {0, (nb - bl) / 2, nb - bl};
    int64_t wh = (int64_t)std::floor(A.T); uint32_t fr = (uint32_t)std::llround((A.T - std::floor(A.T)) * 4294967296.0); if (fr == 0 && A.T != std::floor(A.T)) wh++;
    const auto h = refTaps(fr); A.resid = -200;
    for (int b = 0; b < 3; b++) { double eb = 0, er = 0; for (int c = 0; c < 2; c++) { const int64_t n = (int64_t)cd.c[c].size(); V bv((size_t)bl); base[c]->get(b0[b], b0[b] + bl, bv.data());
            for (int64_t i = b0[b]; i < b0[b] + bl; i++) { Acc a; for (int q = -127; q <= 128; q++) a.mac(h[q + 127], cd.c[c][mir(i + wh + q, n)]); const int s = 31 - up;
                const double r = (double)(s > 0 ? a.shr(s) : (int64_t)a.lo << (-s)), x = (double)bv[(size_t)(i - b0[b])]; eb += x * x; er += (x - r) * (x - r); } }
        if (eb > 0 && er > 0) A.resid = std::max(A.resid, 10 * std::log10(er / eb)); }
    if (A.err == 0) A.err = A.resid;   // LAEY gave no phase line: use the own check
    return A; }
// the -60 / -54 dB bands: within -60 dB the alignment passes; otherwise it is shown and passes only when chosen by hand
static bool acceptAlign(const AutoT& A, const std::string& mPath, bool acceptFlag) {
    printf("%s: T = %.10f base samples (%s search), alignment error %.1f dB, residual re the master's base level %.1f dB\n", mPath.c_str(), A.T, A.search.c_str(), A.err, A.resid);
    if (A.resid > -40) printf("  note: the core master is not a close copy of this master (other mix / level / EQ / edit?); the encoder will likely store it in full\n");
    if (A.err <= -60) return true;
    printf("  alignment error %.1f dB is %s -54 dB: check it (%s)\n", A.err, A.err <= -54 ? "between -60 and" : "worse than", A.notes.c_str());
    if (acceptFlag) { printf("  accepted (--accept)\n"); return true; }
    printf("  use this alignment? [y/N] "); fflush(stdout); char a[16] = {0}; if (!fgets(a, sizeof a, stdin)) { printf("(no answer)\n"); return false; }
    const char* p = a; while (*p && !isalpha((unsigned char)*p)) p++; return *p == 'y' || *p == 'Y'; }   // skips a byte-order mark or spaces
// physical memory in bytes (0 if unknown), for the default memory limit
#ifdef _WIN32
struct LaesMemStatus { unsigned long len, load; unsigned long long totalPhys, availPhys, totalPage, availPage, totalVirt, availVirt, availExt; };   // MEMORYSTATUSEX
extern "C" __declspec(dllimport) int __stdcall GlobalMemoryStatusEx(LaesMemStatus*);
static unsigned long long physMemory() { LaesMemStatus m{}; m.len = sizeof m; return GlobalMemoryStatusEx(&m) ? m.totalPhys : 0; }
// the process's peak working set so far in bytes (--timing, 2026-11-05)
struct LaesProcMem { unsigned long cb, faults; size_t peakWs, ws, qpp, qp, qpnp, qnp, pf, peakPf; };   // PROCESS_MEMORY_COUNTERS
extern "C" __declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(void*, LaesProcMem*, unsigned long);
static unsigned long long peakMemory() { LaesProcMem m{}; m.cb = sizeof m; return K32GetProcessMemoryInfo((void*)(intptr_t)-1, &m, sizeof m) ? m.peakWs : 0; }
#else
static unsigned long long peakMemory() { FILE* f = fopen("/proc/self/status", "r"); if (!f) return 0; char line[256]; unsigned long long t = 0;
    while (fgets(line, sizeof line, f)) { unsigned long long kb = 0; if (sscanf(line, "VmHWM: %llu kB", &kb) == 1) t = kb * 1024; } fclose(f); return t; }
static unsigned long long physMemory() { FILE* f = fopen("/proc/meminfo", "r"); if (!f) return 0; char line[256]; unsigned long long t = 0;
    while (fgets(line, sizeof line, f)) { unsigned long long kb = 0; if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) t = kb * 1024; } fclose(f); return t; }
#endif
#include "laes_selftest.h"
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
// the self-check of a written LAES file, read exactly as a player reads it (laes_play.h): open (file SHA-256, payload CRC-32, core), then
// every master decoded in time order and compared sample by sample with its input (pm: in file order).
// "" when everything matches, else the reason.
static std::string selfCheckFile(const std::string& path, const std::vector<laess::Prep>& pm, int threads) {
    FILE* fp = fopen(path.c_str(), "rb"); if (!fp) return "cannot read " + path;
    _fseeki64(fp, 0, SEEK_END); const uint64_t size = (uint64_t)_ftelli64(fp);
    laesp::FnSrc src; src.n = size; src.fn = [&](uint64_t at, void* d, size_t k) { return (_fseeki64(fp, (long long)at, SEEK_SET) == 0 && fread(d, 1, k, fp) == k) ? 0 : 1; };
    struct Close { FILE* f; ~Close() { fclose(f); } } closer{fp};
    laesp::File f; char mg[5]; std::string e; try { e = f.open(src, mg); } catch (const std::exception& x) { e = x.what(); }
    if (e.empty() && !f.why.empty()) e = f.why;   // the payload failed a check
    if (!e.empty()) return e;
    // one pass over every master (speed, 2026-11-05): the playback decoder as it is, started with allMasters (every master of the file
    // decoded and its SHA-256 checked in this one run, each shared tier decoded once). Master 1 comes out as a player plays it; the others
    // are read from the same decoder's nodes right after the decoder has made them (the values its own hashing reads). Every master is
    // compared sample by sample with its input, as before; only the order of the work changed.
    const int64_t P = 1 << 16; std::vector<int32_t> a((size_t)P * 2), r((size_t)P * 2); V x[2];
    int j0 = -1; for (size_t k = 0; k < f.choices.size(); k++) if (f.choices[k].kind == 1 && f.choices[k].m == 0) j0 = (int)k;
    if (j0 < 0) return "master 1 is not in the file";
    laesp::Player p; e = p.start(f, j0, threads, true); if (!e.empty()) return "master 1: " + e;
    std::vector<std::unique_ptr<laess::In>> raw(pm.size()); for (size_t i = 0; i < pm.size(); i++) raw[i].reset(new laess::In(pm[i].raw));   // the masters as given (a WAV) or decoded copies (a FLAC)
    std::vector<int64_t> at(pm.size(), 0); std::vector<bool> seen(pm.size(), false); seen[0] = true;
    auto others = [&]() -> std::string {   // the other masters up to where the decoder has hashed them
        for (auto& d : p.chain) { const size_t i = (size_t)d->idx; if (i == 0 || i >= pm.size()) continue; seen[i] = true; const int w = f.ms[i].I.w;
            while (at[i] < d->hashed) { const int64_t e1 = std::min<int64_t>(d->hashed, at[i] + P), k = e1 - at[i];
                for (int c = 0; c < 2; c++) { x[c].resize((size_t)k); d->top[c]->get(at[i], e1, x[c].data()); }
                for (int64_t q = 0; q < k; q++) for (int c = 0; c < 2; c++) a[(size_t)(q * 2 + c)] = (int32_t)(x[c][(size_t)q] << w);
                laess::masterFrames(*raw[i], pm[i], at[i], k, r.data()); if (memcmp(a.data(), r.data(), (size_t)k * 8) != 0) return "master " + std::to_string(i + 1) + " differs from its input near frame " + std::to_string(at[i]);
                at[i] = e1; } }
        return ""; };
    for (;;) { const int64_t k = p.read(a.data(), P); if (k < 0) return "master 1: " + p.err; if (k == 0) break;
        if (at[0] + k > (int64_t)pm[0].frames) return "master 1 is longer than its input";
        laess::masterFrames(*raw[0], pm[0], at[0], k, r.data()); if (memcmp(a.data(), r.data(), (size_t)k * 8) != 0) return "master 1 differs from its input near frame " + std::to_string(at[0]);
        at[0] += k; e = others(); if (!e.empty()) return e; }
    if (!p.err.empty()) return "master 1: " + p.err;
    e = others(); if (!e.empty()) return e;
    for (size_t i = 0; i < pm.size(); i++) { if (!seen[i]) return "master " + std::to_string(i + 1) + " was not decoded";
        if (at[i] != (int64_t)pm[i].frames) return "master " + std::to_string(i + 1) + " is " + (at[i] < (int64_t)pm[i].frames ? "shorter" : "longer") + " than its input"; }
    if (!p.verified()) return "SHA-256 mismatch";   // every master's, checked by the decoder
    return ""; }

// ---- the structure of an LAES / LAE file (2026-11-05, for laes tag / laes parts): FLAC metadata blocks or RIFF chunks, with their bodies
struct Part { std::string id; int type = -1; long long off = 0, len = 0; };   // FLAC: type 0..126, id "" (APPLICATION: its 4-byte id); WAV: id, type -1
struct Layout { bool flac = false, rf64 = false; std::vector<Part> parts; long long audioOff = 0, audioLen = 0; std::vector<std::pair<long long, long long>> payload; };
static std::string readLayout(laess::In& in, Layout& L) {
    uint8_t h[12] = {0}; if (in.n < 12) return in.path + ": not FLAC or RIFF/WAVE"; in.read(0, h, 12); L = Layout(); L.flac = !memcmp(h, "fLaC", 4);
    L.rf64 = (!memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4);
    if (L.flac) { long long p = 4; bool last = false;
        while (!last) { if (p + 4 > in.n) return in.path + ": FLAC metadata truncated"; uint8_t bh[4]; in.read(p, bh, 4); last = bh[0] & 0x80; Part q; q.type = bh[0] & 0x7F; q.len = bh[1] << 16 | bh[2] << 8 | bh[3]; q.off = p + 4;
            if (q.off + q.len > in.n) return in.path + ": FLAC metadata truncated";
            if (q.type == 2 && q.len >= 4) { char id[5] = {0}; in.read(q.off, id, 4); q.id = id; if (q.id == "LAE2" || q.id == "LAE1") L.payload.push_back({q.off + 4, q.len - 4}); }
            L.parts.push_back(q); p = q.off + q.len; }
        if (L.parts.empty() || L.parts[0].type != 0) return in.path + ": no STREAMINFO first"; L.audioOff = p; L.audioLen = in.n - p; return ""; }
    if ((memcmp(h, "RIFF", 4) && !L.rf64) || memcmp(h + 8, "WAVE", 4)) return in.path + ": not FLAC or RIFF/WAVE";
    uint64_t dsData = 0; std::vector<std::pair<std::string, uint64_t>> table; long long pos = 12; bool haveData = false;
    while (pos + 8 <= in.n) { uint8_t c[8]; in.read(pos, c, 8); Part q; q.id.assign((const char*)c, 4); uint64_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint64_t)c[7] << 24; q.off = pos + 8;
        if (L.rf64 && q.id == "ds64" && n >= 28) { std::vector<uint8_t> d((size_t)n); in.read(q.off, d.data(), d.size()); auto le64 = [&](size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | d[at + (size_t)i]; return v; };
            dsData = le64(8); const uint32_t cnt = d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24; for (uint32_t i = 0; i < cnt && 28 + 12 * (size_t)(i + 1) <= d.size(); i++) table.push_back({std::string((const char*)&d[28 + 12 * i], 4), le64(32 + 12 * (size_t)i)}); }
        if (L.rf64 && n == 0xFFFFFFFFu) { if (q.id == "data") n = dsData; else for (auto& t : table) if (t.first == q.id) n = t.second; }
        if ((long long)n > in.n - q.off) return in.path + ": chunk " + q.id + " runs past the end of the file";
        q.len = (long long)n; if (q.id == "data") { L.audioOff = q.off; L.audioLen = q.len; haveData = true; } else if (q.id == "LAE ") L.payload.push_back({q.off, q.len});
        L.parts.push_back(q); pos = q.off + q.len + (q.len & 1); }
    if (!haveData) return in.path + ": no data chunk"; return ""; }
static std::string hexOf(const uint8_t* d, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02x", d[i]); s += b; } return s; }
static std::string shaOf(laess::In& in, const std::vector<std::pair<long long, long long>>& ranges) {   // SHA-256 of these byte ranges, in order
    laes::Sha256 s; std::vector<uint8_t> b; for (auto& r : ranges) for (long long a = 0; a < r.second; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, r.second - a)); in.read(r.first + a, b.data(), b.size()); s.update(b.data(), b.size()); }
    uint8_t h[32]; s.final(h); return hexOf(h, 32); }
static void copyRange(laess::In& in, laess::Out& o, long long off, long long len) { std::vector<uint8_t> b; for (long long a = 0; a < len; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, len - a)); in.read(off + a, b.data(), b.size()); o.put(b.data(), b.size()); laess::diskCheck(); } }
// every master of a file decoded by the playback decoder and checked against the SHA-256 stored in its payload (and the core at open)
static std::string checkStored(const std::string& path, int threads) {
    FILE* fp = fopen(path.c_str(), "rb"); if (!fp) return "cannot read " + path; _fseeki64(fp, 0, SEEK_END); const uint64_t size = (uint64_t)_ftelli64(fp);
    laesp::FnSrc src; src.n = size; src.fn = [&](uint64_t at, void* d, size_t k) { return (_fseeki64(fp, (long long)at, SEEK_SET) == 0 && fread(d, 1, k, fp) == k) ? 0 : 1; };
    struct Close { FILE* f; ~Close() { fclose(f); } } closer{fp};
    laesp::File f; char mg[5]; std::string e; try { e = f.open(src, mg); } catch (const std::exception& x) { e = x.what(); }
    if (e.empty() && !f.why.empty()) e = f.why; if (!e.empty()) return e;
    int j0 = -1; for (size_t k = 0; k < f.choices.size(); k++) if (f.choices[k].kind == 1 && f.choices[k].m == 0) j0 = (int)k;
    if (j0 < 0) return "master 1 is not in the file";
    laesp::Player p; e = p.start(f, j0, threads, true); if (!e.empty()) return "master 1: " + e;
    std::vector<int32_t> a((size_t)1 << 17); for (;;) { const int64_t k = p.read(a.data(), 1 << 16); if (k < 0) return p.err; if (k == 0) break; }
    if (!p.err.empty()) return p.err; if (!p.verified()) return "SHA-256 mismatch"; return ""; }


int main(int argc, char** argv) {
    lae_u8::init(argc, argv);   // UTF-8 arguments and file names on Windows (win_utf8.h)
    if (argc < 2) { printf("laes enc | tag | parts | dec | verify | align | selftest | excerpt | mkcd  (see the source header)\n"); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") return selftest::run() ? 2 : 0;
    if (cmd == "reduce" && argc >= 5) {   // laes reduce <in.wav> <out.wav> <bits>: a shorter word length with TPDF dither (+-1 LSB of the result), fixed seed
        Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } const int bits = atoi(argv[4]);
        if ((bits != 16 && bits != 24) || bits >= p.bits) { printf("reduce: <bits> must be 16 or 24 and below the input's %d\n", p.bits); return 1; }
        const int s = p.bits - bits; const int64_t mx = (1LL << (bits - 1)) - 1, mn = -mx - 1; Pcm q; q.rate = p.rate; q.bits = bits; Rng r(4321);
        for (int c = 0; c < 2; c++) { q.c[c].resize(p.c[c].size()); for (size_t i = 0; i < p.c[c].size(); i++) {
                const int64_t r1 = (int64_t)(r.next() >> (64 - s)), r2 = (int64_t)(r.next() >> (64 - s)); q.c[c][i] = std::max<int64_t>(mn, std::min<int64_t>(mx, (p.c[c][i] + r1 - r2 + (1LL << (s - 1))) >> s)); } }
        return writeAll(argv[3], wavBytes(q, nullptr)) ? 0 : 2; }
    if (cmd == "excerpt" && argc >= 7) { Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        int64_t s0 = (int64_t)(atof(argv[4]) * p.rate), L = (int64_t)(atof(argv[5]) * p.rate); int bits = atoi(argv[6]); if (s0 < 0 || s0 + L > p.frames()) { printf("excerpt outside the file\n"); return 2; }
        Pcm q; q.rate = p.rate; q.bits = bits; Rng r(12345); for (int c = 0; c < 2; c++) { q.c[c].assign(p.c[c].begin() + s0, p.c[c].begin() + s0 + L);
            for (auto& v : q.c[c]) { int64_t x = v >> (p.bits - 24) << 0; if (p.bits != 24) x = v; if (bits == 32) v = (x << 8) | (int64_t)(r.next() & 0xFF); else if (bits == 16) { const double u1 = r.uni(), u2 = r.uni(); v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(x / 256.0 + u1 + u2 + 0.5))); } else v = x; } }
        if (argc >= 8) { const int64_t pz = (int64_t)(atof(argv[7]) * p.rate), fd = std::min<int64_t>(L / 2, p.rate / 20); for (int c = 0; c < 2; c++) { for (int64_t i = 0; i < fd; i++) { const double g = 0.5 - 0.5 * std::cos(3.14159265358979323846 * i / fd); q.c[c][i] = (int64_t)std::llround(q.c[c][i] * g); q.c[c][L - 1 - i] = (int64_t)std::llround(q.c[c][L - 1 - i] * g); } q.c[c].insert(q.c[c].begin(), pz, 0); q.c[c].insert(q.c[c].end(), pz, 0); } }   // [pad s]: 50 ms fades, then digital silence at both ends
        return writeAll(argv[3], wavBytes(q, nullptr)) ? 0 : 2; }
    if (cmd == "mkcd" && argc >= 5) {   // test helper: laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]: a 16-bit CD whose sample i is the master's base level at time i + tau (base samples)
        Pcm m; std::string e = readWav(argv[2], m); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } const double tau = atof(argv[4]), gain = argc >= 6 ? atof(argv[5]) : 1.0, noise = argc >= 7 ? atof(argv[6]) : 0.0;   // noise: extra white noise, LSB rms
        const int base = m.rate % 44100 == 0 ? 44100 : 48000; int st = 0; while ((base << st) < m.rate) st++; Pcm cd; cd.rate = base; cd.bits = 16; Rng r(777);
        for (int c = 0; c < 2; c++) { V x = m.c[c]; for (int k = st; k > 0; k--) { V lo, d; laesfast::fwd(x, lo, d); x = lo; } const int64_t n = (int64_t)x.size(); std::vector<double> h(256); const double fr = tau - std::floor(tau); const int64_t ti = (int64_t)std::floor(tau);
            for (int q = -127; q <= 128; q++) { double u = fr - q, rr = u / 128; double si = std::fabs(u) < 1e-12 ? 1 : std::sin(3.14159265358979323846 * u) / (3.14159265358979323846 * u); h[q + 127] = std::fabs(rr) < 1 ? si * I0Series(14 * std::sqrt(1 - rr * rr)) / I0Series(14) : 0; }
            cd.c[c].resize(n); const double sc = std::ldexp(1.0, m.bits - 16);
            for (int64_t i = 0; i < n; i++) { double a = 0; for (int q = -127; q <= 128; q++) { int64_t j = i + ti + q; a += h[q + 127] * (j >= 0 && j < n ? (double)x[j] : 0); }
                if (std::fabs(a * gain / sc) < 0.5 && noise == 0) { cd.c[c][i] = 0; continue; }   // no random values used here, as before
                const double n1 = r.uni(), n2 = r.uni(), n3 = r.uni(), n4 = r.uni(), d1 = r.uni(), d2 = r.uni();   // in this order on every compiler
                cd.c[c][i] = std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(a * gain / sc + noise * 1.732 * (n1 + n2 + n3 + n4) + d1 + d2 + 0.5))); } }
        return writeAll(argv[3], wavBytes(cd, nullptr)) ? 0 : 2; }
    // ---- block-wise commands (2026-10-30): masters, levels, layers and the payload live in a work folder next to the output; memory holds
    // a few pieces (and the core). Piece length from the memory limit (--mem=<GB>, or by default 2 GB, less where min(1/2, max(1/4,
    // all - 28 GB)) of the physical memory is smaller; 2026-11-04); the files written do not depend on it.
    // job (enc, 2026-11-05): a work folder made for the same job (job.txt equal) is used again: the run continues where an earlier one stopped
    auto workDir = [&](const std::string& out, const std::string& job = std::string(), bool* resumed = nullptr) { const std::string wd = out + ".laes.tmp"; std::error_code ec;
        if (fs::exists(wd, ec) && !fs::exists(wd + "/laes.tmp", ec)) laess::fail(wd + " exists and is not a laes work folder: remove it or choose another output name");
        if (resumed) { *resumed = false; std::string old; if (!job.empty() && laess::getState(wd + "/job.txt", old) && old == job) { *resumed = true; return wd; } }
        fs::remove_all(wd, ec); fs::create_directories(wd, ec); if (ec || !fs::exists(wd)) laess::fail("cannot create the work folder " + wd);
        { laess::Out o(wd + "/laes.tmp"); const char* t = "laes work folder (removed when the run ends; kept when an encoding stops, for the same command to continue)\n"; o.put(t, strlen(t)); o.close(); }
        if (!job.empty()) laess::putState(wd + "/job.txt", job); return wd; };
    // the work folder goes at the end; keep: not when the run is stopped by an exception (disk full, closed: the same command continues)
    struct Gone { std::string p; bool keep = false; ~Gone() { if (!p.empty() && !getenv("KEEPTMP") && !(keep && std::uncaught_exceptions())) { std::error_code ec; fs::remove_all(p, ec); } } };
    auto pieceFor = [&](int maxStages) -> int64_t {   // base-level samples per piece, a multiple of 4096
        if (const char* e = getenv("LAES_PIECE")) return std::max<int64_t>(4096, atoll(e) / 4096 * 4096);   // test hook
        unsigned long long total = physMemory(); const std::string mm = arg(argc, argv, "--mem"); const unsigned long long G28 = 28ull << 30;
        unsigned long long budget = !mm.empty() ? (unsigned long long)(atof(mm.c_str()) * 1073741824.0) : std::min(2ull << 30, std::min(total / 2, std::max(total / 4, total > G28 ? total - G28 : 0ull)));
        if (budget < (512ull << 20)) budget = 512ull << 20;
        const unsigned long long top = budget / 160;   // about 80 bytes per top-level sample of a piece (lifting windows), twice for the candidates' reference
        // --small-disk (2026-11-05): half the piece, so that its extra buffers (the level made again, FLAC frames decoded again) stay
        // within the memory of a normal run; the files written do not depend on the piece
        const unsigned long long topUsed = flag(argc, argv, "--small-disk") ? top / 2 : top;
        return std::max<int64_t>(4096, (int64_t)((topUsed >> maxStages) / 4096 * 4096)); };
    const int threads = laesfast::defaultThreads();
    if (cmd == "align" && argc >= 4) {   // check only: prints T and the band; nothing is moved or written
        Pcm cd; std::string e = readWav(argv[2], cd); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        if (cd.bits != 16 || (cd.rate != 44100 && cd.rate != 48000)) { printf("the core master must be 16-bit at 44100 or 48000 Hz\n"); return 2; }
        try { const std::string wd = (fs::temp_directory_path() / ("laes_align_" + std::to_string((unsigned long long)std::hash<std::string>()(argv[3])))).string(); Gone g{workDir(wd)}; laess::Prep m;
            e = laess::prepRead(argv[3], g.p + "/m", m); if (e.empty()) { int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++; e = laess::prepLevels(m, cd.rate, g.p + "/m", pieceFor(st), threads, std::vector<int>()); }
            if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
            const AutoT A = autoAlign(argv[2], cd, argv[3], m, laeEncPath(argc, argv)); if (!A.found) { printf("%s\n", A.why.c_str()); return 2; }
            const bool ok = acceptAlign(A, argv[3], flag(argc, argv, "--accept"));
            printf("ALIGN_LAES T=%.10f err=%.1f resid=%.1f band=%s accepted=%d\n", A.T, A.err, A.resid, A.err <= -60 ? "ok" : A.err <= -54 ? "check" : "large", ok ? 1 : 0);
            if (ok) printf("use %s@%.10f (or %s@auto in enc)\n", argv[3], A.T, argv[3]); return ok ? 0 : 2; }
        catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; } }
    if (cmd == "parts" && argc >= 3) try {   // the structure and the SHA-256 of the audio bytes and of the payload (tests: unchanged by tags)
        laess::In in(argv[2]); Layout L; const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        printf("%s form%s\n", L.flac ? "FLAC" : "WAV", L.rf64 ? " (RF64)" : "");
        for (auto& q : L.parts) { if (L.flac) { static const char* nm[] = {"STREAMINFO", "PADDING", "APPLICATION", "SEEKTABLE", "VORBIS_COMMENT", "CUESHEET", "PICTURE"};
                printf("  %s%s%s  %lld bytes\n", q.type <= 6 ? nm[q.type] : ("type " + std::to_string(q.type)).c_str(), q.id.empty() ? "" : " ", q.id.c_str(), q.len); }
            else printf("  \"%s\"  %lld bytes\n", q.id.c_str(), q.len); }
        printf("audio bytes SHA-256 %s (%lld bytes)\n", shaOf(in, {{L.audioOff, L.audioLen}}).c_str(), L.audioLen);
        long long pl = 0; for (auto& r : L.payload) pl += r.second; printf("payload SHA-256 %s (%lld bytes)\n", shaOf(in, L.payload).c_str(), pl);
        {   // the tags as the playback decoder reads them (laes_play.h: VORBIS_COMMENT, LIST/INFO)
            laesp::FnSrc s; s.n = (uint64_t)in.n; s.fn = [&](uint64_t at, void* d, size_t k) { try { in.read((long long)at, d, k); return 0; } catch (...) { return 1; } };
            laesp::File f; char mg[5]; std::string e; try { e = f.open(s, mg, true); } catch (const std::exception& x) { e = x.what(); }
            if (!e.empty()) printf("player: %s\n", e.c_str()); for (auto& t : f.tags) printf("  tag %s=%s\n", t.first.c_str(), t.second.c_str()); }
        return 0; }
    catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; }
    // laes tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]   (2026-11-05)
    //   writes a NEW file whose tags and pictures are exactly the ones given (none given: an untagged file); the old tags go, the audio bytes,
    //   the payload and every other block or chunk are copied byte for byte. The layout is the encoder's, so the result is the same
    //   as encoding with these tags. Then the new file is decoded completely and every master checked against its stored SHA-256, and the
    //   audio bytes and the payload are compared with the old file; any difference: the new file is removed. The old file is never changed.
    if (cmd == "tag" && argc >= 4) try {
        const std::string src = argv[2], out = argv[3]; std::error_code ec;
        if (out.empty() || out[0] == '-') { printf("give the new file's name: laes tag <in> <out> ...\n"); return 1; }
        if (fs::exists(out, ec)) { printf("%s exists: choose another name (the old file is kept, a new one is written)\n", out.c_str()); return 1; }
        laestag::Tags tags; { const std::string e = laestag::parseArgs(argc, argv, tags); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; } }
        laess::In in(src); Layout L; { const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        if (L.payload.empty()) { printf("%s: no LAE extension found (not an LAES / LAE file)\n", src.c_str()); return 2; }
        { char mg[5] = {0}; if (L.payload[0].second >= 4) in.read(L.payload[0].first, mg, 4);
          if (strcmp(mg, "LAE2")) { printf("%s: not LAES (extension %s); an LAEY file (LAE1) is tagged by lae_enc\n", src.c_str(), mg); return 2; } }
        { laess::Out o(out + ".part");
          if (L.flac) {   // "fLaC" | STREAMINFO | the new tags | the other blocks as they were (old VORBIS_COMMENT, PICTURE, PADDING dropped) | audio frames
              std::vector<const Part*> keep; for (size_t i = 1; i < L.parts.size(); i++) { const int t = L.parts[i].type; if (t != 1 && t != 4 && t != 6 && t != 0) keep.push_back(&L.parts[i]); }
              const std::vector<uint8_t> tb = laestag::flacBlocks(tags, keep.empty());
              std::vector<uint8_t> si(42); in.read(0, si.data(), 42); si[4] = (uint8_t)((tb.empty() && keep.empty() ? 0x80 : 0) | 0); o.put(si.data(), si.size()); o.put(tb.data(), tb.size());
              for (size_t i = 0; i < keep.size(); i++) { const Part& q = *keep[i]; const uint8_t bh[4] = {(uint8_t)((i + 1 == keep.size() ? 0x80 : 0) | q.type), (uint8_t)(q.len >> 16), (uint8_t)(q.len >> 8), (uint8_t)q.len};
                  o.put(bh, 4); copyRange(in, o, q.off, q.len); }
              copyRange(in, o, L.audioOff, L.audioLen); }
          else {   // RIFF (RF64 beyond 4 GB, as the encoder) | "fmt " | the new tags | the other chunks in their order (old LIST/INFO, id3 / ID3, ds64, JUNK dropped)
              const Part* fmt = nullptr; std::vector<const Part*> keep;
              for (auto& q : L.parts) { if (q.id == "fmt ") fmt = &q; else if (q.id == "ds64" || q.id == "id3 " || q.id == "ID3 " || q.id == "JUNK") continue;
                  else if (q.id == "LIST") { char t4[5] = {0}; if (q.len >= 4) in.read(q.off, t4, 4); if (!strcmp(t4, "INFO")) continue; keep.push_back(&q); } else keep.push_back(&q); }
              if (!fmt || fmt->len < 16) { printf("%s: no fmt chunk\n", src.c_str()); return 2; }
              const std::vector<uint8_t> tc = laestag::wavChunks(tags); long long riff = 4 + 8 + fmt->len + (fmt->len & 1) + (long long)tc.size(), lae = 0;
              for (auto* q : keep) { riff += 8 + q->len + (q->len & 1); if (q->id == "LAE ") lae = q->len; }
              const bool rf64 = riff > 0xFFFFFFFFLL || getenv("LAES_FORCE_RF64");
              for (auto* q : keep) if (rf64 && q->id != "data" && q->id != "LAE " && q->len > 0xFFFFFFFFLL) { printf("%s: chunk %s beyond 4 GB\n", src.c_str(), q->id.c_str()); return 2; }
              std::vector<uint8_t> h; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) h.push_back((uint8_t)(v >> (8 * i))); }; auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) h.push_back((uint8_t)(v >> (8 * i))); };
              if (rf64) { uint8_t fb[16]; in.read(fmt->off, fb, 16); const int align = fb[12] | fb[13] << 8;
                  h.insert(h.end(), {'R', 'F', '6', '4', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 40, 0, 0, 0}); u64((uint64_t)(riff + 48)); u64((uint64_t)L.audioLen); u64((uint64_t)(align ? L.audioLen / align : 0));
                  h.insert(h.end(), {1, 0, 0, 0, 'L', 'A', 'E', ' '}); u64((uint64_t)lae); }
              else { h.insert(h.end(), {'R', 'I', 'F', 'F'}); p32((uint32_t)riff); h.insert(h.end(), {'W', 'A', 'V', 'E'}); }
              h.insert(h.end(), {'f', 'm', 't', ' '}); p32((uint32_t)fmt->len); o.put(h.data(), h.size()); copyRange(in, o, fmt->off, fmt->len); if (fmt->len & 1) { const uint8_t z = 0; o.put(&z, 1); }
              o.put(tc.data(), tc.size());
              for (auto* q : keep) { std::vector<uint8_t> ch(q->id.begin(), q->id.end()); const bool big = rf64 && (q->id == "data" || q->id == "LAE ");
                  for (int i = 0; i < 4; i++) ch.push_back(big ? 0xFF : (uint8_t)((uint64_t)q->len >> (8 * i))); o.put(ch.data(), ch.size()); copyRange(in, o, q->off, q->len); if (q->len & 1) { const uint8_t z = 0; o.put(&z, 1); } } }
          o.close(); }
        std::string why; { laess::In nw(out + ".part"); Layout N; why = readLayout(nw, N);
            if (why.empty() && shaOf(in, {{L.audioOff, L.audioLen}}) != shaOf(nw, {{N.audioOff, N.audioLen}})) why = "the audio bytes differ";
            if (why.empty() && shaOf(in, L.payload) != shaOf(nw, N.payload)) why = "the payload differs"; }
        if (why.empty()) why = checkStored(out + ".part", threads);
        if (!why.empty()) { fs::remove(out + ".part", ec); printf("self-check failed: %s; nothing written\n", why.c_str()); return 2; }
        fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; }
        size_t pb = 0; for (auto& p : tags.pics) pb += p.data.size();
        printf("%s: %s form, %zu tag(s), %zu picture(s) (%.1f MB); audio bytes, payload and every master's SHA-256 checked\n", out.c_str(), L.flac ? "FLAC" : "WAV", tags.kv.size(), tags.pics.size(), pb / 1e6);
        return 0; }
    catch (const std::exception& x) { std::error_code ec; fs::remove(std::string(argv[3]) + ".part", ec); printf("stopped: %s; nothing written\n", x.what()); return 2; }
    if (cmd == "enc" && argc >= 4) try {
        const std::string out = argv[2], corePath = arg(argc, argv, "--core"); const bool derive = flag(argc, argv, "--derive-core");
        // --timing: prints the time of each stage at the end (L5, 2026-11-03); the file written does not change
        const bool timing = flag(argc, argv, "--timing"); std::string tlog; auto t0 = std::chrono::steady_clock::now();
        auto stage = [&](const char* s) { const auto t = std::chrono::steady_clock::now(); char b[128]; snprintf(b, sizeof b, "  %-22s %8.2f s   peak memory so far %7.0f MB\n", s, std::chrono::duration<double>(t - t0).count(), peakMemory() / 1048576.0); tlog += b; t0 = t; };
        std::vector<std::string> paths; std::vector<double> Ts; std::vector<bool> autoT; std::vector<laess::WavInfo> wis;
        for (int i = 3; i < argc; i++) { if (argv[i][0] == '-') continue; std::string a = argv[i]; double T = 0; bool au = false; size_t at = a.rfind('@');
            if (at != std::string::npos && at > 2) { au = a.substr(at + 1) == "auto"; T = au ? 0 : atof(a.substr(at + 1).c_str()); a = a.substr(0, at); }
            laess::WavInfo wi; std::string e = laess::probeMaster(a, wi); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } paths.push_back(a); Ts.push_back(T); autoT.push_back(au); wis.push_back(wi); }
        if (paths.empty()) { printf("no master given\n"); return 1; }
        {   // file order: more bits first, then higher rate (a master can only refer to earlier ones; a shorter word refers to a longer one cheaply)
            std::vector<size_t> ord(paths.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return wis[a].bits != wis[b].bits ? wis[a].bits > wis[b].bits : wis[a].rate > wis[b].rate; });
            std::vector<std::string> p2; std::vector<double> t2; std::vector<bool> a2; std::vector<laess::WavInfo> w2;
            for (size_t k = 0; k < ord.size(); k++) { p2.push_back(paths[ord[k]]); t2.push_back(Ts[ord[k]]); a2.push_back(autoT[ord[k]]); w2.push_back(wis[ord[k]]); }
            paths.swap(p2); Ts.swap(t2); autoT.swap(a2); wis.swap(w2); }
        // the form: .flac = FLAC, .wav = WAV, .lae (the format's own name, 2026-11-05) = --form=flac (default) or --form=wav
        const std::string formArg = arg(argc, argv, "--form"); const bool laeName = out.size() > 4 && !_stricmp(out.c_str() + out.size() - 4, ".lae");
        if (!formArg.empty() && formArg != "flac" && formArg != "wav") { printf("--form must be flac or wav\n"); return 1; }
        const bool asFlac = laeName ? formArg != "wav" : out.size() > 5 && !_stricmp(out.c_str() + out.size() - 5, ".flac");
        if (!formArg.empty() && !laeName && (formArg == "flac") != asFlac) { printf("--form=%s does not match the name %s (use .lae to choose the form freely)\n", formArg.c_str(), out.c_str()); return 1; }
        laestag::Tags tags; { const std::string e = laestag::parseArgs(argc, argv, tags); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; } }   // before any work
        const int base = wis[0].rate % 44100 == 0 ? 44100 : 48000;
        const std::string cbArg = arg(argc, argv, "--core-bits"); int maxBits = 0, maxSt = 0; for (auto& w : wis) { maxBits = std::max(maxBits, w.bits); int st = 0; while ((base << st) < w.rate && st < 4) st++; maxSt = std::max(maxSt, st); }
        const int coreBits = !cbArg.empty() ? atoi(cbArg.c_str()) : (asFlac ? std::min(maxBits, 24) : 16);
        if (coreBits != 16 && coreBits != 24 && coreBits != 32) { printf("--core-bits must be 16, 24 or 32\n"); return 1; }
        if (corePath.empty() && !derive) { printf("give --core=<core.wav> or --derive-core\n"); return 1; }
        uint64_t reserve = 2ull << 30; if (const char* e = getenv("LAES_DISK_RESERVE")) reserve = strtoull(e, nullptr, 10);   // bytes kept free; the variable: tests
        const bool smallDisk = flag(argc, argv, "--small-disk"), diskRemove = flag(argc, argv, "--disk-stop-remove"), diskStop = diskRemove || flag(argc, argv, "--disk-stop");
        std::string job = std::string("laes enc work folder, build ") + __DATE__ + " " + __TIME__ + "\n";   // what the files depend on: the arguments and the masters
        for (int i = 2; i < argc; i++) { const std::string a = argv[i]; if (a == "--timing" || a == "--disk-stop" || a == "--disk-stop-remove" || a == "--force-disk" || a.rfind("--mem", 0) == 0 || laestag::isTagArg(a)) continue; job += a + "\n"; }
        for (const std::string& p : paths) { std::error_code ec; const auto sz = fs::file_size(p, ec); const auto tm = fs::last_write_time(p, ec).time_since_epoch().count();
            job += p + "\t" + std::to_string((unsigned long long)sz) + "\t" + std::to_string((long long)tm) + "\n"; }
        bool anyAuto = false; for (bool au : autoT) anyAuto |= au;
        for (const std::string& p : {corePath, anyAuto ? std::string(laeEncPath(argc, argv)) : std::string()}) if (!p.empty()) { std::error_code ec; const auto sz = fs::file_size(p, ec); if (ec) continue;   // the core and LAEY's encoder (@auto) too
            job += p + "\t" + std::to_string((unsigned long long)sz) + "\t" + std::to_string((long long)fs::last_write_time(p, ec).time_since_epoch().count()) + "\n"; }
        uint64_t oldWork = 0; { std::string old; if (laess::getState(out + ".laes.tmp/job.txt", old) && old == job) oldWork = laess::folderBytes(out + ".laes.tmp"); }   // counts as free   // D6 (2026-11-05): no FLAC copy, the largest level not kept; made again when read
        std::vector<int> stOf(wis.size()); for (size_t i = 0; i < wis.size(); i++) { int st = 0; while ((base << st) < wis[i].rate && st < 4) st++; stOf[i] = st; }
        auto othersOf = [&](size_t i) { std::vector<int> o; for (size_t k = 0; k < stOf.size(); k++) if (k != i) o.push_back(stOf[k]); return o; };
        {   // disk needed by the work folder, estimated before it is made (2026-11-05): a FLAC master's decoded copy and the kept rate levels
            // (4 bytes per value, 8 for a 32-bit master; one more level while the next is made) exactly; then two candidates of the largest
            // master at a time, and the two payloads and the output: 1.5 x the masters' PCM (a payload is usually far smaller)
            uint64_t need = 0, pcm = 0, big = 0;
            for (size_t i = 0; i < wis.size(); i++) { const laess::WavInfo& w = wis[i]; const int st = stOf[i]; const uint64_t by = w.bits == 32 ? 8 : 4; if (w.flac && !smallDisk) need += (uint64_t)w.dataBytes;
                std::vector<bool> keep((size_t)st + 1, false); keep[0] = true; for (int o : othersOf(i)) keep[(size_t)std::min(st, o)] = true; uint64_t lv = 0, top = 0;
                for (int k = 0; k < st; k++) { const uint64_t b = (uint64_t)laes::levLen(w.frames, st - k) * 2 * by; if (keep[(size_t)k] && !(smallDisk && k == st - 1)) lv += b; else top = std::max(top, b); }
                need += lv + top; pcm += (uint64_t)w.dataBytes; big = std::max(big, (uint64_t)w.dataBytes); }
            need += 2 * big + pcm * 3 / 2; std::error_code ec; fs::path dir = fs::absolute(fs::path(out), ec).parent_path(); fs::space_info sp = fs::space(dir, ec); sp.available += oldWork;
            if (!ec && need + reserve > sp.available) { printf("the work folder needs about %.1f GB (and 2 GB kept free), %.1f GB free on that disk%s\n", need / 1073741824.0, sp.available / 1073741824.0,
                                                                     flag(argc, argv, "--force-disk") ? "; --force-disk: running anyway" : "; nothing written (--force-disk runs anyway)");
                printf("%s%.1f%s%.1f%s\n", u8"  \u5de5\u4f5c\u6587\u4ef6\u5939\u5927\u7ea6\u9700\u8981 ", need / 1073741824.0, u8" GB\uff08\u53e6\u5916\u4fdd\u7559 2 GB\uff09\uff0c\u8fd9\u5757\u78c1\u76d8\u5269 ", sp.available / 1073741824.0,
                       flag(argc, argv, "--force-disk") ? u8" GB\uff1b--force-disk\uff1a\u7167\u6837\u5f00\u59cb" : u8" GB\uff1b\u6ca1\u6709\u5f00\u59cb\uff0c\u4e5f\u6ca1\u6709\u5199\u4efb\u4f55\u6587\u4ef6\uff08\u52a0 --force-disk \u53ef\u4ee5\u7167\u6837\u5f00\u59cb\uff09");
                if (!flag(argc, argv, "--force-disk")) return 2; }
            if (timing) printf("work folder: about %.1f GB needed, %.1f GB free\n", need / 1073741824.0, ec ? 0.0 : sp.available / 1073741824.0); }
        bool resumed = false; Gone g{workDir(out, job, &resumed)}; g.keep = !diskRemove; const int64_t piece = pieceFor(maxSt);
        if (resumed) printf("continuing from the work folder %s\n%s%s%s\n", g.p.c_str(), u8"  \u4ece\u5de5\u4f5c\u6587\u4ef6\u5939 ", g.p.c_str(), u8" \u63a5\u7740\u505a");
        laess::DiskGuard dg; dg.dir = g.p; dg.track = timing; dg.reserve = reserve; dg.stop = diskStop; dg.removeNote = diskRemove; laess::diskGuard() = &dg; struct Unhook { ~Unhook() { laess::diskGuard() = nullptr; } } unhook;
        std::vector<laess::Prep> pm(paths.size());
        std::vector<bool> restoredPrep(paths.size(), false);
        for (size_t i = 0; i < paths.size(); i++) { const std::string stem = g.p + "/m" + std::to_string(i); pm[i].derive = smallDisk; pm[i].threads = threads;
            std::string t; if (resumed && laess::getState(stem + ".prep", t) && laess::prepFromState(t, pm[i]) && pm[i].path == paths[i]) restoredPrep[i] = true; }   // made by an earlier run
        {   // step 1 for several masters at once (speed, 2026-11-05): each read is one thread (a WAV read and hashed, a FLAC decoded); up to 8
            // at a time, a few tens of MB each. Each master's result depends only on its own file.
            std::vector<std::string> err(paths.size()); std::atomic<size_t> nextM(0);
            auto readSome = [&]() { for (size_t i = nextM++; i < paths.size(); i = nextM++) if (!restoredPrep[i]) {
                try { err[i] = laess::prepRead(paths[i], g.p + "/m" + std::to_string(i), pm[i]); } catch (const std::exception& x) { err[i] = std::string("\x01") + x.what(); } } };
            const size_t nt = std::min<size_t>({paths.size(), 8, (size_t)std::max(1, threads)}); std::vector<std::thread> pool;
            for (size_t t = 1; t < nt; t++) pool.emplace_back(readSome); readSome(); for (auto& p : pool) p.join();
            for (const std::string& e : err) if (!e.empty()) { if (e[0] == '\x01') laess::fail(e.substr(1)); printf("%s\n", e.c_str()); return 2; } }
        for (size_t i = 0; i < paths.size(); i++) if (!restoredPrep[i]) { const std::string stem = g.p + "/m" + std::to_string(i);   // step 2, one master at a time
            const std::string e = laess::prepLevels(pm[i], base, stem, piece, threads, othersOf(i)); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
            laess::putState(stem + ".prep", laess::prepState(pm[i])); }
        stage("read, levels, octaves");
        size_t coreSrc = SIZE_MAX; Pcm core; laess::Core32 core32;   // core: a given core (--core) until the alignment; core32: from then on (a derived one: at once)
        if (!corePath.empty()) { std::string e = readWav(corePath, core); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } if (core.rate != base) { printf("the core master must be at %d Hz\n", base); return 2; } }
        else { bool dith; std::vector<laess::Prep*> pp; for (auto& m : pm) pp.push_back(&m); core32 = laess::deriveCore32(pp, base, coreBits, coreSrc, dith, 20261028); printf("core master: %d-bit base level of %s%s\n", coreBits, paths[coreSrc].c_str(), dith ? " (TPDF-dithered)" : ""); }
        for (size_t i = 0; i < paths.size(); i++) if (autoT[i]) {   // master.wav@auto: T from LAEY's search against the given core
            if (corePath.empty() || core.bits != 16) { printf("@auto needs --core=<cd.wav>, a 16-bit CD / DVD (a derived core is at T = 0)\n"); return 1; }
            if (wis[i].flac) { printf("%s: @auto needs the master as a WAV (LAEY's search reads WAV)\n", paths[i].c_str()); return 1; }
            const AutoT A = autoAlign(corePath, core, paths[i], pm[i], laeEncPath(argc, argv)); if (!A.found) { printf("%s: %s; nothing written\n", paths[i].c_str(), A.why.c_str()); return 2; }
            if (!acceptAlign(A, paths[i], flag(argc, argv, "--accept"))) { printf("alignment not accepted; nothing written\n"); return 2; } Ts[i] = A.T; }
        stage("core master, alignment");
        if (!corePath.empty()) core32 = laess::toCore32(core);   // from here on the core as int32 (memory, 2026-11-05: half of the int64 copy)
        std::vector<laess::MasterSel> sel(paths.size()); for (size_t i = 0; i < paths.size(); i++) { MasterIn t; setT(t, Ts[i]); sel[i].storage = -1; sel[i].whole = t.whole; sel[i].frac = t.frac; }
        // file order, tried two ways, the smaller kept: (a) more bits first; (b) the master the core was derived from first, then the others more bits first
        std::vector<std::vector<size_t>> orders(1, std::vector<size_t>(paths.size())); for (size_t i = 0; i < paths.size(); i++) orders[0][i] = i;
        if (coreSrc != SIZE_MAX && coreSrc != 0) { std::vector<size_t> b(1, coreSrc); for (size_t i = 0; i < paths.size(); i++) if (i != coreSrc) b.push_back(i); orders.push_back(b); }
        std::vector<MasterInfo> info; std::string bodyPath; long long bodySize = -1; std::vector<size_t> pick;
        laess::CandCache cache;   // with two orders, the second takes the sizes of the candidates the first coded (only its chosen ones are coded again)
        const std::string cacheLog = g.p + "/cache.txt";   // the cache's entries as they are made (resume)
        // resume checks (2026-11-05, review): the T of every master as this run has it - a state made with other T values is not used
        std::string tsig; for (const auto& s : sel) tsig += std::to_string(s.whole) + ":" + std::to_string(s.frac) + ";";
        if (resumed) {   // the cache's sizes as made before: whole lines only (key, two numbers), and the file rewritten with just those
            std::string t, good; if (laess::getState(cacheLog, t)) { size_t a = 0; for (size_t e; (e = t.find('\n', a)) != std::string::npos; a = e + 1) { const std::string ln = t.substr(a, e - a);
                    const size_t p1 = ln.find('\t'), p2 = p1 == std::string::npos ? p1 : ln.find('\t', p1 + 1); if (p1 == 0 || p2 == std::string::npos || ln.find('\t', p2 + 1) != std::string::npos) continue;
                    char* q1 = nullptr; char* q2 = nullptr; const double b0 = strtod(ln.c_str() + p1 + 1, &q1), b1 = strtod(ln.c_str() + p2 + 1, &q2);
                    if (q1 != ln.c_str() + p2 || q2 != ln.c_str() + ln.size()) continue;
                    laess::CachedCand& cc = cache[ln.substr(0, p1)]; cc.b[0] = b0; cc.b[1] = b1; good += ln + "\n"; } }
            laess::putState(cacheLog, good); }
        std::vector<long long> osz(orders.size(), -1); std::vector<std::vector<MasterInfo>> oinfo(orders.size());
        const std::string pickP = g.p + "/pick.done"; int picked = -1;   // the order kept, once both are done (the other is not made again)
        { std::string t; if (resumed && laess::getState(pickP, t)) { laess::StateR r(t); const int64_t pi = r.i(); const std::string ts = r.str(); if (!r.bad && ts == tsig && pi >= 0 && pi < (int64_t)orders.size()) picked = (int)pi; } }
        for (size_t oi = 0; oi < orders.size(); oi++) { if (picked >= 0 && (int)oi != picked) continue;
            const auto& ord = orders[oi]; std::vector<const laess::Prep*> m2; std::vector<laess::MasterSel> s2; for (size_t k : ord) { m2.push_back(&pm[k]); s2.push_back(sel[k]); }
            std::vector<MasterInfo>& i2 = oinfo[oi]; const std::string bp = g.p + "/body" + std::to_string(oi), stO = g.p + "/o" + std::to_string(oi) + ".done";
            if (resumed) { std::string t; std::error_code ec; if (laess::getState(stO, t)) { laess::StateR r(t); const long long sz = r.i(); const std::string ts = r.str(); const int64_t n = r.i(); i2.assign((size_t)std::max<int64_t>(0, std::min<int64_t>(n, 16)), MasterInfo());
                  for (auto& I : i2) laess::getInfo(r, I);
                  if (!r.bad && ts == tsig && n == (int64_t)ord.size() && fs::exists(bp, ec) && sz == (long long)fs::file_size(bp, ec)) { osz[oi] = sz; continue; }   // made by an earlier run
                  // a stale o<oi>.done (other T, damaged, body gone): the whole order is made again. Without one the order was not
                  // finished: its masters' choice states stay, each checked (files' sizes, T) when encodeLaesS restores it.
                  fs::remove(stO, ec); for (size_t mi = 0; mi < ord.size(); mi++) fs::remove(g.p + "/o" + std::to_string(oi) + "_m" + std::to_string(mi) + ".done", ec); } }
            std::vector<std::string> kept;
            const std::string err = laess::encodeLaesS(core32, base, m2, s2, i2, bp, g.p + "/o" + std::to_string(oi), asFlac ? 1 : 0, piece, threads, orders.size() > 1 ? &cache : nullptr, cacheLog, &kept);
            if (!err.empty()) { printf("%s\n", err.c_str()); return 2; } osz[oi] = (long long)fs::file_size(bp);
            { laess::StateW w; w.i(osz[oi]); w.str(tsig); w.i((int64_t)i2.size()); for (const auto& I : i2) laess::putInfo(w, I); laess::putState(stO, w.s); }
            for (const std::string& f : kept) { std::error_code ec; fs::remove(f, ec); } }   // the chosen layers, now in the payload
        for (size_t oi = 0; oi < orders.size(); oi++) if (osz[oi] >= 0 && (bodySize < 0 || osz[oi] < bodySize)) { bodyPath = g.p + "/body" + std::to_string(oi); bodySize = osz[oi]; info = oinfo[oi]; pick = orders[oi]; picked = (int)oi; }
        { laess::StateW w; w.i(picked); w.str(tsig); laess::putState(pickP, w.s); }   // before the other body goes
        for (size_t oi = 0; oi < orders.size(); oi++) { const std::string bp = g.p + "/body" + std::to_string(oi); if (bp != bodyPath) { std::error_code ec; fs::remove(bp, ec); } }
        laess::diskCheck(true);   // the largest size: before the work folder goes
        stage("candidates, payload");
        { std::vector<std::string> p2; std::vector<laess::Prep> q2; for (size_t k : pick) { p2.push_back(paths[k]); q2.push_back(pm[k]); } paths.swap(p2); pm.swap(q2); }
        printf("file order:"); for (size_t k = 0; k < paths.size(); k++) printf(" %zu = %s;", k + 1, paths[k].c_str()); printf("%s\n", orders.size() > 1 ? " (the smaller of two orders)" : "");
        if (asFlac) {   // FLAC form, then read back: core, MD5 and LAES block must come back unchanged
            std::vector<uint8_t> si;   // "fLaC" and STREAMINFO (42 bytes)
            const std::vector<uint8_t> tagBlocks = laestag::flacBlocks(tags, bodySize == 0);
            { std::vector<uint8_t> fl0 = laesflac::writeFlac(core32.c, base, core32.bits, std::vector<uint8_t>());   // "fLaC", STREAMINFO, the audio frames
              si.assign(fl0.begin(), fl0.begin() + 42);
              const size_t pieceA = (1u << 24) - 1 - 4; const long long napp = bodySize == 0 ? 0 : (bodySize + (long long)pieceA - 1) / (long long)pieceA;
              laess::Out o(out + ".part"); laess::In body(bodyPath); std::vector<uint8_t> h(si); if (napp || !tags.empty()) h[4] = 0x00; o.put(h.data(), h.size());   // STREAMINFO is not the last block
              o.put(tagBlocks.data(), tagBlocks.size());   // tags and pictures, before the LAES block (none: nothing)
              std::vector<uint8_t> b; for (long long i = 0; i < napp; i++) { const long long a = i * (long long)pieceA, len = std::min<long long>((long long)pieceA, bodySize - a);
                  const uint32_t L = (uint32_t)(len + 4); const uint8_t bh[8] = {(uint8_t)((i + 1 == napp ? 0x80 : 0) | 2), (uint8_t)(L >> 16), (uint8_t)(L >> 8), (uint8_t)L, 'L', 'A', 'E', '2'}; o.put(bh, 8);
                  b.resize((size_t)len); body.read(a, b.data(), b.size()); o.put(b.data(), b.size()); laess::diskCheck(); }
              o.put(fl0.data() + 42, fl0.size() - 42); o.close(); }   // the FLAC bytes go here (memory, 2026-11-05)
            {   // read back: the APPLICATION blocks against the payload, then the audio frames one at a time against the core and their MD5
                // against STREAMINFO (memory, 2026-11-05: frame by frame; before, the whole core was decoded at once next to the one in memory)
                laess::In f(out + ".part"), body(bodyPath); long long p = 4; std::vector<uint8_t> b, c; long long got = 0; bool ok = true, last = false; std::string fe;
                while (!last && ok) { uint8_t bh[4]; f.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const long long len = bh[1] << 16 | bh[2] << 8 | bh[3]; p += 4;
                    if (type == 2) { if (len < 4) { ok = false; break; } b.resize((size_t)len - 4); f.read(p + 4, b.data(), b.size()); c.resize(b.size()); body.read(got, c.data(), c.size()); ok = b == c; got += (long long)b.size(); }
                    p += len; }
                if (!ok || got != bodySize) fe = "LAES block differs";
                if (!tagBlocks.empty()) { c.resize(tagBlocks.size()); f.read(42, c.data(), c.size()); if (c != tagBlocks) fe = "tag blocks differ"; }
                laesflac::Info fi; { laesflac::BR r{si.data() + 8, 34}; fi.minBs = (int)r.get(16); fi.maxBs = (int)r.get(16); r.get(24); r.get(24); fi.rate = (int)r.get(20); fi.channels = (int)r.get(3) + 1; fi.bps = (int)r.get(5) + 1; fi.total = r.get(36); memcpy(fi.md5, si.data() + 8 + 18, 16); }
                alignhash::Md5 md5; const int by = (fi.bps + 7) / 8; std::vector<uint8_t> fb, mb; size_t head = 0; long long at = p; uint64_t frameNo = 0, have = 0;
                while (fe.empty()) {
                    if (at < f.n && fb.size() - head < (8u << 20)) { const size_t k = (size_t)std::min<long long>(16 << 20, f.n - at); const size_t o0 = fb.size(); fb.resize(o0 + k); f.read(at, fb.data() + o0, k); at += (long long)k; continue; }
                    if (head >= fb.size()) break;
                    V ch[2]; size_t used = 0; fe = laesflac::decodeFrame(fb.data() + head, fb.size() - head, fi, frameNo, have, ch, used); if (!fe.empty()) break;
                    head += used; frameNo++; const size_t k = ch[0].size(); if (have + k > (uint64_t)core32.frames()) { fe = "more audio than the core master"; break; }
                    for (int q = 0; q < 2; q++) if (!std::equal(ch[q].begin(), ch[q].end(), core32.c[q].begin() + (long long)have)) fe = "audio differs from the core master";
                    mb.resize(k * 2 * (size_t)by); for (size_t i = 0; i < k; i++) for (int q = 0; q < 2; q++) { const uint64_t v = (uint64_t)ch[q][i]; for (int z = 0; z < by; z++) mb[(i * 2 + (size_t)q) * (size_t)by + (size_t)z] = (uint8_t)(v >> (8 * z)); }
                    md5.update(mb.data(), mb.size()); have += k;
                    if (head > (32u << 20)) { fb.erase(fb.begin(), fb.begin() + (long long)head); head = 0; } }
                if (fe.empty() && (have != (uint64_t)core32.frames() || fi.total != have)) fe = "sample count differs";
                if (fe.empty()) { char want[33]; for (int i = 0; i < 16; i++) snprintf(want + 2 * i, 3, "%02x", fi.md5[i]); if (md5.hex() != want) fe = "MD5 differs"; }
                if (!fe.empty()) { fs::remove(out + ".part"); printf("FLAC self-check failed: %s; nothing written\n", fe.c_str()); return 2; } } }
        else {   // WAV form: the core, then the "LAE " chunk with the payload
            const int by = core32.bits / 8; const long long n = core32.frames(), db = n * 2 * by; const bool ext = core32.bits > 16; const uint32_t fmtLen = ext ? 40 : 16; const long long extra = bodySize ? 8 + bodySize + (bodySize & 1) : 0;
            // beyond 4 GB: RF64 (EBU Tech 3306 / ITU-R BS.2088): "RF64", a "ds64" chunk with the 64-bit sizes (RIFF, data, the LAE chunk in its
            // table), the 32-bit size fields set to 0xFFFFFFFF; everything else as in RIFF
            const std::vector<uint8_t> tagChunks = laestag::wavChunks(tags); const long long tc = (long long)tagChunks.size();   // LIST/INFO + "id3 " between "fmt " and "data" (none: nothing)
            const long long riff = 4 + 8 + fmtLen + tc + 8 + db + (db & 1) + extra; const bool rf64 = riff > 0xFFFFFFFFLL || getenv("LAES_FORCE_RF64");   // the variable: tests on small files
            Pcm hdr; hdr.rate = core32.rate; hdr.bits = core32.bits; std::vector<uint8_t> lae; std::vector<uint8_t> h = wavBytes(hdr, nullptr);   // header of an empty file: sizes patched below
            h.insert(h.end() - 8, tagChunks.begin(), tagChunks.end());
            auto p32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); }; p32(4, (uint32_t)riff); p32(h.size() - 4, (uint32_t)db);
            if (rf64) { std::vector<uint8_t> r = {'R', 'F', '6', '4', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 40, 0, 0, 0};
                auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) r.push_back((uint8_t)(v >> (8 * i))); };
                u64((uint64_t)(riff + 48)); u64((uint64_t)db); u64((uint64_t)n); r.insert(r.end(), {1, 0, 0, 0, 'L', 'A', 'E', ' '}); u64((uint64_t)bodySize);
                r.insert(r.end(), h.begin() + 12, h.end()); for (size_t i = r.size() - 4; i < r.size(); i++) r[i] = 0xFF; h.swap(r); }   // the data size: in ds64
            laess::Out o(out + ".part"); o.put(h.data(), h.size()); std::vector<uint8_t> b; const long long P = 1 << 18;
            for (long long a = 0; a < n; a += P) { const long long e = std::min(n, a + P); b.clear(); for (long long i = a; i < e; i++) for (int c = 0; c < 2; c++) { const uint32_t v = (uint32_t)(int32_t)core32.c[c][(size_t)i]; for (int k = 0; k < by; k++) b.push_back((uint8_t)(v >> (8 * k))); } o.put(b.data(), b.size()); }
            if (db & 1) { const uint8_t z = 0; o.put(&z, 1); }
            if (bodySize) { const uint8_t ch[8] = {'L', 'A', 'E', ' ', rf64 ? (uint8_t)0xFF : (uint8_t)bodySize, rf64 ? (uint8_t)0xFF : (uint8_t)(bodySize >> 8), rf64 ? (uint8_t)0xFF : (uint8_t)(bodySize >> 16), rf64 ? (uint8_t)0xFF : (uint8_t)(bodySize >> 24)}; o.put(ch, 8);
                laess::In body(bodyPath); for (long long a = 0; a < bodySize; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, bodySize - a)); body.read(a, b.data(), b.size()); o.put(b.data(), b.size()); laess::diskCheck(); }
                if (bodySize & 1) { const uint8_t z = 0; o.put(&z, 1); } }
            o.close(); }
        stage("core master frames, file");
        const int64_t coreFrames = core32.frames(); for (int c = 0; c < 2; c++) std::vector<int32_t>().swap(core32.c[c]);   // not needed any more: freed before the self-check (memory, 2026-11-05)
        // self-check of the written file, read exactly as a player reads it (the playback decoder, laes_play.h; 2026-11-03): the whole file
        // (file SHA-256, payload CRC-32, core), then every master decoded and compared sample by sample with its input; any difference:
        // the file is removed and nothing is written
        if (const char* fb = getenv("LAES_SELFCHECK_FLIP")) {   // tests only: one byte of the written file flipped at this offset (from the end when negative)
            FILE* t = fopen((out + ".part").c_str(), "r+b"); if (t) { long long at = atoll(fb); _fseeki64(t, 0, SEEK_END); const long long n = _ftelli64(t); if (at < 0) at += n;
                if (at >= 0 && at < n) { _fseeki64(t, at, SEEK_SET); const int c = fgetc(t); _fseeki64(t, at, SEEK_SET); fputc(c ^ 0x10, t); } fclose(t); } }
        { const std::string why = selfCheckFile(out + ".part", pm, threads);
          if (!why.empty()) { std::error_code ec; fs::remove(out + ".part", ec); printf("self-check failed: %s; nothing written\n", why.c_str()); return 2; } }
        stage("self-check");
        { std::error_code ec; fs::remove(out, ec); fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; } }
        double secs = (double)coreFrames / base; printf("%s: %s form, core master %d Hz %d-bit, %zu master(s), LAES block %.1f kb/s (file %.1f MB, %.1f kb/s)\n", out.c_str(), asFlac ? "FLAC" : "WAV", base, core32.bits, pm.size(),
                                                         bodySize * 8 / secs / 1000, (double)fs::file_size(out) / 1e6, (double)fs::file_size(out) * 8 / secs / 1000);
        for (size_t i = 0; i < info.size(); i++) { const std::string tier = info[i].refTier == 0 ? "the core master" : "master " + std::to_string(info[i].refTier);
            const std::string how = info[i].storage == 1 ? "full" : info[i].storage == 0 ? "difference to " + tier + " at the base level"
                                  : "difference to " + tier + " at " + std::to_string(base << std::min(info[i].stages, info[info[i].refTier - 1].stages)) + " Hz";
            printf("  master %zu: %d Hz %d-bit, %s, layers %.1f kb/s\n", i + 1, info[i].rate, info[i].bits, how.c_str(), info[i].layerBits / secs / 1000); }
        if (timing) printf("timing:\n%s", tlog.c_str());
        if (timing) printf("  work folder, largest   %8.1f MB\n", dg.peak / 1048576.0);
        return 0; }
    catch (const std::exception& x) { std::error_code ec; fs::remove(std::string(argv[2]) + ".part", ec); printf("stopped: %s; nothing written\n", x.what());
        if (fs::exists(std::string(argv[2]) + ".laes.tmp/job.txt", ec)) { printf("the work folder %s.laes.tmp is kept: run the same command again to continue (--disk-stop-remove: removed instead)\n", argv[2]);
            printf("%s%s%s\n", u8"  \u5de5\u4f5c\u6587\u4ef6\u5939 ", argv[2], u8".laes.tmp \u5df2\u4fdd\u7559\uff1a\u518d\u8fd0\u884c\u540c\u4e00\u6761\u547d\u4ee4\uff0c\u5c31\u4ece\u505c\u4e0b\u7684\u5730\u65b9\u63a5\u7740\u505a\uff08\u52a0 --disk-stop-remove \u65f6\u6539\u4e3a\u5220\u9664\uff09"); }
        return 2; }
    if ((cmd == "dec" || cmd == "verify") && argc >= 4) try {
        Gone g{workDir(std::string(argv[2]) + (cmd == "dec" ? ".dec" : ".verify"))}; const std::string bodyPath = g.p + "/body.bin"; Pcm core; long long bodySize = 0;
        { laess::In in(argv[2]); uint8_t mg[4] = {0}; if (in.n >= 4) in.read(0, mg, 4);
          if (!memcmp(mg, "fLaC", 4)) {   // metadata: STREAMINFO kept, the "LAE2" APPLICATION blocks into the payload file; the audio through the FLAC reader
              long long p = 4; bool last = false; std::vector<uint8_t> meta = {'f', 'L', 'a', 'C'}, b; laess::Out bo(bodyPath);
              while (!last) { if (p + 4 > in.n) { printf("%s: metadata truncated\n", argv[2]); return 2; } uint8_t bh[4]; in.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const long long len = bh[1] << 16 | bh[2] << 8 | bh[3];
                  if (p + 4 + len > in.n) { printf("%s: metadata truncated\n", argv[2]); return 2; }
                  if (type == 0 || (type != 2)) { b.resize((size_t)len + 4); in.read(p, b.data(), b.size()); if (type == 0) meta.insert(meta.end(), b.begin(), b.end()); }
                  else { b.resize((size_t)len); in.read(p + 4, b.data(), b.size()); if (len >= 4 && !memcmp(b.data(), "LAE2", 4)) { bo.put(b.data() + 4, b.size() - 4); bodySize += len - 4; } }
                  p += 4 + len; }
              bo.close(); if (meta.size() > 4) meta[4] |= 0x80; std::vector<uint8_t> rest((size_t)(in.n - p)); in.read(p, rest.data(), rest.size()); meta.insert(meta.end(), rest.begin(), rest.end());
              laesflac::Info fi; bool md5ok; std::vector<uint8_t> app; std::string e = laesflac::readFlac(meta, fi, core.c, app, md5ok);
              if (!e.empty()) { printf("%s: %s\n", argv[2], e.c_str()); return 2; } core.rate = fi.rate; core.bits = fi.bps;
              printf("FLAC core master: %d Hz %d-bit, %lld frames, MD5 %s\n", core.rate, core.bits, (long long)core.frames(), md5ok ? "ok" : "MISMATCH"); if (!md5ok) return 2; }
          else { laess::WavInfo wi; long long lo = -1, ll = 0; std::string e = laess::probeWav(argv[2], wi, &lo, &ll); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
              core.rate = wi.rate; core.bits = wi.bits; std::vector<int32_t> x; laess::wavFrames(in, wi, 0, wi.frames, x); for (int c = 0; c < 2; c++) { core.c[c].resize((size_t)wi.frames); for (int64_t i = 0; i < wi.frames; i++) core.c[c][(size_t)i] = x[(size_t)i * 2 + c]; }
              laess::Out bo(bodyPath); std::vector<uint8_t> b; if (lo >= 0) for (long long a = 0; a < ll; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, ll - a)); in.read(lo + a, b.data(), b.size()); bo.put(b.data(), b.size()); }
              bo.close(); bodySize = lo >= 0 ? ll : 0; } }
        // the extension's first 4 bytes name the format: "LAE2" = LAES (lossless, this tool), "LAE1" = LAEY (lossy, decoded by lae_dec)
        uint8_t mg[4] = {0}; if (bodySize >= 4) { laess::In b(bodyPath); b.read(0, mg, 4); }
        if (bodySize >= 4 && !memcmp(mg, "LAE1", 4)) { printf("%s: this is an LAEY file (lossy, extension LAE1): decode it with lae_dec; laes reads LAES (extension LAE2)\n", argv[2]); return 2; }
        if (bodySize < 8 || memcmp(mg, "LAE2", 4)) { printf("%s: no LAES extension (LAE2) found\n", argv[2]); return 2; }
        printf("format: LAES (lossless, extension LAE2)\n");
        int maxSt = 0; for (int s = 1; s <= 3; s++) maxSt = s;   // pieces sized for the highest rate a master can have
        const int64_t piece = pieceFor(maxSt); std::vector<laess::DecMaster> dec; std::string de = laess::decodeLaesS(bodyPath, core, core.rate, g.p, dec, piece, threads);
        if (!de.empty()) { printf("%s: %s (the core master is still playable)\n", argv[2], de.c_str()); return 2; }
        bool all = true; for (size_t i = 0; i < dec.size(); i++) { printf("master %zu: %d Hz %d-bit, %lld frames, SHA-256 %s\n", i + 1, dec[i].I.rate, dec[i].I.bits, (long long)dec[i].I.frames, dec[i].shaOk ? "ok" : "MISMATCH"); all &= dec[i].shaOk; }
        // a choice's samples: the core (memory), a master (top level << w) or a rate level (level << w, limited to the word length)
        struct Src { int kind = 0; size_t m = 0; int k = 0; int rate = 0, bits = 0; long long n = 0; };
        auto readSrc = [&](const Src& s, int c, long long a, long long b, V& out) { out.resize((size_t)(b - a));
            if (s.kind == 0) { std::copy(core.c[c].begin() + a, core.c[c].begin() + b, out.begin()); return; }
            const laess::DecMaster& D = dec[s.m]; std::unique_ptr<laess::Sig> sg = laess::levelOf(D, c, s.kind == 1 ? D.I.stages : s.k); sg->get(a, b, out.data()); const int w = D.I.w;
            if (s.kind == 1) { for (auto& v : out) v <<= w; return; }
            const int64_t mx = (1LL << (s.bits - 1)) - 1, mn = -(1LL << (s.bits - 1)); for (auto& v : out) v = std::max(mn, std::min(mx, v << w)); };
        if (cmd == "dec") {
            // every choice the file offers: the core, each master, each master's rate levels (lossless), and a 16-bit version derived
            // from the longest-word choice at each rate that has no 16-bit choice of its own (a new master: lossy against its source)
            struct Choice { std::string name, what; Src s; bool derived = false; int from = -1; };
            std::vector<Choice> ch; { Src s; s.rate = core.rate; s.bits = core.bits; s.n = core.frames(); ch.push_back({"core", "the core master (lossless)", s}); }
            for (size_t i = 0; i < dec.size(); i++) { const std::string mk = "master_" + std::to_string(i + 1); const MasterInfo& I = dec[i].I;
                if (!dec[i].shaOk) { printf("master %zu: SHA-256 mismatch: not offered (nor its rate levels)\n", i + 1); continue; }
                { Src s; s.kind = 1; s.m = i; s.rate = I.rate; s.bits = I.bits; s.n = I.frames; ch.push_back({mk + "_" + std::to_string(I.rate) + "_" + std::to_string(I.bits), "master " + std::to_string(i + 1) + " (lossless)", s}); }
                for (int k = I.stages - 1; k >= 0; k--) { Src s; s.kind = 2; s.m = i; s.k = k; s.rate = I.rate >> (I.stages - k); s.bits = I.bits; s.n = levLen(I.frames, I.stages - k);
                    ch.push_back({mk + "_level_" + std::to_string(s.rate) + "_" + std::to_string(s.bits), "rate level of master " + std::to_string(i + 1) + " (lossless)", s}); } }
            { std::vector<int> rates; for (auto& c : ch) if (std::find(rates.begin(), rates.end(), c.s.rate) == rates.end()) rates.push_back(c.s.rate);
              for (int r : rates) { int best = -1; bool has16 = false; for (size_t j = 0; j < ch.size(); j++) if (ch[j].s.rate == r) { has16 |= ch[j].s.bits == 16; if (best < 0 || ch[j].s.bits > ch[best].s.bits) best = (int)j; }
                  if (has16 || best < 0 || ch[best].s.bits <= 16) continue;
                  const Choice& sc = ch[(size_t)best]; Choice dv; dv.derived = true; dv.s = sc.s; dv.s.bits = 16; dv.from = best;
                  dv.name = "derived16_" + std::to_string(r) + "_from_" + sc.name; dv.what = "16-bit master derived from " + (sc.name == "core" ? std::string("the core master") : sc.name) + " by TPDF requantisation: lossless relative to its definition (bit-identical on every decoder), not lossless relative to its " + std::to_string(sc.s.bits) + "-bit source";
                  ch.push_back(dv); } }
            if (flag(argc, argv, "--list")) { printf("choices (use --pick=<n>[,<n>...] or --pick=<rate>:<bits>):\n");
                for (size_t j = 0; j < ch.size(); j++) printf("  %2zu  %6d Hz %2d-bit  %s\n", j + 1, ch[j].s.rate, ch[j].s.bits, ch[j].what.c_str()); return all ? 0 : 2; }
            std::vector<size_t> pick; const std::string pk = arg(argc, argv, "--pick");
            if (pk.empty()) { pick.push_back(0); for (size_t j = 1; j < ch.size(); j++) if (!ch[j].derived && (ch[j].what.rfind("master", 0) == 0 || flag(argc, argv, "--levels"))) pick.push_back(j); }
            else { size_t a = 0; for (;;) { const size_t b = pk.find(',', a); const std::string t = pk.substr(a, b == std::string::npos ? std::string::npos : b - a); const size_t col = t.find(':'); bool found = false;
                    if (col == std::string::npos) { const size_t j = (size_t)atoi(t.c_str()); if (j >= 1 && j <= ch.size()) { pick.push_back(j - 1); found = true; } }
                    else { const int r = atoi(t.c_str()), bb = atoi(t.c_str() + col + 1); for (size_t j = 0; j < ch.size() && !found; j++) if (ch[j].s.rate == r && ch[j].s.bits == bb) { pick.push_back(j); found = true; } }
                    if (!found) { printf("--pick: '%s' is not one of the choices (see --list)\n", t.c_str()); return 2; } if (b == std::string::npos) break; a = b + 1; } }
            fs::create_directories(argv[3]); const std::string d = std::string(argv[3]) + "\\";
            for (size_t j : pick) { const Choice& C = ch[j]; const Src& S = C.derived ? ch[(size_t)C.from].s : C.s; const int by = C.s.bits / 8; const long long n = C.s.n, db = n * 2 * by;
                Pcm hdr; hdr.rate = C.s.rate; hdr.bits = C.s.bits; std::vector<uint8_t> h = wavBytes(hdr, nullptr); const uint32_t fmtLen = C.s.bits > 16 ? 40 : 16;
                for (int i = 0; i < 4; i++) { h[4 + i] = (uint8_t)((uint32_t)(4 + 8 + fmtLen + 8 + db + (db & 1)) >> (8 * i)); h[h.size() - 4 + i] = (uint8_t)((uint32_t)db >> (8 * i)); }
                // beyond 4 GB: RF64 as the encoder writes it (ds64 with the 64-bit RIFF and data sizes, no table; 32-bit fields 0xFFFFFFFF), 2026-11-04
                const long long riff = 4 + 8 + fmtLen + 8 + db + (db & 1);
                if (riff > 0xFFFFFFFFLL || getenv("LAES_FORCE_RF64")) { std::vector<uint8_t> r = {'R', 'F', '6', '4', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 28, 0, 0, 0};
                    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) r.push_back((uint8_t)(v >> (8 * i))); };
                    u64((uint64_t)(riff + 36)); u64((uint64_t)db); u64((uint64_t)n); r.insert(r.end(), {0, 0, 0, 0});
                    r.insert(r.end(), h.begin() + 12, h.end()); for (size_t i = r.size() - 4; i < r.size(); i++) r[i] = 0xFF; h.swap(r); }   // the data size: in ds64
                bool okw = true; try { laess::Out o(d + C.name + ".wav"); o.put(h.data(), h.size()); V x0, x1; std::vector<uint8_t> b; const long long P = 1 << 18;
                    uint64_t stc[2] = {20261029ull + 0x9E3779B97F4A7C15ull * 1, 20261029ull + 0x9E3779B97F4A7C15ull * 2}; const int sft = C.derived ? S.bits - 16 : 0;   // derive16: one stream per channel
                    for (long long a = 0; a < n; a += P) { const long long e = std::min(n, a + P); readSrc(S, 0, a, e, x0); readSrc(S, 1, a, e, x1); b.clear();
                        for (long long i = a; i < e; i++) for (int c = 0; c < 2; c++) { int64_t v = (c ? x1 : x0)[(size_t)(i - a)];
                            if (C.derived) { uint64_t& st = stc[c]; auto nx = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st >> (64 - sft); }; const int64_t r1 = (int64_t)nx(), r2 = (int64_t)nx();
                                v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (v + r1 - r2 + (1LL << (sft - 1))) >> sft)); }
                            const uint32_t u = (uint32_t)(int32_t)v; for (int k = 0; k < by; k++) b.push_back((uint8_t)(u >> (8 * k))); }
                        o.put(b.data(), b.size()); }
                    if (db & 1) { const uint8_t z = 0; o.put(&z, 1); } o.close(); } catch (const std::exception&) { okw = false; }
                all &= okw; printf("%s.wav: %d Hz %d-bit, %s\n", C.name.c_str(), C.s.rate, C.s.bits, C.what.c_str()); }
            printf("written to %s\n", argv[3]); }
        else { std::vector<bool> used(dec.size(), false);   // each given file must equal one decoded master (the file order may differ from the command line)
            for (int i = 3; i < argc; i++) { if (argv[i][0] == '-') continue; laess::WavInfo wi; std::string re = laess::probeWav(argv[i], wi); int hit = -1;
                for (size_t k = 0; k < dec.size() && hit < 0 && re.empty(); k++) { if (used[k] || wi.rate != dec[k].I.rate || wi.bits != dec[k].I.bits || wi.frames != dec[k].I.frames) continue;
                    Src s; s.kind = 1; s.m = k; laess::In in(argv[i]); bool same = true; std::vector<int32_t> r; V x0, x1; const long long P = 1 << 20;
                    for (long long a = 0; a < wi.frames && same; a += P) { const long long e = std::min<long long>(wi.frames, a + P); laess::wavFrames(in, wi, a, e, r); readSrc(s, 0, a, e, x0); readSrc(s, 1, a, e, x1);
                        for (long long q = 0; q < e - a && same; q++) same = r[(size_t)q * 2] == x0[(size_t)q] && r[(size_t)q * 2 + 1] == x1[(size_t)q]; }
                    if (same) hit = (int)k; }
                if (hit >= 0) { used[(size_t)hit] = true; printf("master %d vs %s: IDENTICAL\n", hit + 1, argv[i]); } else { printf("%s: DIFFERENT (no decoded master equals it)\n", argv[i]); all = false; } }
            for (size_t k = 0; k < dec.size(); k++) if (!used[k] && argc > 3) printf("master %zu: not compared\n", k + 1); }
        return all ? 0 : 2; }
    catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; }
    printf("bad arguments\n"); return 1; }
