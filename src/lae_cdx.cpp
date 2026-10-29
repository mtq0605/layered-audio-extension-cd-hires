// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE encapsulation tool (spec/cdx/SPEC.md): extension files, CD-Extra images, the reader and the closed-loop self-check.
//   lae_cdx pack <v5.wav> <out.lax> [track=N]          extension file of one track (section 3)
//   lae_cdx split <v5.wav> <base.wav> <ext.lax> [track=N]   base-layer WAV + extension file (F2, distribution as two files)
//   lae_cdx repack <base.wav> <ext.lax> <out.v5.wav>    base-layer file + extension file -> v5 WAV (F3 pairing, else section 6)
//   lae_cdx image <outdir> <t1.v5.wav> [t2.v5.wav ...] [noext=K[,K...]] [label=NAME] [date=YYYYMMDDhhmmss] [capacity=SECTORS] [verbose=1] [layout=<publisher.cue>]
//        CD-Extra image (sections 2, 5): AUDIO.BIN, IMAGE.CUE, DATA.ISO, SESSION1.CUE, BURN.TXT, LAE\TRACKnn.LAX; self-check first.
//        <outdir> must be new or empty. noext: these tracks go on the disc as CD only, without an extension file (D7, P18)
//        layout: independent-CD mode (L14, P8-P11): the publisher's INDEX 00/01/02+, ISRC, FLAGS and CATALOG; every CD layer must be the
//        publisher's track (INDEX 01 to the next INDEX 01; track 1 may begin with silent frames in the pregap); session 1 = the publisher's program
//   lae_cdx data <laxdir> <startLba> <out.iso> [label=NAME] [date=...]   session 2 at a reported start address (two-step burning)
//   lae_cdx read <outdir> <extensions> <audio...> [W=N]
//        the reader (section 6); <extensions> = a folder with .lax files, or iso:<DATA.ISO>@<startLba>;
//        <audio> = one .cue, or one audio file (no TOC), or several audio files (TOC from the file order)
//   lae_cdx sim <imagedir> <outdir> <form> [offset=N] [pregap=1] [overread=N] [change=K] [slip=K]
//        simulated rips for tests; form: disc, disc-nocue, tracks, tracks-nocue, singleK (the file of track K alone),
//        tracks-gapnext[-nocue] / tracks-gapomit[-nocue] (the INDEX 00 gaps of tracks 2... in the next file / left out)
//   lae_cdx selftest                                    known answers (C19)
//   lae_cdx test <fixturedir>                           section-7 cases on the test discs X, Y, S, T (in memory)
// Measured offsets are only printed (R16, P4); no file this tool writes contains a position, offset or shift.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include "wavio.h"
#include "lae_decoder.h"   // P2: every input v5 file must pass the decoder's own checks
#include "lae_cdx_read.h"
namespace fs = std::filesystem;
using namespace std;
using cdxr::Disc; using cdxr::Ext; using cdxr::Report; using cdxr::Track; using cdxr::u8;
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall GetACP(void);
#endif
static const long long SECTOR = cdxr::SECTOR, PREGAP = cdxr::PREGAP, WDEF = cdxr::WDEF, GAP2 = 11400, CAPDEF = 359849;

static bool writeBytes(const fs::path& p, const void* d, size_t n) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"wb");
#else
    FILE* f = fopen(p.c_str(), "wb");
#endif
    if (!f) return false; bool ok = n == 0 || fwrite(d, 1, n, f) == n; ok = (fclose(f) == 0) && ok;
    if (!ok) { std::error_code ec; fs::remove(p, ec); } return ok; }
static string msf(long long sectors) { char b[16]; snprintf(b, sizeof b, "%02lld:%02lld:%02lld", sectors / 4500, sectors / 75 % 60, sectors % 75); return b; }
static string arg(int argc, char** argv, const char* key, const string& def) { string k = string(key) + "="; for (int i = 1; i < argc; i++) if (!strncmp(argv[i], k.c_str(), k.size())) return argv[i] + k.size(); return def; }
static vector<string> positional(int argc, char** argv, int from) { vector<string> v; std::error_code ec; for (int i = from; i < argc; i++) if (!strchr(argv[i], '=') || fs::exists(argv[i], ec)) v.push_back(argv[i]); return v; }
static string nowUtc() { time_t t = time(nullptr); tm g; gmtime_s(&g, &t); char b[16]; strftime(b, sizeof b, "%Y%m%d%H%M%S", &g); return b; }

// ---- one track's v5 file (F1): CD layer + LAE chunk body
struct V5 { string name; int rate = 0; vector<int16_t> cd; vector<uint8_t> block; bool canonical = true; long long ncd() const { return (long long)cd.size() / 2; } };
static vector<uint8_t> canonV5(int rate, const vector<int16_t>& cd, const vector<uint8_t>& block) {   // = wavio::writeCD byte for byte (F1)
    size_t db = cd.size() * 2, extra = block.empty() ? 0 : 8 + block.size() + (block.size() & 1); vector<uint8_t> o; wavio::hdr(o, rate, 2, 16, db, extra);
    const uint8_t* c = (const uint8_t*)cd.data(); o.insert(o.end(), c, c + db); if (db & 1) o.push_back(0);
    if (!block.empty()) { o.insert(o.end(), {'L', 'A', 'E', ' '}); uint32_t n = (uint32_t)block.size(); for (int i = 0; i < 4; i++) o.push_back((uint8_t)(n >> (8 * i)));
        o.insert(o.end(), block.begin(), block.end()); if (n & 1) o.push_back(0); }
    return o; }
static vector<uint8_t> canonOf(const V5& v) { return canonV5(v.rate, v.cd, v.block); }
// reads and checks a v5 WAV: 16-bit stereo, LAE chunk, CD layer 44100 or 48000 Hz (P1), ncd < 2^28 (P7), every decoder check (P2).
// Only whole stereo frames are kept; canonical = the file is exactly the canonical layout of F1
static string loadV5(const fs::path& p, V5& v) {
    vector<uint8_t> b; if (!cdxr::readBytes(p, b)) return "cannot read the file";
    int rate = 0; vector<int16_t> pcm; vector<uint8_t> lae; string e = cdxr::parseWav(b, rate, pcm, &lae); if (!e.empty()) return e;
    if (lae.size() < 60) return "no LAE chunk (not an LAE v5 file)";
    if (rate != 44100 && rate != 48000) return "CD layer at " + to_string(rate) + " Hz: 44100 or 48000 expected (P1)";
    if (pcm.size() / 2 >= (1u << 28)) return "longer than 2^28 CD frames (P7)";
    { Decoder D; int code = 0; string de = D.openRef(pcm, lae, "full", 0, &code); if (!de.empty()) return "the decoder refuses this file (P2): " + de;
      if (D.base != rate) return "the LAE base rate " + to_string(D.base) + " Hz differs from the WAV sample rate " + to_string(rate) + " Hz (P2, X2)"; }   // the decoder itself does not compare them
    v.name = u8(p.filename()); v.rate = rate; v.cd.swap(pcm); v.block.swap(lae); v.canonical = canonOf(v) == b; return ""; }
static vector<uint8_t> laxOf(const V5& v, uint16_t track) {
    cdx::Trailer t = cdx::compute(v.cd.data(), (uint32_t)v.ncd(), (uint32_t)v.rate, track); vector<uint8_t> o = v.block; auto tr = cdx::serialize(t); o.insert(o.end(), tr.begin(), tr.end()); return o; }

// ---- the CD-Extra image (2.2, 2.3, 5.4)
struct Image { vector<V5> t; vector<long long> p, z; long long A2 = 0, N2 = 0, total = 0, cap = CAPDEF; vector<int16_t> audio; vector<vector<uint8_t>> lax; set<int> noext;
               vector<uint8_t> iso; uint32_t isoLba = 0; string label, date;
               vector<long long> idx1; bool pub = false; cdxr::Layout lay; };   // idx1: INDEX 01 (= p except track 1 in independent-CD mode); pub: independent-CD mode
static string readme() {
    return "LAE (Layered Audio Extension) / LAE 分层音频扩展\r\n\r\n"
           "This is a CD-Extra disc. Session 1 is an ordinary audio CD that plays in every CD player.\r\n"
           "The folder LAE holds the extension files. An LAE reader or an LAE player plugin combines the CD audio\r\n"
           "with these files and gives the high-resolution version of each track. Rip the audio tracks with any\r\n"
           "ripping program and any ripping settings, then give the rip and this folder to the LAE reader.\r\n\r\n"
           "这是一张 CD-Extra 光盘。会话 1 是普通的音乐 CD，任何 CD 机都能播放。\r\n"
           "LAE 文件夹里是扩展文件。LAE 读取器或 LAE 播放器插件把 CD 音频和这些文件合在一起，得到每一轨的高解析版本。\r\n"
           "用任何抓轨软件、任何设置翻录音频轨，再把翻录和这个文件夹交给 LAE 读取器即可。\r\n"; }
static vector<iso::File> dataFiles(const vector<vector<uint8_t>>& lax, const vector<int>& trackNo) {
    vector<iso::File> f; string rm = readme(); f.push_back({"README.TXT", vector<uint8_t>(rm.begin(), rm.end())});
    for (size_t k = 0; k < lax.size(); k++) { char nm[32]; snprintf(nm, sizeof nm, "LAE/TRACK%02d.LAX", trackNo[k]); f.push_back({nm, lax[k]}); } return f; }
static string finishImage(Image& im, vector<V5>& t, const string& label, const string& date, const set<int>& noext);
static string layoutImage(vector<V5> t, const string& label, const string& date, long long cap, Image& im, const set<int>& noext = {}) {
    if (t.empty() || t.size() > 98) return "1 to 98 audio tracks are needed (L2: the data track takes one of the 99 track numbers)";
    for (int k : noext) if (k < 1 || k > (int)t.size()) return "noext: track " + to_string(k) + " is not on the disc";
    if (noext.size() == t.size()) return "noext: every track would be CD only, nothing for the data session";
    for (size_t k = 0; k < t.size(); k++) {
        if (t[k].rate != 44100) return "track " + to_string(k + 1) + " (" + t[k].name + "): CD layer at " + to_string(t[k].rate) + " Hz; a CD carries only the 44.1 kHz family (L1, P1)";
        if (t[k].ncd() >= (1LL << 28)) return "track " + to_string(k + 1) + ": ncd >= 2^28 (P7)"; }
    im = Image(); im.label = label; im.date = date; im.cap = cap; im.noext = noext; long long at = 0;
    for (auto& v : t) { long long n = v.ncd(), z = (SECTOR - n % SECTOR) % SECTOR; im.p.push_back(at); im.z.push_back(z); at += n + z; }
    im.A2 = at / SECTOR; im.idx1 = im.p;
    for (size_t k = 0; k < t.size(); k++) { long long secs = (t[k].ncd() + im.z[k]) / SECTOR;
        if (secs < 300) return "track " + to_string(k + 1) + " is " + to_string(secs) + " sectors; every CD track needs at least 300 sectors = 4 s (L10)"; }
    im.audio.assign((size_t)(im.A2 * SECTOR) * 2, 0);
    for (size_t k = 0; k < t.size(); k++) memcpy(&im.audio[(size_t)im.p[k] * 2], t[k].cd.data(), (size_t)t[k].ncd() * 4);
    return finishImage(im, t, label, date, noext); }
// the common end of both layouts: extension files (data session) and the capacity check
static string finishImage(Image& im, vector<V5>& t, const string& label, const string& date, const set<int>& noext) {
    vector<vector<uint8_t>> lax; vector<int> no; im.lax.resize(t.size());
    for (size_t k = 0; k < t.size(); k++) { if (noext.count((int)k + 1)) continue; im.lax[k] = laxOf(t[k], (uint16_t)(k + 1)); lax.push_back(im.lax[k]); no.push_back((int)k + 1); }
    im.isoLba = (uint32_t)(im.A2 + GAP2); string err;
    im.iso = iso::build(dataFiles(lax, no), im.isoLba, label, date, 150, 300, &err); if (im.iso.empty()) return "data session: " + err;
    im.N2 = (long long)im.iso.size() / 2048; im.total = im.A2 + GAP2 + im.N2; im.t.swap(t);
    if (im.total > im.cap) return "does not fit (P18): " + to_string(im.total) + " sectors needed, " + to_string(im.cap) + " available. Options: leave the extension out of some tracks "
                                  "(noext=K,...), use a larger disc, or limit the octaves (needs re-encoding). Nothing is removed automatically.";
    return ""; }
