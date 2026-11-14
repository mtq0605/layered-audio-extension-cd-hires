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
//       masters (2026-11-11): up to 255; --name=<k>:<text> names the k-th master as given (the publisher's free text, UTF-8, at most 255
//       bytes, no control characters; shown by players as it is, never used to choose anything); --no-derive / --no-derive=<k>: that
//       master offers no derived 16-bit versions. Any name, a --no-derive or more than 16 masters: payload version 2 (players from before
//       2026-11-11 then play the core master only); otherwise version 1, byte for byte as before. Above 16 masters the reference of each
//       master is chosen from the 3 candidates with the smallest size on 4 fixed excerpts (coded in full), and one file order only.
//       menu order (2026-11-11): the order the masters are given in (--menu-order=<k1>,<k2>,...: another order); the file order stays
//       the encoder's smallest one; a menu order other than the file order is stored (payload version 2). Every number shown or taken (list, master:<k>, master_<k>_ file names, names / add) is the menu number.
//       default (2026-11-12): --default=<k> (the k-th master as given) is what players play when nothing is chosen; not given or
//       --default=core: the core master (header byte 56). --no-derive=core: the core master offers no derived version (byte 57); a
//       bare --no-derive turns off every derived version, the core master's too. Either set: payload version 2.
//   laes names <in> <out> | --in-place [--name=<k>:<text>] [--no-derive[=<k>|core]] [--derive[=<k>|core]] [--menu-order=<k1>,...] [--default=<k>|core]   a NEW file (--in-place: the file itself, default and core flag only) with other master
//       names / derive flags / menu order (k = the master's menu number; an empty text removes the name); every layer and the tags copied
//       byte for byte, every master checked
//   laes add <in> <out> <master>[@T] ... [--name=<k>:<text>] [--no-derive[=<k>]] [--menu-order=<k1>,...]   a NEW file: the old one with new
//       masters at the end (the old parts byte for byte; each new master against the core master and every master before it)
//   laes extract <in> <out> --pick=master:<k> | name:<text> | <rate>:<bits>[,...]   a NEW file with the chosen masters, decoded and encoded
//       again in full (same core master, tags kept); the order of --pick is the new menu order
//   laes tag <in> <out> [--tag=... --cover=... --picture=...]   a NEW file with exactly these tags (none: no tags); audio bytes and
//       payload copied byte for byte, then every master checked against its SHA-256; the old file is kept. Refuses an existing <out>.
//   laes parts <file>                                  blocks / chunks, SHA-256 of the audio bytes and of the payload, the tags as read
//   laes dec <in.wav | in.flac> <out dir> [--levels]   writes core.wav and master_<k>_<rate>_<bits>.wav (and every rate level with --levels)
//   laes dec <in> --list                              every choice: core, masters, rate levels (lossless) and derived 16-bit versions
//   laes dec <in> <out dir> --pick=<n>[,<n>...] | <rate>:<bits> | master:<k> | name:<text>   writes only the chosen ones. <rate>:<bits>
//       and name:<text> match masters only (never a rate level or a derived version); several match: the user ticks them (laes_pick.h; in a script they are listed, nothing written). <a>-<b>: a range of entries.
//       The list (laes_choices.h, 2026-11-11): the core master; then per master its group - the master, its rate levels, and its derived
//       16-bit versions (each master above 16 bit, at its own rate and every level, unless the file turns them off for it), made with a
//       fixed TPDF rule: bit-identical on every decoder, lossless relative to that rule, not lossless relative to the master
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
#include "laes_pick.h"   // the tick list for several masters that fit one --pick (2026-11-11)
#ifdef _WIN32
static const char* kLaeEnc = "lae_enc.exe"; static const char* kQ = "\"";   // cmd.exe needs the whole command line quoted once more
static const char* kShellBad = "%\"";   // FU15-1: characters cmd.exe changes inside the quotes (%NAME% is expanded)
#else   // POSIX names of the same calls
#include <strings.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _stricmp strcasecmp
#define _popen popen
#define _pclose pclose
static const char* kLaeEnc = "lae_enc"; static const char* kQ = "";
static const char* kShellBad = "$`\"\\";   // FU15-1: characters sh still reads inside double quotes ($(...), `...`, ", \)
#endif
using namespace laes;
namespace fs = std::filesystem;
static bool readAll(const std::string& p, std::vector<uint8_t>& b) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return false; _fseeki64(f, 0, SEEK_END); long long s = _ftelli64(f); if (s < 0) { fclose(f); return false; } _fseeki64(f, 0, SEEK_SET);   // FU15-7 (1): a failed ftell is a read error
    b.resize((size_t)s); bool ok = fread(b.data(), 1, b.size(), f) == b.size(); fclose(f); return ok; }
// a master or core master WAV whose header gives a data length beyond the end of the file (2026-11-14, as lae_enc): told once, read to its end
static void longHeaderNote(const std::string& path, unsigned long long hdrLen, long long have) {
    if (hdrLen) printf("NOTE: the header of %s gives a data length of %llu bytes, but the file holds only %lld bytes of it: the file may be cut off (incomplete). It is read to its end and the run continues.\n", path.c_str(), hdrLen, have); }
static bool writeAll(const std::string& p, const std::vector<uint8_t>& b) { FILE* f = fopen(p.c_str(), "wb"); if (!f) return false; bool ok = fwrite(b.data(), 1, b.size(), f) == b.size(); return fclose(f) == 0 && ok; }
// ---- WAV read (16/24/32-bit integer PCM, stereo; PCM or EXTENSIBLE with the PCM sub-format) + the "LAE " chunk body
// hdrLen / haveLen (2026-11-14): the header's data length and the bytes the file holds of it, when the header gives more than the file holds (the file
// is read to its end as before); else 0
static std::string readWav(const std::string& path, Pcm& p, std::vector<uint8_t>* lae = nullptr, unsigned long long* hdrLen = nullptr, long long* haveLen = nullptr) {
    std::vector<uint8_t> b; if (!readAll(path, b)) return "cannot read " + path; if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(&b[8], "WAVE", 4)) return path + ": not RIFF/WAVE";
    size_t pos = 12; int tag = 0, ch = 0, rate = 0, bits = 0; const uint8_t* data = nullptr; size_t dlen = 0; if (lae) lae->clear(); if (hdrLen) *hdrLen = 0; if (haveLen) *haveLen = 0;
    auto u16 = [&](size_t o) { return (int)(b[o] | b[o + 1] << 8); }; auto u32 = [&](size_t o) { return (uint32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24); };
    while (pos + 8 <= b.size()) { uint32_t n = u32(pos + 4); size_t body = pos + 8; if (n > b.size() - body) { if (hdrLen && !memcmp(&b[pos], "data", 4)) { *hdrLen = n; if (haveLen) *haveLen = (long long)(b.size() - body); } n = (uint32_t)(b.size() - body); }
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
// T (base samples, decimal) -> whole + fraction in 2^-32 base samples. DEC 14 (2026-11-15): the whole part goes up by one only when the
// fraction really rounds up to a whole (llround(fraction x 2^32) = 2^32); before, also when it rounded down to 0 (T = 0.00000000001 gave 1)
static void splitT(double T, int64_t& whole, uint32_t& frac) { whole = (int64_t)std::floor(T); const long long q = std::llround((T - std::floor(T)) * 4294967296.0);
    if (q >= 4294967296LL) { whole += 1; frac = 0; } else frac = (uint32_t)q; }
static void setT(MasterIn& m, double T) { splitT(T, m.whole, m.frac); }
// FU15-3 (2026-11-14): "<master>[@T]" of enc / add. The text after the last @ is T only when all of it is a finite decimal number
// below 2^40 base samples (or "auto" for enc); otherwise the whole argument is the file name when that file exists (a name with
// an @), else it is refused with the reason. Before, atof took any text: "m.wav@0,37" or "m.wav@abc" gave T = 0 without a word.
// FU15-5 / FU16-1 (3) (2026-11-14): T of each master relative to the core master in 2^-32 base samples, through its chain. A master
// stored in full: its own T from its record, relative to the core master (DEC 15, 2026-11-15: written by the encoder since then; a
// file from before holds 0 there, the value this function took before). |whole| below 2^30 and each sum below 2^62, so that nothing overflows (the spec allows up to 2^40:
// such a file is refused by add / extract with the reason, never computed wrongly); a reference to a later master is refused too.
static std::string absTimes(const std::vector<MasterInfo>& ms, std::vector<int64_t>& absT) {
    absT.assign(ms.size(), 0); const int64_t lim = 1LL << 62;
    for (size_t i = 0; i < ms.size(); i++) { const MasterInfo& I = ms[i];
        if ((size_t)I.refTier > i) return "master " + std::to_string(i + 1) + " refers to a later master";
        const int64_t ref = I.refTier ? absT[(size_t)I.refTier - 1] : 0;
        if (I.whole >= (1LL << 30) || I.whole <= -(1LL << 30) || ref >= lim || ref <= -lim) return "master " + std::to_string(i + 1) + ": a time offset of 2^30 base samples or more is not supported by this command";
        absT[i] = I.whole * 4294967296LL + (int64_t)I.frac + ref; }
    return ""; }
// DEC 15 (2026-11-15): laes extract hands the T of each master to its inner enc as the exact number (2^-32 base samples, relative to
// the core master), never as text: the master's path and its T. Empty outside extract.
static std::vector<std::pair<std::string, int64_t>> gExactT;
static std::string masterArg(const std::string& a, bool allowAuto, std::string& path, double& T, bool& au) {
    path = a; T = 0; au = false; const size_t at = a.rfind('@'); if (at == std::string::npos || at <= 2) return "";
    const std::string t = a.substr(at + 1); if (allowAuto && t == "auto") { au = true; path = a.substr(0, at); return ""; }
    char* e = nullptr; const double v = t.empty() ? 0 : strtod(t.c_str(), &e);
    if (!t.empty() && *e == 0 && std::isfinite(v) && std::fabs(v) < 1099511627776.0) { T = v; path = a.substr(0, at); return ""; }
    std::error_code ec; if (fs::exists(a, ec)) return "";   // a file whose name has an @
    return a + ": the text after the last @ must be T in base samples (a decimal number below 2^40, e.g. -0.37)" + (allowAuto ? " or auto" : ""); }
// derived 16-bit version (decoder rule): y = floor((v + r1 - r2 + 2^(s-1)) / 2^s), s = bits - 16, r1, r2 uniform in [0, 2^s) from
// xorshift64 (seed 20261029, one stream per channel): TPDF dither of +-1 LSB at 16 bits; deterministic. A new master, lossy against src.
static Pcm derive16(const Pcm& sc) { Pcm dv; dv.rate = sc.rate; dv.bits = 16; const int s = sc.bits - 16;
    for (int c = 0; c < 2; c++) { uint64_t st = 20261029ull + 0x9E3779B97F4A7C15ull * (uint64_t)(c + 1); auto nx = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st >> (64 - s); };
        dv.c[c].resize(sc.c[c].size()); for (size_t i = 0; i < sc.c[c].size(); i++) { const int64_t r1 = (int64_t)nx(), r2 = (int64_t)nx();
            dv.c[c][i] = std::max<int64_t>(-32768, std::min<int64_t>(32767, (sc.c[c][i] + r1 - r2 + (1LL << (s - 1))) >> s)); } }
    return dv; }
static std::string selfPath(const char* argv0);   // FU15-9: this program's own file (below, after win_utf8.h)
static std::string laeEncPath(int argc, char** argv) { std::string e = arg(argc, argv, "--lae-enc"); return e.empty() ? (fs::absolute(fs::path(selfPath(argv[0]))).parent_path() / kLaeEnc).string() : e; }
// ---- automatic alignment: LAEY's search (lae_enc cdcheck, design D). LAEY moves the master by its offset (master samples); LAES never
// moves the master and stores T = offset / 2^stages (base samples, same sign) exactly as measured, fraction included.
// err = LAEY's pooled residual after the fraction (its phase line), the alignment error the -60 / -54 dB bands apply to.
struct AutoT { bool found = false; double T = 0, err = 0, resid = 0; std::string search, notes, why; std::vector<int> silentBlocks; };   // silentBlocks (DEC 25): check blocks where the master is silent and the moved core master is not
static AutoT autoAlign(const std::string& cdPath, const Pcm& cd, const std::string& mPath, const laess::Prep& m, const std::string& enc) {   // m: read and its levels made (block-wise, 2026-10-30)
    AutoT A; int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++;
    if ((cd.rate << st) != m.rate || st > 3) { A.why = "master rate must be the core master rate times 1, 2, 4 or 8"; return A; }
    if (st == 0) { A.why = "LAEY's search needs a master at 2, 4 or 8 times the core master rate; give T by hand (master.wav@T)"; return A; }
    if (cd.frames() < 1 || m.frames < 1) { A.why = std::string(cd.frames() < 1 ? "the core master" : "the master") + " has no samples: there is nothing to align"; return A; }   // FU15-4: the own check below needs samples on both sides
    if (!fs::exists(enc)) { A.why = "lae_enc not found: " + enc + " (give --lae-enc=<path>)"; return A; }
    const std::string dummy = (fs::temp_directory_path() / "laes_align_unused.lae").string();   // cdcheck encodes nothing
    for (const std::string* p : {&enc, &mPath, &cdPath, &dummy}) if (p->find_first_of(kShellBad) != std::string::npos) {   // FU15-1 (2026-11-14): the command goes through the shell
        A.why = "the path " + *p + " has a character the shell would change (" + kShellBad + "), so lae_enc cannot be run for the search; rename it or give T by hand (master.wav@T)"; return A; }
    const std::string cl = std::string(kQ) + "\"" + enc + "\" \"" + mPath + "\" \"" + dummy + "\" 3 \"cd=" + cdPath + "\" cdcheck" + (m.bits == 32 ? " bits=24" : "") + kQ;
    FILE* pp = _popen(cl.c_str(), "r"); if (!pp) { A.why = "cannot run lae_enc"; return A; }
    const char* nm[2] = {"whitened", "plain"}; std::string sug, why[2]; double pooled[2] = {1, 1}; char ln[8192];
    while (fgets(ln, sizeof ln, pp)) { std::string s = ln; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.rfind("ALIGN_SUGGEST ", 0)) sug = s;
        for (int k = 0; k < 2; k++) { const std::string pre = std::string("align: ") + nm[k] + " search: ", ph = std::string("align (") + nm[k] + "): phase: "; size_t p;
            if (!s.rfind(pre, 0)) why[k] = s.substr(pre.size()); else if (!s.rfind(ph, 0) && (p = s.find("pooled residual ")) != std::string::npos) pooled[k] = atof(s.c_str() + p + 16); } }
    _pclose(pp); if (sug.empty()) { A.why = "lae_enc gave no ALIGN_SUGGEST line (old lae_enc? design D is needed)"; return A; }
    bool have[2] = {false, false}; double T[2] = {0, 0};
    for (int k = 0; k < 2; k++) { size_t p = sug.find(std::string(nm[k]) + "="); if (p == std::string::npos) continue; size_t c = sug.find(':', p), sp = sug.find(' ', p);
        if (c == std::string::npos || (sp != std::string::npos && c > sp)) continue;   // FU15-3: no "<name>=...:<offset>" here: not found
        std::string v = sug.substr(c + 1, sp == std::string::npos ? std::string::npos : sp - c - 1); char* e = nullptr; const double tv = v.empty() ? 0 : strtod(v.c_str(), &e);
        if (v != "-" && !v.empty() && *e == 0 && std::isfinite(tv)) { have[k] = true; T[k] = tv / (double)(1 << st); } }
    A.notes = std::string("LAEY's own placement checks (LAEY rules, not LAES): whitened: ") + why[0] + "; plain: " + why[1];
    if (!have[0] && !have[1]) { A.why = "LAEY's search found no position; give T by hand. " + A.notes; return A; }
    const int k = have[0] && have[1] ? (pooled[1] < pooled[0] ? 1 : 0) : (have[0] ? 0 : 1); A.found = true; A.T = T[k]; A.search = nm[k]; A.err = pooled[k] <= 0 ? pooled[k] : 0;
    // own check: the master's base level against the core moved by T, worst of three blocks (head / middle / tail, up to 5 s each)
    const int w = m.w, up = (m.bits - 16) - w; std::unique_ptr<laess::Sig> base[2] = {laess::levelOf(m, 0, 0), laess::levelOf(m, 1, 0)};   // the base level (samples >> w)
    const int64_t nb = base[0]->n, bl = std::max<int64_t>(1, std::min<int64_t>(nb / 3, 5 * (int64_t)cd.rate)), b0[3] = {0, (nb - bl) / 2, nb - bl};
    int64_t wh = 0; uint32_t fr = 0; splitT(A.T, wh, fr);   // DEC 14: the same rule as setT
    const auto h = refTaps(fr); A.resid = -200;
    for (int b = 0; b < 3; b++) { double eb = 0, er = 0; for (int c = 0; c < 2; c++) { const int64_t n = (int64_t)cd.c[c].size(); V bv((size_t)bl); base[c]->get(b0[b], b0[b] + bl, bv.data());
            for (int64_t i = b0[b]; i < b0[b] + bl; i++) { Acc a; for (int q = -127; q <= 128; q++) a.mac(h[q + 127], cd.c[c][mir(i + wh + q, n)]); const int s = 31 - up;
                const double r = (double)(s > 0 ? a.shr(s) : (int64_t)a.lo << (-s)), x = (double)bv[(size_t)(i - b0[b])]; eb += x * x; er += (x - r) * (x - r); } }
        if (eb > 0 && er > 0) A.resid = std::max(A.resid, 10 * std::log10(er / eb));
        else if (eb == 0 && er > 0) { A.resid = std::max(A.resid, 0.0); A.silentBlocks.push_back(b); } }   // DEC 25 (2026-11-15): not matching (0 dB, the worst); before, skipped
    if (A.err == 0) A.err = A.resid;   // LAEY gave no phase line: use the own check
    return A; }
