// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// laes_export: a server's tool for streaming LAES (2026-11-03). A streaming service keeps the LAES file and sends each listener plain FLAC
// of the choice they asked for (the core, a master or a rate level): the listener needs no LAES decoder. This tool writes that FLAC, for
// a whole track or for consecutive pieces of a fixed length that a service packs into its own stream format. It only decodes what is
// asked for: it never picks a choice or a quality by itself and does not follow the listener's playback (rules of the 2026-11-03 check).
//
//   laes_export <in> --list
//   laes_export <in> <choice> <out.flac> [--from=<s>] [--len=<s>] [--index=<record>] [--unverified]
//   laes_export <in> <choice> --seg=<s> --dir=<folder> [--index=<record>] [--unverified]   pieces <folder>/seg_00000.flac, seg_00001.flac ...
//
// <choice>: its number from --list (1 = the core) or its name (e.g. master_1_352800_32).
// A whole-track export of a master decodes it from the start and checks its SHA-256 (nothing is kept when it does not match). A part or
// pieces start where they are asked for (block positions of the layers, from --index when the record is there, else made and saved
// there).
// FU14-2 (2026-11-14; specification 1.3: a master is given out only after it and its reference chain matched their SHA-256): a part or
// pieces of a master (or of a version made from it) are exported only when this tool has checked that master for this file content
// before. A run that decodes the whole track (a whole-track export, or --seg over the whole track: its pieces keep the name .part until
// the SHA-256 matched) is such a check and, with --index=<record>, notes it in <record>.verified (text: the LAES file's SHA-256 and the
// SHA-256 of each master that matched). Without that note a part is refused; --unverified exports it without the check and adds the
// tag LAES_UNVERIFIED=1. The core master needs no note (it is checked when the file is opened).
// Every output is one ordinary FLAC file (RFC 9639): its samples are exactly those of the choice in that time range, its STREAMINFO has
// their MD5, and its tags name the source: LAES_CHOICE, LAES_FILE_SHA256 (the LAES file), LAES_MASTER_SHA256 (for a master), LAES_FIRST_FRAME.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include "laes_play.h"
#include "laes_flac_enc.h"
#include "laes_pick.h"   // DEC 21: mayReplace (an existing output is not replaced silently)
#include "win_utf8.h"   // Windows: arguments, file names and output in UTF-8 (2026-11-05)
namespace fs = std::filesystem;