// independent-CD mode (L14, P8-P11): the publisher's layout. Track k's CD layer must be the publisher's samples from INDEX 01 of track k to
// INDEX 01 of track k+1 (z_k = 0). Track 1's may begin with digital-silence frames that lie in the track-1 pregap: the planner adds one
// when the master's sound starts less than one CD sample before the CD's (the silence at the start of the disc absorbs
// that error); the pregap stays digital silence (L4, L5) and session 1 stays the publisher's program sample for sample (P11).
static string layoutPublisher(vector<V5> t, const Disc& pd, const cdxr::Layout& lay, const string& label, const string& date, long long cap, Image& im, const set<int>& noext = {}) {
    if (t.empty() || t.size() > 98) return "1 to 98 audio tracks are needed (L2)";
    if (t.size() != lay.tracks.size()) return to_string(t.size()) + " v5 files for the " + to_string(lay.tracks.size()) + " tracks of the publisher's disc";
    if (lay.tracks.empty() || lay.tracks[0].num != 1) return "the publisher's disc does not number its first audio track 01: not supported (the image numbers its tracks from 01)";
    for (int k : noext) if (k < 1 || k > (int)t.size()) return "noext: track " + to_string(k) + " is not on the disc";
    if (noext.size() == t.size()) return "noext: every track would be CD only, nothing for the data session";
    const size_t n = t.size(); vector<long long> P(n + 1, -1);
    for (size_t k = 0; k < n; k++) { const auto& lt = lay.tracks[k];
        if (lt.flags.find("PRE") != string::npos) return "track " + to_string(k + 1) + ": the publisher's disc has FLAGS PRE (pre-emphasis): not supported (L11, P10)";
        for (auto& ix : lt.idx) { if (ix.second % SECTOR) return "track " + to_string(k + 1) + ": an INDEX of the publisher's disc is not on the sector grid"; if (ix.first == 1) P[k] = ix.second; }
        if (t[k].rate != 44100) return "track " + to_string(k + 1) + ": CD layer at " + to_string(t[k].rate) + " Hz (L1, P1)";
        if (t[k].ncd() >= (1LL << 28)) return "track " + to_string(k + 1) + ": ncd >= 2^28 (P7)"; }
    P[n] = lay.end; if (P[0] != 0 || P[n] % SECTOR) return "the publisher's program must start at track 1 INDEX 01 and end on a sector boundary";
    for (long long a = pd.base; a < 0; a++) if (pd.L(a) || pd.R(a)) return "the publisher's track 1 INDEX 00 (pregap) has sound: not supported (L4, P10)";
    for (long long a = 0; a < P[n]; a++) if (!pd.has(a)) return "the publisher's program has an unknown sample at LBA " + to_string(a / SECTOR) + " (a PREGAP, POSTGAP or missing file): give the whole-disc CUE and BIN";
    im = Image(); im.pub = true; im.lay = lay; im.label = label; im.date = date; im.cap = cap; im.noext = noext;
    im.A2 = P[n] / SECTOR; im.audio.assign((size_t)P[n] * 2, 0);
    for (size_t k = 0; k < n; k++) { const long long span = P[k + 1] - P[k], L = t[k].ncd() - span;
        if (span < 300 * SECTOR) return "track " + to_string(k + 1) + " is " + to_string(span / SECTOR) + " sectors; every CD track needs at least 300 sectors = 4 s (L10)";
        if (k == 0 ? (L < 0 || L > PREGAP - WDEF) : L != 0) return "track " + to_string(k + 1) + ": its CD layer has " + to_string(t[k].ncd()) + " frames, the publisher's track " + to_string(span) + " (L14: INDEX 01 to the next INDEX 01)";
        for (long long j = 0; j < L; j++) if (t[k].cd[(size_t)j * 2] || t[k].cd[(size_t)j * 2 + 1]) return "track 1: the frames in front of the publisher's first sample are not digital silence (L4, L5)";
        for (long long j = 0; j < span; j++) { const int16_t l = t[k].cd[(size_t)(L + j) * 2], r = t[k].cd[(size_t)(L + j) * 2 + 1];
            if (l != pd.L(P[k] + j) || r != pd.R(P[k] + j)) return "track " + to_string(k + 1) + ": its CD layer differs from the publisher's track at frame " + to_string(j) + " (P11: the CD layer is the publisher's CD)";
            im.audio[(size_t)(P[k] + j) * 2] = l; im.audio[(size_t)(P[k] + j) * 2 + 1] = r; }
        im.p.push_back(P[k] - L); im.idx1.push_back(P[k]); im.z.push_back(0); }
    return finishImage(im, t, label, date, noext); }

static string cueText(const Image& im, bool withData) {
    string c = "REM SESSION 01\r\n"; if (im.pub && !im.lay.catalog.empty()) c += "CATALOG " + im.lay.catalog + "\r\n"; c += "FILE \"AUDIO.BIN\" BINARY\r\n";
    for (size_t k = 0; k < im.p.size(); k++) { char b[96];
        if (!im.pub) { snprintf(b, sizeof b, "  TRACK %02d AUDIO\r\n    INDEX 01 %s\r\n", (int)k + 1, msf(im.p[k] / SECTOR).c_str()); c += b; continue; }
        snprintf(b, sizeof b, "  TRACK %02d AUDIO\r\n", (int)k + 1); c += b; const auto& lt = im.lay.tracks[k];
        if (!lt.flags.empty()) c += "    FLAGS " + lt.flags + "\r\n"; if (!lt.isrc.empty()) c += "    ISRC " + lt.isrc + "\r\n";
        auto ix = lt.idx; sort(ix.begin(), ix.end());
        for (auto& i : ix) { if (k == 0 && i.first == 0) continue;   // the track-1 pregap is written by the burner (L4, P12)
            snprintf(b, sizeof b, "    INDEX %02d %s\r\n", i.first, msf(i.second / SECTOR).c_str()); c += b; } }
    c += "REM LEAD-OUT " + msf(im.A2) + "\r\n";
    if (withData) { char b[128]; snprintf(b, sizeof b, "REM SESSION 02\r\nFILE \"DATA.ISO\" BINARY\r\n  TRACK %02d MODE2/2048\r\n    INDEX 01 00:00:00\r\n", (int)im.p.size() + 1); c += b; }
    return c; }
static string burnText(const Image& im) {
    string b = "LAE CD-Extra: burning and acceptance / 刻录与验收\r\n\r\nLayout / 布局 (LBA = sector address; track 1 INDEX 01 = LBA 0):\r\n"; char x[240];
    for (size_t k = 0; k < im.p.size(); k++) { if (im.pub) for (auto& i : im.lay.tracks[k].idx) if (i.first == 0 && k > 0) { snprintf(x, sizeof x, "  track %02zu INDEX 00: LBA %lld (%s)\r\n", k + 1, i.second / SECTOR, msf(i.second / SECTOR).c_str()); b += x; }
        snprintf(x, sizeof x, "  track %02zu INDEX 01: LBA %lld (%s)%s\r\n", k + 1, im.idx1[k] / SECTOR, msf(im.idx1[k] / SECTOR).c_str(),
                                                  im.noext.count((int)k + 1) ? "  CD only, no extension file / 只作 CD" : ""); b += x; }
    snprintf(x, sizeof x, "  A2 (lead-out of session 1): LBA %lld (%s)\r\n  session 2 data track INDEX 01: LBA %u (= A2 + 11400, to be confirmed on the burner)\r\n"
                          "  session 2: %lld sectors; disc total %lld of %lld sectors\r\n\r\n", im.A2, msf(im.A2).c_str(), im.isoLba, im.N2, im.total, im.cap); b += x;
    b += "Route A (one pass): burn IMAGE.CUE with ImgBurn (write mode DAO/SAO). Before burning, compare the addresses ImgBurn shows with the table.\r\n"
         "Route B (two passes): burn SESSION1.CUE and leave the disc open (multisession). Read the next writable address N of the disc\r\n"
         "  (READ TRACK INFORMATION); it must equal A2 + 11400. Then run\r\n"
         "    lae_cdx data LAE <N> DATA_N.ISO\r\n"
         "  (LAE = the folder with the TRACKnn.LAX files) and write DATA_N.ISO as the second session (MODE2/2048).\r\n"
         "Acceptance: the disc plays in a CD player; a computer shows README.TXT and the LAE folder; rip the audio tracks with any program\r\n"
         "  and run lae_cdx read on the rip and the LAE folder of the disc: every track with an extension file must be verified.\r\n\r\n"
         "路线 A（一次写完）：用 ImgBurn 以 DAO/SAO 方式刻录 IMAGE.CUE。刻录前先把 ImgBurn 显示的地址和上表核对。\r\n"
         "路线 B（两步）：先刻 SESSION1.CUE，光盘保持开放（多会话）；读出下一可写地址 N（应等于 A2 + 11400），\r\n"
         "  运行上面的命令生成 DATA_N.ISO，作为第二个会话写入（MODE2/2048）。\r\n"
         "验收：CD 机能播放；电脑上能看到 README.TXT 和 LAE 文件夹；用任何软件翻录音频轨，再对翻录和光盘上的 LAE 文件夹运行\r\n"
         "  lae_cdx read，每一条带扩展文件的轨都必须验证通过。\r\n";
    return b; }

// ---- simulated discs and rips (tests and the self-check). Disc audio: LBA 0 .. A2; pregap and lead-out read as digital silence.
// A rip with combined offset o has at rip position y the disc sample y - o (2.2).
struct Model { vector<int16_t> a; long long n = 0; map<int, long long> p, gap; };   // gap: track -> samples from its INDEX 00 to its INDEX 01 (tracks 2...)
static void gapsOf(const cdxr::Layout& lay, Model& m) { for (auto& t : lay.tracks) { if (t.num < 2) continue; long long i0 = LLONG_MIN, i1 = LLONG_MIN;
        for (auto& i : t.idx) { if (i.first == 0) i0 = i.second; if (i.first == 1) i1 = i.second; } if (i0 > LLONG_MIN && i1 > i0) m.gap[t.num] = i1 - i0; } }
static Model modelOf(const Image& im) { Model m; m.a = im.audio; m.n = (long long)m.a.size() / 2; for (size_t k = 0; k < im.p.size(); k++) m.p[(int)k + 1] = im.idx1[k]; if (im.pub) gapsOf(im.lay, m); return m; }
static vector<int16_t> ripRange(const Model& m, long long y0, long long y1, const function<long long(long long)>& off) {
    vector<int16_t> r((size_t)max(0LL, y1 - y0) * 2, 0);
    for (long long y = y0; y < y1; y++) { long long a = y - off(y); if (a >= 0 && a < m.n) { r[(size_t)(y - y0) * 2] = m.a[(size_t)a * 2]; r[(size_t)(y - y0) * 2 + 1] = m.a[(size_t)a * 2 + 1]; } }
    return r; }
static function<long long(long long)> fixedOff(long long o) { return [o](long long) { return o; }; }
static long long trackEnd(const Model& m, int k) { auto it = m.p.find(k); ++it; return it == m.p.end() ? m.n : it->second; }
// whole-disc rip covering rip positions [y0, y1), written straight into the disc stream. toc: the disc's own TOC (a drive TOC, which
// the tool cannot read: the "simulated drive TOC" cases); otherwise the file alone (position 0 = first sample, no TOC)
static Disc wholeRip(const Model& m, long long y0, long long y1, const function<long long(long long)>& off, bool toc) {
    Disc d; d.toc = toc; d.rate = 44100; if (toc) d.p = m.p; const long long at = toc ? y0 : 0; cdxr::reserve(d, at, at + (y1 - y0));
    for (long long y = y0; y < y1; y++) { const size_t j = (size_t)(at + y - y0 - d.base); const long long a = y - off(y); d.st[j] = 1;
        if (a >= 0 && a < m.n) { d.s[j * 2] = m.a[(size_t)a * 2]; d.s[j * 2 + 1] = m.a[(size_t)a * 2 + 1]; } }
    return d; }
static Disc wholeRip(const Model& m, long long o, bool toc, long long before = PREGAP, long long after = 6000) { return wholeRip(m, -before, m.n + after, fixedOff(o), toc); }
struct FileSet { vector<int> track; vector<long long> at; vector<vector<int16_t>> pcm; };
static FileSet trackFiles(const Model& m, long long o, bool pregap1, long long after = 0) {
    FileSet f; for (auto& kv : m.p) { long long y0 = kv.first == m.p.begin()->first && pregap1 ? kv.second - PREGAP : kv.second, y1 = trackEnd(m, kv.first);
        if (kv.first == m.p.rbegin()->first) y1 += after; f.track.push_back(kv.first); f.at.push_back(y0); f.pcm.push_back(ripRange(m, y0, y1, fixedOff(o))); }
    return f; }
// files placed at their true disc positions with the disc's own TOC: what a drive TOC would give ("simulated drive TOC")
static Disc driveToc(const FileSet& f, const Model& m) {
    Disc d; d.toc = true; d.rate = 44100; d.p = m.p; for (size_t i = 0; i < f.pcm.size(); i++) cdxr::place(d, f.at[i], f.pcm[i].data(), (long long)f.pcm[i].size() / 2); return d; }
// the reader's own input paths for files without a CUE (the read command uses these too)
static Disc fromFiles(const vector<vector<int16_t>>& pcm, int rate, int firstTrack = 1) {   // several files: TOC from the order and lengths (R2)
    Disc d; d.toc = true; d.tocFromOrder = true; d.rate = rate; long long at = 0;
    for (size_t i = 0; i < pcm.size(); i++) { long long n = (long long)pcm[i].size() / 2; d.p[firstTrack + (int)i] = at; cdxr::place(d, at, pcm[i].data(), n); at += n; }
    return d; }
static Disc singleFile(const vector<int16_t>& pc, int rate = 44100) { Disc d; d.toc = false; d.rate = rate; cdxr::place(d, 0, pc.data(), (long long)pc.size() / 2); return d; }
// rips as files + CUE in memory, read through the reader's CUE code (the same CUE text the sim command writes)
// files: handed over to the reader (moved, read once); refs: borrowed (copied on reading)
struct MemRip { string cue; map<string, vector<int16_t>> files; map<string, const vector<int16_t>*> refs; };
static cdxr::PcmSource memSource(MemRip& r) {
    return [&r](const fs::path& p, const string&, vector<int16_t>& pcm, int& rate) -> string {
        const string n = u8(p.filename()); rate = 44100; auto it = r.files.find(n); if (it != r.files.end()) { pcm.swap(it->second); r.files.erase(it); return ""; }
        auto rf = r.refs.find(n); if (rf != r.refs.end()) { pcm = *rf->second; return ""; } return "cannot read " + u8(p); }; }
static Disc viaCue(MemRip& r, string* err = nullptr) { Disc d; string e = cdxr::loadCueText(r.cue, fs::path("rip"), d, memSource(r)); if (err) *err = e; return d; }
static Disc viaCue(MemRip&& r, string* err = nullptr) { return viaCue(r, err); }
static string discCue(const Model& m, bool pg) {
    const long long y0 = pg ? -PREGAP : 0; string cue = "FILE \"DISC.wav\" WAVE\r\n";
    for (auto& kv : m.p) { char b[120]; snprintf(b, sizeof b, "  TRACK %02d AUDIO\r\n%s    INDEX 01 %s\r\n", kv.first, kv.first == m.p.begin()->first && pg ? "    INDEX 00 00:00:00\r\n" : "", msf((kv.second - y0) / SECTOR).c_str()); cue += b; }
    return cue; }