// the -60 / -54 dB bands: within -60 dB the alignment passes; otherwise it is shown and passes only when chosen by hand
static bool acceptAlign(const AutoT& A, const std::string& mPath, bool acceptFlag) {
    printf("%s: T = %.10f base samples (%s search), alignment error %.1f dB, residual re the master's base level %.1f dB\n", mPath.c_str(), A.T, A.search.c_str(), A.err, A.resid);
    if (A.resid > -40) printf("  note: the core master is not a close copy of this master (other mix / level / EQ / edit?); the encoder will likely store it in full\n");
    static const char* blockName[3] = {"head", "middle", "tail"};
    for (int b : A.silentBlocks) printf("  check block %d (%s): the master is silent there and the core master moved by T is not: not matching\n", b + 1, blockName[b]);   // DEC 25
    if (A.err <= -60 && A.silentBlocks.empty()) return true;
    if (!A.silentBlocks.empty()) printf("  the alignment did not match in %zu check block(s): check it (%s)\n", A.silentBlocks.size(), A.notes.c_str()); else
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
// FU15-9 (2026-11-14): lae_enc is looked for next to this program's own file; argv[0] is only "laes" when the program is started through
// PATH (then the current folder was searched). The system's name of the running program; argv[0] when it cannot be had.
#if defined(_WIN32) && defined(_MSC_VER)
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void*, wchar_t*, unsigned long);
static std::string selfPath(const char* argv0) { std::vector<wchar_t> b(32768); const unsigned long n = GetModuleFileNameW(nullptr, b.data(), (unsigned long)b.size());
    if (n == 0 || n >= b.size()) return argv0; b[n] = 0; const std::string s = lae_u8::fromWide(b.data()); return s.empty() ? std::string(argv0) : s; }
#elif defined(__linux__)
static std::string selfPath(const char* argv0) { std::error_code ec; const fs::path p = fs::read_symlink("/proc/self/exe", ec); return ec || p.empty() ? std::string(argv0) : p.string(); }
#else
static std::string selfPath(const char* argv0) { return argv0; }
#endif
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
struct Layout { bool flac = false, rf64 = false; std::vector<Part> parts; long long audioOff = 0, audioLen = 0; std::vector<std::pair<long long, long long>> payload;
    long long front = 0, behind = 0;   // DEC 19 (FLAC): ID3v2 tags in front of "fLaC", APEv2 / ID3v1 tags after the audio frames: copied as they are
    long long dataPart = -1, laePart = -1;   // WAV: the index in parts of the "data" and "LAE " chunk used - the last of each, as a player reads it (SPEC 7)
    bool unused(size_t i) const { const std::string& id = parts[i].id; return !flac && ((id == "data" && (long long)i != dataPart) || (id == "LAE " && (long long)i != laePart)); } };   // FU15-8: not written again (writers write one of each)
static std::string readLayout(laess::In& in, Layout& L) {
    uint8_t h[12] = {0}; if (in.n < 12) return in.path + ": not FLAC or RIFF/WAVE"; in.read(0, h, 12); L = Layout();
    L.front = (long long)laesflac::flacStart([&](uint64_t at, void* d, size_t k) { in.read((long long)at, d, k); }, (uint64_t)in.n);   // DEC 19
    if (L.front) { if (in.n < L.front + 12) return in.path + ": not FLAC or RIFF/WAVE"; in.read(L.front, h, 12); if (memcmp(h, "fLaC", 4)) return in.path + ": not FLAC or RIFF/WAVE"; }
    L.flac = !memcmp(h, "fLaC", 4);
    L.rf64 = (!memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4)) && !memcmp(h + 8, "WAVE", 4);
    if (L.flac) { long long p = L.front + 4; bool last = false;
        while (!last) { if (p + 4 > in.n) return in.path + ": FLAC metadata truncated"; uint8_t bh[4]; in.read(p, bh, 4); last = bh[0] & 0x80; Part q; q.type = bh[0] & 0x7F; q.len = bh[1] << 16 | bh[2] << 8 | bh[3]; q.off = p + 4;
            if (q.off + q.len > in.n) return in.path + ": FLAC metadata truncated";
            if (q.type == 2 && q.len >= 4) { char id[5] = {0}; in.read(q.off, id, 4); q.id = id; if (q.id == "LAE2" || q.id == "LAE1") L.payload.push_back({q.off + 4, q.len - 4}); }
            L.parts.push_back(q); p = q.off + q.len; }
        if (L.parts.empty() || L.parts[0].type != 0) return in.path + ": no STREAMINFO first";
        L.behind = (long long)laesflac::trailerLen([&](uint64_t at, void* d, size_t k) { in.read((long long)at, d, k); }, (uint64_t)p, (uint64_t)in.n);   // DEC 19
        L.audioOff = p; L.audioLen = in.n - p - L.behind; return ""; }
    if ((memcmp(h, "RIFF", 4) && !L.rf64) || memcmp(h + 8, "WAVE", 4)) return in.path + ": not FLAC or RIFF/WAVE";
    uint64_t dsData = 0; std::vector<std::pair<std::string, uint64_t>> table; long long pos = 12; bool haveData = false;
    while (pos + 8 <= in.n) { uint8_t c[8]; in.read(pos, c, 8); Part q; q.id.assign((const char*)c, 4); uint64_t n = c[4] | c[5] << 8 | c[6] << 16 | (uint64_t)c[7] << 24; q.off = pos + 8;
        if (L.rf64 && q.id == "ds64" && n >= 28 && n <= (uint64_t)(in.n - q.off)) {   // U14-b (2026-11-14): read only when it lies in the file (else refused below, nothing allocated)
            std::vector<uint8_t> d((size_t)n); in.read(q.off, d.data(), d.size()); auto le64 = [&](size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | d[at + (size_t)i]; return v; };
            dsData = le64(8); const uint32_t cnt = d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24; for (uint32_t i = 0; i < cnt && 28 + 12 * (size_t)(i + 1) <= d.size(); i++) table.push_back({std::string((const char*)&d[28 + 12 * i], 4), le64(32 + 12 * (size_t)i)}); }
        if (L.rf64 && n == 0xFFFFFFFFu) { if (q.id == "data") n = dsData; else for (auto& t : table) if (t.first == q.id) n = t.second; }
        if (n > (uint64_t)(in.n - q.off)) return in.path + ": chunk " + q.id + " runs past the end of the file";   // compared unsigned: a ds64 length of 2^63 or more is refused too (2026-11-14)
        q.len = (long long)n; if (q.id == "data") { L.audioOff = q.off; L.audioLen = q.len; haveData = true; L.dataPart = (long long)L.parts.size(); }
        else if (q.id == "LAE ") { L.payload.assign(1, {q.off, q.len}); L.laePart = (long long)L.parts.size(); }   // FU15-8: a second "LAE " chunk replaces the first, as in a player
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


// the file in its layout (FLAC blocks / RIFF chunks in their order, the tags kept as they are) with a new LAES payload (bodyP, bsz bytes)
// in place of the old one: FLAC: the LAE2 APPLICATION blocks replaced where the first one was; WAV: the "LAE " chunk; ds64 / JUNK made again.
// laes names / add / extract (2026-11-11: always a new file in the encoder's layout)
static std::string writeWithPayload(laess::In& in, const Layout& L, const std::string& bodyP, long long bsz, const std::string& outPart) {
        { laess::Out o(outPart); laess::In body(bodyP);
          if (L.flac) {   // the blocks in their order; the LAE2 APPLICATION blocks replaced, where the first one was, by the new payload's
              const size_t pieceA = (1u << 24) - 1 - 4; const long long napp = (bsz + (long long)pieceA - 1) / (long long)pieceA; copyRange(in, o, 0, L.front); o.put("fLaC", 4);   // DEC 19: ID3v2 in front kept
              struct Em { int type; long long off, len; bool lae; long long a; }; std::vector<Em> em; bool done = false;
              for (const Part& q : L.parts) { if (q.type == 2 && q.id == "LAE2") { if (!done) for (long long i = 0; i < napp; i++) em.push_back({2, 0, std::min<long long>((long long)pieceA, bsz - i * (long long)pieceA), true, i * (long long)pieceA}); done = true; }
                  else em.push_back({q.type, q.off, q.len, false, 0}); }
              for (size_t i = 0; i < em.size(); i++) { const Em& e = em[i]; const long long L4 = e.lae ? e.len + 4 : e.len;
                  const uint8_t bh[4] = {(uint8_t)((i + 1 == em.size() ? 0x80 : 0) | e.type), (uint8_t)(L4 >> 16), (uint8_t)(L4 >> 8), (uint8_t)L4}; o.put(bh, 4);
                  if (e.lae) { o.put("LAE2", 4); copyRange(body, o, e.a, e.len); } else copyRange(in, o, e.off, e.len); }
              copyRange(in, o, L.audioOff, L.audioLen); copyRange(in, o, L.audioOff + L.audioLen, L.behind); }   // DEC 19: APEv2 / ID3v1 at the end kept
          else {   // RIFF / RF64: "fmt " and every chunk in its order (tags kept as they are); the "LAE " chunk with the new payload; ds64 / JUNK made again
              const Part* fmt = nullptr; std::vector<const Part*> keep; for (size_t i = 0; i < L.parts.size(); i++) { const Part& q = L.parts[i]; if (q.id == "fmt ") fmt = &q; else if (q.id == "ds64" || q.id == "JUNK" || L.unused(i)) continue; else keep.push_back(&q); }
              if (!fmt || fmt->len < 16) return "no fmt chunk";
              long long riff = 4 + 8 + fmt->len + (fmt->len & 1); for (auto* q : keep) { const long long len = q->id == "LAE " ? bsz : q->len; riff += 8 + len + (len & 1); }
              const bool rf64 = riff > 0xFFFFFFFFLL || getenv("LAES_FORCE_RF64");
              for (auto* q : keep) if (rf64 && q->id != "data" && q->id != "LAE " && q->len > 0xFFFFFFFFLL) return "chunk " + q->id + " beyond 4 GB";   // FU15-8: as tag (ds64 lists only data and LAE)
              std::vector<uint8_t> h; auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) h.push_back((uint8_t)(v >> (8 * i))); }; auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) h.push_back((uint8_t)(v >> (8 * i))); };
              if (rf64) { uint8_t fb[16]; in.read(fmt->off, fb, 16); const int align = fb[12] | fb[13] << 8;
                  h.insert(h.end(), {'R', 'F', '6', '4', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 40, 0, 0, 0}); u64((uint64_t)(riff + 48)); u64((uint64_t)L.audioLen); u64((uint64_t)(align ? L.audioLen / align : 0));
                  h.insert(h.end(), {1, 0, 0, 0, 'L', 'A', 'E', ' '}); u64((uint64_t)bsz); }
              else { h.insert(h.end(), {'R', 'I', 'F', 'F'}); p32((uint32_t)riff); h.insert(h.end(), {'W', 'A', 'V', 'E'}); }
              h.insert(h.end(), {'f', 'm', 't', ' '}); p32((uint32_t)fmt->len); o.put(h.data(), h.size()); copyRange(in, o, fmt->off, fmt->len); if (fmt->len & 1) { const uint8_t z = 0; o.put(&z, 1); }
              for (auto* q : keep) { const bool isLae = q->id == "LAE "; const long long len = isLae ? bsz : q->len; std::vector<uint8_t> ch(q->id.begin(), q->id.end()); const bool big = rf64 && (q->id == "data" || isLae);
                  for (int i = 0; i < 4; i++) ch.push_back(big ? 0xFF : (uint8_t)((uint64_t)len >> (8 * i))); o.put(ch.data(), ch.size());
                  if (isLae) copyRange(body, o, 0, bsz); else copyRange(in, o, q->off, q->len); if (len & 1) { const uint8_t z = 0; o.put(&z, 1); } } }
          o.close(); }
    return ""; }
