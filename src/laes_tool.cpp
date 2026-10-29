// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// laes: LAE Lossless tool, WAV form and FLAC form (draft v1, 2026-10-28 / 29; format in laes_file.h, FLAC in laes_flac.h).
//   laes enc <out.wav | out.flac> (--core=<core.wav> | --derive-core) [--core-bits=16|24|32] <master.wav>[@T] ...
//       T = time of the master on the core's time line, in base-rate samples (decimal, e.g. -0.37); default 0.
//       out.flac = FLAC form: the core in the FLAC audio frames, the LAES block in "LAE2" APPLICATION blocks.
//       --derive-core: the core is a master's base level at --core-bits; default 16 for WAV (CD / DVD), for FLAC the highest master
//       word length up to 24 (a 32-bit master gives a 24-bit core; --core-bits=32 keeps 32). A master with exactly that word length is
//       the source (its base level, no requantisation), otherwise the longest one, TPDF-dithered down.
//   laes dec <in.wav | in.flac> <out dir> [--levels]   writes core.wav and master_<k>_<rate>_<bits>.wav (and every rate level with --levels)
//   laes dec <in> --list                              every choice: core, masters, rate levels (lossless) and derived 16-bit versions
//   laes dec <in> <out dir> --pick=<n>[,<n>...] | --pick=<rate>:<bits>   writes only the chosen ones. A derived 16-bit version exists for each
//       rate without a 16-bit choice of its own: made from the longest-word choice there with a fixed TPDF rule; it is a new master,
//       LOSSY against its source, and its file name says so
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
#include "laes_file_enc.h"
#include "laes_flac_enc.h"
#include "laes_stream_enc.h"   // block-wise enc / dec / verify / align (2026-10-30)
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
    const int by = bits / 8; const long n = (long)(dlen / (2 * by)); p.rate = rate; p.bits = bits;
    for (int c = 0; c < 2; c++) { p.c[c].resize(n); for (long i = 0; i < n; i++) { const uint8_t* q = data + (i * 2 + c) * by; uint32_t v = 0; for (int k = 0; k < by; k++) v |= (uint32_t)q[k] << (8 * k);
            if (bits < 32 && (v >> (bits - 1) & 1)) v |= ~0u << bits; p.c[c][i] = (int64_t)(int32_t)v; } }
    return ""; }