static string tracksCue(const FileSet& f, bool pg) {
    string cue; for (size_t i = 0; i < f.track.size(); i++) { char b[200]; snprintf(b, sizeof b, "FILE \"TRACK%02d.wav\" WAVE\r\n  TRACK %02d AUDIO\r\n%s    INDEX 01 %s\r\n", f.track[i], f.track[i],
        i == 0 && pg ? "    INDEX 00 00:00:00\r\n" : "", i == 0 && pg ? "00:02:00" : "00:00:00"); cue += b; }
    return cue; }
static MemRip discRip(const Model& m, long long o, bool pg, long long over) { MemRip r; r.cue = discCue(m, pg); r.files["DISC.wav"] = ripRange(m, pg ? -PREGAP : 0, m.n + over, fixedOff(o)); return r; }
static MemRip tracksRip(const FileSet& f, bool pg) { MemRip r; r.cue = tracksCue(f, pg); for (size_t i = 0; i < f.track.size(); i++) { char n[32]; snprintf(n, sizeof n, "TRACK%02d.wav", f.track[i]); r.files[n] = f.pcm[i]; } return r; }
// per-track files with the INDEX 00 gaps of tracks 2... in the next track's file (mode 1) or left out (mode 2); the CUE a ripper writes for them
static FileSet trackFilesGap(const Model& m, int mode) {
    FileSet f; for (auto& kv : m.p) { const int k = kv.first; auto g = m.gap.find(k), nx = m.gap.find(k + 1);
        const long long gk = g == m.gap.end() ? 0 : g->second, gn = nx == m.gap.end() ? 0 : nx->second;
        const long long y0 = kv.second - (mode == 1 ? gk : 0), y1 = trackEnd(m, k) - gn; f.track.push_back(k); f.at.push_back(y0); f.pcm.push_back(ripRange(m, y0, y1, fixedOff(0))); }
    return f; }
static MemRip tracksRipGap(const FileSet& f, const Model& m, int mode) {
    MemRip r; for (size_t i = 0; i < f.track.size(); i++) { auto g = m.gap.find(f.track[i]); const long long gk = g == m.gap.end() ? 0 : g->second; char b[240], n[32];
        if (mode == 1 && gk) snprintf(b, sizeof b, "FILE \"TRACK%02d.wav\" WAVE\r\n  TRACK %02d AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 %s\r\n", f.track[i], f.track[i], msf(gk / SECTOR).c_str());
        else if (mode == 2 && gk) snprintf(b, sizeof b, "FILE \"TRACK%02d.wav\" WAVE\r\n  TRACK %02d AUDIO\r\n    PREGAP %s\r\n    INDEX 01 00:00:00\r\n", f.track[i], f.track[i], msf(gk / SECTOR).c_str());
        else snprintf(b, sizeof b, "FILE \"TRACK%02d.wav\" WAVE\r\n  TRACK %02d AUDIO\r\n    INDEX 01 00:00:00\r\n", f.track[i], f.track[i]);
        r.cue += b; snprintf(n, sizeof n, "TRACK%02d.wav", f.track[i]); r.files[n] = f.pcm[i]; }
    return r; }

// ---- checking a reading against the expected outcome. want: track -> "pass", "cd" (no extension file), or a reason prefix
static bool sameV5(const Track& r, const V5& v) { return r.state == Track::PASS && canonV5((int)r.ext->t.rate, r.cd, vector<uint8_t>(r.ext->bytes.begin(), r.ext->bytes.begin() + (ptrdiff_t)r.ext->blockLen)) == canonOf(v); }
static string describe(const Report& r, const Disc& d) {
    string s; for (auto& t : r.tracks) { char b[200];
        if (t.state == Track::PASS) snprintf(b, sizeof b, "%d:pass(%+lld) ", t.track, d.toc ? t.start - d.p.at(t.track) : t.start);
        else if (t.state == Track::NOEXT) snprintf(b, sizeof b, "%d:cd ", t.track); else snprintf(b, sizeof b, "%d:fail(%s) ", t.track, t.reason.c_str()); s += b; }
    for (auto& u : r.unused) s += "unused[" + u + "] "; for (auto& j : r.rejected) s += "rejected[" + j + "] "; if (r.otherDisc) s += "other-disc-hint "; if (d.conflicts) s += "conflicts=" + to_string(d.conflicts) + " ";
    return s; }
static bool matches(const Report& r, const vector<V5>& tracks, const map<int, string>& want, string& why) {
    set<int> seen; bool ok = true;
    for (auto& t : r.tracks) { seen.insert(t.track); auto w = want.find(t.track); if (w == want.end()) { if (t.state == Track::PASS) { ok = false; why += "track " + to_string(t.track) + " passed unexpectedly; "; } continue; }
        if (w->second == "pass") { if (t.track < 1 || t.track > (int)tracks.size() || !sameV5(t, tracks[(size_t)t.track - 1])) { ok = false; why += "track " + to_string(t.track) + " not rebuilt byte-identical; "; } }
        else if (w->second == "cd") { if (t.state != Track::NOEXT) { ok = false; why += "track " + to_string(t.track) + " not CD-only; "; } }
        else if (w->second == "fail") { if (t.state != Track::FAIL) { ok = false; why += "track " + to_string(t.track) + " should fall back; "; } }   // any reason
        else if (w->second == "pass or fail") { if (t.state == Track::PASS ? (t.track < 1 || t.track > (int)tracks.size() || !sameV5(t, tracks[(size_t)t.track - 1])) : t.state != Track::FAIL) { ok = false; why += "track " + to_string(t.track) + " neither rebuilt byte-identical nor fallen back; "; } }
        else if (t.state != Track::FAIL || t.reason.rfind(w->second, 0) != 0) { ok = false; why += "track " + to_string(t.track) + " expected '" + w->second + "'; "; } }
    for (auto& w : want) if (!seen.count(w.first)) { ok = false; why += "track " + to_string(w.first) + " missing; "; }
    return ok; }

// ---- closed-loop self-check (5.5, P16, P17): extension files from the image's data session; rips read through the reader's own
// CUE code (IMAGE.CUE itself, and the CUE a ripper writes), its file-order and single-file paths. The disc audio is borrowed from
// the image (moved, never copied) and every sample changed for P17 is put back.
static bool selfCheck(Image& im, int sabotage, bool verbose) {
    vector<Ext> exts; { string e = cdxr::loadExtIso(im.iso, im.isoLba, exts); if (!e.empty()) { printf("self-check: %s\n", e.c_str()); return false; } }
    if (exts.size() != im.t.size() - im.noext.size()) { printf("self-check: %zu extension files in the data session for %zu tracks\n", exts.size(), im.t.size() - im.noext.size()); return false; }
    struct Borrow { Image& im; Model m; Borrow(Image& i) : im(i) { m.a.swap(im.audio); m.n = (long long)m.a.size() / 2; for (size_t k = 0; k < im.p.size(); k++) m.p[(int)k + 1] = im.idx1[k]; if (im.pub) gapsOf(im.lay, m); }
                    ~Borrow() { im.audio.swap(m.a); } } bw(im);
    Model& m = bw.m; int runs = 0, bad = 0; bool sabotaged = false; map<int, string> all; for (size_t k = 1; k <= im.t.size(); k++) all[(int)k] = im.noext.count((int)k) ? "cd" : "pass";
    auto run = [&](const Disc& d, const vector<Ext>& x, const map<int, string>& want, const string& what, const string& loadErr = "") {
        Report r = cdxr::read(d, x, WDEF); runs++;
        if (sabotage == 2 && what.rfind("one sample changed", 0) == 0) for (auto& t : r.tracks) if (t.state == Track::FAIL) t.state = Track::PASS;   // C32 (b): reader made to accept a changed track
        string why = loadErr.empty() ? "" : "CUE: " + loadErr + "; "; bool ok = loadErr.empty() && matches(r, im.t, want, why);
        if (sabotage == 1 && !sabotaged) { ok = false; sabotaged = true; }             // C32 (a): the first positive reading made to fail
        if (!ok) bad++; if (verbose || !ok) printf("  self-check %-56s %s %s\n", what.c_str(), ok ? "ok" : "FAILED", ok ? "" : (why + describe(r, d)).c_str()); };
    // IMAGE.CUE and SESSION1.CUE as delivered, through the reader's CUE parser (P13, P16)
    {   string why; for (int withData = 1; withData >= 0; withData--) { cdxr::CueSheet c; string e = cdxr::parseCueText(cueText(im, withData != 0), fs::path("."), c);
            if (!e.empty()) { why += e + "; "; continue; } size_t n = im.t.size(), audio = 0;
            for (auto& t : c.tracks) { if (!t.audio) continue; audio++; if (t.num != (int)audio || t.idx1File != 0 || t.idx1 != im.idx1[audio - 1]) why += "track " + to_string(t.num) + " position; ";
                if (im.pub && audio <= im.lay.tracks.size()) { const auto& lt = im.lay.tracks[audio - 1]; auto want = lt.idx; if (audio == 1) want.erase(remove_if(want.begin(), want.end(), [](const pair<int, long long>& q) { return q.first == 0; }), want.end());
                    vector<pair<int, long long>> got; for (auto& ix : t.idx) got.push_back({ix.num, ix.pos}); sort(want.begin(), want.end()); sort(got.begin(), got.end());
                    if (got != want || t.isrc != lt.isrc || t.flags != lt.flags) why += "track " + to_string(t.num) + " INDEX / ISRC / FLAGS differ from the publisher's disc (L14); "; } }
            if (im.pub && c.catalog != im.lay.catalog) why += "CATALOG; ";
            if (audio != n) why += "audio track count; ";
            if (withData && (c.tracks.size() != n + 1 || c.tracks.back().audio || c.tracks.back().num != (int)n + 1 || c.tracks.back().mode != "MODE2/2048" || c.files.size() != 2 || c.files[1].name != "DATA.ISO")) why += "data track; ";
            if (!withData && c.tracks.size() != n) why += "SESSION1.CUE has more than the audio tracks; ";
            const string t = cueText(im, withData != 0); const size_t s1 = t.find("REM SESSION 01"), s2 = t.find("REM SESSION 02"), f1 = t.find("FILE \"AUDIO.BIN\""), f2 = t.find("FILE \"DATA.ISO\"");
            if (s1 != 0 || f1 == string::npos || f1 < s1 || (withData && (s2 == string::npos || f2 == string::npos || !(f1 < s2 && s2 < f2))) || (!withData && (s2 != string::npos || f2 != string::npos))) why += "session lines; ";
            if (t.find("REM LEAD-OUT " + msf(im.A2)) == string::npos) why += "lead-out; "; }
        runs++; if (!why.empty()) bad++; if (verbose || !why.empty()) printf("  self-check %-56s %s %s\n", "IMAGE.CUE / SESSION1.CUE read back", why.empty() ? "ok" : "FAILED", why.c_str()); }
    MemRip img; img.cue = cueText(im, true); img.refs["AUDIO.BIN"] = &m.a;
    { string e; Disc d = viaCue(img, &e); run(d, exts, all, "the image itself (AUDIO.BIN + IMAGE.CUE)", e); }
    const long long offs[] = {0, 1, 6, 48, 102, 588, 667, 1164, 1292, 1776, 2939, 4103, 4715, 5880};
    for (long long o0 : offs) for (int sg : {1, -1}) { if (!o0 && sg < 0) continue; string e; Disc d = viaCue(discRip(m, o0 * sg, true, 6000), &e);
        run(d, exts, all, "whole-disc rip + CUE, offset " + to_string(o0 * sg), e); }
    { map<int, string> w; for (auto& kv : all) if (kv.second != "cd") w.insert(kv); run(singleFile(ripRange(m, 0, m.n, fixedOff(0))), exts, w, "whole-disc rip in one file, no CUE, offset 0"); }   // no TOC: CD-only tracks are not listed
    for (int pg : {0, 1}) { FileSet f = trackFiles(m, 0, pg != 0); string e; Disc d = viaCue(tracksRip(f, pg != 0), &e);
        run(d, exts, all, string("per-track files + CUE") + (pg ? ", track 1 with pregap" : ""), e);
        run(fromFiles(f.pcm, 44100), exts, all, string("per-track files, file order only") + (pg ? ", track 1 with pregap" : "")); }
    if (im.pub && !m.gap.empty()) for (int mode : {1, 2}) {   // P16: the gaps in the next track's file, or left out; a gap left out without its digital silence (or without a CUE) is unknown to the reader
        FileSet f = trackFilesGap(m, mode); map<int, string> wc = all, wo = all;
        if (mode == 2) for (auto& g : m.gap) { const int prev = g.first - 1; if (!all.count(prev) || all[prev] == "cd") continue; bool silent = true;
            for (long long a = m.p.at(g.first) - g.second; a < m.p.at(g.first) && silent; a++) if (m.a[(size_t)a * 2] || m.a[(size_t)a * 2 + 1]) silent = false;
            if (!silent) wc[prev] = "unknown samples"; wo[prev] = "pass or fail"; }   // file order only: passes only if the window happens to be unchanged
        string e; Disc d = viaCue(tracksRipGap(f, m, mode), &e);
        run(d, exts, wc, mode == 1 ? "per-track files + CUE, gaps in the next file" : "per-track files + CUE, gaps left out (PREGAP)", e);
        run(fromFiles(f.pcm, 44100), exts, wo, mode == 1 ? "per-track files, file order only, gaps in the next file" : "per-track files, file order only, gaps left out"); }
    for (size_t k = 1; k <= im.t.size(); k++) { if (im.noext.count((int)k)) continue;   // P17: one changed sample in track k (changed in place, put back)
        const size_t a = (size_t)(im.p[k - 1] + im.t[k - 1].ncd() / 2) * 2; m.a[a] ^= 1;
        map<int, string> w = all; w[(int)k] = "fail"; string e; Disc d = viaCue(img, &e); m.a[a] ^= 1;   // P17: falls back (the reason is counted, R11)
        run(d, exts, w, "one sample changed in track " + to_string(k), e); }
    const int first = exts.front().t.track, last = exts.back().t.track;
    {   vector<Ext> x = exts; V5 other = im.t[(size_t)first - 1]; other.cd[other.cd.size() / 2] ^= 1;   // a foreign extension file (another CD layer) for the first track
        for (auto& e : x) if (e.t.track == first) { e.bytes = laxOf(other, (uint16_t)first); cdxr::check(e, true); }
        map<int, string> w = all; w[first] = "no match"; run(wholeRip(m, 0, true), x, w, "foreign extension file for track " + to_string(first)); }
    {   vector<Ext> x = exts; for (auto& e : x) if (e.t.track == last) { e.bytes.resize(e.bytes.size() - cdx::TRAILER); cdxr::check(e, true); }   // trailer removed
        map<int, string> w = all; w[last] = "cd"; run(wholeRip(m, 0, true), x, w, "trailer removed from track " + to_string(last) + "'s file"); }
    printf("self-check: %d runs, %s\n", runs, bad ? "FAILED: the image is void (P16/P17)" : "all as required"); return bad == 0; }