static std::string hex(const uint8_t* p, int n) { static const char* d = "0123456789abcdef"; std::string s; for (int i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }

// one FLAC file of `frames` frames read from p, written in blocks of 4096; STREAMINFO (sizes, total, MD5) is filled in at the end.
// FU14-3 (2026-11-14): written as <out>.part with every write checked (a failure removes it); the caller renames it with finishPart
static int closeF(FILE*& f) { const int r = f ? fclose(f) : 0; f = nullptr; return r; }
struct CloseGuard { FILE*& f; ~CloseGuard() { closeF(f); } };
static bool writeFlacFrom0(laesp::Player& p, int64_t frames, int rate, int bps, const std::vector<std::string>& tags, const std::string& outName, std::string& err) {
    const std::string out = outName + ".part";
    FILE* fo = fopen(out.c_str(), "wb"); if (!fo) { err = "cannot write " + out; return false; }
    CloseGuard guard{fo};   // FU14-7: closed also when an exception leaves this function
    std::vector<uint8_t> h = {'f', 'L', 'a', 'C'}; laesflac::putBlockHeader(h, false, 0, 34); const size_t siAt = h.size(); h.resize(h.size() + 34, 0);
    { std::vector<uint8_t> vc; auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) vc.push_back((uint8_t)(v >> (8 * i))); };   // VORBIS_COMMENT (little-endian lengths)
      const std::string vendor = "laes_export"; u32((uint32_t)vendor.size()); vc.insert(vc.end(), vendor.begin(), vendor.end()); u32((uint32_t)tags.size());
      for (const std::string& t : tags) { u32((uint32_t)t.size()); vc.insert(vc.end(), t.begin(), t.end()); }
      laesflac::putBlockHeader(h, true, 4, (uint32_t)vc.size()); h.insert(h.end(), vc.begin(), vc.end()); }
    if (fwrite(h.data(), 1, h.size(), fo) != h.size()) { err = "cannot write " + out + " (disk full?)"; closeF(fo); fs::remove(out); return false; }
    // the frames are independent: a batch of blocks is encoded on all cores and written in order (the same bytes as one after another)
    const int64_t B = 4096; const int64_t G = 128; const unsigned T = std::max(1u, std::thread::hardware_concurrency());
    laes::V ch[2]; std::vector<int32_t> buf; alignhash::Md5 md5; const int by = bps / 8; std::vector<uint8_t> mb;
    uint32_t fmin = 0xFFFFFF, fmax = 0; int64_t done = 0; uint64_t frameNo = 0; const int64_t lim = 1LL << (bps - 1);
    while (done < frames) { const int64_t n = std::min<int64_t>(B * G, frames - done); buf.resize((size_t)n * 2); int64_t got = 0;
        while (got < n) { const int64_t k = p.read(buf.data() + got * 2, n - got); if (k <= 0) { err = p.err.empty() ? "the choice ended early" : p.err; closeF(fo); fs::remove(out); return false; } got += k; }
        for (int c = 0; c < 2; c++) { ch[c].resize((size_t)n); for (int64_t i = 0; i < n; i++) ch[c][(size_t)i] = buf[(size_t)i * 2 + c]; }
        mb.clear(); for (int64_t i = 0; i < n; i++) for (int c = 0; c < 2; c++) { const int64_t v = ch[c][(size_t)i]; if (v < -lim || v >= lim) { err = "a sample does not fit the word length"; closeF(fo); fs::remove(out); return false; }
            for (int q = 0; q < by; q++) mb.push_back((uint8_t)((uint64_t)v >> (8 * q))); }
        md5.update(mb.data(), mb.size());
        const size_t nb = (size_t)((n + B - 1) / B); std::vector<std::vector<uint8_t>> fr(nb); std::atomic<size_t> next{0};
        auto work = [&] { for (size_t b; (b = next++) < nb;) fr[b] = laesflac::encodeFrame(ch, (int64_t)b * B, (int64_t)std::min<int64_t>(B, n - (int64_t)b * B), frameNo + b, rate, bps); };
        std::vector<std::thread> ts; for (unsigned t = 1; t < std::min<unsigned>(T, (unsigned)nb); t++) ts.emplace_back(work); work(); for (auto& t : ts) t.join();
        for (auto& x : fr) { fmin = std::min<uint32_t>(fmin, (uint32_t)x.size()); fmax = std::max<uint32_t>(fmax, (uint32_t)x.size());
            if (fwrite(x.data(), 1, x.size(), fo) != x.size()) { err = "cannot write " + out + " (disk full?)"; closeF(fo); fs::remove(out); return false; } }
        frameNo += nb; done += n; }
    if (!frames) fmin = 0;
    laes::BitW si; const uint32_t bs = frames >= B ? (uint32_t)B : (uint32_t)std::max<int64_t>(16, frames);
    si.put(bs, 16); si.put(bs, 16); si.put(fmin, 24); si.put(fmax, 24); si.put((uint64_t)rate, 20); si.put(1, 3); si.put((uint64_t)(bps - 1), 5); si.put((uint64_t)frames, 36);
    const std::string mh = md5.hex(); for (int i = 0; i < 16; i++) si.b.push_back((uint8_t)std::stoul(mh.substr((size_t)i * 2, 2), nullptr, 16));
    const bool ok = fseek(fo, (long)siAt, SEEK_SET) == 0 && fwrite(si.b.data(), 1, si.b.size(), fo) == 34; if (closeF(fo) != 0 || !ok) { err = "cannot write " + out + " (disk full?)"; fs::remove(out); return false; }
    return true; }
static bool writeFlacFrom(laesp::Player& p, int64_t frames, int rate, int bps, const std::vector<std::string>& tags, const std::string& outName, std::string& err) {   // FU14-7: an exception (memory) removes the .part, as a failed write does
    try { return writeFlacFrom0(p, frames, rate, bps, tags, outName, err); }
    catch (const std::exception& x) { err = std::string(x.what()) + ": nothing written"; std::error_code ec; fs::remove(outName + ".part", ec); return false; } }
// <out>.part (written and checked) becomes <out>; a failure removes the .part
static bool finishPart(const std::string& out, std::string& err) {
    std::error_code ec; fs::rename(out + ".part", out, ec); if (!ec) return true;
    err = "cannot write " + out + " (" + ec.message() + ")"; fs::remove(out + ".part", ec); return false; }