// ---- WAV write: 16-bit PCM (for the core) or EXTENSIBLE for 24/32 bit; optional "LAE " chunk
static std::vector<uint8_t> wavBytes(const Pcm& p, const std::vector<uint8_t>* lae) {
    const int by = p.bits / 8; const long n = p.frames(); const size_t db = (size_t)n * 2 * by; const bool ext = p.bits > 16; const uint32_t fmtLen = ext ? 40 : 16;
    const size_t extra = lae && !lae->empty() ? 8 + lae->size() + (lae->size() & 1) : 0;
    std::vector<uint8_t> o; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i))); }; auto p16 = [&](int v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); };
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); p32((uint32_t)(4 + 8 + fmtLen + 8 + db + (db & 1) + extra)); o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); p32(fmtLen);
    p16(ext ? 0xFFFE : 1); p16(2); p32((uint32_t)p.rate); p32((uint32_t)(p.rate * 2 * by)); p16(2 * by); p16(p.bits);
    if (ext) { p16(22); p16(p.bits); p32(3); const uint8_t guid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}; o.insert(o.end(), guid, guid + 16); }
    o.insert(o.end(), {'d', 'a', 't', 'a'}); p32((uint32_t)db); for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) { uint32_t v = (uint32_t)(int32_t)p.c[c][i]; for (int k = 0; k < by; k++) o.push_back((uint8_t)(v >> (8 * k))); }
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
    for (int c = 0; c < 2; c++) { V x(m.frames()); for (long i = 0; i < m.frames(); i++) x[i] = m.c[c][i] >> w; for (int k = st; k > 0; k--) { V lo, d; laesfast::fwd(x, lo, d); x = lo; }
        core.c[c].resize(x.size()); for (size_t i = 0; i < x.size(); i++) { int64_t v;
            if (drop <= 0) v = x[i] << (-drop);   // exact (the core keeps every bit of the base level)
            else v = (int64_t)std::floor((double)x[i] / std::ldexp(1.0, drop) + r.uni() + r.uni() + 0.5);   // TPDF dither
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
    if ((cd.rate << st) != m.rate || st > 3) { A.why = "master rate must be the core rate times 1, 2, 4 or 8"; return A; }
    if (st == 0) { A.why = "LAEY's search needs a master at 2, 4 or 8 times the core rate; give T by hand (master.wav@T)"; return A; }
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
    if (A.resid > -40) printf("  note: the core is not a close copy of this master (other mix / level / EQ / edit?); the encoder will likely store it in full\n");
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
#else
static unsigned long long physMemory() { FILE* f = fopen("/proc/meminfo", "r"); if (!f) return 0; char line[256]; unsigned long long t = 0;
    while (fgets(line, sizeof line, f)) { unsigned long long kb = 0; if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) t = kb * 1024; } fclose(f); return t; }
#endif
#include "laes_selftest.h"
int main(int argc, char** argv) {
    if (argc < 2) { printf("laes enc | dec | verify | align | selftest | excerpt | mkcd  (see the source header)\n"); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") return selftest::run() ? 2 : 0;
    if (cmd == "reduce" && argc >= 5) {   // laes reduce <in.wav> <out.wav> <bits>: a shorter word length with TPDF dither (+-1 LSB of the result), fixed seed
        Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } const int bits = atoi(argv[4]);
        if ((bits != 16 && bits != 24) || bits >= p.bits) { printf("reduce: <bits> must be 16 or 24 and below the input's %d\n", p.bits); return 1; }
        const int s = p.bits - bits; const int64_t mx = (1LL << (bits - 1)) - 1, mn = -mx - 1; Pcm q; q.rate = p.rate; q.bits = bits; Rng r(4321);
        for (int c = 0; c < 2; c++) { q.c[c].resize(p.c[c].size()); for (size_t i = 0; i < p.c[c].size(); i++) {
                const int64_t r1 = (int64_t)(r.next() >> (64 - s)), r2 = (int64_t)(r.next() >> (64 - s)); q.c[c][i] = std::max(mn, std::min(mx, (p.c[c][i] + r1 - r2 + (1LL << (s - 1))) >> s)); } }
        return writeAll(argv[3], wavBytes(q, nullptr)) ? 0 : 2; }
    if (cmd == "excerpt" && argc >= 7) { Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        long s0 = (long)(atof(argv[4]) * p.rate), L = (long)(atof(argv[5]) * p.rate); int bits = atoi(argv[6]); if (s0 < 0 || s0 + L > p.frames()) { printf("excerpt outside the file\n"); return 2; }
        Pcm q; q.rate = p.rate; q.bits = bits; Rng r(12345); for (int c = 0; c < 2; c++) { q.c[c].assign(p.c[c].begin() + s0, p.c[c].begin() + s0 + L);
            for (auto& v : q.c[c]) { int64_t x = v >> (p.bits - 24) << 0; if (p.bits != 24) x = v; if (bits == 32) v = (x << 8) | (int64_t)(r.next() & 0xFF); else if (bits == 16) v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(x / 256.0 + r.uni() + r.uni() + 0.5))); else v = x; } }
        if (argc >= 8) { const long pz = (long)(atof(argv[7]) * p.rate), fd = std::min<long>(L / 2, p.rate / 20); for (int c = 0; c < 2; c++) { for (long i = 0; i < fd; i++) { const double g = 0.5 - 0.5 * std::cos(3.14159265358979323846 * i / fd); q.c[c][i] = (int64_t)std::llround(q.c[c][i] * g); q.c[c][L - 1 - i] = (int64_t)std::llround(q.c[c][L - 1 - i] * g); } q.c[c].insert(q.c[c].begin(), pz, 0); q.c[c].insert(q.c[c].end(), pz, 0); } }   // [pad s]: 50 ms fades, then digital silence at both ends
        return writeAll(argv[3], wavBytes(q, nullptr)) ? 0 : 2; }
    if (cmd == "mkcd" && argc >= 5) {   // test helper: laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]: a 16-bit CD whose sample i is the master's base level at time i + tau (base samples)
        Pcm m; std::string e = readWav(argv[2], m); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } const double tau = atof(argv[4]), gain = argc >= 6 ? atof(argv[5]) : 1.0, noise = argc >= 7 ? atof(argv[6]) : 0.0;   // noise: extra white noise, LSB rms
        const int base = m.rate % 44100 == 0 ? 44100 : 48000; int st = 0; while ((base << st) < m.rate) st++; Pcm cd; cd.rate = base; cd.bits = 16; Rng r(777);
        for (int c = 0; c < 2; c++) { V x = m.c[c]; for (int k = st; k > 0; k--) { V lo, d; laesfast::fwd(x, lo, d); x = lo; } const long n = (long)x.size(); std::vector<double> h(256); const double fr = tau - std::floor(tau); const long ti = (long)std::floor(tau);
            for (int q = -127; q <= 128; q++) { double u = fr - q, rr = u / 128; double si = std::fabs(u) < 1e-12 ? 1 : std::sin(3.14159265358979323846 * u) / (3.14159265358979323846 * u); h[q + 127] = std::fabs(rr) < 1 ? si * I0Series(14 * std::sqrt(1 - rr * rr)) / I0Series(14) : 0; }
            cd.c[c].resize(n); const double sc = std::ldexp(1.0, m.bits - 16);
            for (long i = 0; i < n; i++) { double a = 0; for (int q = -127; q <= 128; q++) { long j = i + ti + q; a += h[q + 127] * (j >= 0 && j < n ? (double)x[j] : 0); }
                cd.c[c][i] = std::fabs(a * gain / sc) < 0.5 && noise == 0 ? 0 : std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(a * gain / sc + noise * 1.732 * (r.uni() + r.uni() + r.uni() + r.uni()) + r.uni() + r.uni() + 0.5))); } }
        return writeAll(argv[3], wavBytes(cd, nullptr)) ? 0 : 2; }
    // ---- block-wise commands (2026-10-30): masters, levels, layers and the payload live in a work folder next to the output; memory holds
    // a few pieces (and the core). Piece length from the memory limit (--mem=<GB>, or by default min(1/2, max(1/4, all - 28 GB)) of the
    // physical memory); the files written do not depend on it.
    auto workDir = [&](const std::string& out) { const std::string wd = out + ".laes.tmp"; std::error_code ec;
        if (fs::exists(wd, ec) && !fs::exists(wd + "/laes.tmp", ec)) laess::fail(wd + " exists and is not a laes work folder: remove it or choose another output name");
        fs::remove_all(wd, ec); fs::create_directories(wd, ec); if (ec || !fs::exists(wd)) laess::fail("cannot create the work folder " + wd);
        { laess::Out o(wd + "/laes.tmp"); const char* t = "laes work folder (removed at the end of each run)\n"; o.put(t, strlen(t)); o.close(); } return wd; };
    struct Gone { std::string p; ~Gone() { if (!p.empty() && !getenv("KEEPTMP")) { std::error_code ec; fs::remove_all(p, ec); } } };
    auto pieceFor = [&](int maxStages) -> int64_t {   // base-level samples per piece, a multiple of 4096
        if (const char* e = getenv("LAES_PIECE")) return std::max<int64_t>(4096, atoll(e) / 4096 * 4096);   // test hook
        unsigned long long total = physMemory(); const std::string mm = arg(argc, argv, "--mem"); const unsigned long long G28 = 28ull << 30;
        unsigned long long budget = !mm.empty() ? (unsigned long long)(atof(mm.c_str()) * 1073741824.0) : std::min(total / 2, std::max(total / 4, total > G28 ? total - G28 : 0ull));
        if (budget < (512ull << 20)) budget = 512ull << 20;
        const unsigned long long top = budget / 160;   // about 80 bytes per top-level sample of a piece (lifting windows), twice for the candidates' reference
        return std::max<int64_t>(4096, (int64_t)((top >> maxStages) / 4096 * 4096)); };
    const int threads = laesfast::defaultThreads();
    if (cmd == "align" && argc >= 4) {   // check only: prints T and the band; nothing is moved or written
        Pcm cd; std::string e = readWav(argv[2], cd); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        if (cd.bits != 16 || (cd.rate != 44100 && cd.rate != 48000)) { printf("the core must be 16-bit at 44100 or 48000 Hz\n"); return 2; }
        try { const std::string wd = (fs::temp_directory_path() / ("laes_align_" + std::to_string((unsigned long long)std::hash<std::string>()(argv[3])))).string(); Gone g{workDir(wd)}; laess::Prep m;
            e = laess::prepRead(argv[3], g.p + "/m", m); if (e.empty()) { int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++; e = laess::prepLevels(m, cd.rate, g.p + "/m", pieceFor(st), threads); }
            if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
            const AutoT A = autoAlign(argv[2], cd, argv[3], m, laeEncPath(argc, argv)); if (!A.found) { printf("%s\n", A.why.c_str()); return 2; }
            const bool ok = acceptAlign(A, argv[3], flag(argc, argv, "--accept"));
            printf("ALIGN_LAES T=%.10f err=%.1f resid=%.1f band=%s accepted=%d\n", A.T, A.err, A.resid, A.err <= -60 ? "ok" : A.err <= -54 ? "check" : "large", ok ? 1 : 0);
            if (ok) printf("use %s@%.10f (or %s@auto in enc)\n", argv[3], A.T, argv[3]); return ok ? 0 : 2; }
        catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; } }
    if (cmd == "enc" && argc >= 4) try {
        const std::string out = argv[2], corePath = arg(argc, argv, "--core"); const bool derive = flag(argc, argv, "--derive-core");
        std::vector<std::string> paths; std::vector<double> Ts; std::vector<bool> autoT; std::vector<laess::WavInfo> wis;
        for (int i = 3; i < argc; i++) { if (argv[i][0] == '-') continue; std::string a = argv[i]; double T = 0; bool au = false; size_t at = a.rfind('@');
            if (at != std::string::npos && at > 2) { au = a.substr(at + 1) == "auto"; T = au ? 0 : atof(a.substr(at + 1).c_str()); a = a.substr(0, at); }
            laess::WavInfo wi; std::string e = laess::probeWav(a, wi); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } paths.push_back(a); Ts.push_back(T); autoT.push_back(au); wis.push_back(wi); }
        if (paths.empty()) { printf("no master given\n"); return 1; }
        {   // file order: more bits first, then higher rate (a master can only refer to earlier ones; a shorter word refers to a longer one cheaply)
            std::vector<size_t> ord(paths.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return wis[a].bits != wis[b].bits ? wis[a].bits > wis[b].bits : wis[a].rate > wis[b].rate; });
            std::vector<std::string> p2; std::vector<double> t2; std::vector<bool> a2; std::vector<laess::WavInfo> w2;
            for (size_t k = 0; k < ord.size(); k++) { p2.push_back(paths[ord[k]]); t2.push_back(Ts[ord[k]]); a2.push_back(autoT[ord[k]]); w2.push_back(wis[ord[k]]); }
            paths.swap(p2); Ts.swap(t2); autoT.swap(a2); wis.swap(w2); }
        const int base = wis[0].rate % 44100 == 0 ? 44100 : 48000; const bool asFlac = out.size() > 5 && !_stricmp(out.c_str() + out.size() - 5, ".flac");
        const std::string cbArg = arg(argc, argv, "--core-bits"); int maxBits = 0, maxSt = 0; for (auto& w : wis) { maxBits = std::max(maxBits, w.bits); int st = 0; while ((base << st) < w.rate && st < 4) st++; maxSt = std::max(maxSt, st); }
        const int coreBits = !cbArg.empty() ? atoi(cbArg.c_str()) : (asFlac ? std::min(maxBits, 24) : 16);
        if (coreBits != 16 && coreBits != 24 && coreBits != 32) { printf("--core-bits must be 16, 24 or 32\n"); return 1; }
        if (corePath.empty() && !derive) { printf("give --core=<core.wav> or --derive-core\n"); return 1; }
        Gone g{workDir(out)}; const int64_t piece = pieceFor(maxSt);
        std::vector<laess::Prep> pm(paths.size());
        for (size_t i = 0; i < paths.size(); i++) { const std::string stem = g.p + "/m" + std::to_string(i); std::string e = laess::prepRead(paths[i], stem, pm[i]);
            if (e.empty()) e = laess::prepLevels(pm[i], base, stem, piece, threads); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        size_t coreSrc = SIZE_MAX; Pcm core;
        if (!corePath.empty()) { std::string e = readWav(corePath, core); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } if (core.rate != base) { printf("the core must be at %d Hz\n", base); return 2; } }
        else { bool dith; std::vector<laess::Prep*> pp; for (auto& m : pm) pp.push_back(&m); core = laess::deriveCoreS(pp, base, coreBits, coreSrc, dith, 20261028); printf("core: %d-bit base level of %s%s\n", coreBits, paths[coreSrc].c_str(), dith ? " (TPDF-dithered)" : ""); }
        for (size_t i = 0; i < paths.size(); i++) if (autoT[i]) {   // master.wav@auto: T from LAEY's search against the given core
            if (corePath.empty() || core.bits != 16) { printf("@auto needs --core=<cd.wav>, a 16-bit CD / DVD (a derived core is at T = 0)\n"); return 1; }
            const AutoT A = autoAlign(corePath, core, paths[i], pm[i], laeEncPath(argc, argv)); if (!A.found) { printf("%s: %s; nothing written\n", paths[i].c_str(), A.why.c_str()); return 2; }
            if (!acceptAlign(A, paths[i], flag(argc, argv, "--accept"))) { printf("alignment not accepted; nothing written\n"); return 2; } Ts[i] = A.T; }
        std::vector<laess::MasterSel> sel(paths.size()); for (size_t i = 0; i < paths.size(); i++) { MasterIn t; setT(t, Ts[i]); sel[i].storage = -1; sel[i].whole = t.whole; sel[i].frac = t.frac; }
        // file order, tried two ways, the smaller kept: (a) more bits first; (b) the master the core was derived from first, then the others more bits first
        std::vector<std::vector<size_t>> orders(1, std::vector<size_t>(paths.size())); for (size_t i = 0; i < paths.size(); i++) orders[0][i] = i;
        if (coreSrc != SIZE_MAX && coreSrc != 0) { std::vector<size_t> b(1, coreSrc); for (size_t i = 0; i < paths.size(); i++) if (i != coreSrc) b.push_back(i); orders.push_back(b); }
        std::vector<MasterInfo> info; std::string bodyPath; long long bodySize = -1; std::vector<size_t> pick;
        for (size_t oi = 0; oi < orders.size(); oi++) { const auto& ord = orders[oi]; std::vector<const laess::Prep*> m2; std::vector<laess::MasterSel> s2; for (size_t k : ord) { m2.push_back(&pm[k]); s2.push_back(sel[k]); }
            std::vector<MasterInfo> i2; const std::string bp = g.p + "/body" + std::to_string(oi); const std::string err = laess::encodeLaesS(core, base, m2, s2, i2, bp, g.p + "/o" + std::to_string(oi), asFlac ? 1 : 0, piece, threads);
            if (!err.empty()) { printf("%s\n", err.c_str()); return 2; } const long long sz = (long long)fs::file_size(bp);
            if (bodySize < 0 || sz < bodySize) { if (!bodyPath.empty()) fs::remove(bodyPath); bodyPath = bp; bodySize = sz; info = i2; pick = ord; } else fs::remove(bp); }
        { std::vector<std::string> p2; std::vector<laess::Prep> q2; for (size_t k : pick) { p2.push_back(paths[k]); q2.push_back(pm[k]); } paths.swap(p2); pm.swap(q2); }
        printf("file order:"); for (size_t k = 0; k < paths.size(); k++) printf(" %zu = %s;", k + 1, paths[k].c_str()); printf("%s\n", orders.size() > 1 ? " (the smaller of two orders)" : "");
        // self-check before writing: decode the block exactly as a player would, and compare every master with its input
        { std::vector<laess::DecMaster> dec; const std::string de = laess::decodeLaesS(bodyPath, core, base, g.p, dec, piece, threads);
          if (!de.empty()) { printf("self-check failed: %s; nothing written\n", de.c_str()); return 2; }
          for (size_t i = 0; i < pm.size(); i++) { bool same = dec[i].shaOk && dec[i].I.frames == pm[i].frames;
              if (same) { laess::FileSig t0(dec[i].top[0]), t1(dec[i].top[1]); laess::In raw(pm[i].raw); const int64_t P = 1 << 20; V a, b; std::vector<int32_t> r;
                  for (int64_t s = 0; s < t0.n && same; s += P) { const int64_t e = std::min(t0.n, s + P); a.resize((size_t)(e - s)); b.resize((size_t)(e - s)); t0.get(s, e, a.data()); t1.get(s, e, b.data());
                      r.resize((size_t)(e - s) * 2); raw.read(s * 8, r.data(), r.size() * 4);
                      for (int64_t k = 0; k < e - s && same; k++) same = (int64_t)r[(size_t)k * 2] == (a[(size_t)k] << dec[i].I.w) && (int64_t)r[(size_t)k * 2 + 1] == (b[(size_t)k] << dec[i].I.w); } }
              if (!same) { printf("self-check failed for master %zu; nothing written\n", i + 1); return 2; } } }
        if (asFlac) {   // FLAC form, then read back: core, MD5 and LAES block must come back unchanged
            const std::vector<uint8_t> fl0 = laesflac::writeFlac(core.c, base, core.bits, std::vector<uint8_t>());   // "fLaC", STREAMINFO, the audio frames
            const size_t pieceA = (1u << 24) - 1 - 4; const long long napp = bodySize == 0 ? 0 : (bodySize + (long long)pieceA - 1) / (long long)pieceA;
            { laess::Out o(out + ".part"); laess::In body(bodyPath); std::vector<uint8_t> h(fl0.begin(), fl0.begin() + 42); if (napp) h[4] = 0x00; o.put(h.data(), h.size());   // STREAMINFO is not the last block
              std::vector<uint8_t> b; for (long long i = 0; i < napp; i++) { const long long a = i * (long long)pieceA, len = std::min<long long>((long long)pieceA, bodySize - a);
                  const uint32_t L = (uint32_t)(len + 4); const uint8_t bh[8] = {(uint8_t)((i + 1 == napp ? 0x80 : 0) | 2), (uint8_t)(L >> 16), (uint8_t)(L >> 8), (uint8_t)L, 'L', 'A', 'E', '2'}; o.put(bh, 8);
                  b.resize((size_t)len); body.read(a, b.data(), b.size()); o.put(b.data(), b.size()); }
              o.put(fl0.data() + 42, fl0.size() - 42); o.close(); }
            {   // read back: the APPLICATION blocks against the payload, the rest through the FLAC reader
                laess::In f(out + ".part"), body(bodyPath); long long p = 4; std::vector<uint8_t> meta(fl0.begin(), fl0.begin() + 42), b, c; long long got = 0; bool ok = true, last = false;
                while (!last && ok) { uint8_t bh[4]; f.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const long long len = bh[1] << 16 | bh[2] << 8 | bh[3]; p += 4;
                    if (type == 2) { if (len < 4) { ok = false; break; } b.resize((size_t)len - 4); f.read(p + 4, b.data(), b.size()); c.resize(b.size()); body.read(got, c.data(), c.size()); ok = b == c; got += (long long)b.size(); }
                    p += len; }
                std::vector<uint8_t> rest((size_t)(f.n - p)); f.read(p, rest.data(), rest.size()); meta[4] = 0x80; meta.insert(meta.end(), rest.begin(), rest.end());
                laesflac::Info fi; V rc[2]; std::vector<uint8_t> rb; bool md5ok = false; const std::string fe = ok && got == bodySize ? laesflac::readFlac(meta, fi, rc, rb, md5ok) : "LAES block differs";
                if (!fe.empty() || !md5ok || rc[0] != core.c[0] || rc[1] != core.c[1] || !rb.empty()) { fs::remove(out + ".part"); printf("FLAC self-check failed%s%s; nothing written\n", fe.empty() ? "" : ": ", fe.c_str()); return 2; } } }
        else {   // WAV form: the core, then the "LAE " chunk with the payload
            const int by = core.bits / 8; const long long n = core.frames(), db = n * 2 * by; const bool ext = core.bits > 16; const uint32_t fmtLen = ext ? 40 : 16; const long long extra = bodySize ? 8 + bodySize + (bodySize & 1) : 0;
            if (4 + 8 + fmtLen + 8 + db + (db & 1) + extra > 0xFFFFFFFFLL) { printf("the file would exceed the 4 GB limit of WAV (use the FLAC form, out.flac); nothing written\n"); return 2; }
            Pcm hdr; hdr.rate = core.rate; hdr.bits = core.bits; std::vector<uint8_t> lae; std::vector<uint8_t> h = wavBytes(hdr, nullptr);   // header of an empty file: sizes patched below
            auto p32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); }; p32(4, (uint32_t)(4 + 8 + fmtLen + 8 + db + (db & 1) + extra)); p32(h.size() - 4, (uint32_t)db);
            laess::Out o(out + ".part"); o.put(h.data(), h.size()); std::vector<uint8_t> b; const long long P = 1 << 18;
            for (long long a = 0; a < n; a += P) { const long long e = std::min(n, a + P); b.clear(); for (long long i = a; i < e; i++) for (int c = 0; c < 2; c++) { const uint32_t v = (uint32_t)(int32_t)core.c[c][(size_t)i]; for (int k = 0; k < by; k++) b.push_back((uint8_t)(v >> (8 * k))); } o.put(b.data(), b.size()); }
            if (db & 1) { const uint8_t z = 0; o.put(&z, 1); }
            if (bodySize) { const uint8_t ch[8] = {'L', 'A', 'E', ' ', (uint8_t)bodySize, (uint8_t)(bodySize >> 8), (uint8_t)(bodySize >> 16), (uint8_t)(bodySize >> 24)}; o.put(ch, 8);
                laess::In body(bodyPath); for (long long a = 0; a < bodySize; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, bodySize - a)); body.read(a, b.data(), b.size()); o.put(b.data(), b.size()); }
                if (bodySize & 1) { const uint8_t z = 0; o.put(&z, 1); } }
            o.close(); }
        { std::error_code ec; fs::remove(out, ec); fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; } }
        double secs = (double)core.frames() / base; printf("%s: %s form, core %d Hz %d-bit, %zu master(s), LAES block %.1f kb/s (file %.1f MB, %.1f kb/s)\n", out.c_str(), asFlac ? "FLAC" : "WAV", base, core.bits, pm.size(),
                                                         bodySize * 8 / secs / 1000, (double)fs::file_size(out) / 1e6, (double)fs::file_size(out) * 8 / secs / 1000);
        for (size_t i = 0; i < info.size(); i++) { const std::string tier = info[i].refTier == 0 ? "the core" : "master " + std::to_string(info[i].refTier);
            const std::string how = info[i].storage == 1 ? "full" : info[i].storage == 0 ? "difference to " + tier + " at the base level"
                                  : "difference to " + tier + " at " + std::to_string(base << std::min(info[i].stages, info[info[i].refTier - 1].stages)) + " Hz";
            printf("  master %zu: %d Hz %d-bit, %s, layers %.1f kb/s\n", i + 1, info[i].rate, info[i].bits, how.c_str(), info[i].layerBits / secs / 1000); }
        return 0; }
    catch (const std::exception& x) { std::error_code ec; fs::remove(std::string(argv[2]) + ".part", ec); printf("stopped: %s; nothing written\n", x.what()); return 2; }
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
              printf("FLAC core: %d Hz %d-bit, %ld frames, MD5 %s\n", core.rate, core.bits, core.frames(), md5ok ? "ok" : "MISMATCH"); if (!md5ok) return 2; }
          else { laess::WavInfo wi; long long lo = -1, ll = 0; std::string e = laess::probeWav(argv[2], wi, &lo, &ll); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
              core.rate = wi.rate; core.bits = wi.bits; std::vector<int32_t> x; laess::wavFrames(in, wi, 0, wi.frames, x); for (int c = 0; c < 2; c++) { core.c[c].resize((size_t)wi.frames); for (long i = 0; i < wi.frames; i++) core.c[c][(size_t)i] = x[(size_t)i * 2 + c]; }
              laess::Out bo(bodyPath); std::vector<uint8_t> b; if (lo >= 0) for (long long a = 0; a < ll; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, ll - a)); in.read(lo + a, b.data(), b.size()); bo.put(b.data(), b.size()); }
              bo.close(); bodySize = lo >= 0 ? ll : 0; } }
        // the extension's first 4 bytes name the format: "LAE2" = LAES (lossless, this tool), "LAE1" = LAEY (lossy, decoded by lae_dec)
        uint8_t mg[4] = {0}; if (bodySize >= 4) { laess::In b(bodyPath); b.read(0, mg, 4); }
        if (bodySize >= 4 && !memcmp(mg, "LAE1", 4)) { printf("%s: this is an LAEY file (lossy, extension LAE1): decode it with lae_dec; laes reads LAES (extension LAE2)\n", argv[2]); return 2; }
        if (bodySize < 8 || memcmp(mg, "LAE2", 4)) { printf("%s: no LAES extension (LAE2) found\n", argv[2]); return 2; }
        printf("format: LAES (lossless, extension LAE2)\n");
        int maxSt = 0; for (int s = 1; s <= 3; s++) maxSt = s;   // pieces sized for the highest rate a master can have
        const int64_t piece = pieceFor(maxSt); std::vector<laess::DecMaster> dec; std::string de = laess::decodeLaesS(bodyPath, core, core.rate, g.p, dec, piece, threads);
        if (!de.empty()) { printf("%s: %s (the core is still playable)\n", argv[2], de.c_str()); return 2; }
        bool all = true; for (size_t i = 0; i < dec.size(); i++) { printf("master %zu: %d Hz %d-bit, %ld frames, SHA-256 %s\n", i + 1, dec[i].I.rate, dec[i].I.bits, dec[i].I.frames, dec[i].shaOk ? "ok" : "MISMATCH"); all &= dec[i].shaOk; }
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
            std::vector<Choice> ch; { Src s; s.rate = core.rate; s.bits = core.bits; s.n = core.frames(); ch.push_back({"core", "the core (lossless)", s}); }
            for (size_t i = 0; i < dec.size(); i++) { const std::string mk = "master_" + std::to_string(i + 1); const MasterInfo& I = dec[i].I;
                if (!dec[i].shaOk) { printf("master %zu: SHA-256 mismatch: not offered (nor its rate levels)\n", i + 1); continue; }
                { Src s; s.kind = 1; s.m = i; s.rate = I.rate; s.bits = I.bits; s.n = I.frames; ch.push_back({mk + "_" + std::to_string(I.rate) + "_" + std::to_string(I.bits), "master " + std::to_string(i + 1) + " (lossless)", s}); }
                for (int k = I.stages - 1; k >= 0; k--) { Src s; s.kind = 2; s.m = i; s.k = k; s.rate = I.rate >> (I.stages - k); s.bits = I.bits; s.n = levLen(I.frames, I.stages - k);
                    ch.push_back({mk + "_level_" + std::to_string(s.rate) + "_" + std::to_string(s.bits), "rate level of master " + std::to_string(i + 1) + " (lossless)", s}); } }
            { std::vector<int> rates; for (auto& c : ch) if (std::find(rates.begin(), rates.end(), c.s.rate) == rates.end()) rates.push_back(c.s.rate);
              for (int r : rates) { int best = -1; bool has16 = false; for (size_t j = 0; j < ch.size(); j++) if (ch[j].s.rate == r) { has16 |= ch[j].s.bits == 16; if (best < 0 || ch[j].s.bits > ch[best].s.bits) best = (int)j; }
                  if (has16 || best < 0 || ch[best].s.bits <= 16) continue;
                  const Choice& sc = ch[(size_t)best]; Choice dv; dv.derived = true; dv.s = sc.s; dv.s.bits = 16; dv.from = best;
                  dv.name = "derived16_" + std::to_string(r) + "_from_" + sc.name + "_LOSSY"; dv.what = "16-bit version derived from " + sc.name + ": a new master, LOSSY against its " + std::to_string(sc.s.bits) + "-bit source";
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