static int laesMain(int argc, char** argv);
int main(int argc, char** argv) { lae_u8::init(argc, argv); return laesMain(argc, argv); }   // UTF-8 arguments and file names on Windows (win_utf8.h)
// laes add / extract (2026-11-11) run the dec and enc commands of this same program through laesMain
static int laesMain(int argc, char** argv) {
    if (argc < 2) { printf("laes enc | add | extract | names | tag | parts | dec | verify | align | selftest | excerpt | mkcd | reduce  (see the source header)\n"); return 1; }
    const std::string cmd = argv[1];
    bool madePart = false;   // FU16-3 (2026-11-14): tag / add / extract / names remove <out>.part and <out>.body.part on an error only when this run made them
    if (cmd == "selftest") return selftest::run() ? 2 : 0;
    if (cmd == "reduce" && argc >= 5) {   // laes reduce <in.wav> <out.wav> <bits>: a shorter word length with TPDF dither (+-1 LSB of the result), fixed seed
        Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } const int bits = atoi(argv[4]);
        if ((bits != 16 && bits != 24) || bits >= p.bits) { printf("reduce: <bits> must be 16 or 24 and below the input's %d\n", p.bits); return 1; }
        const int s = p.bits - bits; const int64_t mx = (1LL << (bits - 1)) - 1, mn = -mx - 1; Pcm q; q.rate = p.rate; q.bits = bits; Rng r(4321);
        for (int c = 0; c < 2; c++) { q.c[c].resize(p.c[c].size()); for (size_t i = 0; i < p.c[c].size(); i++) {
                const int64_t r1 = (int64_t)(r.next() >> (64 - s)), r2 = (int64_t)(r.next() >> (64 - s)); q.c[c][i] = std::max<int64_t>(mn, std::min<int64_t>(mx, (p.c[c][i] + r1 - r2 + (1LL << (s - 1))) >> s)); } }
        return writeAll(argv[3], wavBytes(q, nullptr)) ? 0 : 2; }
    if (cmd == "excerpt" && argc >= 7) { Pcm p; std::string e = readWav(argv[2], p); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        int64_t s0 = (int64_t)(atof(argv[4]) * p.rate), L = (int64_t)(atof(argv[5]) * p.rate); int bits = atoi(argv[6]); if (s0 < 0 || L < 0 || s0 + L > p.frames()) { printf("excerpt outside the file\n"); return 2; }
        if (bits != 16 && bits != 24 && bits != 32) { printf("excerpt: <bits> must be 16, 24 or 32 (given: %s)\n", argv[6]); return 1; }   // FU15-6: a negative length ran past the samples, other bits wrote a WAV of the wrong word length
        Pcm q; q.rate = p.rate; q.bits = bits; Rng r(12345); for (int c = 0; c < 2; c++) { q.c[c].assign(p.c[c].begin() + s0, p.c[c].begin() + s0 + L);
            for (auto& v : q.c[c]) { const int64_t x = v; if (bits == 32) v = (x << 8) | (int64_t)(r.next() & 0xFF); else if (bits == 16) { const double u1 = r.uni(), u2 = r.uni(); v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(x / 256.0 + u1 + u2 + 0.5))); } else v = x; } }
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
    { const std::string mm = arg(argc, argv, "--mem"); char* e = nullptr; const double gb = mm.empty() ? 1 : strtod(mm.c_str(), &e);   // FU15-7 (2) (2026-11-14): checked before any work
      if (!mm.empty() && (*e || !(gb > 0 && gb <= 1048576))) { printf("--mem=<GB> must be a number of GB above 0, e.g. --mem=4 (given: %s)\n", mm.c_str()); return 1; } }
    if (cmd == "align" && argc >= 4) {   // check only: prints T and the band; nothing is moved or written
        Pcm cd; std::string e = readWav(argv[2], cd); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        if (cd.bits != 16 || (cd.rate != 44100 && cd.rate != 48000)) { printf("the core master must be 16-bit at 44100 or 48000 Hz\n"); return 2; }
        try { const std::string wd = (fs::temp_directory_path() / ("laes_align_" + std::to_string((unsigned long long)std::hash<std::string>()(argv[3])))).string(); Gone g{workDir(wd)}; laess::Prep m;
            e = laess::prepRead(argv[3], g.p + "/m", m); if (e.empty()) { int st = 0; while ((cd.rate << st) < m.rate && st < 4) st++; e = laess::prepLevels(m, cd.rate, g.p + "/m", pieceFor(st), threads, std::vector<int>()); }
            if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
            const AutoT A = autoAlign(argv[2], cd, argv[3], m, laeEncPath(argc, argv)); if (!A.found) { printf("%s\n", A.why.c_str()); return 2; }
            const bool ok = acceptAlign(A, argv[3], flag(argc, argv, "--accept"));
            printf("ALIGN_LAES T=%.10f err=%.1f resid=%.1f band=%s accepted=%d\n", A.T, A.err, A.resid, !A.silentBlocks.empty() ? "nomatch" : A.err <= -60 ? "ok" : A.err <= -54 ? "check" : "large", ok ? 1 : 0);   // DEC 25: nomatch = a silent check block
            if (ok) printf("use %s@%.10f (or %s@auto in enc)\n", argv[3], A.T, argv[3]); return ok ? 0 : 2; }
        catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; } }
    if (cmd == "parts" && argc >= 3) try {   // the structure and the SHA-256 of the audio bytes and of the payload (tests: unchanged by tags)
        laess::In in(argv[2]); Layout L; const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        printf("%s form%s\n", L.flac ? "FLAC" : "WAV", L.rf64 ? " (RF64)" : "");
        if (L.front) printf("  ID3v2 tag(s) of another program, in front  %lld bytes (kept, not read)\n", L.front);   // DEC 19
        for (auto& q : L.parts) { if (L.flac) { static const char* nm[] = {"STREAMINFO", "PADDING", "APPLICATION", "SEEKTABLE", "VORBIS_COMMENT", "CUESHEET", "PICTURE"};
                printf("  %s%s%s  %lld bytes\n", q.type <= 6 ? nm[q.type] : ("type " + std::to_string(q.type)).c_str(), q.id.empty() ? "" : " ", q.id.c_str(), q.len); }
            else printf("  \"%s\"  %lld bytes\n", q.id.c_str(), q.len); }
        if (L.behind) printf("  APEv2 / ID3v1 tag(s) of another program, at the end  %lld bytes (kept, not read)\n", L.behind);
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
        madePart = true; { laess::Out o(out + ".part");
          if (L.flac) {   // "fLaC" | STREAMINFO | the new tags | the other blocks as they were (old VORBIS_COMMENT, PICTURE, PADDING dropped) | audio frames
              std::vector<const Part*> keep; for (size_t i = 1; i < L.parts.size(); i++) { const int t = L.parts[i].type; if (t != 1 && t != 4 && t != 6 && t != 0) keep.push_back(&L.parts[i]); }
              const std::vector<uint8_t> tb = laestag::flacBlocks(tags, keep.empty());
              copyRange(in, o, 0, L.front);   // DEC 19: ID3v2 tags in front kept as they are
              std::vector<uint8_t> si(42); in.read(L.front, si.data(), 42); si[4] = (uint8_t)((tb.empty() && keep.empty() ? 0x80 : 0) | 0); o.put(si.data(), si.size()); o.put(tb.data(), tb.size());
              for (size_t i = 0; i < keep.size(); i++) { const Part& q = *keep[i]; const uint8_t bh[4] = {(uint8_t)((i + 1 == keep.size() ? 0x80 : 0) | q.type), (uint8_t)(q.len >> 16), (uint8_t)(q.len >> 8), (uint8_t)q.len};
                  o.put(bh, 4); copyRange(in, o, q.off, q.len); }
              copyRange(in, o, L.audioOff, L.audioLen); copyRange(in, o, L.audioOff + L.audioLen, L.behind); }   // DEC 19: APEv2 / ID3v1 kept
          else {   // RIFF (RF64 beyond 4 GB, as the encoder) | "fmt " | the new tags | the other chunks in their order (old LIST/INFO, id3 / ID3, ds64, JUNK dropped)
              const Part* fmt = nullptr; std::vector<const Part*> keep;
              for (size_t i = 0; i < L.parts.size(); i++) { const Part& q = L.parts[i]; if (q.id == "fmt ") fmt = &q; else if (q.id == "ds64" || q.id == "id3 " || q.id == "ID3 " || q.id == "JUNK" || L.unused(i)) continue;
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
    catch (const std::exception& x) { std::error_code ec; if (madePart) fs::remove(std::string(argv[3]) + ".part", ec); printf("stopped: %s; nothing written\n", x.what()); return 2; }
    if (cmd == "add" && argc >= 5) try {   // 2026-11-11: a NEW file = the old one + new masters at the end of the payload
        // laes add <in> <out> <master>[@T] ... [--name=<k>:<text>] [--no-derive[=<k>]] [--menu-order=<k1>,...]   (k of --name / --no-derive: the new
        // masters as given; --menu-order: the masters by their present numbers, the new ones numbered after them in the order given).
        // The core master, every old record and every old layer are copied byte for byte; each new master is coded against the core
        // master and every master before it, as laes enc does (the smallest candidate). The old masters are decoded once (they are the
        // references); every master of the new file is then checked against its SHA-256.
        const std::string src = argv[2], out = argv[3]; std::error_code ec;
        if (out.empty() || out[0] == '-') { printf("laes add <in> <out> <master> ...\n"); return 1; }
        if (fs::exists(out, ec)) { printf("%s exists: choose another name (the old file is kept, a new one is written)\n", out.c_str()); return 1; }
        laess::In in(src); Layout L; { const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        if (L.payload.empty()) { printf("%s: no LAE extension found\n", src.c_str()); return 2; }
        long long ptot = 0; for (auto& r : L.payload) ptot += r.second;
        auto pread = [&](long long at, void* d, size_t n) { uint8_t* q = (uint8_t*)d; long long base0 = 0;
            for (auto& r : L.payload) { if (n && at < base0 + r.second) { const long long o = at - base0; const size_t k = (size_t)std::min<long long>((long long)n, r.second - o); in.read(r.first + o, q, k); q += k; n -= k; at += (long long)k; } base0 += r.second; }
            if (n) laess::fail("payload truncated"); };
        { char mg[5] = {0}; if (ptot >= 4) pread(0, mg, 4); if (strcmp(mg, "LAE2")) { printf("%s: not LAES (extension %s)\n", src.c_str(), mg); return 2; } }
        if (ptot < 68) { printf("%s: LAES block too short\n", src.c_str()); return 2; }
        auto le = [](const uint8_t* p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i]; return v; };
        uint8_t hd[64]; pread(0, hd, 64); const int ver0 = hd[4] | hd[5] << 8, nOld = hd[14], base = (int)le(hd + 8, 4), coreMode = hd[15];
        if ((ver0 != 1 && ver0 != 2) || !laes::headerCountOk((uint64_t)ver0, (uint64_t)nOld)) { printf("%s: unsupported LAES version or header\n", src.c_str()); return 2; }
        struct ORec { uint8_t r[72]; std::string name; std::vector<std::pair<long long, long long>> lay; MasterInfo I; };
        std::vector<ORec> rs; long long pos = 64;
        for (int mi = 0; mi < nOld; mi++) { ORec R; pread(pos, R.r, 72); pos += 72; MasterInfo& I = R.I;
            I.bits = R.r[0]; I.stages = R.r[1]; I.w = R.r[2]; I.storage = R.r[3]; I.refTier = R.r[4]; I.rate = (int)le(R.r + 8, 4); I.frames = (int64_t)le(R.r + 16, 8); I.whole = (int64_t)le(R.r + 24, 8); I.frac = (uint32_t)le(R.r + 32, 4); memcpy(I.sha, R.r + 40, 32);
            if (ver0 == 2) { const int nl = R.r[5]; I.flags = R.r[6]; I.menu = R.r[7]; I.name.resize((size_t)nl); if (nl) pread(pos, &I.name[0], (size_t)nl); pos += nl; }
            if (I.stages > 3 || (I.storage == 2 && (I.refTier < 1 || I.refTier > mi))) { printf("%s: invalid master record %d\n", src.c_str(), mi + 1); return 2; }
            const int j = I.storage == 2 ? std::min(I.stages, rs[(size_t)I.refTier - 1].I.stages) : 0;
            for (int c = 0; c < 2; c++) for (int k = j; k <= I.stages; k++) { uint8_t t[4]; pread(pos, t, 4); const long long l = (long long)le(t, 4); if (pos + 4 + l > ptot - 4) { printf("%s: LAES block truncated\n", src.c_str()); return 2; } R.lay.push_back({pos, 4 + l}); pos += 4 + l; }
            rs.push_back(R); }
        if (pos != ptot - 4) { printf("%s: unexpected bytes after the last master\n", src.c_str()); return 2; }
        // the new masters and the options
        std::vector<std::string> paths; std::vector<double> Ts; std::vector<laess::WavInfo> wis;
        for (int i = 4; i < argc; i++) { if (argv[i][0] == '-') continue; std::string a; double T = 0; bool au = false; { const std::string me = masterArg(argv[i], false, a, T, au); if (!me.empty()) { printf("%s\n", me.c_str()); return 1; } }   // FU15-3
            laess::WavInfo wi; const std::string e = laess::probeMaster(a, wi); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } longHeaderNote(a, wi.hdrLen, wi.dataBytes); paths.push_back(a); Ts.push_back(T); wis.push_back(wi); }
        if (paths.empty()) { printf("no new master given\n"); return 1; }
        const size_t nAll = (size_t)nOld + paths.size(); if (nAll > 255) { printf("at most 255 masters (%zu now, %zu new)\n", (size_t)nOld, paths.size()); return 1; }
        for (auto& w : wis) { int st = 0; while ((base << st) < w.rate && st < 4) st++; if ((base << st) != w.rate || st > 3) { printf("a new master must be at %d Hz times 1, 2, 4 or 8\n", base); return 1; } }
        std::vector<std::string> names(paths.size()); std::vector<int> mflags(paths.size(), 0); std::vector<size_t> menuList;
        for (int i = 4; i < argc; i++) { const std::string a = argv[i]; if (a[0] != '-') continue;
            if (a.rfind("--name=", 0) == 0) { const std::string v = a.substr(7); const size_t col = v.find(':'); char* e = nullptr; const long k = col == std::string::npos ? 0 : strtol(v.c_str(), &e, 10);
                if (col == std::string::npos || e != v.c_str() + col || k < 1 || k > (long)paths.size()) { printf("--name=<k>:<text>, k = 1 ... %zu (the new masters as given)\n", paths.size()); return 1; }
                const std::string t = v.substr(col + 1); if (!laes::validMasterName(t)) { printf("--name=%ld: a name is UTF-8 of at most 255 bytes without control characters\n", k); return 1; } names[(size_t)k - 1] = t; }
            else if (a == "--no-derive") { for (int& f : mflags) f |= laes::kNoDerive; }
            else if (a.rfind("--no-derive=", 0) == 0) { char* e = nullptr; const long k = strtol(a.c_str() + 12, &e, 10); if (*e || k < 1 || k > (long)paths.size()) { printf("--no-derive=<k>, k = 1 ... %zu\n", paths.size()); return 1; } mflags[(size_t)k - 1] |= laes::kNoDerive; }
            else if (a.rfind("--menu-order=", 0) == 0) { std::vector<bool> seen(nAll + 1, false); size_t a0 = 13;
                for (;;) { const size_t b0 = a.find(',', a0); const std::string t = a.substr(a0, b0 == std::string::npos ? std::string::npos : b0 - a0); char* e = nullptr; const long k = strtol(t.c_str(), &e, 10);
                    if (*e || k < 1 || k > (long)nAll || seen[(size_t)k]) { printf("--menu-order=<k1>,<k2>,...: every master (1 ... %zu; the new ones from %d on) once\n", nAll, nOld + 1); return 1; } seen[(size_t)k] = true; menuList.push_back((size_t)k);
                    if (b0 == std::string::npos) break; a0 = b0 + 1; }
                if (menuList.size() != nAll) { printf("--menu-order=<k1>,<k2>,...: every master (1 ... %zu) once\n", nAll); return 1; } }
            else if (a == "--timing" || a.rfind("--threads", 0) == 0) continue;
            else { printf("unknown option %s\n", a.c_str()); return 1; } }
        // the new masters in the encoder's order among themselves (more bits first, then higher rate); "num" = their number in menus (after the old ones)
        std::vector<size_t> ordN(paths.size()); for (size_t i = 0; i < ordN.size(); i++) ordN[i] = i;
        std::stable_sort(ordN.begin(), ordN.end(), [&](size_t a, size_t b) { return wis[a].bits != wis[b].bits ? wis[a].bits > wis[b].bits : wis[a].rate > wis[b].rate; });
        // menu positions of all masters (old ones by their present number, new ones after them as given), then by --menu-order
        std::vector<MasterInfo> oldInfo; for (auto& R : rs) oldInfo.push_back(R.I); const std::vector<size_t> oldOrder = laesc::menuOrder(oldInfo);
        std::vector<size_t> numOf(nAll, 0);   // the present number of each master in the new file order (old: as now; new: nOld + k as given)
        for (size_t p = 0; p < oldOrder.size(); p++) numOf[oldOrder[p]] = p + 1; for (size_t q = 0; q < ordN.size(); q++) numOf[(size_t)nOld + q] = (size_t)nOld + ordN[q] + 1;
        std::vector<int> menus(nAll, 0); { bool same = true; for (size_t i = 0; i < nAll; i++) { size_t p = numOf[i]; if (!menuList.empty()) for (size_t z = 0; z < menuList.size(); z++) if (menuList[z] == numOf[i]) p = z + 1; menus[i] = (int)p; same &= p == i + 1; }
          if (same) std::fill(menus.begin(), menus.end(), 0); }
        // the old masters decoded once (this program's dec command): they are the references of the new ones
        Gone g{workDir(out + ".add")}; const std::string dd = g.p + "/dec";
        { std::vector<std::string> av = {"laes", "dec", src, dd}; std::vector<char*> ap; for (auto& s : av) ap.push_back(&s[0]); ap.push_back(nullptr);
          if (laesMain((int)av.size(), ap.data()) != 0) { printf("%s: the masters could not be decoded and checked; nothing written\n", src.c_str()); return 2; } }
        Pcm core; { const std::string e = readWav(dd + "/core.wav", core); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        int maxSt = 0; std::vector<int> stOf(nAll); for (size_t i = 0; i < nAll; i++) { stOf[i] = i < (size_t)nOld ? rs[i].I.stages : [&] { int st = 0; while ((base << st) < wis[ordN[i - nOld]].rate) st++; return st; }(); maxSt = std::max(maxSt, stOf[i]); }
        auto othersOf = [&](size_t i) { std::vector<int> o; for (size_t k = 0; k < stOf.size(); k++) if (k != i) o.push_back(stOf[k]); return o; };
        const int64_t piece = pieceFor(maxSt); std::vector<laess::Prep> pm(nAll);
        for (size_t i = 0; i < nAll; i++) { const std::string stem = g.p + "/m" + std::to_string(i); pm[i].threads = threads;
            const std::string p = i < (size_t)nOld ? dd + "/master_" + std::to_string(laesc::displayNo(oldInfo, i)) + "_" + std::to_string(rs[i].I.rate) + "_" + std::to_string(rs[i].I.bits) + ".wav" : paths[ordN[i - nOld]];
            std::string e = laess::prepRead(p, stem, pm[i]); if (e.empty()) e = laess::prepLevels(pm[i], base, stem, piece, threads, othersOf(i)); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
            if (i < (size_t)nOld && memcmp(pm[i].sha, rs[i].I.sha, 32)) { printf("master %zu decoded differs from its record; nothing written\n", i + 1); return 2; } }
        // T relative to the core master for every master: old ones from their chain (a master stored in full has none: 0), new ones as given
        std::vector<int64_t> absT; { const std::string te = absTimes(oldInfo, absT); if (!te.empty()) { printf("%s: %s; nothing written\n", src.c_str(), te.c_str()); return 2; } } absT.resize(nAll, 0);   // FU15-5
        std::vector<laess::MasterSel> sel(nAll); std::vector<MasterInfo> fixedInfo(oldInfo);
        for (size_t i = 0; i < nAll; i++) { laess::MasterSel& s = sel[i];
            if (i < (size_t)nOld) { s.whole = absT[i] >> 32; s.frac = (uint32_t)(absT[i] & 0xFFFFFFFFLL); s.name = rs[i].I.name; s.flags = rs[i].I.flags; fixedInfo[i].menu = menus[i]; }
            else { const size_t q = ordN[i - nOld]; MasterIn t; setT(t, Ts[q]); s.storage = -1; s.whole = t.whole; s.frac = t.frac; s.name = names[q]; s.flags = mflags[q]; }
            s.menu = menus[i]; }
        laess::Core32 core32 = laess::toCore32(core); std::vector<const laess::Prep*> m2; for (auto& p : pm) m2.push_back(&p); std::vector<MasterInfo> i2; const std::string bp = g.p + "/body";
        { const std::string e = laess::encodeLaesS(core32, base, m2, sel, i2, bp, g.p + "/o", coreMode, piece, threads, nullptr, std::string(), nullptr, &fixedInfo); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        // the new payload: header (version, M), the old records and layers byte for byte, the new masters as coded, a new CRC-32
        std::vector<MasterInfo> vi(nAll); for (size_t i = 0; i < nAll; i++) { vi[i].name = sel[i].name; vi[i].flags = sel[i].flags; vi[i].menu = sel[i].menu; }
        laes::Head Hold; if (!laes::headFrom((uint64_t)ver0, hd + 56, (uint64_t)nOld, Hold)) { printf("%s: invalid LAES header\n", src.c_str()); return 2; }   // kept: the default master's index does not change (new masters go last)
        const int ver = laes::payloadVersionFor(vi, Hold);
        auto crcOf = [](uint32_t c, const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; } return c; };
        const std::string bodyP = out + ".body.part"; long long bsz = 0; madePart = true;
        { laess::Out o(bodyP); uint32_t c = 0xFFFFFFFFu; auto put = [&](const void* d, size_t n) { o.put(d, n); c = crcOf(c, (const uint8_t*)d, n); bsz += (long long)n; };
          uint8_t h2[64]; memcpy(h2, hd, 64); h2[4] = (uint8_t)ver; h2[5] = 0; h2[14] = (uint8_t)nAll; laes::headTo(ver, Hold, h2 + 56); put(h2, 64); std::vector<uint8_t> b;
          for (size_t i = 0; i < rs.size(); i++) { const ORec& R = rs[i]; uint8_t r2[72]; memcpy(r2, R.r, 72); r2[5] = ver == 2 ? (uint8_t)R.I.name.size() : 0; r2[6] = ver == 2 ? (uint8_t)R.I.flags : 0; r2[7] = ver == 2 ? (uint8_t)menus[i] : 0;
              put(r2, 72); if (ver == 2 && !R.I.name.empty()) put(R.I.name.data(), R.I.name.size());
              for (auto& l : R.lay) for (long long a = 0; a < l.second; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, l.second - a)); pread(l.first + a, b.data(), b.size()); put(b.data(), b.size()); } }
          { laess::In nb(bp); for (long long a = 64; a < nb.n - 4; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, nb.n - 4 - a)); nb.read(a, b.data(), b.size()); put(b.data(), b.size()); } }
          c ^= 0xFFFFFFFFu; const uint8_t t[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)}; o.put(t, 4); bsz += 4; o.close(); }
        { const std::string we = writeWithPayload(in, L, bodyP, bsz, out + ".part"); fs::remove(bodyP, ec); if (!we.empty()) { fs::remove(out + ".part", ec); printf("%s: %s\n", src.c_str(), we.c_str()); return 2; } }
        std::string why; { laess::In nw(out + ".part"); Layout N; why = readLayout(nw, N);
            if (why.empty() && shaOf(in, {{L.audioOff, L.audioLen}}) != shaOf(nw, {{N.audioOff, N.audioLen}})) why = "the audio bytes differ"; }
        if (why.empty()) why = checkStored(out + ".part", threads);   // every master, old and new, decoded and checked against its SHA-256
        if (!why.empty()) { fs::remove(out + ".part", ec); printf("self-check failed: %s; nothing written\n", why.c_str()); return 2; }
        fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; }
        printf("%s: %s form, payload version %d, %zu master(s) (%d before); the old ones copied byte for byte, every master's SHA-256 checked\n", out.c_str(), L.flac ? "FLAC" : "WAV", ver, nAll, nOld);
        { std::vector<MasterInfo> m1(nAll); for (size_t i = 0; i < nAll; i++) m1[i] = i < (size_t)nOld ? rs[i].I : i2[i]; for (size_t i = 0; i < nAll; i++) { m1[i].name = sel[i].name; m1[i].menu = sel[i].menu; m1[i].flags = sel[i].flags; }
          const std::vector<size_t> o1 = laesc::menuOrder(m1);
          for (size_t p = 0; p < o1.size(); p++) { const size_t i = o1[p]; const MasterInfo& I = m1[i]; const std::string tier = I.storage == 1 ? "full" : I.refTier == 0 ? "difference to the core master" : "difference to master " + std::to_string(laesc::displayNo(m1, (size_t)I.refTier - 1));
              printf("  master %zu%s: %d Hz %d-bit%s%s\n", p + 1, I.name.empty() ? "" : (" \"" + I.name + "\"").c_str(), I.rate, I.bits, i < (size_t)nOld ? " (as before)" : (", new, " + tier).c_str(), (I.flags & laes::kNoDerive) ? " (no derived versions)" : ""); } }
        return 0; }
    catch (const std::exception& x) { std::error_code ec; if (madePart) { fs::remove(std::string(argv[3]) + ".part", ec); fs::remove(std::string(argv[3]) + ".body.part", ec); } printf("stopped: %s; nothing written\n", x.what()); return 2; }
    if (cmd == "extract" && argc >= 5) try {   // 2026-11-11: a NEW file with some of the masters, packed again
        // laes extract <in> <out> --pick=<p>[,<p>...]   p = master:<k> | name:<text> | <rate>:<bits> (masters only; several fit: the user ticks them; master:<a>-<b> a range).
        // The chosen masters are decoded and encoded again in full (laes enc with the same core master), so each one gets the best
        // reference among those kept; nothing is cut out of the old payload. The order of --pick is the menu order of the new file;
        // names and "no derived versions" go with their masters; the tags, pictures and the core master's audio bytes are kept as they are.
        const std::string src = argv[2], out = argv[3]; std::error_code ec;
        if (out.empty() || out[0] == '-') { printf("laes extract <in> <out> --pick=master:<k>,...\n"); return 1; }
        if (fs::exists(out, ec)) { printf("%s exists: choose another name (the old file is kept, a new one is written)\n", out.c_str()); return 1; }
        const std::string pk = arg(argc, argv, "--pick"); if (pk.empty()) { printf("give --pick=master:<k>,name:<text>,<rate>:<bits>,...\n"); return 1; }
        laess::In in(src); Layout L; { const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        if (L.payload.empty()) { printf("%s: no LAE extension found\n", src.c_str()); return 2; }
        Gone g{workDir(out + ".extract")}; const std::string dd = g.p + "/dec";
        // the records (names, flags, menu numbers, T) as the player reads them
        laesp::FnSrc fsrc; fsrc.n = (uint64_t)in.n; fsrc.fn = [&](uint64_t at, void* d, size_t k) { in.read((long long)at, d, k); return 0; }; laesp::File pf; { char mg[5]; const std::string e = pf.open(fsrc, mg, true); if (!e.empty() || !pf.why.empty()) { printf("%s: %s\n", src.c_str(), e.empty() ? pf.why.c_str() : e.c_str()); return 2; } }
        std::vector<MasterInfo> mi; for (const auto& M : pf.ms) mi.push_back(M.I); const std::vector<size_t> mo = laesc::menuOrder(mi);
        std::vector<size_t> chosen;   // file indices, in the order of --pick
        { size_t a = 0; for (;;) { const size_t b = pk.find(',', a); const std::string t = pk.substr(a, b == std::string::npos ? std::string::npos : b - a); std::vector<size_t> hit;
              if (t.rfind("master:", 0) == 0) { std::vector<long> v; if (laespick::parseRanges(t.substr(7), v)) { bool okR = true; for (long k : v) okR = okR && k >= 1 && k <= (long)mo.size(); if (okR) for (long k : v) hit.push_back(mo[(size_t)k - 1]); } }   // master:<k> or master:<a>-<b>
              else if (t.rfind("name:", 0) == 0) { for (size_t p = 0; p < mo.size(); p++) if (!t.substr(5).empty() && mi[mo[p]].name == t.substr(5)) hit.push_back(mo[p]); }
              else if (t.find(':') != std::string::npos) { const int r = atoi(t.c_str()), bb = atoi(t.c_str() + t.find(':') + 1); for (size_t p = 0; p < mo.size(); p++) if (mi[mo[p]].rate == r && mi[mo[p]].bits == bb) hit.push_back(mo[p]); }
              if (hit.empty()) { printf("--pick: '%s' is no master of %s (master:<k>, name:<text> or <rate>:<bits>; see laes dec --list)\n", t.c_str(), src.c_str()); return 1; }
              // several masters fit (<rate>:<bits> or name:): the user ticks the ones to keep; in a console a tick list, else listed, nothing written
              if (hit.size() > 1 && t.rfind("master:", 0) != 0) {
                  std::vector<laespick::Item> it; for (size_t h : hit) { char lb[48]; snprintf(lb, sizeof lb, "%6d Hz %2d-bit  ", mi[h].rate, mi[h].bits); const long no = laesc::displayNo(mi, h); it.push_back({no, lb + laesc::masterLabel((size_t)no, mi[h])}); }
                  char tl[256]; snprintf(tl, sizeof tl, "--pick: '%s' fits %zu masters. Tick the ones to keep (the numbers are master:<k>):", t.c_str(), hit.size());
                  if (!laespick::interactive()) { printf("%s\n", tl); for (const auto& x : it) printf("  master:%ld  %s\n", x.no, x.line.c_str());
                      printf("Not in a console: give --pick=master:<k>, several master:<k>,master:<k>, a range master:<a>-<b>. Nothing written.\n"); return 1; }
                  const std::vector<long> got = laespick::tick(tl, it, "e.g. " + std::to_string(it.front().no) + "," + std::to_string(it.back().no) + " or " + std::to_string(it.front().no) + "-" + std::to_string(it.back().no));
                  if (got.empty()) return 1; hit.clear(); for (long x : got) hit.push_back(mo[(size_t)x - 1]); }
              for (size_t h : hit) if (std::find(chosen.begin(), chosen.end(), h) == chosen.end()) chosen.push_back(h);
              if (b == std::string::npos) break; a = b + 1; } }
        const std::string userMem = arg(argc, argv, "--mem");   // FU16-2 (2026-11-14): the user's --mem and --small-disk go to the inner dec / enc (they change no byte)
        { std::vector<std::string> av = {"laes", "dec", src, dd}; if (!userMem.empty()) av.push_back("--mem=" + userMem); std::vector<char*> ap; for (auto& s : av) ap.push_back(&s[0]); ap.push_back(nullptr);   // every master, checked
          if (laesMain((int)av.size(), ap.data()) != 0) { printf("%s: the masters could not be decoded and checked; nothing written\n", src.c_str()); return 2; } }
        // T relative to the core master (from each chain; a master stored in full has none: 0) in base samples
        std::vector<int64_t> absT; { const std::string te = absTimes(mi, absT); if (!te.empty()) { printf("%s: %s; nothing written\n", src.c_str(), te.c_str()); return 2; } }   // FU16-1 (3)
        const std::string tmpOut = g.p + (L.flac ? "/new.flac" : "/new.wav");
        std::vector<std::string> av = {"laes", "enc", tmpOut, "--core=" + dd + "/core.wav"};
        gExactT.clear(); struct ClearT { ~ClearT() { gExactT.clear(); } } clearT;
        for (size_t q = 0; q < chosen.size(); q++) { const size_t i = chosen[q];   // DEC 15 (2026-11-15): T as the exact number (gExactT); before, as "@<%.17g text>", which lost bits above 2^21 samples
            av.push_back(dd + "/master_" + std::to_string(laesc::displayNo(mi, i)) + "_" + std::to_string(mi[i].rate) + "_" + std::to_string(mi[i].bits) + ".wav"); gExactT.push_back({av.back(), absT[i]});
            if (!mi[i].name.empty()) av.push_back("--name=" + std::to_string(q + 1) + ":" + mi[i].name);
            if (mi[i].flags & laes::kNoDerive) av.push_back("--no-derive=" + std::to_string(q + 1));
            if (pf.head.def == (int)i + 1) av.push_back("--default=" + std::to_string(q + 1)); }   // the default goes with its master; not chosen: the core master (2026-11-12)
        if (pf.head.coreFlags & laes::kNoDerive) av.push_back("--no-derive=core");
        if (!userMem.empty()) av.push_back("--mem=" + userMem); if (flag(argc, argv, "--small-disk")) av.push_back("--small-disk");
        av.push_back("--menu-order");
        { std::vector<char*> ap; for (auto& s : av) ap.push_back(&s[0]); ap.push_back(nullptr); if (laesMain((int)av.size(), ap.data()) != 0) { printf("encoding the chosen masters failed; nothing written\n"); return 2; } }
        // the new payload in the old file's layout (tags, pictures and the core master's audio bytes as they were)
        laess::In nt(tmpOut); Layout NL; { const std::string e = readLayout(nt, NL); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        const std::string bodyP = out + ".body.part"; long long bsz = 0; madePart = true; { laess::Out o(bodyP); for (auto& r : NL.payload) { copyRange(nt, o, r.first, r.second); bsz += r.second; } o.close(); }
        { const std::string we = writeWithPayload(in, L, bodyP, bsz, out + ".part"); fs::remove(bodyP, ec); if (!we.empty()) { fs::remove(out + ".part", ec); printf("%s: %s\n", src.c_str(), we.c_str()); return 2; } }
        std::string why; { laess::In nw(out + ".part"); Layout N; why = readLayout(nw, N);
            if (why.empty() && shaOf(in, {{L.audioOff, L.audioLen}}) != shaOf(nw, {{N.audioOff, N.audioLen}})) why = "the audio bytes differ"; }
        if (why.empty()) why = checkStored(out + ".part", threads);
        if (!why.empty()) { fs::remove(out + ".part", ec); printf("self-check failed: %s; nothing written\n", why.c_str()); return 2; }
        fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; }
        printf("%s: %s form, %zu of %zu master(s), encoded again; tags and the core master's audio bytes as before; every master's SHA-256 checked\n", out.c_str(), L.flac ? "FLAC" : "WAV", chosen.size(), mi.size());
        for (size_t q = 0; q < chosen.size(); q++) { const MasterInfo& I = mi[chosen[q]]; printf("  master %zu%s: %d Hz %d-bit (master %d before)\n", q + 1, I.name.empty() ? "" : (" \"" + I.name + "\"").c_str(), I.rate, I.bits, laesc::displayNo(mi, chosen[q])); }
        return 0; }
    catch (const std::exception& x) { std::error_code ec; if (madePart) { fs::remove(std::string(argv[3]) + ".part", ec); fs::remove(std::string(argv[3]) + ".body.part", ec); } printf("stopped: %s; nothing written\n", x.what()); return 2; }
    if (cmd == "names" && argc >= 4) try {   // 2026-11-11: a NEW file with other master names / derive flags; every layer copied byte for byte
        const std::string src = argv[2], out = argv[3]; std::error_code ec;
        // --in-place (2026-11-12): only the default and the core master's flag change; the file keeps its size and every
        // other byte: header bytes 4 (version) and 56 / 57 and the CRC-32 of the payload, 7 bytes at most; the old bytes are printed first,
        // every master is checked afterwards, and on any failure the old bytes are written back
        const bool inPlace = out == "--in-place";
        if (!inPlace && (out.empty() || out[0] == '-')) { printf("give the new file's name: laes names <in> <out> --name=<k>:<text> ... (or laes names <in> --in-place --default=<k>|core)\n"); return 1; }
        if (!inPlace && fs::exists(out, ec)) { printf("%s exists: choose another name (the old file is kept, a new one is written)\n", out.c_str()); return 1; }
        if (inPlace) for (int i = 4; i < argc; i++) { const std::string a = argv[i];
            if (a.rfind("--default=", 0) != 0 && a != "--no-derive=core" && a != "--derive=core") { printf("--in-place takes only --default=<k>|core, --no-derive=core, --derive=core (%s changes the size: write a new file)\n", a.c_str()); return 1; } }
        laess::In in(src); Layout L; { const std::string e = readLayout(in, L); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } }
        if (L.payload.empty()) { printf("%s: no LAE extension found (not an LAES / LAE file)\n", src.c_str()); return 2; }
        long long ptot = 0; for (auto& r : L.payload) ptot += r.second;
        auto pread = [&](long long at, void* d, size_t n) { uint8_t* q = (uint8_t*)d; long long base = 0;   // the payload as one byte string over its ranges
            for (auto& r : L.payload) { if (n && at < base + r.second) { const long long o = at - base; const size_t k = (size_t)std::min<long long>((long long)n, r.second - o); in.read(r.first + o, q, k); q += k; n -= k; at += (long long)k; } base += r.second; }
            if (n) laess::fail("payload truncated"); };
        { char mg[5] = {0}; if (ptot >= 4) pread(0, mg, 4); if (strcmp(mg, "LAE2")) { printf("%s: not LAES (extension %s)\n", src.c_str(), mg); return 2; } }
        if (ptot < 68) { printf("%s: LAES block too short\n", src.c_str()); return 2; }
        auto crcOf = [](uint32_t c, const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; } return c; };
        // pass 1: the records as they are (and the old CRC-32)
        uint8_t hd[64]; pread(0, hd, 64); const int ver0 = hd[4] | hd[5] << 8, nm = hd[14]; if ((ver0 != 1 && ver0 != 2) || !laes::headerCountOk((uint64_t)ver0, (uint64_t)nm)) { printf("%s: unsupported LAES version or header\n", src.c_str()); return 2; }
        struct Rec { uint8_t r[72]; std::string name; int flags = 0, menu = 0; int stages = 0, j = 0; std::vector<std::pair<long long, long long>> lay; };   // lay: (offset, length) of each layer with its u32 length
        std::vector<Rec> rs; long long pos = 64;
        for (int mi = 0; mi < nm; mi++) { Rec R; pread(pos, R.r, 72); pos += 72; if (ver0 == 2) { const int nl = R.r[5]; R.flags = R.r[6]; R.menu = R.r[7]; R.name.resize((size_t)nl); if (nl) pread(pos, &R.name[0], (size_t)nl); pos += nl; }
            R.stages = R.r[1]; const int storage = R.r[3], tier = R.r[4]; if (R.stages > 3 || (storage == 2 && (tier < 1 || tier > mi))) { printf("%s: invalid master record %d\n", src.c_str(), mi + 1); return 2; }
            R.j = storage == 2 ? std::min(R.stages, rs[(size_t)tier - 1].stages) : 0;
            for (int c = 0; c < 2; c++) for (int k = R.j; k <= R.stages; k++) { uint8_t t[4]; pread(pos, t, 4); const long long l = t[0] | t[1] << 8 | t[2] << 16 | (long long)t[3] << 24; if (pos + 4 + l > ptot - 4) { printf("%s: LAES block truncated\n", src.c_str()); return 2; } R.lay.push_back({pos, 4 + l}); pos += 4 + l; }
            rs.push_back(R); }
        if (pos != ptot - 4) { printf("%s: unexpected bytes after the last master\n", src.c_str()); return 2; }
        { uint32_t c = 0xFFFFFFFFu; std::vector<uint8_t> b; for (long long a = 0; a < ptot - 4; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, ptot - 4 - a)); pread(a, b.data(), b.size()); c = crcOf(c, b.data(), b.size()); }
          uint8_t t[4]; pread(ptot - 4, t, 4); if ((c ^ 0xFFFFFFFFu) != (uint32_t)(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24)) { printf("%s: LAES block damaged (CRC-32); nothing written\n", src.c_str()); return 2; } }
        // the changes: --name=<k>:<text> (k = master k as menus number it; an empty text removes the name), --no-derive[=<k>], --derive[=<k>],
        // --menu-order=<k1>,<k2>,... (the masters by their present numbers, in the new order)
        std::vector<size_t> mord; { std::vector<MasterInfo> m0(rs.size()); for (size_t i = 0; i < rs.size(); i++) m0[i].menu = rs[i].menu; mord = laesc::menuOrder(m0); }   // mord[k - 1] = the file index of master k
        bool changed = false;
        laes::Head H; if (!laes::headFrom((uint64_t)ver0, hd + 56, (uint64_t)nm, H)) { printf("%s: invalid LAES header\n", src.c_str()); return 2; }   // the default and the core master's flags (2026-11-12)
        for (int i = 4; i < argc; i++) { const std::string a = argv[i];
            auto num = [&](const char* s, long& k) { char* e = nullptr; k = strtol(s, &e, 10); return !*e && k >= 1 && k <= nm; };
            if (a.rfind("--default=", 0) == 0) { long k = 0; if (a.substr(10) == "core") H.def = 0; else if (num(a.c_str() + 10, k)) H.def = (int)mord[(size_t)k - 1] + 1;
                else { printf("--default=<k> (k = 1 ... %d, as menus number them) or --default=core\n", nm); return 1; } changed = true; continue; }
            if (a == "--no-derive=core" || a == "--derive=core") { H.coreFlags = a[2] == 'n' ? (H.coreFlags | laes::kNoDerive) : (H.coreFlags & ~laes::kNoDerive); changed = true; continue; }
            if (a == "--no-derive" || a == "--derive") H.coreFlags = a == "--derive" ? (H.coreFlags & ~laes::kNoDerive) : (H.coreFlags | laes::kNoDerive);
            if (a.rfind("--name=", 0) == 0) { const std::string v = a.substr(7); const size_t col = v.find(':'); long k = 0;
                if (col == std::string::npos || !num(v.substr(0, col).c_str(), k)) { printf("--name=<k>:<text>, k = 1 ... %d (the masters of the file)\n", nm); return 1; }
                const std::string t = v.substr(col + 1); if (!laes::validMasterName(t)) { printf("--name=%ld: a name is UTF-8 of at most 255 bytes without control characters (%zu bytes given)\n", k, t.size()); return 1; }
                rs[mord[(size_t)k - 1]].name = t; changed = true; }
            else if (a == "--no-derive" || a == "--derive") { for (Rec& R : rs) R.flags = a == "--derive" ? (R.flags & ~laes::kNoDerive) : (R.flags | laes::kNoDerive); changed = true; }
            else if (a.rfind("--no-derive=", 0) == 0 || a.rfind("--derive=", 0) == 0) { const bool off = a[2] == 'n'; long k = 0; if (!num(a.c_str() + (off ? 12 : 9), k)) { printf("%s: k = 1 ... %d\n", a.c_str(), nm); return 1; }
                Rec& R = rs[mord[(size_t)k - 1]]; R.flags = off ? (R.flags | laes::kNoDerive) : (R.flags & ~laes::kNoDerive); changed = true; }
            else if (a.rfind("--menu-order=", 0) == 0) { std::vector<bool> seen((size_t)nm + 1, false); std::vector<size_t> nl2; size_t a0 = 13;
                for (;;) { const size_t b0 = a.find(',', a0); const std::string t = a.substr(a0, b0 == std::string::npos ? std::string::npos : b0 - a0); long k = 0;
                    if (!num(t.c_str(), k) || seen[(size_t)k]) { printf("--menu-order=<k1>,<k2>,...: every master (1 ... %d) once\n", nm); return 1; } seen[(size_t)k] = true; nl2.push_back((size_t)k);
                    if (b0 == std::string::npos) break; a0 = b0 + 1; }
                if ((int)nl2.size() != nm) { printf("--menu-order=<k1>,<k2>,...: every master (1 ... %d) once\n", nm); return 1; }
                std::vector<int> nmenu(rs.size(), 0); bool same = true; for (size_t p = 0; p < nl2.size(); p++) nmenu[mord[nl2[p] - 1]] = (int)p + 1;
                for (size_t i = 0; i < rs.size(); i++) same &= nmenu[i] == (int)i + 1;
                for (size_t i = 0; i < rs.size(); i++) rs[i].menu = same ? 0 : nmenu[i]; changed = true; }
            else { printf("unknown option %s (laes names <in> <out> --name=<k>:<text> --no-derive[=<k>|core] --derive[=<k>|core] --menu-order=<k1>,... --default=<k>|core)\n", a.c_str()); return 1; } }
        if (!changed) { printf("nothing to change: give --name=<k>:<text>, --no-derive[=<k>|core], --derive[=<k>|core], --menu-order=<k1>,... or --default=<k>|core\n"); return 1; }
        std::vector<MasterInfo> vi(rs.size()); for (size_t i = 0; i < rs.size(); i++) { vi[i].name = rs[i].name; vi[i].flags = rs[i].flags; vi[i].menu = rs[i].menu; } const int ver = laes::payloadVersionFor(vi, H);
        if (inPlace) {
            uint8_t h2[64]; memcpy(h2, hd, 64); h2[4] = (uint8_t)ver; h2[5] = 0; laes::headTo(ver, H, h2 + 56);
            uint32_t c = crcOf(0xFFFFFFFFu, h2, 64); { std::vector<uint8_t> b; for (long long a = 64; a < ptot - 4; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, ptot - 4 - a)); pread(a, b.data(), b.size()); c = crcOf(c, b.data(), b.size()); } }
            c ^= 0xFFFFFFFFu; const uint8_t t4[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
            // the file offset of payload byte p (the payload may be split over FLAC APPLICATION blocks)
            auto fileAt = [&](long long p) { long long base = 0; for (auto& r : L.payload) { if (p < base + r.second) return r.first + (p - base); base += r.second; } return -1LL; };
            struct Ch { long long at; uint8_t old, now; }; std::vector<Ch> chg;
            uint8_t o4[4]; pread(ptot - 4, o4, 4);
            for (int i = 0; i < 64; i++) if (h2[i] != hd[i]) chg.push_back({fileAt(i), hd[i], h2[i]});
            for (int i = 0; i < 4; i++) if (t4[i] != o4[i]) chg.push_back({fileAt(ptot - 4 + i), o4[i], t4[i]});
            if (in.f) { fclose(in.f); in.f = nullptr; }   // closed before writing
            if (chg.empty()) { printf("%s: nothing changes\n", src.c_str()); return 0; }
            printf("%s: %zu bytes change in place (file offset: old -> new):", src.c_str(), chg.size()); for (auto& x : chg) printf(" %lld: %02X -> %02X;", x.at, x.old, x.now); printf("\n");
            bool opened = false;   // FU16-4 (2026-11-14): whether the file could be opened for writing at all
            auto put = [&](bool now) { FILE* f = fopen(src.c_str(), "r+b"); opened = f != nullptr; if (!f) return false; bool ok = true;
                for (auto& x : chg) { ok = ok && laess::seekTo(f, x.at) && fputc(now ? x.now : x.old, f) != EOF; } ok = fflush(f) == 0 && ok; fclose(f); return ok; };
            if (!put(true)) { if (!opened) { printf("cannot write %s (it cannot be opened for writing); nothing changed\n", src.c_str()); return 2; }
                const bool back = put(false);   // FU16-4: the writing back is checked too, as after the self-check
                printf("cannot write %s; %s\n", src.c_str(), back ? "the old bytes are written back" : "WRITING THE OLD BYTES BACK FAILED"); return 2; }
            const std::string why = checkStored(src, threads);
            if (!why.empty()) { const bool back = put(false); printf("self-check failed: %s; %s\n", why.c_str(), back ? "the old bytes are written back" : "WRITING THE OLD BYTES BACK FAILED"); return 2; }
            printf("%s: payload version %d, changed in place; every master's SHA-256 checked\n", src.c_str(), ver);
            if (H.def) printf("  default: master %d%s\n", laesc::displayNo(vi, (size_t)H.def - 1), rs[(size_t)H.def - 1].name.empty() ? "" : (" \"" + rs[(size_t)H.def - 1].name + "\"").c_str()); else printf("  default: the core master\n");
            if (H.coreFlags & laes::kNoDerive) printf("  the core master: no derived 16-bit version\n");
            return 0; }
        // pass 2: the new payload (header with the version, records with names and flags, the layers byte for byte, a new CRC-32)
        const std::string bodyP = out + ".body.part"; long long bsz = 0; madePart = true;
        { laess::Out o(bodyP); uint32_t c = 0xFFFFFFFFu; auto put = [&](const void* d, size_t n) { o.put(d, n); c = crcOf(c, (const uint8_t*)d, n); bsz += (long long)n; };
          uint8_t h2[64]; memcpy(h2, hd, 64); h2[4] = (uint8_t)ver; h2[5] = 0; laes::headTo(ver, H, h2 + 56); put(h2, 64);
          std::vector<uint8_t> b;
          for (const Rec& R : rs) { uint8_t r2[72]; memcpy(r2, R.r, 72); r2[5] = ver == 2 ? (uint8_t)R.name.size() : 0; r2[6] = ver == 2 ? (uint8_t)R.flags : 0; r2[7] = ver == 2 ? (uint8_t)R.menu : 0; put(r2, 72); if (ver == 2 && !R.name.empty()) put(R.name.data(), R.name.size());
              for (auto& l : R.lay) for (long long a = 0; a < l.second; a += 1 << 22) { b.resize((size_t)std::min<long long>(1 << 22, l.second - a)); pread(l.first + a, b.data(), b.size()); put(b.data(), b.size()); } }
          c ^= 0xFFFFFFFFu; const uint8_t t[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)}; o.put(t, 4); bsz += 4; o.close(); }
        { const std::string we = writeWithPayload(in, L, bodyP, bsz, out + ".part"); fs::remove(bodyP, ec); if (!we.empty()) { fs::remove(out + ".part", ec); printf("%s: %s\n", src.c_str(), we.c_str()); return 2; } }   // FU16-3: removed first, as in add / extract
        std::string why; { laess::In nw(out + ".part"); Layout N; why = readLayout(nw, N);
            if (why.empty() && shaOf(in, {{L.audioOff, L.audioLen}}) != shaOf(nw, {{N.audioOff, N.audioLen}})) why = "the audio bytes differ"; }
        if (why.empty()) why = checkStored(out + ".part", threads);   // every master decoded and checked against its SHA-256
        if (!why.empty()) { fs::remove(out + ".part", ec); printf("self-check failed: %s; nothing written\n", why.c_str()); return 2; }
        fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s\n", out.c_str()); return 2; }
        printf("%s: %s form, payload version %d, %zu master(s); layers copied byte for byte, every master's SHA-256 checked\n", out.c_str(), L.flac ? "FLAC" : "WAV", ver, rs.size());
        { std::vector<MasterInfo> m1(rs.size()); for (size_t i = 0; i < rs.size(); i++) m1[i].menu = rs[i].menu; const std::vector<size_t> o1 = laesc::menuOrder(m1);
          for (size_t p = 0; p < o1.size(); p++) { const Rec& R = rs[o1[p]]; const std::string inFile = o1[p] != p ? " (master " + std::to_string(o1[p] + 1) + " in the file)" : std::string();
              printf("  master %zu%s%s%s%s\n", p + 1, R.name.empty() ? "" : (" \"" + R.name + "\"").c_str(), (R.flags & laes::kNoDerive) ? " (no derived versions)" : "", inFile.c_str(), H.def == (int)o1[p] + 1 ? "  [default]" : ""); }
          if (!H.def) printf("  default: the core master\n"); if (H.coreFlags & laes::kNoDerive) printf("  the core master: no derived 16-bit version\n"); }
        return 0; }
    catch (const std::exception& x) { std::error_code ec; if (madePart) { fs::remove(std::string(argv[3]) + ".part", ec); fs::remove(std::string(argv[3]) + ".body.part", ec); } printf("stopped: %s; nothing written\n", x.what()); return 2; }
    if (cmd == "enc" && argc >= 4) try {
        const std::string out = argv[2], corePath = arg(argc, argv, "--core"); const bool derive = flag(argc, argv, "--derive-core");
        // --timing: prints the time of each stage at the end (L5, 2026-11-03); the file written does not change
        const bool timing = flag(argc, argv, "--timing"); std::string tlog; auto t0 = std::chrono::steady_clock::now();
        auto stage = [&](const char* s) { const auto t = std::chrono::steady_clock::now(); char b[128]; snprintf(b, sizeof b, "  %-22s %8.2f s   peak memory so far %7.0f MB\n", s, std::chrono::duration<double>(t - t0).count(), peakMemory() / 1048576.0); tlog += b; t0 = t; };
        std::vector<std::string> paths; std::vector<double> Ts; std::vector<bool> autoT; std::vector<laess::WavInfo> wis;
        for (int i = 3; i < argc; i++) { if (argv[i][0] == '-') continue; std::string a; double T = 0; bool au = false;
            { const std::string me = masterArg(argv[i], true, a, T, au); if (!me.empty()) { printf("%s\n", me.c_str()); return 1; } }   // FU15-3
            laess::WavInfo wi; std::string e = laess::probeMaster(a, wi); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } longHeaderNote(a, wi.hdrLen, wi.dataBytes); paths.push_back(a); Ts.push_back(T); autoT.push_back(au); wis.push_back(wi); }
        if (paths.empty()) { printf("no master given\n"); return 1; }
        if (paths.size() > 255) { printf("at most 255 masters\n"); return 1; }
        { std::error_code ec; std::vector<std::string> ins(paths); if (!corePath.empty()) ins.push_back(corePath);   // FU16-7 (2026-11-14): <out> must not be an input
          if (fs::exists(out, ec)) for (const std::string& p : ins) if (fs::equivalent(out, p, ec)) { printf("%s is one of the inputs (%s): choose another output name; nothing written\n", out.c_str(), p.c_str()); return 1; } }
        // names and "no derived versions" (2026-11-11, payload version 2): --name=<k>:<text> (k = the k-th master as given, from 1; may repeat),
        // --no-derive (every master) or --no-derive=<k>. Names come only from here: nothing is filled in from file names, paths or dates.
        std::vector<std::string> names(paths.size()); std::vector<int> mflags(paths.size(), 0);
        // the default master (2026-11-12; not given = the core master): --default=<k> (k = the k-th master as given) or
        // --default=core; the core master's derived 16-bit version: off with --no-derive=core or a bare --no-derive (every derived version)
        long defGiven = 0; int coreFlags = 0;
        for (int i = 3; i < argc; i++) { const std::string a = argv[i];
            if (a.rfind("--default=", 0) == 0) { const std::string v = a.substr(10); char* e = nullptr; const long k = v == "core" ? 0 : strtol(v.c_str(), &e, 10);
                if (v != "core" && (v.empty() || *e || k < 0 || k > (long)paths.size())) { printf("--default=<k> (k = 1 ... %zu, the masters in the order given) or --default=core\n", paths.size()); return 1; } defGiven = k; continue; }
            if (a == "--no-derive=core") { coreFlags |= laes::kNoDerive; continue; }
            if (a == "--no-derive") coreFlags |= laes::kNoDerive;
            if (a.rfind("--name=", 0) == 0) { const std::string v = a.substr(7); const size_t col = v.find(':'); char* e = nullptr; const long k = col == std::string::npos ? 0 : strtol(v.c_str(), &e, 10);
                if (col == std::string::npos || e != v.c_str() + col || k < 1 || k > (long)paths.size()) { printf("--name=<k>:<text>, k = 1 ... %zu (the masters in the order given)\n", paths.size()); return 1; }
                const std::string t = v.substr(col + 1); if (!laes::validMasterName(t)) { printf("--name=%ld: a name is UTF-8 of at most 255 bytes without control characters (%zu bytes given)\n", k, t.size()); return 1; }
                names[(size_t)k - 1] = t; }
            else if (a == "--no-derive") { for (int& f : mflags) f |= laes::kNoDerive; }
            else if (a.rfind("--no-derive=", 0) == 0) { char* e = nullptr; const long k = strtol(a.c_str() + 12, &e, 10); if (*e || k < 1 || k > (long)paths.size()) { printf("--no-derive=<k>, k = 1 ... %zu\n", paths.size()); return 1; } mflags[(size_t)k - 1] |= laes::kNoDerive; } }
        // the menu order (2026-11-11: the order in the file stays the smallest one, the order people choose from is the one marked):
        // the menu order is the order the masters are given in (the order in the file is the
        // code's); --menu-order=<k1>,<k2>,... = another order (the k of the masters as given). When it equals the file order nothing is stored.
        std::vector<size_t> menuList; { const std::string mo = arg(argc, argv, "--menu-order");
            if (mo.empty()) for (size_t i = 0; i < paths.size(); i++) menuList.push_back(i + 1);   // the default (a bare --menu-order: the same)
            else { size_t a0 = 0; std::vector<bool> seen(paths.size() + 1, false);
                for (;;) { const size_t b0 = mo.find(',', a0); const std::string t = mo.substr(a0, b0 == std::string::npos ? std::string::npos : b0 - a0); char* e = nullptr; const long k = strtol(t.c_str(), &e, 10);
                    if (*e || k < 1 || k > (long)paths.size() || seen[(size_t)k]) { printf("--menu-order=<k1>,<k2>,...: every master as given (1 ... %zu) once\n", paths.size()); return 1; } seen[(size_t)k] = true; menuList.push_back((size_t)k);
                    if (b0 == std::string::npos) break; a0 = b0 + 1; }
                if (menuList.size() != paths.size()) { printf("--menu-order=<k1>,<k2>,...: every master as given (1 ... %zu) once\n", paths.size()); return 1; } } }
        std::vector<size_t> given(paths.size()); for (size_t i = 0; i < given.size(); i++) given[i] = i + 1;   // the k of --name for each master, through the reordering
        {   // file order: more bits first, then higher rate (a master can only refer to earlier ones; a shorter word refers to a longer one cheaply)
            std::vector<size_t> ord(paths.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return wis[a].bits != wis[b].bits ? wis[a].bits > wis[b].bits : wis[a].rate > wis[b].rate; });
            std::vector<std::string> p2, n2; std::vector<double> t2; std::vector<bool> a2; std::vector<laess::WavInfo> w2; std::vector<int> f2; std::vector<size_t> g2;
            for (size_t k = 0; k < ord.size(); k++) { p2.push_back(paths[ord[k]]); t2.push_back(Ts[ord[k]]); a2.push_back(autoT[ord[k]]); w2.push_back(wis[ord[k]]); n2.push_back(names[ord[k]]); f2.push_back(mflags[ord[k]]); g2.push_back(given[ord[k]]); }
            paths.swap(p2); Ts.swap(t2); autoT.swap(a2); wis.swap(w2); names.swap(n2); mflags.swap(f2); given.swap(g2); }
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
        for (const auto& x : gExactT) job += "T\t" + x.first + "\t" + std::to_string((long long)x.second) + "\n";   // DEC 15: T from extract
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
        if (!corePath.empty()) { unsigned long long hl = 0; long long hv = 0; std::string e = readWav(corePath, core, nullptr, &hl, &hv); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; } longHeaderNote(corePath, hl, hv); if (core.rate != base) { printf("the core master must be at %d Hz\n", base); return 2; } }
        else { bool dith; std::vector<laess::Prep*> pp; for (auto& m : pm) pp.push_back(&m); core32 = laess::deriveCore32(pp, base, coreBits, coreSrc, dith, 20261028); printf("core master: %d-bit base level of %s%s\n", coreBits, paths[coreSrc].c_str(), dith ? " (TPDF-dithered)" : ""); }
        for (size_t i = 0; i < paths.size(); i++) if (autoT[i]) {   // master.wav@auto: T from LAEY's search against the given core
            if (corePath.empty() || core.bits != 16) { printf("@auto needs --core=<cd.wav>, a 16-bit CD / DVD (a derived core is at T = 0)\n"); return 1; }
            if (wis[i].flac) { printf("%s: @auto needs the master as a WAV (LAEY's search reads WAV)\n", paths[i].c_str()); return 1; }
            const AutoT A = autoAlign(corePath, core, paths[i], pm[i], laeEncPath(argc, argv)); if (!A.found) { printf("%s: %s; nothing written\n", paths[i].c_str(), A.why.c_str()); return 2; }
            if (!acceptAlign(A, paths[i], flag(argc, argv, "--accept"))) { printf("alignment not accepted; nothing written\n"); return 2; } Ts[i] = A.T; }
        stage("core master, alignment");
        if (!corePath.empty()) core32 = laess::toCore32(core);   // from here on the core as int32 (memory, 2026-11-05: half of the int64 copy)
        std::vector<laess::MasterSel> sel(paths.size()); for (size_t i = 0; i < paths.size(); i++) { MasterIn t; setT(t, Ts[i]); sel[i].storage = -1;
            for (const auto& x : gExactT) if (x.first == paths[i]) { t.whole = x.second >> 32; t.frac = (uint32_t)(x.second & 0xFFFFFFFFLL); }   /* DEC 15: from extract, exact */ sel[i].whole = t.whole; sel[i].frac = t.frac; sel[i].name = names[i]; sel[i].flags = mflags[i]; }
        if (!menuList.empty()) for (size_t i = 0; i < paths.size(); i++) for (size_t p = 0; p < menuList.size(); p++) if (menuList[p] == given[i]) sel[i].menu = (int)p + 1;
        // file order, tried two ways, the smaller kept: (a) more bits first; (b) the master the core was derived from first, then the others more bits first.
        // More than 16 masters (2026-11-11): order (a) only.
        std::vector<std::vector<size_t>> orders(1, std::vector<size_t>(paths.size())); for (size_t i = 0; i < paths.size(); i++) orders[0][i] = i;
        if (coreSrc != SIZE_MAX && coreSrc != 0 && paths.size() <= 16) { std::vector<size_t> b(1, coreSrc); for (size_t i = 0; i < paths.size(); i++) if (i != coreSrc) b.push_back(i); orders.push_back(b); }
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
            { bool same = true; for (size_t k = 0; k < s2.size(); k++) same &= s2[k].menu == 0 || s2[k].menu == (int)k + 1; if (same) for (auto& s : s2) s.menu = 0; }   // the menu order is the file order: nothing to store
            std::vector<MasterInfo>& i2 = oinfo[oi]; const std::string bp = g.p + "/body" + std::to_string(oi), stO = g.p + "/o" + std::to_string(oi) + ".done";
            if (resumed) { std::string t; std::error_code ec; if (laess::getState(stO, t)) { laess::StateR r(t); const long long sz = r.i(); const std::string ts = r.str(); const int64_t n = r.i(); i2.assign((size_t)std::max<int64_t>(0, std::min<int64_t>(n, 255)), MasterInfo());
                  for (auto& I : i2) laess::getInfo(r, I);
                  if (!r.bad && ts == tsig && n == (int64_t)ord.size() && fs::exists(bp, ec) && sz == (long long)fs::file_size(bp, ec)) { osz[oi] = sz; continue; }   // made by an earlier run
                  // a stale o<oi>.done (other T, damaged, body gone): the whole order is made again. Without one the order was not
                  // finished: its masters' choice states stay, each checked (files' sizes, T) when encodeLaesS restores it.
                  fs::remove(stO, ec); for (size_t mi = 0; mi < ord.size(); mi++) fs::remove(g.p + "/o" + std::to_string(oi) + "_m" + std::to_string(mi) + ".done", ec); } }
            std::vector<std::string> kept;
            laes::Head hd; hd.coreFlags = coreFlags; if (defGiven) for (size_t k = 0; k < ord.size(); k++) if (given[ord[k]] == (size_t)defGiven) hd.def = (int)k + 1;   // the default in this file order
            const std::string err = laess::encodeLaesS(core32, base, m2, s2, i2, bp, g.p + "/o" + std::to_string(oi), asFlac ? 1 : 0, piece, threads, orders.size() > 1 ? &cache : nullptr, cacheLog, &kept, nullptr, hd);
            if (!err.empty()) { printf("%s\n", err.c_str()); return 2; } osz[oi] = (long long)fs::file_size(bp);
            { laess::StateW w; w.i(osz[oi]); w.str(tsig); w.i((int64_t)i2.size()); for (const auto& I : i2) laess::putInfo(w, I); laess::putState(stO, w.s); }
            for (const std::string& f : kept) { std::error_code ec; fs::remove(f, ec); } }   // the chosen layers, now in the payload
        for (size_t oi = 0; oi < orders.size(); oi++) if (osz[oi] >= 0 && (bodySize < 0 || osz[oi] < bodySize)) { bodyPath = g.p + "/body" + std::to_string(oi); bodySize = osz[oi]; info = oinfo[oi]; pick = orders[oi]; picked = (int)oi; }
        { laess::StateW w; w.i(picked); w.str(tsig); laess::putState(pickP, w.s); }   // before the other body goes
        for (size_t oi = 0; oi < orders.size(); oi++) { const std::string bp = g.p + "/body" + std::to_string(oi); if (bp != bodyPath) { std::error_code ec; fs::remove(bp, ec); } }
        laess::diskCheck(true);   // the largest size: before the work folder goes
        stage("candidates, payload");
        { std::vector<std::string> p2, n2; std::vector<laess::Prep> q2; std::vector<size_t> g2; std::vector<int> f2; for (size_t k : pick) { p2.push_back(paths[k]); q2.push_back(pm[k]); n2.push_back(names[k]); g2.push_back(given[k]); f2.push_back(mflags[k]); }
          paths.swap(p2); pm.swap(q2); names.swap(n2); given.swap(g2); mflags.swap(f2); }
        std::vector<int> menus(paths.size(), 0);   // the menu position of each master in the file order (all 0 when it is the file order)
        if (!menuList.empty()) { bool same = true; for (size_t k = 0; k < paths.size(); k++) { for (size_t p = 0; p < menuList.size(); p++) if (menuList[p] == given[k]) menus[k] = (int)p + 1; same &= menus[k] == (int)k + 1; } if (same) std::fill(menus.begin(), menus.end(), 0); }
        printf("file order:"); for (size_t k = 0; k < paths.size(); k++) printf(" %zu = %s;", k + 1, paths[k].c_str()); printf("%s\n", orders.size() > 1 ? " (the smaller of two orders)" : "");
        { bool any = false; for (size_t k = 0; k < paths.size(); k++) any |= !names[k].empty() || mflags[k] != 0 || given[k] != k + 1 || menus[k] != 0;
          if (any) for (size_t k = 0; k < paths.size(); k++) printf("  given %zu -> master %d in menus, %zu in the file%s%s\n", given[k], menus[k] ? menus[k] : (int)k + 1, k + 1, names[k].empty() ? "" : (" \"" + names[k] + "\"").c_str(),
                                                                    (mflags[k] & laes::kNoDerive) ? " (no derived versions)" : ""); }
        laes::Head headF; headF.coreFlags = coreFlags; for (size_t k = 0; k < paths.size(); k++) if (defGiven && given[k] == (size_t)defGiven) headF.def = (int)k + 1;   // in the file order kept
        if (headF.def) printf("default: given %ld -> master %d in menus%s\n", defGiven, menus[(size_t)headF.def - 1] ? menus[(size_t)headF.def - 1] : headF.def, names[(size_t)headF.def - 1].empty() ? "" : (" \"" + names[(size_t)headF.def - 1] + "\"").c_str());
        else printf("default: the core master\n");
        if (coreFlags & laes::kNoDerive) printf("  the core master: no derived 16-bit version\n");
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
        { std::error_code ec; fs::rename(out + ".part", out, ec); if (ec) { printf("cannot write %s (the new file stays as %s.part)\n", out.c_str(), out.c_str()); return 2; } }   // FU16-7: replaces <out> in one step; a failure keeps the old <out>
        double secs = (double)coreFrames / base; printf("%s: %s form, core master %d Hz %d-bit, %zu master(s), LAES block %.1f kb/s (file %.1f MB, %.1f kb/s)\n", out.c_str(), asFlac ? "FLAC" : "WAV", base, core32.bits, pm.size(),
                                                         bodySize * 8 / secs / 1000, (double)fs::file_size(out) / 1e6, (double)fs::file_size(out) * 8 / secs / 1000);
        for (size_t i = 0; i < info.size(); i++) { const std::string tier = info[i].refTier == 0 ? "the core master" : "master " + std::to_string(info[i].refTier);
            const std::string how = info[i].storage == 1 ? "full" : info[i].storage == 0 ? "difference to " + tier + " at the base level"
                                  : "difference to " + tier + " at " + std::to_string(base << std::min(info[i].stages, info[info[i].refTier - 1].stages)) + " Hz";
            const std::string nmq = i < names.size() && !names[i].empty() ? " \"" + names[i] + "\"" : std::string();
            printf("  master %zu%s: %d Hz %d-bit, %s, layers %.1f kb/s\n", i + 1, nmq.c_str(), info[i].rate, info[i].bits, how.c_str(), info[i].layerBits / secs / 1000); }
        { std::vector<MasterInfo> vi(info); for (size_t i = 0; i < vi.size() && i < names.size(); i++) { vi[i].name = names[i]; vi[i].flags = mflags[i]; } printf("  payload version %d\n", laes::payloadVersionFor(vi, headF)); }
        if (timing) printf("timing:\n%s", tlog.c_str());
        if (timing) printf("  work folder, largest   %8.1f MB\n", dg.peak / 1048576.0);
        return 0; }
    catch (const std::exception& x) { std::error_code ec; fs::remove(std::string(argv[2]) + ".part", ec); printf("stopped: %s; nothing written\n", x.what());
        if (fs::exists(std::string(argv[2]) + ".laes.tmp/job.txt", ec)) { printf("the work folder %s.laes.tmp is kept: run the same command again to continue (--disk-stop-remove: removed instead)\n", argv[2]);
            printf("%s%s%s\n", u8"  \u5de5\u4f5c\u6587\u4ef6\u5939 ", argv[2], u8".laes.tmp \u5df2\u4fdd\u7559\uff1a\u518d\u8fd0\u884c\u540c\u4e00\u6761\u547d\u4ee4\uff0c\u5c31\u4ece\u505c\u4e0b\u7684\u5730\u65b9\u63a5\u7740\u505a\uff08\u52a0 --disk-stop-remove \u65f6\u6539\u4e3a\u5220\u9664\uff09"); }
        return 2; }
    if ((cmd == "dec" || cmd == "verify") && argc >= 4) try {
        if (cmd == "dec" && argv[3][0] == '-' && strncmp(argv[3], "--list", 6) != 0) {   // FU16-9 (2026-11-14): an option is not taken as the output folder
            printf("give the output folder: laes dec <in> <out dir> [--levels] [--pick=...] (or laes dec <in> --list)\n"); return 1; }
        Gone g{workDir(std::string(argv[2]) + (cmd == "dec" ? ".dec" : ".verify"))}; const std::string bodyPath = g.p + "/body.bin"; Pcm core; long long bodySize = 0;
        { laess::In in(argv[2]); uint8_t mg[4] = {0}; const long long s0 = (long long)laesflac::flacStart([&](uint64_t at, void* d, size_t k) { in.read((long long)at, d, k); }, (uint64_t)in.n);   // DEC 19: ID3v2 in front
          if (in.n >= s0 + 4) in.read(s0, mg, 4); if (s0 && memcmp(mg, "fLaC", 4)) { printf("%s: no FLAC stream after the ID3v2 tag\n", argv[2]); return 2; }
          if (!memcmp(mg, "fLaC", 4)) {   // metadata: STREAMINFO kept, the "LAE2" APPLICATION blocks into the payload file; the audio through the FLAC reader
              long long p = s0 + 4; bool last = false; std::vector<uint8_t> meta = {'f', 'L', 'a', 'C'}, b; laess::Out bo(bodyPath);
              while (!last) { if (p + 4 > in.n) { printf("%s: metadata truncated\n", argv[2]); return 2; } uint8_t bh[4]; in.read(p, bh, 4); last = bh[0] & 0x80; const int type = bh[0] & 0x7F; const long long len = bh[1] << 16 | bh[2] << 8 | bh[3];
                  if (p + 4 + len > in.n) { printf("%s: metadata truncated\n", argv[2]); return 2; }
                  if (type == 0 || (type != 2)) { b.resize((size_t)len + 4); in.read(p, b.data(), b.size()); if (type == 0) meta.insert(meta.end(), b.begin(), b.end()); }
                  else { b.resize((size_t)len); in.read(p + 4, b.data(), b.size()); if (len >= 4 && !memcmp(b.data(), "LAE2", 4)) { bo.put(b.data() + 4, b.size() - 4); bodySize += len - 4; } }
                  p += 4 + len; }
              bo.close(); if (meta.size() > 4) meta[4] |= 0x80; std::vector<uint8_t> rest((size_t)(in.n - p)); in.read(p, rest.data(), rest.size()); meta.insert(meta.end(), rest.begin(), rest.end());
              laesflac::Info fi; bool md5ok; std::vector<uint8_t> app; std::string e = laesflac::readFlac(meta, fi, core.c, app, md5ok);
              if (!e.empty()) { printf("%s: %s\n", argv[2], e.c_str()); return 2; } core.rate = fi.rate; core.bits = fi.bps;
              { const uint64_t tl = laesflac::trailerLen([&](uint64_t at, void* d, size_t k) { in.read((long long)at, d, k); }, (uint64_t)p, (uint64_t)in.n); const std::string fn = laesflac::foreignNote((uint64_t)s0, tl); if (!fn.empty()) printf("note: %s\n", fn.c_str()); }   // DEC 19
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
        {   // FU16-8 (b) (2026-11-14): the temporary files of the decoding (every master: its samples and its rate levels as int64, and a
            // reference difference for a while: about 32 bytes per frame; 24 left at the end of a test run) against the free space of the work folder's disk; short: said, the run goes on
            laess::In hin(argv[2]); laesp::FnSrc hs; hs.n = (uint64_t)hin.n; hs.fn = [&](uint64_t at, void* d, size_t k) { hin.read((long long)at, d, k); return 0; };
            laesp::File hf; char hm[5]; std::string he; try { he = hf.open(hs, hm, true); } catch (const std::exception&) { he = "?"; }
            if (he.empty() && hf.why.empty()) { double need = 0; for (const auto& M : hf.ms) need += 32.0 * (double)M.I.frames;
                std::error_code ec; const fs::space_info sp = fs::space(fs::absolute(fs::path(g.p), ec), ec);
                if (!ec && need > (double)sp.available) {
                    printf("note: the decoding needs about %.1f GB of temporary files in %s, %.1f GB free on that disk: it goes on, and stops if the disk is full\n", need / 1073741824.0, g.p.c_str(), sp.available / 1073741824.0);
                    printf("%s%.1f%s%s%s%.1f%s\n", u8"  注意：解码大约需要 ", need / 1073741824.0, u8" GB 临时文件，放在 ", g.p.c_str(), u8"，这块磁盘剩 ", sp.available / 1073741824.0, u8" GB；照样开始，磁盘满时会停下"); } } }
        const int64_t piece = pieceFor(maxSt); std::vector<laess::DecMaster> dec; laes::Head head; std::string de = laess::decodeLaesS(bodyPath, core, core.rate, g.p, dec, piece, threads, &head);
        if (!de.empty()) { printf("%s: %s (the core master is still playable)\n", argv[2], de.c_str()); return 2; }
        bool all = true; { std::vector<MasterInfo> mi0; for (auto& D : dec) mi0.push_back(D.I); const std::vector<size_t> mo = laesc::menuOrder(mi0);   // in the menu order, numbered as in menus
          for (size_t p = 0; p < mo.size(); p++) { const size_t i = mo[p]; printf("master %zu%s: %d Hz %d-bit, %lld frames, SHA-256 %s%s\n", p + 1, dec[i].I.name.empty() ? "" : (" \"" + dec[i].I.name + "\"").c_str(), dec[i].I.rate, dec[i].I.bits,
                                                     (long long)dec[i].I.frames, dec[i].shaOk ? "ok" : "MISMATCH", mo[p] != p ? (" (master " + std::to_string(i + 1) + " in the file)").c_str() : ""); all &= dec[i].shaOk; }
          if (head.def) { const MasterInfo& D = dec[(size_t)head.def - 1].I; printf("default: master %d%s (set when encoded)\n", laesc::displayNo(mi0, (size_t)head.def - 1), D.name.empty() ? "" : (" \"" + D.name + "\"").c_str()); }
          else printf("default: the core master\n");
          if (head.coreFlags & laes::kNoDerive) printf("the core master: no derived 16-bit version\n"); }
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
            // every choice the file offers (laes_choices.h, SPEC 10; the same list as the plug-ins): the core master, then per master its
            // own group - the master, its rate levels (lossless), its derived 16-bit versions (unless the file turns them off for it)
            struct Choice { std::string name, what; Src s; bool derived = false; int from = -1; int kind = 0; int no = 0; };   // no: the master's number in the menu order
            std::vector<MasterInfo> infos; std::vector<bool> offered; for (size_t i = 0; i < dec.size(); i++) { infos.push_back(dec[i].I); offered.push_back(dec[i].shaOk);
                if (!dec[i].shaOk) printf("master %zu: SHA-256 mismatch: not offered (nor its rate levels or derived versions)\n", i + 1); }
            const std::vector<laesc::Spec> specs = laesc::build(core.rate, core.bits, core.frames(), infos, &offered, &head); const int defJ = laesc::defaultIndex(specs, head);
            std::vector<Choice> ch; for (const laesc::Spec& sp : specs) { Choice c; c.name = sp.name; c.what = sp.what; c.kind = sp.kind;
                c.no = sp.no; c.s.kind = sp.kind == 3 ? 0 : sp.kind; c.s.rate = sp.rate; c.s.bits = sp.bits; c.s.n = sp.frames; if (sp.m >= 0) c.s.m = (size_t)sp.m; c.s.k = sp.level;
                if (sp.kind == 3) { c.derived = true; c.from = sp.from; c.s = ch[(size_t)sp.from].s; c.s.bits = 16; } ch.push_back(c); }
            const bool listAll = arg(argc, argv, "--list") == "all";
            if (flag(argc, argv, "--list") || listAll) { printf("choices (use --pick=<n>[,<n>...], --pick=<rate>:<bits> (masters only), --pick=master:<k> or --pick=name:<text>):\n");
                // --list: the first level only (the masters with their whole description, nothing derived; spec/lossless/SPEC.md, Section 10.3): the core master
                // and each master - number, rate, word length, name, "lossless", length, the default marked. --list=all: also each master's
                // rate levels and derived versions under it, marked "-". The numbers are those of --pick either way.
                int hidden = 0;
                for (size_t j = 0; j < ch.size(); j++) { if (!listAll && ch[j].kind >= 2) { hidden++; continue; } std::string extra; if (ch[j].kind <= 1) { const int64_t sec = ch[j].s.rate ? ch[j].s.n / ch[j].s.rate : 0; char t[48]; snprintf(t, sizeof t, "  %lld:%02lld", (long long)(sec / 60), (long long)(sec % 60)); extra = t; if ((int)j == defJ) extra += "  [default]"; }
                    printf("  %3zu  %6d Hz %2d-bit  %s%s%s\n", j + 1, ch[j].s.rate, ch[j].s.bits, ch[j].kind >= 2 ? "  - " : "", ch[j].what.c_str(), extra.c_str()); }
                if (hidden) printf("(%d rate levels and derived versions not shown: --list=all)\n", hidden);
                return all ? 0 : 2; }
            std::vector<size_t> pick; const std::string pk = arg(argc, argv, "--pick");
            if (pk.empty()) { pick.push_back(0); for (size_t j = 1; j < ch.size(); j++) if (ch[j].kind == 1 || (ch[j].kind == 2 && flag(argc, argv, "--levels"))) pick.push_back(j); }
            else { size_t a = 0; for (;;) { const size_t b = pk.find(',', a); const std::string t = pk.substr(a, b == std::string::npos ? std::string::npos : b - a); const size_t col = t.find(':');
                    // <n>: the n-th line of --list. <rate>:<bits>: the master (never a rate level or a derived version) with that rate and word
                    // length; more than one: refused, with the candidates. master:<k>: master k. name:<text>: the master with exactly this name.
                    std::vector<size_t> hit;
                    if (col == std::string::npos) { std::vector<long> v; if (laespick::parseRanges(t, v)) { bool okR = true; for (long j : v) okR = okR && j >= 1 && j <= (long)ch.size(); if (okR) for (long j : v) hit.push_back((size_t)j - 1); } }   // <n> or a range <a>-<b>
                    else if (t.rfind("master:", 0) == 0) { char* e = nullptr; const long k = strtol(t.c_str() + 7, &e, 10); for (size_t j = 0; j < ch.size(); j++) if (!*e && ch[j].kind == 1 && (long)ch[j].no == k) hit.push_back(j); }
                    else if (t.rfind("name:", 0) == 0) { const std::string nm = t.substr(5); for (size_t j = 0; j < ch.size(); j++) if (ch[j].kind == 1 && !nm.empty() && infos[ch[j].s.m].name == nm) hit.push_back(j); }
                    else { const int r = atoi(t.c_str()), bb = atoi(t.c_str() + col + 1); for (size_t j = 0; j < ch.size(); j++) if (ch[j].kind == 1 && ch[j].s.rate == r && ch[j].s.bits == bb) hit.push_back(j); }
                    if (hit.empty()) { printf("--pick: '%s' is not one of the choices (see --list)\n", t.c_str()); return 2; }
                    // several masters fit (the same rate and word length, or the same name): the user ticks the ones to write;
                    // in a console a tick list (laes_pick.h), else the candidates are listed and nothing is written
                    if (hit.size() > 1 && col != std::string::npos) {
                        std::vector<laespick::Item> it; for (size_t j : hit) { char lb[48]; snprintf(lb, sizeof lb, "%6d Hz %2d-bit  ", ch[j].s.rate, ch[j].s.bits); it.push_back({(long)j + 1, lb + laesc::masterLabel((size_t)ch[j].no, infos[ch[j].s.m])}); }
                        char tl[256]; snprintf(tl, sizeof tl, "--pick: '%s' fits %zu masters. Tick the ones to write:", t.c_str(), hit.size());
                        if (!laespick::interactive()) { printf("%s\n", tl); for (const auto& x : it) printf("  %4ld  %s\n", x.no, x.line.c_str());
                            printf("Not in a console: give the numbers with --pick=<n>, several --pick=<n>,<n>, a range --pick=<a>-<b>. Nothing written.\n"); return 1; }
                        const std::vector<long> got = laespick::tick(tl, it, "e.g. " + std::to_string(it.front().no) + "," + std::to_string(it.back().no) + " or " + std::to_string(it.front().no) + "-" + std::to_string(it.back().no));
                        if (got.empty()) return 1; hit.clear(); for (long x : got) hit.push_back((size_t)x - 1); }
                    for (size_t j : hit) if (std::find(pick.begin(), pick.end(), j) == pick.end()) pick.push_back(j); if (b == std::string::npos) break; a = b + 1; } }
            fs::create_directories(argv[3]); const fs::path d = fs::path(argv[3]);   // FU16-6: joined with /, a "\\" was part of the name on Linux
            {   // FU16-5 (2026-11-14): the WAVs' size against the free space, before anything is written (as enc does for its work folder)
                uint64_t need = 0; for (size_t j : pick) { const uint64_t db = (uint64_t)ch[j].s.n * 2 * (uint64_t)(ch[j].s.bits / 8); need += 128 + db + (db & 1); }
                std::error_code ec; const fs::space_info sp = fs::space(fs::absolute(fs::path(argv[3]), ec), ec);
                if (!ec && need > sp.available) { const bool force = flag(argc, argv, "--force-disk");
                    printf("the WAVs need about %.1f GB, %.1f GB free on that disk%s\n", need / 1073741824.0, sp.available / 1073741824.0, force ? "; --force-disk: running anyway" : "; nothing written (--force-disk runs anyway)");
                    printf("%s%.1f%s%.1f%s\n", u8"  输出大约需要 ", need / 1073741824.0, u8" GB，这块磁盘剩 ", sp.available / 1073741824.0,
                           force ? u8" GB；--force-disk：照样开始" : u8" GB；没有开始，也没有写任何文件（加 --force-disk 可以照样开始）");
                    if (!force) return 2; } }
            bool wroteAll = true;
            for (size_t j : pick) { const Choice& C = ch[j]; const Src& S = C.derived ? ch[(size_t)C.from].s : C.s; const int by = C.s.bits / 8; const long long n = C.s.n, db = n * 2 * by;
                Pcm hdr; hdr.rate = C.s.rate; hdr.bits = C.s.bits; std::vector<uint8_t> h = wavBytes(hdr, nullptr); const uint32_t fmtLen = C.s.bits > 16 ? 40 : 16;
                for (int i = 0; i < 4; i++) { h[4 + i] = (uint8_t)((uint32_t)(4 + 8 + fmtLen + 8 + db + (db & 1)) >> (8 * i)); h[h.size() - 4 + i] = (uint8_t)((uint32_t)db >> (8 * i)); }
                // beyond 4 GB: RF64 as the encoder writes it (ds64 with the 64-bit RIFF and data sizes, no table; 32-bit fields 0xFFFFFFFF), 2026-11-04
                const long long riff = 4 + 8 + fmtLen + 8 + db + (db & 1);
                if (riff > 0xFFFFFFFFLL || getenv("LAES_FORCE_RF64")) { std::vector<uint8_t> r = {'R', 'F', '6', '4', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'd', 's', '6', '4', 28, 0, 0, 0};
                    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) r.push_back((uint8_t)(v >> (8 * i))); };
                    u64((uint64_t)(riff + 36)); u64((uint64_t)db); u64((uint64_t)n); r.insert(r.end(), {0, 0, 0, 0});
                    r.insert(r.end(), h.begin() + 12, h.end()); for (size_t i = r.size() - 4; i < r.size(); i++) r[i] = 0xFF; h.swap(r); }   // the data size: in ds64
                bool okw = true; try { laess::Out o((d / (C.name + ".wav")).string()); o.put(h.data(), h.size()); V x0, x1; std::vector<uint8_t> b; const long long P = 1 << 18;
                    uint64_t stc[2] = {20261029ull + 0x9E3779B97F4A7C15ull * 1, 20261029ull + 0x9E3779B97F4A7C15ull * 2}; const int sft = C.derived ? S.bits - 16 : 0;   // derive16: one stream per channel
                    for (long long a = 0; a < n; a += P) { const long long e = std::min(n, a + P); readSrc(S, 0, a, e, x0); readSrc(S, 1, a, e, x1); b.clear();
                        for (long long i = a; i < e; i++) for (int c = 0; c < 2; c++) { int64_t v = (c ? x1 : x0)[(size_t)(i - a)];
                            if (C.derived) { uint64_t& st = stc[c]; auto nx = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st >> (64 - sft); }; const int64_t r1 = (int64_t)nx(), r2 = (int64_t)nx();
                                v = std::max<int64_t>(-32768, std::min<int64_t>(32767, (v + r1 - r2 + (1LL << (sft - 1))) >> sft)); }
                            const uint32_t u = (uint32_t)(int32_t)v; for (int k = 0; k < by; k++) b.push_back((uint8_t)(u >> (8 * k))); }
                        o.put(b.data(), b.size()); }
                    if (db & 1) { const uint8_t z = 0; o.put(&z, 1); } o.close(); }
                catch (const std::exception& x) {   // FU16-5: said, and the part written is removed (the file is closed: o is gone)
                    okw = false; std::error_code ec; fs::remove((d / (C.name + ".wav")).string(), ec); printf("%s.wav: FAILED, the file was removed: %s\n", C.name.c_str(), x.what()); }
                all &= okw; wroteAll &= okw; if (okw) printf("%s.wav: %d Hz %d-bit, %s\n", C.name.c_str(), C.s.rate, C.s.bits, C.what.c_str()); }
            if (wroteAll) printf("written to %s\n", argv[3]); else printf("not every file was written: see above (%s)\n", argv[3]); }
        else { std::vector<bool> used(dec.size(), false);   // each given file must equal one decoded master (the file order may differ from the command line)
            for (int i = 3; i < argc; i++) { if (argv[i][0] == '-') continue; laess::WavInfo wi; std::string re; try { re = laess::probeWav(argv[i], wi); } catch (const std::exception& x) { re = x.what(); } int hit = -1;   // U14-a: a file that cannot be opened is reported, the other files are still compared
                for (size_t k = 0; k < dec.size() && hit < 0 && re.empty(); k++) { if (used[k] || wi.rate != dec[k].I.rate || wi.bits != dec[k].I.bits || wi.frames != dec[k].I.frames) continue;
                    Src s; s.kind = 1; s.m = k; laess::In in(argv[i]); bool same = true; std::vector<int32_t> r; V x0, x1; const long long P = 1 << 20;
                    for (long long a = 0; a < wi.frames && same; a += P) { const long long e = std::min<long long>(wi.frames, a + P); laess::wavFrames(in, wi, a, e, r); readSrc(s, 0, a, e, x0); readSrc(s, 1, a, e, x1);
                        for (long long q = 0; q < e - a && same; q++) same = r[(size_t)q * 2] == x0[(size_t)q] && r[(size_t)q * 2 + 1] == x1[(size_t)q]; }
                    if (same) hit = (int)k; }
                if (hit >= 0) { used[(size_t)hit] = true; printf("master %d vs %s: IDENTICAL\n", hit + 1, argv[i]); }
                else if (!re.empty()) {   // U14-a (2026-11-14): a file the reader refused is named with the reason (as the Java tool, U14-c)
                    const std::string pre = std::string(argv[i]) + ": "; printf("%s: DIFFERENT (not compared: %s)\n", argv[i], (re.rfind(pre, 0) == 0 ? re.substr(pre.size()) : re).c_str()); all = false; }
                else { printf("%s: DIFFERENT (no decoded master equals it)\n", argv[i]); all = false; } }
            for (size_t k = 0; k < dec.size(); k++) if (!used[k] && argc > 3) printf("master %zu: not compared\n", k + 1); }
        return all ? 0 : 2; }
    catch (const std::exception& x) { printf("stopped: %s\n", x.what()); return 2; }
    printf("bad arguments\n"); return 1; }