// ---- reader output (R19): verified tracks as v5 WAV, the others as 16-bit CD audio named .cd.wav, at the rate of the input
// without TOC: the parts of the file that belong to no verified track and are not all digital silence
static vector<pair<long long, long long>> leftovers(const Disc& d, const Report& r) {
    vector<pair<long long, long long>> cov, out; for (auto& t : r.tracks) if (t.state == Track::PASS) cov.push_back({t.start, t.start + (long long)t.ext->t.ncd}); sort(cov.begin(), cov.end());
    long long a = d.base; cov.push_back({d.end(), d.end()});
    for (auto& c : cov) { if (c.first > a) { bool zero = true; for (long long x = a; x < c.first && zero; x++) if (d.L(x) || d.R(x)) zero = false; if (!zero) out.push_back({a, c.first}); } a = max(a, c.second); }
    return out; }
static string writeOutputs(const fs::path& out, const Disc& d, const Report& r, const string& stem) {
    std::error_code ec; fs::create_directories(out, ec); char nm[300]; string e; const int rate = d.rate > 0 ? d.rate : 44100;
    auto cdFile = [&](const string& name, long long a, long long b) { vector<int16_t> pc; pc.reserve((size_t)max(0LL, b - a) * 2); for (long long x = a; x < b; x++) { pc.push_back(d.L(x)); pc.push_back(d.R(x)); }
        auto by = canonV5(rate, pc, {}); if (!writeBytes(out / fs::u8path(name), by.data(), by.size())) e = "cannot write " + name; };
    for (auto& t : r.tracks) {
        if (t.state == Track::PASS) { snprintf(nm, sizeof nm, "TRACK%02d.lae.wav", t.track); auto by = canonV5((int)t.ext->t.rate, t.cd, vector<uint8_t>(t.ext->bytes.begin(), t.ext->bytes.begin() + (ptrdiff_t)t.ext->blockLen));
            if (!writeBytes(out / nm, by.data(), by.size())) e = string("cannot write ") + nm; }
        else if (d.toc) {   // CD route: only the ripped samples (R11, R15): nothing for a track in a missing file, unknown edges trimmed
            if (d.missing.count(t.track)) { printf("track %02d: no CD output (its file is missing)\n", t.track); continue; }
            auto it = d.p.find(t.track); auto nx = next(it); long long a = it->second, b = nx == d.p.end() ? d.end() : nx->second;
            while (a < b && !d.has(a)) a++; while (b > a && !d.has(b - 1)) b--;
            if (a >= b) { printf("track %02d: no CD output (no ripped samples)\n", t.track); continue; }
            snprintf(nm, sizeof nm, "TRACK%02d.cd.wav", t.track); cdFile(nm, a, b); } }
    if (!d.toc) { bool none = true; for (auto& t : r.tracks) if (t.state == Track::PASS) none = false; int part = 0;
        if (none) cdFile(stem + ".cd.wav", d.base, d.end());   // no track verified: the whole file as CD, silent or not (R19)
        else for (auto& g : leftovers(d, r)) { snprintf(nm, sizeof nm, "%s.part%02d.cd.wav", stem.c_str(), ++part); cdFile(nm, g.first, g.second); } }
    return e; }
static void printReport(const Disc& d, const Report& r) {
    if (!d.toc) printf("no TOC: every extension file is searched for in the whole file by content (R2)\n");
    if (d.tocFromOrder) printf("*** TOC derived from the order and lengths of the files (R2): no CUE or drive TOC was given ***\n");
    for (auto& n : d.notes) printf("%s\n", n.c_str());
    if (d.conflicts) printf("conflicting samples between files: %lld (treated as unknown, R5)\n", d.conflicts);
    for (auto& j : r.rejected) printf("extension %s: not used\n", j.c_str());
    for (auto& t : r.tracks) {
        if (t.state == Track::PASS) printf("track %02d: verified (whole-track MD5 %s), found at offset %+lld samples%s\n", t.track, t.ext->t.md5hex().c_str(), d.toc ? t.start - d.p.at(t.track) : t.start, d.toc ? "" : " in the file");
        else if (t.state == Track::NOEXT) printf("track %02d: CD only (no extension file for this track)\n", t.track);
        else printf("track %02d: CD only (%s)\n", t.track, t.reason.c_str()); }
    for (auto& u : r.unused) printf("extension %s: unused\n", u.c_str());
    if (r.otherDisc) printf("no track verified: this may be another disc or another version (R12)\n"); }
// the read command's inputs: one .cue; or one audio file (no TOC); or several audio files (TOC from the file order), all of one rate
static string openInputs(const vector<string>& in, Disc& d) {
    if (in.size() == 1 && cdxr::extOf(fs::path(in[0])) == ".cue") return cdxr::loadCue(fs::path(in[0]), d);
    vector<vector<int16_t>> pcm(in.size()); int rate = 0;
    for (size_t i = 0; i < in.size(); i++) { int r = 0; fs::path p(in[i]); string e = cdxr::loadPcm(p, cdxr::extOf(p) == ".bin" ? "BINARY" : "WAVE", pcm[i], r, 0); if (!e.empty()) return e;
        if (i && r != rate) return "the files have different sample rates (" + to_string(rate) + " and " + to_string(r) + " Hz): one disc or stream at a time"; rate = r; }
    d = in.size() == 1 ? singleFile(pcm[0], rate) : fromFiles(pcm, rate); return ""; }

// ---- known answers (C19)
static int selftest() {
    int bad = 0; auto ck = [&](bool ok, const string& w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); if (!ok) bad++; };
    ck(alignhash::md5SelfTest(), "MD5: the 7 test strings of RFC 1321");
    { const char* s = "123456789"; ck(cdx::crc32((const uint8_t*)s, 9) == 0xCBF43926u, "CRC-32(\"123456789\") = CBF43926"); }
    struct Ex { vector<int16_t> s; uint64_t s1, s2; const char* md5; uint32_t l0, t0; vector<uint32_t> pre; uint64_t A0, B0; };
    vector<Ex> ex = {{{1, -1, -32768, 32767, 0, 0}, 6442352641ULL, 8589803521ULL, "08b7bb4b0c7432f1c9e94e3c493f236e", 0, 1, {0}, 6442352641ULL, 15032156162ULL},
                     {{1, -1, -32768, 32767, 32767, -32768}, 8589869056ULL, 15032352766ULL, "e5e4df0d69a7afa36b20e70c5c09fc82", 0, 0, {131071}, 6442483712ULL, 15032287233ULL},
                     {{0, 0, 0, 0, 0, 0}, 0, 0, nullptr, 3, 3, {}, 0, 0}};
    for (size_t i = 0; i < ex.size(); i++) { auto& e = ex[i]; cdx::Trailer t = cdx::compute(e.s.data(), 3, 44100, 1); string n = "example " + to_string(i + 1) + ": ";
        ck(t.s1 == e.s1 && t.s2 == e.s2, n + "S1, S2"); if (e.md5) ck(t.md5hex() == e.md5, n + "MD5"); ck(t.lead0 == e.l0 && t.tail0 == e.t0, n + "lead0, tail0");
        if (i < 2) { vector<uint32_t> x = e.pre; for (int j = 0; j < 3; j++) x.push_back(cdx::word(e.s[(size_t)j * 2], e.s[(size_t)j * 2 + 1])); if (i == 0) x.push_back(0);
            uint64_t A = 0, B = 0; for (uint32_t j = 0; j < 3; j++) { A = cdx::modM(A + x[j]); B = cdx::modM(B + cdx::modM((uint64_t)(j + 1) * x[j])); }
            ck(A == e.A0 && B == e.B0, n + "sliding A(0), B(0)"); vector<long long> hits;
            cdx::slide([&](long long j) { return x[(size_t)j]; }, (long long)x.size(), 3, 0, (long long)x.size() - 3, e.s1, e.s2, [&](long long t) { hits.push_back(t); });
            ck(hits.size() == 1 && hits[0] == 1, n + "sliding A(1) = S1, B(1) = S2"); } }
    {   vector<int16_t> s = {0, 0, 0, 5, 1, 1, 5, 0, 0, 0}; cdx::Trailer t = cdx::compute(s.data(), 5, 44100, 1);   // only frames with both samples 0 count
        ck(t.lead0 == 1 && t.tail0 == 1, "lead0, tail0 count frames whose two samples are both 0 (one channel 0 at an edge does not count)"); }
    {   uint64_t r = 0x9E3779B97F4A7C15ULL; auto rnd = [&]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return r; };
        vector<uint32_t> x(200000); for (auto& v : x) v = (uint32_t)rnd(); bool ok = true; const uint32_t n = 50000;
        for (int k = 0; k < 1000 && ok; k++) { long long t = (long long)(rnd() % (x.size() - n)); uint64_t A = 0, B = 0;
            for (uint32_t i = 0; i < n; i++) { A = cdx::modM(A + x[(size_t)t + i]); B = cdx::modM(B + cdx::modM((uint64_t)(i + 1) * x[(size_t)t + i])); }
            bool hit = false; cdx::slide([&](long long j) { return x[(size_t)j]; }, (long long)x.size(), n, max(0LL, t - 3), t, A, B, [&](long long h) { if (h == t) hit = true; }); ok = hit; }
        ck(ok, "1000 random offsets: sliding values = direct values (X5)"); }
    {   vector<int16_t> big(2 * 300000); uint64_t r = 7; for (auto& v : big) { r = r * 6364136223846793005ULL + 1442695040888963407ULL; v = (int16_t)(r >> 48); }
        cdx::Trailer t = cdx::compute(big.data(), 300000, 44100, 7); auto s = cdx::serialize(t); vector<uint8_t> f(100, 0xAB); memcpy(f.data(), "LAE1", 4); f.insert(f.end(), s.begin(), s.end()); cdx::Trailer u; size_t bl = 0;
        ck(cdx::parse(f, u, bl) == "" && bl == 100 && u.s1 == t.s1 && u.s2 == t.s2 && u.ncd == 300000 && u.track == 7 && u.md5hex() == t.md5hex(), "trailer serialize / parse round trip");
        bool zero = true; for (int i = 60; i < 76; i++) if (s[(size_t)i]) zero = false; ck(zero && s.size() == 80 && !memcmp(s.data(), "LTVT", 4), "trailer: 80 bytes, LTVT, reserved bytes 0");
        t.track = 120; s = cdx::serialize(t); f.assign(100, 0xAB); memcpy(f.data(), "LAE1", 4); f.insert(f.end(), s.begin(), s.end());
        ck(cdx::parse(f, u, bl, false) == "" && cdx::parse(f, u, bl, true) == "invalid trailer field", "track 120: valid outside CD, invalid on a CD (X2)");
        t.track = 1; t.rate = 48000; s = cdx::serialize(t); f.assign(100, 0xAB); memcpy(f.data(), "LAE1", 4); f.insert(f.end(), s.begin(), s.end());
        ck(cdx::parse(f, u, bl, false) == "" && cdx::parse(f, u, bl, true) == "invalid trailer field", "48000 Hz: valid outside CD, invalid on a CD (X2)"); }
    printf("selftest: %s\n", bad ? "FAILED" : "all passed"); return bad ? 1 : 0; }

// ---- section 7 cases on the test discs (in memory); every case states its expectation first
static int nCases = 0, nBad = 0;
static void verdict(const string& id, const string& what, bool ok, const string& detail) { nCases++; if (!ok) nBad++; printf("%s  %-6s %-78s %s\n", ok ? "PASS" : "FAIL", id.c_str(), what.c_str(), detail.c_str()); }
static string extsFrom(const Image& im, vector<Ext>& x) { x.clear(); return cdxr::loadExtIso(im.iso, im.isoLba, x); }
static void retrailer(Ext& e, const function<void(vector<uint8_t>&)>& f, bool cd = true) {   // edits the trailer and recomputes its CRC
    size_t o = e.bytes.size() - cdx::TRAILER; vector<uint8_t> tr(e.bytes.begin() + (ptrdiff_t)o, e.bytes.end()); f(tr); uint32_t c = cdx::crc32(tr.data(), 76); for (int i = 0; i < 4; i++) tr[76 + (size_t)i] = (uint8_t)(c >> (8 * i));
    memcpy(&e.bytes[o], tr.data(), cdx::TRAILER); cdxr::check(e, cd); }
static void caseRun(const string& id, const string& what, const Disc& d, const vector<Ext>& x, const Image& im, const map<int, string>& want, long long W = WDEF,
                    const function<bool(const Report&, string&)>& extra = nullptr, const string& loadErr = "") {
    Report r = cdxr::read(d, x, W); string why = loadErr.empty() ? "" : "CUE: " + loadErr + "; "; bool ok = loadErr.empty() && matches(r, im.t, want, why); if (extra) ok = extra(r, why) && ok;
    verdict(id, what, ok, ok ? describe(r, d) : why + "| " + describe(r, d)); }