// FU14-2: the note of checked masters, <record>.verified: "file <SHA-256 of the LAES file>", then "master <SHA-256>" per master that matched
static std::vector<std::string> readVerified(const std::string& path, const std::string& fileHex) {
    std::vector<std::string> v; FILE* fi = fopen(path.c_str(), "rb"); if (!fi) return v; char line[256]; bool mine = false;
    while (fgets(line, sizeof line, fi)) { std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.rfind("file ", 0)) mine = s.substr(5) == fileHex; else if (mine && !s.rfind("master ", 0) && s.size() == 7 + 64) v.push_back(s.substr(7)); }
    fclose(fi); return v; }
static bool writeVerified(const std::string& path, const std::string& fileHex, const std::vector<std::string>& v) {
    std::string t = "LAES masters checked by laes_export (SHA-256)\nfile " + fileHex + "\n"; for (const std::string& m : v) t += "master " + m + "\n";
    FILE* fo = fopen((path + ".part").c_str(), "wb"); if (!fo) return false; const bool okW = fwrite(t.data(), 1, t.size(), fo) == t.size();
    std::string e2; if (fclose(fo) != 0 || !okW) { std::error_code ec; fs::remove(path + ".part", ec); return false; }
    return finishPart(path, e2); }

static int run(int argc, char** argv) {
    if (argc < 3) { printf("usage: laes_export <in> --list | <in> <choice> <out.flac> [--from=s] [--len=s] [--index=rec] [--unverified] | <in> <choice> --seg=s --dir=folder [--index=rec] [--unverified]\n"); return 1; }
    FILE* fp = fopen(argv[1], "rb"); if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    const long long sz = laess::sizeOf(fp); if (sz < 0) { printf("cannot read %s\n", argv[1]); return 2; } const uint64_t size = (uint64_t)sz;   // 64-bit, Windows and POSIX (laes_stream.h)
    laesp::FnSrc src; src.n = size; src.fn = [&](uint64_t at, void* d, size_t k) { return (laess::seekTo(fp, (long long)at) && fread(d, 1, k, fp) == k) ? 0 : 1; };
    laesp::File f; char mg[5]; std::string e; try { e = f.open(src, mg); } catch (const std::exception& x) { e = x.what(); }
    if (!e.empty()) { printf("%s: %s\n", argv[1], e.c_str()); return 2; }
    if (!f.why.empty()) printf("note: %s (only the core master can be exported)\n", f.why.c_str());
    if (std::string(argv[2]) == "--list") { for (size_t j = 0; j < f.choices.size(); j++) printf("%2zu  %-34s %6d Hz %2d bit  %s\n", j + 1, f.choices[j].name.c_str(), f.choices[j].rate, f.choices[j].bits, f.choices[j].what.c_str()); return 0; }
    int j = -1; { const std::string c = argv[2]; for (size_t i = 0; i < f.choices.size(); i++) if (f.choices[i].name == c) j = (int)i; if (j < 0 && !c.empty() && c.find_first_not_of("0123456789") == std::string::npos) j = atoi(c.c_str()) - 1; }
    if (j < 0 || j >= (int)f.choices.size()) { printf("no choice %s (see --list)\n", argv[2]); return 1; }
    const laesp::Choice C = f.choices[(size_t)j]; if (!f.why.empty() && C.kind != 0) { printf("the payload failed a check: only the core master\n"); return 2; }
    std::string out, dir, index; double from = 0, len = -1, seg = 0; bool unverified = false, overwrite = false;
    // FU14-7 (2026-11-14): a time is a number of seconds from 0 up to the length a 64-bit frame count holds; an unknown --option is refused
    auto secs = [&](const std::string& a, size_t at, double& v) { const char* s = a.c_str() + at; char* e = nullptr; v = strtod(s, &e);
        if (e == s || *e || !(v >= 0) || v > 1e12) { printf("%s: a number of seconds (0 or more) is expected\n", a.c_str()); return false; } return true; };
    for (int i = 3; i < argc; i++) { const std::string a = argv[i];
        if (!a.rfind("--from=", 0)) { if (!secs(a, 7, from)) return 1; } else if (!a.rfind("--len=", 0)) { if (!secs(a, 6, len)) return 1; } else if (!a.rfind("--seg=", 0)) { if (!secs(a, 6, seg)) return 1; }
        else if (!a.rfind("--dir=", 0)) dir = a.substr(6); else if (!a.rfind("--index=", 0)) index = a.substr(8); else if (a == "--unverified") unverified = true; else if (a == "--overwrite") overwrite = true;
        else if (!a.rfind("--", 0)) { printf("unknown option %s: nothing written\n", a.c_str()); return 1; } else out = a; }
    const int64_t first = std::min<int64_t>(C.frames, (int64_t)(from * C.rate + 0.5)), count = len < 0 ? C.frames - first : std::min<int64_t>(C.frames - first, (int64_t)(len * C.rate + 0.5));
    const bool whole = seg <= 0 && first == 0 && count == C.frames;
    // FU14-2: the master this choice is made from, and whether it was checked for this file content before (the note next to the record)
    const laesp::Choice& Sv = C.kind == 3 ? f.choices[(size_t)C.from] : C; const bool ofMaster = Sv.kind != 0 && Sv.m >= 0;
    const std::string fileHex = hex(f.fileSha, 32), vrec = index.empty() ? std::string() : index + ".verified";
    std::vector<std::string> known; if (!vrec.empty()) known = readVerified(vrec, fileHex);
    const bool marked = ofMaster && std::find(known.begin(), known.end(), hex(f.ms[(size_t)Sv.m].I.sha, 32)) != known.end();
    const bool fullRun = first == 0 && count == C.frames;   // this run decodes the whole track from frame 0: it computes the SHA-256 itself
    const bool mustCheck = ofMaster && !marked && !unverified;
    if (mustCheck && !fullRun) {
        printf("%s: this master has not been checked against its SHA-256 for this file yet (%s), so a part or pieces of it are not exported: nothing written.\n"
               "  Run once over the whole track with --index=<record> (a whole-track export, or --seg without --from / --len): it checks the master and notes it in <record>.verified. Then repeat this command with the same --index.\n"
               "  Or add --unverified to export without the check (the output then carries the tag LAES_UNVERIFIED=1).\n",
               C.name.c_str(), vrec.empty() ? "no --index=<record> given" : ("no note for it in " + vrec).c_str()); return 2; }
    // after a run in which every master of the chain matched: they are added to the note (a note that cannot be written is only reported)
    auto noteVerified = [&](const laesp::Player& pl) { if (vrec.empty() || !pl.verified()) return; bool add = false;
        for (size_t m = 0; m < f.ms.size(); m++) if (pl.masterOk((int)m) == 1) { const std::string h = hex(f.ms[m].I.sha, 32); if (std::find(known.begin(), known.end(), h) == known.end()) { known.push_back(h); add = true; } }
        if (add && !writeVerified(vrec, fileHex, known)) printf("note: cannot write %s (disk full?): the check is not noted\n", vrec.c_str()); };
    // a derived 16-bit version starts at any frame too: the player moves its dither state there (2026-11-12), bit for bit as from frame 0
    if (!whole) {   // the block positions: from the record, else made now (and saved when a record path is given)
        std::vector<uint8_t> rec; if (!index.empty()) { FILE* fi = fopen(index.c_str(), "rb"); if (fi) { int ch; while ((ch = fgetc(fi)) != EOF) rec.push_back((uint8_t)ch); fclose(fi); } }
        if (rec.empty() || !f.importIndex(rec.data(), rec.size())) { std::atomic<bool> no{false};
            try { f.indexLayers(&no, (int)std::max(1u, std::thread::hardware_concurrency())); }   // a failed read comes back as an exception (2026-11-14)
            catch (const std::exception& x) { printf("%s: %s (while making the block positions): nothing written\n", argv[1], x.what()); return 1; }
            if (!index.empty()) { const std::vector<uint8_t> r = f.exportIndex(); FILE* fo = fopen(index.c_str(), "wb");   // FU14-3: a record not written whole is removed
                if (fo) { const bool okR = fwrite(r.data(), 1, r.size(), fo) == r.size(); if (fclose(fo) != 0 || !okR) { fs::remove(index); printf("note: cannot write the block position record %s (disk full?): not saved\n", index.c_str()); } } } } }
    std::vector<std::string> tags = {"LAES_CHOICE=" + C.name, "LAES_FILE_SHA256=" + hex(f.fileSha, 32)};
    if (C.m >= 0) tags.push_back("LAES_MASTER_SHA256=" + hex(f.ms[(size_t)C.m].I.sha, 32));
    if (ofMaster && !marked && unverified && !whole) tags.push_back("LAES_UNVERIFIED=1");   // FU14-2: given out without the SHA-256 check
    for (auto& t : f.tags) tags.push_back(t.first + "=" + t.second);
    const int thr = (int)std::max(1u, std::thread::hardware_concurrency());
    if (seg > 0) {   // consecutive pieces of seg seconds from --from on
        if (dir.empty()) { printf("--seg needs --dir\n"); return 1; }
        const int64_t step = std::max<int64_t>(1, (int64_t)(seg * C.rate + 0.5));
        { int kq = 0; for (int64_t a = first; a < first + count; a += step, kq++) { char nm[64]; snprintf(nm, sizeof nm, "seg_%05d.flac", kq); if (!laespick::mayReplace((fs::path(dir) / nm).string(), overwrite)) return 1; } }   // DEC 21
        fs::create_directories(dir); laesp::Player p; e = p.start(f, j, thr, false, first); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
        // FU14-2: a master not checked before (this run is over the whole track): the pieces keep the name .part until its SHA-256 matched
        std::vector<std::string> held; auto dropHeld = [&](size_t h0) { std::error_code ec2; for (size_t h = h0; h < held.size(); h++) fs::remove(held[h] + ".part", ec2); };
        int k = 0; for (int64_t a = first; a < first + count; a += step, k++) { char nm[64]; snprintf(nm, sizeof nm, "seg_%05d.flac", k); std::vector<std::string> t = tags; t.push_back("LAES_FIRST_FRAME=" + std::to_string(a));
            const std::string segOut = (fs::path(dir) / nm).string();
            if (!writeFlacFrom(p, std::min<int64_t>(step, first + count - a), C.rate, C.bits, t, segOut, e)) { dropHeld(0); printf("%s\n", e.c_str()); return 2; }
            if (mustCheck) held.push_back(segOut); else if (!finishPart(segOut, e)) { printf("%s\n", e.c_str()); return 2; } }
        if (mustCheck) { int64_t kk; int32_t tail[2]; while ((kk = p.read(tail, 1)) > 0) {}
            if (!p.verified()) { dropHeld(0); printf("%s did not match its SHA-256: nothing written\n", C.name.c_str()); return 2; }
            for (size_t h = 0; h < held.size(); h++) if (!finishPart(held[h], e)) { dropHeld(h + 1); printf("%s\n", e.c_str()); return 2; } }
        if (fullRun && ofMaster) noteVerified(p);
        printf("%d pieces of %.3f s, %s, frames %lld..%lld\n", k, seg, C.name.c_str(), (long long)first, (long long)(first + count)); return 0; }
    if (out.empty()) { printf("no output file\n"); return 1; }
    if (!laespick::mayReplace(out, overwrite)) return 1;   // DEC 21: asked twice in a console, else --overwrite
    laesp::Player p; e = whole ? p.start(f, j, thr) : p.start(f, j, thr, false, first); if (!e.empty()) { printf("%s\n", e.c_str()); return 2; }
    std::vector<std::string> t = tags; t.push_back("LAES_FIRST_FRAME=" + std::to_string(first));
    if (!writeFlacFrom(p, count, C.rate, C.bits, t, out, e)) { printf("%s\n", e.c_str()); return 2; }
    if (whole && C.kind != 0) { int64_t k; int32_t tail[2]; while ((k = p.read(tail, 1)) > 0) {} if (!p.verified()) { fs::remove(out + ".part"); printf("%s did not match its SHA-256: nothing written\n", C.name.c_str()); return 2; } }
    if (!finishPart(out, e)) { printf("%s\n", e.c_str()); return 2; }
    if (whole && ofMaster) noteVerified(p);
    printf("%s: %s, %lld frames from %lld%s\n", out.c_str(), C.name.c_str(), (long long)count, (long long)first, whole && C.kind != 0 ? ", SHA-256 ok" : ""); return 0; }
// FU14-7 (2026-11-14): an error thrown anywhere (memory, a folder that cannot be made) ends with a message and exit code 2, not a crash;
// writeFlacFrom removes its .part itself
int main(int argc, char** argv) {
    lae_u8::init(argc, argv);   // UTF-8 arguments and file names on Windows (win_utf8.h)
    try { return run(argc, argv); }
    catch (const std::exception& x) { printf("error: %s: nothing more written\n", x.what()); return 2; } }