static long long shown(const Report& r, const Disc& d, int k) { for (auto& t : r.tracks) if (t.track == k && t.state == Track::PASS) return d.toc ? t.start - d.p.at(k) : t.start; return LLONG_MIN; }
static int runTests(const string& dirArg) {
    const fs::path dir(dirArg);
    auto get = [&](const string& n, V5& v) { string e = loadV5(dir / (n + ".wav"), v); if (!e.empty()) { printf("fixture %s: %s\n", n.c_str(), e.c_str()); exit(2); } };
    V5 X1, X2, Y1, Y2, S1, S2, S3, T1, T2; get("X1", X1); get("X2", X2); get("Y1", Y1); get("Y2", Y2); get("S1", S1); get("S2", S2); get("S3", S3); get("T1", T1); get("T2", T2);
    const string date = "20261021000000"; Image X, Y, S, T, X1only; string e;
    auto mk = [&](vector<V5> v, Image& im, const char* n, const set<int>& noext = {}) { e = layoutImage(v, "LAE_EXT", date, CAPDEF, im, noext); if (!e.empty()) { printf("disc %s: %s\n", n, e.c_str()); exit(2); } };
    mk({X1, X2}, X, "X"); mk({Y1, Y2}, Y, "Y"); mk({S1, S2, S3}, S, "S"); mk({T1, T2}, T, "T"); mk({X1}, X1only, "X (track 1 only)");
    {   cdx::Trailer a = cdx::compute(T1.cd.data(), (uint32_t)T1.ncd(), 44100, 1), b = cdx::compute(T2.cd.data(), (uint32_t)T2.ncd(), 44100, 2); char s[240];
        snprintf(s, sizeof s, "X ncd %lld/%lld, S ncd %lld/%lld/%lld, T ncd %lld/%lld, T1 lead0 %u tail0 %u, T2 lead0 %u tail0 %u", X1.ncd(), X2.ncd(), S1.ncd(), S2.ncd(), S3.ncd(), T1.ncd(), T2.ncd(), a.lead0, a.tail0, b.lead0, b.tail0);
        verdict("FIX", "test discs as specified in 7.1", X1.ncd() == 440875 && X2.ncd() == 441000 && S1.ncd() == 220500 && S2.ncd() == 220500 && S3.ncd() == 220500 && T1.ncd() == 485100 && T2.ncd() == 485100
                && a.lead0 >= 30000 && a.tail0 == 0 && b.tail0 >= 30000 && b.lead0 == 0 && X.A2 == 1500 && S.A2 == 1125 && T.A2 == 1650, s); }
    Model mX = modelOf(X), mY = modelOf(Y), mS = modelOf(S), mT = modelOf(T), m1 = modelOf(X1only); vector<Ext> xX, xY, xS, xT, x1; extsFrom(X, xX); extsFrom(Y, xY); extsFrom(S, xS); extsFrom(T, xT); extsFrom(X1only, x1);
    map<int, string> P2 = {{1, "pass"}, {2, "pass"}}, P3 = {{1, "pass"}, {2, "pass"}, {3, "pass"}};
    // self-check of every test image (P16, P17)
    for (auto pr : vector<pair<Image*, string>>{{&X, "X"}, {&Y, "Y"}, {&S, "S"}, {&T, "T"}, {&X1only, "X track 1 alone"}}) verdict("P16", "self-check of disc " + pr.second, selfCheck(*pr.first, 0, false), "");
    // C01: the image read through its own IMAGE.CUE
    { MemRip r; r.cue = cueText(X, true); r.files["AUDIO.BIN"] = X.audio; string le; Disc d = viaCue(r, &le);
      caseRun("C01", "disc X, offset 0, AUDIO.BIN + IMAGE.CUE (reader's CUE code)", d, xX, X, P2, WDEF, [&](const Report& rr, string& why) {
        bool ok = true; for (size_t k = 0; k < X.t.size(); k++) { alignhash::Md5 m; m.update(&X.audio[(size_t)X.p[k] * 2], (size_t)X.t[k].ncd() * 4); cdx::Trailer t; size_t bl = 0; cdx::parse(X.lax[k], t, bl, true);
                alignhash::Md5 m2; m2.update(X.t[k].cd.data(), X.t[k].cd.size() * 2); string h = m.hex(); if (h != t.md5hex() || h != m2.hex()) ok = false;
                if (!X.t[k].canonical) { ok = false; why += "encoder output is not the canonical v5 layout; "; } }
            if (X.p[0] != 0) ok = false; for (auto& t : rr.tracks) if (t.track == 1 && t.start != 0) ok = false; if (!ok) why += "session-1 MD5 / position check; "; return ok; }, le); }
    // C02: whole-disc rips + CUE through the reader's CUE code
    {   const long long offs[] = {0, 1, 6, 48, 102, 588, 667, 1164, 1292, 1776, 2939, 4103, 4715, 5880}; int bad = 0, n = 0; string first;
        for (int disc = 0; disc < 2; disc++) { const Model& m = disc ? mT : mX; const vector<Ext>& x = disc ? xT : xX; const Image& im = disc ? T : X; vector<long long> list;
            for (long long o : offs) { list.push_back(o); if (o) list.push_back(-o); } for (long long o = -5880; o <= 5880; o += 97) list.push_back(o);
            for (long long o : list) { string le; Disc d = viaCue(discRip(m, o, true, disc ? 0 : 6000), &le); Report r = cdxr::read(d, x, WDEF); string why = le; n++;
                bool ok = le.empty() && matches(r, im.t, P2, why); for (int k = 1; k <= 2; k++) if (shown(r, d, k) != o) ok = false; if (!ok) { bad++; if (first.empty()) first = (disc ? "T " : "X ") + to_string(o) + ": " + why + describe(r, d); } }
            if (disc) for (long long o : {-5880LL, -667LL, 667LL, 5880LL}) { string le; Disc d = viaCue(discRip(m, o, false, 0), &le); Report r = cdxr::read(d, x, WDEF); string why = le; n++;   // T: nothing read outside the program
                if (!le.empty() || !matches(r, im.t, P2, why)) { bad++; if (first.empty()) first = "T unread edges " + to_string(o) + ": " + why; } } }
        verdict("C02", "discs X and T + CUE, 28 listed offsets + scan every 97 samples in +-5880", bad == 0, to_string(n) + " readings, " + to_string(bad) + " wrong " + first); }
    // C03
    for (long long o : {20000LL, -20000LL}) {
        caseRun("C03a", "disc T, offset " + to_string(o) + ", W = 5880 (coarse by information extent)", wholeRip(mT, o, true, PREGAP, 30000), xT, T, P2);
        map<int, string> w = o > 0 ? map<int, string>{{1, "pass"}, {2, "no match"}, {3, "no match"}} : map<int, string>{{1, "no match"}, {2, "no match"}, {3, "pass"}};
        caseRun("C03b", "disc S, offset " + to_string(o) + ", W = 5880", wholeRip(mS, o, true, PREGAP, 30000), xS, S, w);
        caseRun("C03c", "disc S, offset " + to_string(o) + ", W = 44100", wholeRip(mS, o, true, PREGAP, 70000), xS, S, P3, 44100); }
    // C04 slips in the middle of track 2 of disc S, offset +667 (whole-disc rip + CUE)
    for (int kind = 0; kind < 3; kind++) { Model m = mS; long long mid = S.p[1] + S.t[1].ncd() / 2; vector<int16_t>& a = m.a;
        if (kind == 0 || kind == 2) a.erase(a.begin() + (ptrdiff_t)mid * 2, a.begin() + (ptrdiff_t)mid * 2 + 2);
        if (kind == 1) { int16_t l = a[(size_t)mid * 2], r = a[(size_t)mid * 2 + 1]; a.insert(a.begin() + (ptrdiff_t)mid * 2, {l, r}); }
        if (kind == 2) { long long m2 = mid + 44100; int16_t l = a[(size_t)m2 * 2], r = a[(size_t)m2 * 2 + 1]; a.insert(a.begin() + (ptrdiff_t)m2 * 2, {l, r}); }
        m.n = (long long)a.size() / 2; long long want3 = kind == 0 ? 666 : kind == 1 ? 668 : 667; string le; Disc d = viaCue(discRip(m, 667, true, 6000), &le);
        caseRun("C04", string("disc S + CUE, offset +667, track 2: ") + (kind == 0 ? "one sample lost" : kind == 1 ? "one sample extra" : "one lost, one extra 1 s later"), d, xS, S,
                {{1, "pass"}, {2, "no match"}, {3, "pass"}}, WDEF, [&](const Report& r, string& why) { bool ok = shown(r, d, 3) == want3 && shown(r, d, 1) == 667; if (!ok) why += "track 3 offset should be " + to_string(want3) + "; "; return ok; }, le); }
    // C05
    { Model m = mX; m.a[(size_t)(X.p[0] + 1000) * 2] ^= 1; string le; Disc d = viaCue(discRip(m, 0, true, 6000), &le);
      caseRun("C05", "disc X + CUE, one sample changed in track 1", d, xX, X, {{1, "no match"}, {2, "pass"}}, WDEF, nullptr, le); }
    // C06: per-track files + CUE through the reader's CUE code; the cases that need an independent TOC are run on a simulated drive TOC
    for (int pg : {0, 1}) { FileSet f = trackFiles(mX, 0, pg != 0); string le; Disc d = viaCue(tracksRip(f, pg != 0), &le);
        caseRun("C06a", string("disc X per-track files + CUE, track 1 ") + (pg ? "with" : "without") + " pregap", d, xX, X, P2, WDEF, [&](const Report&, string& why) {
            alignhash::Md5 a, b; vector<int16_t> s; for (long long y = 0; y < mX.n; y++) { s.push_back(d.L(y)); s.push_back(d.R(y)); } a.update(s.data(), s.size() * 2); b.update(X.audio.data(), X.audio.size() * 2);
            bool ok = a.hex() == b.hex(); if (!ok) why += "assembled disc stream differs from the image; "; return ok; }, le);
        caseRun("C06a", string("disc X per-track files, file order only, track 1 ") + (pg ? "with" : "without") + " pregap", fromFiles(f.pcm, 44100), xX, X, P2); }
    { FileSet f = trackFiles(mX, 0, false); auto& f1 = f.pcm[0]; const auto f2 = f.pcm[1]; for (int i = 0; i < 588 * 2; i++) f1.push_back((int16_t)(f2[(size_t)i] + 1));
      Disc d = driveToc(f, mX);
      caseRun("C06c", "disc X per-track, track-1 file 588 longer, other content (simulated drive TOC)", d, xX, X, {{1, "pass"}, {2, "unknown samples"}}, WDEF, [&](const Report&, string& why) { if (d.conflicts != 588) why += "conflicts should be 588; "; return d.conflicts == 588; });
      string le; Disc c = viaCue(tracksRip(f, false), &le);   // a CUE places the files one after the other: track 2 is found 588 samples later
      caseRun("C06c", "the same files + CUE (CUE convention: no conflict, track 2 at its CUE position, 588 later than on the disc)", c, xX, X, P2, WDEF, [&](const Report& r, string& why) { bool ok = c.conflicts == 0 && shown(r, c, 2) == 0; if (!ok) why += "expected no conflict, track 2 at its CUE position; "; return ok; }, le); }
    // C07
    { FileSet f = trackFiles(mS, -667, true); f.track.erase(f.track.begin() + 1); f.at.erase(f.at.begin() + 1); f.pcm.erase(f.pcm.begin() + 1);
      caseRun("C07", "disc S per-track (pregap), offset -667, track-2 file missing (simulated drive TOC)", driveToc(f, mS), xS, S, {{1, "pass"}, {2, "unknown samples"}, {3, "unknown samples"}});
      FileSet g = trackFiles(mS, -667, true); MemRip r = tracksRip(g, true); r.files.erase("TRACK02.wav"); string le; Disc d = viaCue(r, &le);
      caseRun("C07", "the same + CUE: TRACK02.wav missing does not stop the reading (R5)", d, xS, S, {{1, "pass"}, {2, "unknown samples"}, {3, "unknown samples"}}, WDEF,
              [&](const Report&, string& why) { bool ok = !d.notes.empty(); if (!ok) why += "missing file not reported; "; return ok; }, le); }
    // C08
    caseRun("C08a", "image of disc X with the extension files of disc Y", wholeRip(mX, 0, true), xY, X, {{1, "no match"}, {2, "no match"}}, WDEF, [](const Report& r, string& why) { if (!r.otherDisc) why += "no other-disc hint; "; return r.otherDisc; });
    { vector<Ext> x = xX; for (auto& q : x) if (q.t.track == 1) { q = xX[1]; q.name = "TRACK01.LAX"; retrailer(q, [](vector<uint8_t>& t) { t[10] = 1; t[11] = 0; }); }
      caseRun("C08b", "disc X, TRACK01.LAX replaced by track 2's file renumbered 1", wholeRip(mX, 0, true), x, X, {{1, "no match"}, {2, "pass"}}); }
    // C09
    { Model m = mX; for (auto& v : m.a) v = (int16_t)(v / 2); caseRun("C09a", "disc X whole-disc rip with the gain changed (-6 dB)", wholeRip(m, 0, true), xX, X, {{1, "no match"}, {2, "no match"}}, WDEF,
          [](const Report& r, string& why) { if (!r.otherDisc) why += "no other-disc hint; "; return r.otherDisc; }); }
    { FileSet f = trackFiles(mS, 0, false); f.pcm[1].erase(f.pcm[1].begin(), f.pcm[1].begin() + 6000 * 2);
      caseRun("C09b", "disc S per-track, first 6000 samples of track 2's file cut (simulated drive TOC)", driveToc(f, mS), xS, S, {{1, "pass"}, {2, "unknown samples"}, {3, "pass"}});
      string le; Disc d = viaCue(tracksRip(f, false), &le);   // with a CUE the later files move 6000 earlier, and so does the TOC: track 3 is still found
      caseRun("C09b", "the same + CUE (CUE convention: track 2 'no match', track 3 still found)", d, xS, S, {{1, "pass"}, {2, "no match"}, {3, "pass"}}, WDEF, nullptr, le); }
    // C10
    caseRun("C10a", "disc X whole-disc rip, 588 samples more after the last track", wholeRip(mX, -PREGAP, mX.n + 588, fixedOff(0), true), xX, X, P2);
    caseRun("C10b", "disc T whole-disc rip, last 5000 samples cut", wholeRip(mT, 0, mT.n - 5000, fixedOff(0), true), xT, T, P2);
    // C11, C12, C13: damaged or unusable extension files of track 1 -> track 1 CD only, the reason reported
    auto damaged = [&](const string& id, const string& what, const function<void(Ext&)>& f, const string& reason) {
        vector<Ext> x = xX; for (auto& q : x) if (q.name.find("TRACK01") != string::npos) f(q);
        caseRun(id, what, wholeRip(mX, 0, true), x, X, {{1, "cd"}, {2, "pass"}}, WDEF, [&](const Report& r, string& why) {
            bool ok = r.rejected.size() == 1 && r.rejected[0].find(reason) != string::npos; if (!ok) why += "expected rejection '" + reason + "'; "; return ok; }); };
    damaged("C11", "TRACK01.LAX without trailer", [](Ext& q) { q.bytes.resize(q.bytes.size() - 80); cdxr::check(q, true); }, "no trailer");
    damaged("C12", "trailer: one byte flipped", [](Ext& q) { q.bytes[q.bytes.size() - 60] ^= 0x10; cdxr::check(q, true); }, "trailer damaged");
    damaged("C12", "extension block: one byte flipped", [](Ext& q) { q.bytes[1000] ^= 0x10; cdxr::check(q, true); }, "extension layer damaged");
    damaged("C12", "extension file cut short", [](Ext& q) { q.bytes.resize(q.bytes.size() - 37); cdxr::check(q, true); }, "no trailer");
    damaged("C12", "trailer track number 5 (TOC has 2 tracks)", [](Ext& q) { retrailer(q, [](vector<uint8_t>& t) { t[10] = 5; }); }, "track number not in the TOC");
    damaged("C13", "extension block type 2", [](Ext& q) { retrailer(q, [](vector<uint8_t>& t) { t[8] = 2; }); }, "LAES extension block");
    damaged("C13", "trailer version 2", [](Ext& q) { retrailer(q, [](vector<uint8_t>& t) { t[4] = 2; }); }, "unknown trailer version");
    damaged("C13", "lead0 = ncd + 1", [](Ext& q) { retrailer(q, [](vector<uint8_t>& t) { uint32_t n; memcpy(&n, &t[16], 4); n++; memcpy(&t[52], &n, 4); }); }, "invalid trailer field");
    damaged("C13", "track 99 on a CD (X2: 1-98 on a CD)", [](Ext& q) { retrailer(q, [](vector<uint8_t>& t) { t[10] = 99; }); }, "invalid trailer field");
    // C14 (a); R13 without TOC: rejection reported as a failed track, not as unused; a file found nowhere is unused even when its group passed
    { vector<Ext> x = xX; Ext b = xY[0]; b.name = "TRACK01B.LAX"; x.push_back(b); caseRun("C14a", "disc X with X's and Y's TRACK01 in one folder", wholeRip(mX, 0, true), x, X, P2); }
    { vector<Ext> x = {xX[0], xX[0]}; x[1].name = "TRACK01B.LAX"; x[1].bytes[200] ^= 1; { uint32_t c = cdx::crc32(x[1].bytes.data(), x[1].blockLen - 4); memcpy(&x[1].bytes[x[1].blockLen - 4], &c, 4); } cdxr::check(x[1]);
      caseRun("R13", "no TOC: two .lax pass with different blocks -> track refused, no other-disc hint", singleFile(X.t[0].cd), x, X, {{1, "several extension files"}}, WDEF,
              [](const Report& r, string& why) { bool ok = !r.otherDisc && r.unused.empty(); if (!ok) why += "hint or unused; "; return ok; }); }
    { vector<Ext> x = {xX[0], xY[0]}; x[1].name = "TRACK01B.LAX";
      caseRun("R13", "no TOC: the unmatched file of a passing group is listed unused", singleFile(X.t[0].cd), x, X, {{1, "pass"}}, WDEF,
              [](const Report& r, string& why) { bool ok = r.unused.size() == 1 && r.unused[0].find("TRACK01B") != string::npos && !r.otherDisc; if (!ok) why += "unused list; "; return ok; }); }
    // C15 per-track offsets 0, +6, +48
    { map<int, long long> po = {{1, 0}, {2, 6}, {3, 48}}; long long y0 = -PREGAP, y1 = mS.n + 6000; vector<int16_t> pc((size_t)(y1 - y0) * 2, 0);
      for (long long a = y0; a < mS.n + 6000 - 48; a++) { long long o = 0; for (auto& kv : mS.p) if (a >= kv.second) o = po[kv.first];   // each track's disc region moved by its own offset
          long long y = a + o; if (y < y0 || y >= y1) continue; int16_t l = a >= 0 && a < mS.n ? mS.a[(size_t)a * 2] : 0, r = a >= 0 && a < mS.n ? mS.a[(size_t)a * 2 + 1] : 0; pc[(size_t)(y - y0) * 2] = l; pc[(size_t)(y - y0) * 2 + 1] = r; }
      MemRip r; r.cue = discCue(mS, true); r.files["DISC.wav"] = pc; string le; Disc d = viaCue(r, &le);
      caseRun("C15", "disc S + CUE, tracks at offsets 0, +6, +48", d, xS, S, P3, WDEF, [&](const Report& rr, string& why) { bool ok = shown(rr, d, 1) == 0 && shown(rr, d, 2) == 6 && shown(rr, d, 3) == 48; if (!ok) why += "offsets; "; return ok; }, le); }
    // C16 a 4 s all-zero track between the two tracks of disc X (fixture Z), offset +667
    if (fs::exists(dir / "Z.wav")) { V5 Z; get("Z", Z); Image XZ; mk({X1, Z, X2}, XZ, "X with a zero track"); vector<Ext> xz; extsFrom(XZ, xz);
        caseRun("C16", "disc X with a 300-sector all-zero track between its tracks, offset +667", wholeRip(modelOf(XZ), 667, true), xz, XZ, P3); }
    else verdict("C16", "zero-track fixture Z.wav missing", false, "");
    // C17, C18
    { bool ok = X.z[0] == 125 && X.z[1] == 0; for (long long i = X.t[0].ncd(); i < X.p[1]; i++) if (X.audio[(size_t)i * 2] || X.audio[(size_t)i * 2 + 1]) ok = false;
      verdict("C17", "disc X: z_1 = 125 digital zeros after track 1 (ncd 440,875)", ok, "z = " + to_string(X.z[0]) + ", " + to_string(X.z[1])); }
    { bool ok = X1only.A2 == 750 && X1only.z[0] == 125; for (long long i = X1only.t[0].ncd(); i < X1only.A2 * SECTOR; i++) if (X1only.audio[(size_t)i * 2] || X1only.audio[(size_t)i * 2 + 1]) ok = false;
      verdict("C18", "one track only: z_1 = 125, A2 = 750, nothing else after the CD layer", ok, "A2 = " + to_string(X1only.A2));
      caseRun("C18", "one track only: read at offset +667", wholeRip(m1, 667, true), x1, X1only, {{1, "pass"}}); }
    // C20 pack / unpack (lead0/tail0 counted here by an independent loop)
    { bool ok = true; string why; for (size_t k = 0; k < X.t.size(); k++) { auto lax = laxOf(X.t[k], (uint16_t)(k + 1)); cdx::Trailer t; size_t bl = 0;
          if (cdx::parse(lax, t, bl, true) != "" || bl != X.t[k].block.size() || memcmp(lax.data(), X.t[k].block.data(), bl)) { ok = false; why += "block; "; }
          const auto& c = X.t[k].cd; const long long n = X.t[k].ncd(); long long l0 = 0, t0 = 0; while (l0 < n && c[(size_t)l0 * 2] == 0 && c[(size_t)l0 * 2 + 1] == 0) l0++;
          if (l0 == n) t0 = n; else while (c[(size_t)(n - 1 - t0) * 2] == 0 && c[(size_t)(n - 1 - t0) * 2 + 1] == 0) t0++;
          if (t.lead0 != (uint32_t)l0 || t.tail0 != (uint32_t)t0) { ok = false; why += "lead0/tail0; "; }
          for (size_t i = bl + 60; i < bl + 76; i++) if (lax[i]) { ok = false; why += "reserved; "; }
          if (lax != xX[k].bytes) { ok = false; why += "CD TRACKnn.LAX differs from the distributed .lax; "; }
          if (canonV5((int)t.rate, X.t[k].cd, vector<uint8_t>(lax.begin(), lax.begin() + (ptrdiff_t)bl)) != canonOf(X.t[k]) || !X.t[k].canonical) { ok = false; why += "unpacked v5 differs from the encoder output; "; } }
      for (const V5* v : {&T1, &T2}) { cdx::Trailer t = cdx::compute(v->cd.data(), (uint32_t)v->ncd(), 44100, 1); long long l0 = 0, t0 = 0, n = v->ncd(); const auto& c = v->cd;
          while (l0 < n && c[(size_t)l0 * 2] == 0 && c[(size_t)l0 * 2 + 1] == 0) l0++; while (t0 < n - l0 && c[(size_t)(n - 1 - t0) * 2] == 0 && c[(size_t)(n - 1 - t0) * 2 + 1] == 0) t0++;
          if (t.lead0 != (uint32_t)l0 || t.tail0 != (uint32_t)t0) { ok = false; why += "lead0/tail0 of disc T; "; } }
      verdict("C20", "disc X: pack, unpack, trailer fields, CD file = distributed file", ok, why); }
    // C22 WAV base layer + .lax; a no-TOC file shorter than the track whose missing end is digital silence still matches (R5, R7)
    { vector<Ext> x = {xX[0]}; caseRun("C22", "base-layer WAV = CD layer of X track 1, no TOC", singleFile(X.t[0].cd), x, X, {{1, "pass"}}, WDEF, [](const Report& r, string& why) { bool ok = r.tracks.size() == 1 && r.tracks[0].start == 0; if (!ok) why += "position; "; return ok; });
      vector<int16_t> c = X.t[0].cd; c.resize(c.size() - 2);
      caseRun("C22", "base-layer WAV one sample short (music)", singleFile(c), x, X, {}, WDEF, [](const Report& r, string& why) { bool ok = r.unused.size() == 1 && r.unused[0].find("file too short") != string::npos; if (!ok) why += "expected unused (file too short); "; return ok; });
      vector<Ext> y = {xT[1]}; vector<int16_t> t2 = T.t[1].cd; t2.resize(t2.size() - 2000);
      caseRun("R7", "no TOC: track-2 file of disc T with its last 1000 (silent) samples cut", singleFile(t2), y, T, {{2, "pass"}}); }
    // C24 single track files without CUE
    { auto one = [&](const Model& m, int k, long long o) { return singleFile(ripRange(m, m.p.at(k), trackEnd(m, k), fixedOff(o))); };
      auto unusedN = [](size_t n) { return [n](const Report& r, string& why) { bool ok = r.unused.size() == n; if (!ok) why += "unused count; "; return ok; }; };
      caseRun("C24a", "disc T track-2 file alone, offset +667", one(mT, 2, 667), xT, T, {{2, "pass"}}, WDEF, unusedN(1));
      caseRun("C24b", "disc T track-1 file alone, offset -667", one(mT, 1, -667), xT, T, {{1, "pass"}}, WDEF, unusedN(1));
      { Disc d = one(mS, 2, 667); caseRun("C24c", "disc S track-2 file alone, offset +667", d, xS, S, {}, WDEF, [&](const Report& r, string& why) {
            auto lo = leftovers(d, r); bool ok = r.unused.size() == 3 && r.otherDisc && lo.size() == 1 && lo[0].first == 0 && lo[0].second == d.end();
            if (!ok) why += "expected 3 unused, the other-disc hint and the whole file as CD; "; return ok; }); }
      FileSet f = trackFiles(mS, 667, false, 6000); f.track.erase(f.track.begin()); f.at.erase(f.at.begin()); f.pcm.erase(f.pcm.begin());   // tracks 2 and 3 + CUE
      string le; Disc d = viaCue(tracksRip(f, false), &le);
      caseRun("C24c", "disc S track-2 and track-3 files + CUE (tracks 2, 3), offset +667", d, xS, S, {{2, "pass"}, {3, "pass"}}, WDEF, [](const Report& r, string& why) {
          bool ok = r.rejected.size() == 1 && r.rejected[0].find("track number not in the TOC") != string::npos; if (!ok) why += "track 1 file should be 'not in the TOC'; "; return ok; }, le); }
    // C25 whole disc in one file without CUE
    caseRun("C25a", "disc S whole-disc file, no CUE, offset 0", wholeRip(mS, 0, mS.n, fixedOff(0), false), xS, S, P3);
    { Disc d = wholeRip(mT, 0, mT.n, fixedOff(667), false); caseRun("C25b", "disc T whole-disc file, no CUE, offset +667", d, xT, T, P2, WDEF, [&](const Report& r, string& why) {
          bool ok = shown(r, d, 1) == 667 && shown(r, d, 2) == 667 + 485100 && leftovers(d, r).empty(); if (!ok) why += "positions, or the 667 leading zeros would be output; "; return ok; }); }
    // C27 refusals of the packaging tool
    { auto refuse = [&](const string& what, vector<V5> v, long long cap, const string& key) { Image im; string r = layoutImage(v, "LAE_EXT", date, cap, im); verdict("C27", what, r.find(key) != string::npos, r); };
      V5 r48 = X1; r48.rate = 48000; refuse("48 kHz family CD layer", {r48, X2}, CAPDEF, "44.1 kHz family");
      V5 shortT = S2; shortT.cd.resize((size_t)3 * 44100 * 2); refuse("track of 3 s", {S1, shortT, S3}, CAPDEF, "at least 300 sectors");
      refuse("99 audio tracks", vector<V5>(99, X2), CAPDEF * 40, "1 to 98");
      refuse("capacity too small", {X1, X2}, 2000, "does not fit");
      vector<uint8_t> by; cdxr::readBytes(dir / "X1.wav", by); fs::path tmp = dir / "damaged.tmp.wav"; V5 bad;
      by[by.size() - 100] ^= 1; writeBytes(tmp, by.data(), by.size()); string r = loadV5(tmp, bad); by[by.size() - 100] ^= 1;
      verdict("C27", "LAE chunk CRC damaged", r.find("CRC") != string::npos, r);
      const size_t bo = by.size() - X1.block.size() - (X1.block.size() & 1);   // the LAE chunk body (a pad byte follows an odd body)
      int32_t st = 4; memcpy(&by[bo + 4 * 4], &st, 4);   // header field 'stages' = 4, CRC recomputed: only the decoder's own checks see it
      { uint32_t c = cdx::crc32(&by[bo], X1.block.size() - 4); memcpy(&by[bo + X1.block.size() - 4], &c, 4); }
      writeBytes(tmp, by.data(), by.size()); r = loadV5(tmp, bad); std::error_code ec; fs::remove(tmp, ec);
      verdict("C27", "v5 file the decoder refuses (header inconsistent, CRC valid) (P2)", r.find("decoder refuses") != string::npos && r.find("CRC") == string::npos, r); }
    // C32 self-check sabotage
    verdict("C32a", "self-check with a positive case made to fail -> void", !selfCheck(X, 1, false), "");
    verdict("C32b", "self-check with a changed track accepted -> void", !selfCheck(X, 2, false), "");
    // C33 image structure (IMAGE.CUE read back by the reader's parser inside every self-check, see P16)
    { cdxr::CueSheet c; string pe = cdxr::parseCueText(cueText(S, true), fs::path("."), c); bool ok = pe.empty() && c.tracks.size() == 4 && c.tracks[3].num == 4 && !c.tracks[3].audio;
      for (size_t k = 0; ok && k < 3; k++) ok = c.tracks[k].audio && c.tracks[k].idx1 == S.p[k] && c.tracks[k].idx1File == 0;
      ok = ok && S.isoLba == S.A2 + 11400 && S.N2 >= 300 && S.audio.size() == (size_t)S.A2 * SECTOR * 2; for (size_t k = 0; k < S.t.size(); k++) ok = ok && !memcmp(&S.audio[(size_t)S.p[k] * 2], S.t[k].cd.data(), S.t[k].cd.size() * 2);
      string t = cueText(S, true); ok = ok && t.find("PREGAP") == string::npos && t.find("FLAGS") == string::npos && t.find("REM LEAD-OUT " + msf(S.A2)) != string::npos;
      verdict("C33", "disc S image: IMAGE.CUE parsed back (INDEX 01 = layout, data track 4), AUDIO.BIN, data LBA", ok, pe + " A2 " + to_string(S.A2) + ", data LBA " + to_string(S.isoLba)); }
    // C34 data session through both trees
    { bool ok = true; string why; for (int j = 0; j < 2; j++) { vector<iso::Entry> es; string err; if (!iso::list(X.iso, X.isoLba, j == 1, es, err)) { ok = false; why += err; continue; }
          set<string> names; for (auto& en : es) { names.insert(en.path); if (!en.dir) { vector<uint8_t> b; iso::readFile(X.iso, X.isoLba, en, b);
                  if (en.path == "README.TXT") { string tx(b.begin(), b.end()); if (tx != readme()) ok = false; } else if (en.path.rfind("LAE/TRACK", 0) == 0) { int k = atoi(en.path.c_str() + 9); if (k < 1 || k > 2 || b != X.lax[(size_t)k - 1]) ok = false; } else { ok = false; why += "unexpected " + en.path + "; "; } } }
          if (names != set<string>{"README.TXT", "LAE", "LAE/TRACK01.LAX", "LAE/TRACK02.LAX"}) { ok = false; why += "file set; "; } }
      string rm = readme(); for (const char* k : {"MD5", "LBA", "offset", "偏移"}) if (rm.find(k) != string::npos) { ok = false; why += string("README mentions ") + k + "; "; }
      verdict("C34", "data session: primary and Joliet trees, files byte-identical, only README.TXT and LAE/TRACKnn.LAX", ok, why); }
    // C35 CUE writing the reader does not know stops it (R3); an MP3 base layer gets the F7 reason; a FILE without INDEX stops it
    { auto stops = [&](const string& what, const string& cue, const string& key) { MemRip r; r.cue = cue; r.files["x.wav"] = X.t[0].cd; string le; viaCue(r, &le); verdict("C35", what, le.find(key) != string::npos, le); };
      stops("unknown keyword", "FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n  FOO BAR\r\n", "unknown keyword");
      stops("FILE without INDEX", "FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\nFILE \"y.wav\" WAVE\r\n", "has no INDEX");
      stops("MP3 FILE: lossy base layer (F7)", "FILE \"x.mp3\" MP3\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n", "lossy base layer");
      stops("absurd INDEX time", "FILE \"x.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 99999999999:00:00\r\n", "bad INDEX"); }
    // C37; D7 in the image: a track without extension file (noext)
    { vector<Ext> x = {xX[0]}; caseRun("C37", "disc X without an extension file for track 2", wholeRip(mX, 0, true), x, X, {{1, "pass"}, {2, "cd"}}, WDEF, [](const Report& r, string& why) { if (r.otherDisc) why += "hint; "; return !r.otherDisc; }); }
    { Image XN; mk({X1, X2}, XN, "X, track 2 CD only", {2}); vector<Ext> x; extsFrom(XN, x);
      verdict("D7", "image with noext=2: one extension file in the data session, self-check expects track 2 as CD", x.size() == 1 && x[0].t.track == 1 && selfCheck(XN, 0, false), ""); }
    // R19 / R1: CD outputs at the input rate; a silent file without any verified track is output whole
    { fs::path out = dir / "outputs.tmp"; std::error_code ec; fs::remove_all(out, ec);
      Disc d = singleFile(X.t[0].cd, 48000); Report r = cdxr::read(d, {}, WDEF); writeOutputs(out, d, r, "s48");
      vector<uint8_t> b; cdxr::readBytes(out / "s48.cd.wav", b); int rate = 0; vector<int16_t> pc; bool ok = cdxr::parseWav(b, rate, pc, nullptr).empty() && rate == 48000 && pc == X.t[0].cd;
      verdict("R19", "CD output of a 48 kHz input written at 48 kHz, unchanged", ok, to_string(rate));
      vector<int16_t> zero((size_t)44100 * 2 * 5, 0); Disc z = singleFile(zero); Report rz = cdxr::read(z, {xX[0]}, WDEF); writeOutputs(out, z, rz, "silent");
      verdict("R19", "all-silent file, no verified track: the whole file is output as CD", fs::exists(out / "silent.cd.wav"), ""); fs::remove_all(out, ec); }
    // UTF-8 CUE with a Chinese file name, read from disk (wide paths)
    { fs::path rip = dir / "rip.tmp"; std::error_code ec; fs::remove_all(rip, ec); fs::create_directories(rip, ec); const string name = "第一轨.wav";
      auto wav = canonV5(44100, ripRange(mX, -PREGAP, mX.n + 6000, fixedOff(0)), {}); writeBytes(rip / fs::u8path(name), wav.data(), wav.size());
      string cue = "\xEF\xBB\xBF" "FILE \"" + name + "\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 " + msf(X.p[1] / SECTOR + 150) + "\r\n";
      writeBytes(rip / fs::u8path("专辑.cue"), cue.data(), cue.size()); Disc d; string le = cdxr::loadCue(rip / fs::u8path("专辑.cue"), d);
      caseRun("UTF8", "UTF-8 CUE and file names outside the ANSI code page, read from disk", d, xX, X, P2, WDEF, nullptr, le); fs::remove_all(rip, ec); }
    // missing CUE files: no invented CD output (R11, R15); a missing FILE with several tracks; nothing readable -> stop
    { fs::path out = dir / "missing.tmp"; std::error_code ec; fs::remove_all(out, ec);
      FileSet g = trackFiles(mS, 0, false); MemRip r = tracksRip(g, false); r.files.erase("TRACK02.wav"); string le; Disc d = viaCue(r, &le); Report rp = cdxr::read(d, xS, WDEF);
      writeOutputs(out, d, rp, "x"); vector<uint8_t> b; int rate = 0; vector<int16_t> pc; bool t3 = cdxr::readBytes(out / "TRACK03.cd.wav", b) && cdxr::parseWav(b, rate, pc, nullptr).empty();
      const long long n3 = (long long)pc.size() / 2; bool ok = le.empty() && !fs::exists(out / "TRACK02.cd.wav") && fs::exists(out / "TRACK01.lae.wav") && fs::exists(out / "TRACK03.lae.wav") == true;
      for (auto& t : rp.tracks) if (t.track == 2 && t.reason != "unknown samples (file missing)") ok = false;
      verdict("R11", "TRACK02.wav missing: no TRACK02.cd.wav written, tracks 1 and 3 verified", ok, describe(rp, d) + (t3 ? " track 3 CD file " + to_string(n3) : ""));
      fs::remove_all(out, ec);
      MemRip ab; ab.cue = "FILE \"AB.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 " + msf(S.p[1] / SECTOR) + "\r\nFILE \"C.wav\" WAVE\r\n  TRACK 03 AUDIO\r\n    INDEX 01 00:00:00\r\n";
      ab.files["C.wav"] = ripRange(mS, S.p[2], mS.n, fixedOff(0)); Disc dab = viaCue(ab, &le);
      caseRun("R5", "CUE FILE with tracks 1 and 2 missing: both 'file missing', track 3 still verified", dab, xS, S, {{1, "unknown samples (file missing)"}, {2, "unknown samples (file missing)"}, {3, "pass"}}, WDEF, nullptr, le);
      MemRip none; none.cue = discCue(mX, false); string ne; viaCue(none, &ne); verdict("R5", "no audio file of the sheet readable: the reader stops", ne.rfind("cannot read", 0) == 0, ne); }
    // the LAE base rate must equal the WAV rate (P2, X2): fmt says 48000, the LAE header 44100
    { vector<uint8_t> by; cdxr::readBytes(dir / "X1.wav", by); uint32_t r48 = 48000, br = 48000 * 4; memcpy(&by[24], &r48, 4); memcpy(&by[28], &br, 4);
      fs::path tmp = dir / "rate.tmp.wav"; writeBytes(tmp, by.data(), by.size()); V5 v; string r = loadV5(tmp, v); std::error_code ec; fs::remove(tmp, ec);
      verdict("C27", "fmt 48000 Hz around an LAE header with base 44100 Hz: refused", r.find("base rate") != string::npos, r); }
#ifdef _WIN32
    // a GBK (ANSI code page 936) CUE without BOM naming a Chinese file, read from disk
    if (GetACP() == 936) { fs::path rip = dir / "gbk.tmp"; std::error_code ec; fs::remove_all(rip, ec); fs::create_directories(rip, ec);
        auto wav = canonV5(44100, ripRange(mX, -PREGAP, mX.n + 6000, fixedOff(0)), {}); writeBytes(rip / fs::u8path("第一轨.wav"), wav.data(), wav.size());
        const string gbkName = "\xB5\xDA\xD2\xBB\xB9\xEC.wav";   // 第一轨 in GBK
        string cue = "FILE \"" + gbkName + "\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 " + msf(X.p[1] / SECTOR + 150) + "\r\n";
        writeBytes(rip / "album.cue", cue.data(), cue.size()); Disc d; string le = cdxr::loadCue(rip / "album.cue", d);
        caseRun("GBK", "GBK CUE (no BOM) naming a Chinese file, read from disk", d, xX, X, P2, WDEF, nullptr, le); fs::remove_all(rip, ec); }
#endif
    printf("\ntest: %d cases, %d failed\n", nCases, nBad); return nBad ? 1 : 0; }

static int realMain(int argc, char** argv) {
    if (argc < 2) { printf("usage: see the header of lae_cdx.cpp\n"); return 1; }
    string cmd = argv[1]; vector<string> pos = positional(argc, argv, 2);
    if (cmd == "selftest") return selftest();
    if (cmd == "test" && pos.size() == 1) return runTests(pos[0]);
    if ((cmd == "pack" && pos.size() == 2) || (cmd == "split" && pos.size() == 3)) {   // pack: extension file; split: base-layer WAV + extension file (F2)
        V5 v; string e = loadV5(fs::path(pos[0]), v); if (!e.empty()) { printf("%s: %s; nothing written\n", pos[0].c_str(), e.c_str()); return 1; }
        int track = atoi(arg(argc, argv, "track", "1").c_str()); if (track < 1 || track > 65535) { printf("track must be 1-65535 (1-98 on a CD); nothing written\n"); return 1; }
        auto lax = laxOf(v, (uint16_t)track); cdx::Trailer t; size_t bl = 0; string pe = cdx::parse(lax, t, bl);
        if (!pe.empty()) { printf("the extension file would not be readable (%s); nothing written\n", pe.c_str()); return 2; }
        const fs::path laxPath = fs::path(pos.back());
        if (cmd == "split") { auto base = canonV5(v.rate, v.cd, {}); if (!writeBytes(fs::path(pos[1]), base.data(), base.size())) { printf("cannot write %s\n", pos[1].c_str()); return 2; } }
        if (!writeBytes(laxPath, lax.data(), lax.size())) { printf("cannot write %s\n", pos.back().c_str()); return 2; }
        printf("%sextension file %s: track %d, %u CD frames at %u Hz, MD5 %s, extension block %zu bytes\n", cmd == "split" ? ("base layer " + pos[1] + "; ").c_str() : "", pos.back().c_str(), track, t.ncd, t.rate, t.md5hex().c_str(), bl); return 0; }
    if (cmd == "repack" && pos.size() == 3) {
        vector<int16_t> pcm; int rate = 0; string e = cdxr::loadPcm(fs::path(pos[0]), "WAVE", pcm, rate, 0); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; }
        vector<Ext> x(1); x[0].name = pos[1]; if (!cdxr::readBytes(fs::path(pos[1]), x[0].bytes)) { printf("cannot read %s\n", pos[1].c_str()); return 1; } cdxr::check(x[0]);
        if (!x[0].bad.empty()) { printf("%s: %s; only the base layer can be played\n", x[0].name.c_str(), x[0].bad.c_str()); return 3; }
        if ((int)x[0].t.rate != rate) { printf("the base layer is %d Hz, the extension file %u Hz; only the base layer can be played\n", rate, x[0].t.rate); return 3; }
        Disc d = singleFile(pcm, rate); Report r = cdxr::read(d, x, WDEF); const Track* t = nullptr; for (auto& q : r.tracks) if (q.state == Track::PASS) t = &q;
        if (!t) { printf("the base layer does not contain this track (%s); only the base layer can be played\n", !r.unused.empty() ? r.unused[0].c_str() : !r.tracks.empty() ? r.tracks[0].reason.c_str() : ""); return 3; }
        auto by = canonV5((int)t->ext->t.rate, t->cd, vector<uint8_t>(x[0].bytes.begin(), x[0].bytes.begin() + (ptrdiff_t)x[0].blockLen));
        if (!writeBytes(fs::path(pos[2]), by.data(), by.size())) { printf("cannot write %s\n", pos[2].c_str()); return 2; }
        printf("%s: v5 WAV rebuilt (%s)\n", pos[2].c_str(), t->start == 0 && (long long)pcm.size() / 2 == (long long)t->ext->t.ncd ? "base layer paired by MD5 (F3)" : "track located in the base-layer file by content (section 6)"); return 0; }
    if (cmd == "image" && pos.size() >= 2) {
        const fs::path out(pos[0]); std::error_code ec;
        if (fs::exists(out, ec) && !fs::is_empty(out, ec)) { printf("%s is not empty: an image goes into a new or empty folder (no earlier TRACKnn.LAX may reach the disc); nothing written\n", pos[0].c_str()); return 1; }
        set<int> noext; { string s = arg(argc, argv, "noext", ""); size_t i = 0; while (i < s.size()) { size_t j = s.find(',', i); noext.insert(atoi(s.substr(i, j - i).c_str())); if (j == string::npos) break; i = j + 1; } }
        vector<V5> t; for (size_t i = 1; i < pos.size(); i++) { V5 v; string e = loadV5(fs::path(pos[i]), v); if (!e.empty()) { printf("%s: %s; nothing written\n", pos[i].c_str(), e.c_str()); return 1; } t.push_back(std::move(v)); }
        Image im; string e; const string lp = arg(argc, argv, "layout", "");
        if (lp.empty()) e = layoutImage(std::move(t), arg(argc, argv, "label", "LAE_EXT"), arg(argc, argv, "date", nowUtc()), atoll(arg(argc, argv, "capacity", to_string(CAPDEF)).c_str()), im, noext);
        else { Disc pd; cdxr::Layout lay; e = cdxr::loadCue(fs::path(lp), pd, cdxr::diskSource(), &lay); if (e.empty() && !pd.notes.empty()) e = pd.notes[0]; if (e.empty() && pd.conflicts) e = "conflicting samples in the publisher's files";
            if (!e.empty()) { printf("publisher's disc %s: %s; nothing written\n", lp.c_str(), e.c_str()); return 1; }
            e = layoutPublisher(std::move(t), pd, lay, arg(argc, argv, "label", "LAE_EXT"), arg(argc, argv, "date", nowUtc()), atoll(arg(argc, argv, "capacity", to_string(CAPDEF)).c_str()), im, noext);
            if (e.empty()) printf("independent-CD mode: the publisher's layout (%s): CD layers checked against the publisher's tracks, session 1 = the publisher's program (P11)\n", lp.c_str()); }
        if (!e.empty()) { printf("%s; nothing written\n", e.c_str()); return 1; }
        printf("layout: %zu tracks (%zu CD only), session 1 = %lld sectors (A2 = LBA %lld), session 2 data track at LBA %u with %lld sectors; disc %lld of %lld sectors (%.1f %%)\n",
               im.t.size(), im.noext.size(), im.A2, im.A2, im.isoLba, im.N2, im.total, im.cap, 100.0 * (double)im.total / (double)im.cap);
        for (auto& v : im.t) if (!v.canonical) printf("note: %s is not in the canonical v5 layout (F1); readers rebuild the canonical layout\n", v.name.c_str());
        if (!selfCheck(im, 0, arg(argc, argv, "verbose", "0") == "1")) { printf("nothing written\n"); return 2; }
        if (im.total > im.cap) { printf("does not fit after the self-check (P18); nothing written\n"); return 1; }
        fs::create_directories(out / "LAE", ec); string c1 = cueText(im, false), c2 = cueText(im, true), bt = burnText(im);
        bool ok = writeBytes(out / "AUDIO.BIN", im.audio.data(), im.audio.size() * 2) && writeBytes(out / "DATA.ISO", im.iso.data(), im.iso.size())
                  && writeBytes(out / "IMAGE.CUE", c2.data(), c2.size()) && writeBytes(out / "SESSION1.CUE", c1.data(), c1.size()) && writeBytes(out / "BURN.TXT", bt.data(), bt.size());
        for (size_t k = 0; k < im.lax.size() && ok; k++) { if (im.lax[k].empty()) continue; char nm[32]; snprintf(nm, sizeof nm, "TRACK%02d.LAX", (int)k + 1); ok = writeBytes(out / "LAE" / nm, im.lax[k].data(), im.lax[k].size()); }
        if (!ok) { printf("cannot write the deliverables in %s\n", pos[0].c_str()); return 2; }
        printf("CD-Extra image written to %s: AUDIO.BIN, IMAGE.CUE, DATA.ISO, SESSION1.CUE, BURN.TXT, LAE\\TRACKnn.LAX\n", pos[0].c_str()); return 0; }
    if (cmd == "data" && pos.size() == 3) {   // session 2 of a CD: every file must be a CD extension file (track 1-98, 44100 Hz)
        vector<Ext> x; string e = cdxr::loadExtDir(fs::path(pos[0]), x, true); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; }
        vector<vector<uint8_t>> lax; vector<int> no; set<int> seen;
        for (auto& q : x) { if (!q.bad.empty()) { printf("%s: %s (a CD needs track 1-98 and a 44100 Hz CD layer); nothing written\n", q.name.c_str(), q.bad.c_str()); return 1; }
            if (!seen.insert(q.t.track).second) { printf("two files for track %d; nothing written\n", q.t.track); return 1; } lax.push_back(q.bytes); no.push_back(q.t.track); }
        if (lax.empty()) { printf("no .lax files in %s\n", pos[0].c_str()); return 1; }
        printf("tracks in the data session:"); for (int k : no) printf(" %d", k); printf(" (compare with BURN.TXT)\n");
        string err; auto img = iso::build(dataFiles(lax, no), (uint32_t)strtoul(pos[1].c_str(), nullptr, 10), arg(argc, argv, "label", "LAE_EXT"), arg(argc, argv, "date", nowUtc()), 150, 300, &err);
        if (img.empty()) { printf("%s\n", err.c_str()); return 1; } if (!writeBytes(fs::path(pos[2]), img.data(), img.size())) { printf("cannot write %s\n", pos[2].c_str()); return 2; }
        printf("session 2 written: %s, %zu sectors, start LBA %s\n", pos[2].c_str(), img.size() / 2048, pos[1].c_str()); return 0; }
    if (cmd == "read" && pos.size() >= 3) {
        const fs::path out(pos[0]); const string src = pos[1]; vector<Ext> x; string e;
        if (src.rfind("iso:", 0) == 0) { size_t at = src.rfind('@'); vector<uint8_t> img; if (at == string::npos || !cdxr::readBytes(fs::path(src.substr(4, at - 4)), img)) { printf("iso:<path>@<startLba> expected\n"); return 1; }
            e = cdxr::loadExtIso(img, (uint32_t)strtoul(src.substr(at + 1).c_str(), nullptr, 10), x); }
        else e = cdxr::loadExtDir(fs::path(src), x);
        if (!e.empty()) { printf("%s\n", e.c_str()); return 1; } if (x.empty()) printf("no extension files in %s: everything plays as CD\n", src.c_str());
        long long W = max(WDEF, atoll(arg(argc, argv, "W", to_string(WDEF)).c_str())); vector<string> in(pos.begin() + 2, pos.end()); Disc d;
        e = openInputs(in, d); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; }
        Report r = cdxr::read(d, x, W); printReport(d, r); e = writeOutputs(out, d, r, u8(fs::path(in[0]).stem())); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        int pass = 0, fail = 0; for (auto& t : r.tracks) { if (t.state == Track::PASS) pass++; if (t.state == Track::FAIL) fail++; }
        printf("read: %d verified, %d CD only; output in %s\n", pass, (int)r.tracks.size() - pass, pos[0].c_str()); return fail ? 3 : 0; }
    if (cmd == "sim" && pos.size() == 3) {
        Disc img; cdxr::Layout lay; string e = cdxr::loadCue(fs::path(pos[0]) / "IMAGE.CUE", img, cdxr::diskSource(), &lay); if (!e.empty()) { printf("%s\n", e.c_str()); return 1; }
        if (!img.notes.empty()) { printf("%s; the image is incomplete\n", img.notes[0].c_str()); return 1; }
        Model m; m.p = img.p; gapsOf(lay, m); m.n = img.end(); m.a.assign((size_t)m.n * 2, 0); for (long long a = 0; a < m.n; a++) { m.a[(size_t)a * 2] = img.L(a); m.a[(size_t)a * 2 + 1] = img.R(a); }
        const fs::path out(pos[1]); string form = pos[2]; long long o = atoll(arg(argc, argv, "offset", "0").c_str()); bool pg = arg(argc, argv, "pregap", "0") == "1";
        long long over = atoll(arg(argc, argv, "overread", "0").c_str());   // samples read beyond the end of session 1 (drives that overread into the lead-out)
        int change = atoi(arg(argc, argv, "change", "0").c_str()), slip = atoi(arg(argc, argv, "slip", "0").c_str());
        if (change && m.p.count(change)) m.a[(size_t)(m.p[change] + 1000) * 2] ^= 1;
        if (slip && m.p.count(slip)) { long long mid = (m.p[slip] + trackEnd(m, slip)) / 2; m.a.erase(m.a.begin() + (ptrdiff_t)mid * 2, m.a.begin() + (ptrdiff_t)mid * 2 + 2); m.n--; }
        std::error_code ec; fs::create_directories(out, ec); bool ok = true; string cue;
        auto wav = [&](const string& n, const vector<int16_t>& pc) { auto by = canonV5(44100, pc, {}); ok = writeBytes(out / n, by.data(), by.size()) && ok; };
        if (form == "disc" || form == "disc-nocue") { MemRip r = discRip(m, o, pg, over); wav("DISC.wav", r.files["DISC.wav"]); if (form == "disc") cue = r.cue; }
        else if (form == "tracks" || form == "tracks-nocue") { FileSet f = trackFiles(m, o, pg, over); MemRip r = tracksRip(f, pg); for (auto& kv : r.files) wav(kv.first, kv.second); if (form == "tracks") cue = r.cue; }
        else if (form.rfind("tracks-gap", 0) == 0) { const int mode = form.rfind("tracks-gapnext", 0) == 0 ? 1 : form.rfind("tracks-gapomit", 0) == 0 ? 2 : 0; if (!mode) { printf("unknown form %s\n", form.c_str()); return 1; }
            if (o || pg || over) { printf("%s takes no offset=, pregap= or overread= (it rips at offset 0, track 1 from INDEX 01)\n", form.c_str()); return 1; }
            FileSet f = trackFilesGap(m, mode); MemRip r = tracksRipGap(f, m, mode); for (auto& kv : r.files) wav(kv.first, kv.second); if (form.find("-nocue") == string::npos) cue = r.cue; }
        else if (form.rfind("single", 0) == 0) { int k = atoi(form.c_str() + 6); if (!m.p.count(k)) { printf("no track %d\n", k); return 1; } wav("SINGLE.wav", ripRange(m, m.p[k], trackEnd(m, k), fixedOff(o))); }
        else { printf("unknown form %s\n", form.c_str()); return 1; }
        if (!cue.empty()) ok = writeBytes(out / "RIP.CUE", cue.data(), cue.size()) && ok;
        if (!ok) { printf("cannot write in %s\n", pos[1].c_str()); return 2; }
        printf("simulated rip (%s, combined offset %+lld%s%s%s) written to %s\n", form.c_str(), o, pg ? ", with the track-1 pregap" : "", change ? ", one sample changed" : "", slip ? ", one sample lost" : "", pos[1].c_str()); return 0; }
    printf("unknown command or wrong arguments (see the header of lae_cdx.cpp)\n"); return 1; }
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   // every name in the reports is UTF-8
#endif
    try { return realMain(argc, argv); }
    catch (const std::bad_alloc&) { printf("out of memory; nothing more written\n"); return 2; }
    catch (const std::exception& ex) { printf("internal error: %s; nothing more written\n", ex.what()); return 2; } }
