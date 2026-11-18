// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// lae_repair (2026-11-18): puts back into the LAE form a file that another program changed
// without losing the LAE data. Both formats (LAEY, extension LAE1; LAES, extension LAE2), told apart by the content.
//   lae_repair <in> <out> [--overwrite]
// Case 1, FLAC form re-encoded by another encoder (flac 1.3 / 1.5 by default use inter-channel decorrelation, which the LAE forms
//   do not allow): every audio frame is decoded (here only, decorrelation accepted), the decoded audio must match STREAMINFO's MD5,
//   and the core is coded again with the frame writer of the encoders (laesflac::encodeFrame, block size 4096, independent channels),
//   so the frames are those the LAE encoders write. The metadata blocks are kept as they are and in their order (tags, pictures,
//   padding, the LAE1 / LAE2 blocks byte for byte); only SEEKTABLE and CUESHEET are left out (the forms do not have them).
//   STREAMINFO keeps rate, bit depth, sample count and MD5; its block and frame sizes are those of the new frames.
// Case 2, WAV form packed by `flac --keep-foreign-metadata`: the RIFF chunks are in APPLICATION blocks with ID "riff", in order, the
//   "data" chunk as its header only. The WAV file is put together again: the chunks in order, the decoded audio after the "data"
//   header (as `flac -d --keep-foreign-metadata` does; byte-identical with the file flac read). The audio must match STREAMINFO's MD5.
// The result is written as <out>.part and then checked by the readers of the players (lae_any, as `lae_udec verify`: LAES every
// master's SHA-256; LAEY every extension pack decoded to its end); only then is it renamed to <out>. When anything fails, nothing
// is left. The extension bytes are never changed; no record is added.
// Exit code: 0 repaired and every check passed; 1 usage, or nothing to repair (no LAE data, or the file is already in the LAE form);
// 2 a check failed (audio against MD5, or the result against the LAE records); 3 the file cannot be read or written; 4 this build
// fails the floating-point self-test.
// Build (Windows): cl /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DLAE_STATIC /bigobj
//   src\lae_repair.cpp src\lae_any.cpp src\lae_api.cpp src\laes_api.cpp /Fe:lae_repair.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src\win_longpath.manifest
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <exception>
#include <algorithm>
#include <filesystem>
#include "win_utf8.h"
#include "laes_flac_enc.h"
#include "align_hash.h"
#include "lae_api.h"
namespace fs = std::filesystem;
using laes::V;

namespace {
struct InFile {   // a file read at any offset (64-bit)
    FILE* f = nullptr; uint64_t n = 0;
    bool open(const std::string& p) { f = fopen(p.c_str(), "rb"); if (!f) return false;
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END); n = (uint64_t)_ftelli64(f);
#else
        fseeko(f, 0, SEEK_END); n = (uint64_t)ftello(f);
#endif
        return true; }
    bool read(uint64_t at, void* d, size_t k) { if (at > n || k > n - at) return false;
#ifdef _WIN32
        if (_fseeki64(f, (long long)at, SEEK_SET)) return false;
#else
        if (fseeko(f, (off_t)at, SEEK_SET)) return false;
#endif
        return fread(d, 1, k, f) == k; }
    ~InFile() { if (f) fclose(f); } };
struct Block { int type = 0; uint64_t at = 0; uint32_t len = 0; char id[5] = {0}; std::vector<uint8_t> data; };   // data: loaded for small blocks and "riff"
bool copyRange(InFile& in, uint64_t at, uint64_t len, FILE* o) { std::vector<uint8_t> b(1 << 20);
    while (len) { const size_t k = (size_t)std::min<uint64_t>(len, b.size()); if (!in.read(at, b.data(), k) || fwrite(b.data(), 1, k, o) != k) return false; at += k; len -= k; }
    return true; }
void putHeader(FILE* o, bool last, int type, uint32_t len) { uint8_t h[4] = { (uint8_t)((last ? 0x80 : 0) | type), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len }; fwrite(h, 1, 4, o); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// the result checked by the players' readers, as lae_udec verify does. Returns "" or the reason; fmt = the format found.
std::string verifyResult(const std::string& path, int want) {
    lae_probe_info pi; char err[512] = {0}; lae_any* a = lae_any_open_file(path.c_str(), &pi, err, (int)sizeof err);
    if (!a) return std::string("the result does not open: ") + err;
    lae_any_info info; lae_any_get_info(a, &info); std::string bad;
    if (info.format != want) bad = "the result is not the format of the input";
    else if (!info.usable) { bad = "the extension of the result failed a check";
        for (int i = 0; i < info.checks; i++) { lae_any_check c; if (lae_any_get_check(a, i, &c)) break; if (c.result == 0) { bad += std::string(": ") + c.method + " of " + c.object; break; } } }
    std::vector<int> todo;
    if (bad.empty() && info.format == LAE_FORMAT_LAES) todo.push_back(0);   // the core master, with every master's SHA-256 (LAE_ANY_VERIFY_ALL)
    if (bad.empty() && info.format == LAE_FORMAT_LAEY) { int packs = 0; for (int i = 0; i < info.choices; i++) packs = std::max(packs, lae_any_choice_pack(a, i) + 1);
        for (int k = 0; k < packs; k++) { int hit = -1; for (int i = 0; i < info.choices; i++) { lae_any_choice c; lae_any_get_choice(a, i, &c); if (lae_any_choice_pack(a, i) == k && c.route == LAE_ROUTE_FULL) hit = i; }
            if (hit >= 0) todo.push_back(hit); } }
    if (bad.empty() && todo.empty()) bad = "nothing to decode in the result";
    std::vector<int32_t> blk(2 * 16384);
    for (int one : todo) { if (!bad.empty()) break; lae_any_choice C; lae_any_get_choice(a, one, &C);
        if (lae_any_start(a, one, LAE_ANY_VERIFY_ALL, 0, err, (int)sizeof err)) { bad = std::string("cannot start ") + C.name + ": " + err; break; }
        uint64_t got = 0; for (int64_t n; (n = lae_any_read(a, blk.data(), 16384)) > 0;) got += (uint64_t)n;
        int failed = 0; for (int i = 0; i < info.checks; i++) { lae_any_check c; if (lae_any_get_check(a, i, &c)) break; if (c.result == 0) { failed++; if (bad.empty()) bad = std::string("check failed: ") + c.method + " of " + c.object; } }
        if (bad.empty() && (got != C.frames || lae_any_verified(a) != 1)) { const std::string e = lae_any_error(a); bad = std::string(C.name) + " not decoded to its end" + (e.empty() ? "" : ": " + e); }
        if (bad.empty()) printf("checked: %s decoded to its end (%llu frames), every check passed\n", C.name, (unsigned long long)got); }
    lae_any_close(a); return bad; }
}  // namespace

int main(int argc, char** argv) {
    lae_u8::init(argc, argv);
#ifdef _WIN32
    if (!getenv("LAE_INDEX_DIR")) _putenv_s("LAE_INDEX_DIR", "off");   // no player's record: every stream is checked
#else
    setenv("LAE_INDEX_DIR", "off", 0);
#endif
    std::vector<std::string> pos; bool overwrite = false;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--overwrite")) overwrite = true; else if (argv[i][0] == '-' && argv[i][1] == '-') { printf("unknown option %s\n", argv[i]); return 1; } else pos.push_back(argv[i]); }
    if (pos.size() != 2) { printf("lae_repair <in> <out> [--overwrite]\n"
        "  puts an LAEY / LAES file that another program changed back into the LAE form, when the LAE data is still complete:\n"
        "  - FLAC form re-encoded by another FLAC encoder (inter-channel decorrelation): the core is coded again by the LAE rules;\n"
        "  - WAV form packed by flac --keep-foreign-metadata (APPLICATION \"riff\" blocks): the WAV file is put together again.\n"
        "  The extension bytes are not changed; the result is written only when the audio matches the file's records and every check passed.\n"); return 1; }
    if (lae_selftest() != 1) { printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the formats require; refusing to run\n"); return 4; }
    const std::string inPath = pos[0], outPath = pos[1], part = outPath + ".part";
    { std::error_code ec; if (fs::exists(fs::u8path(outPath), ec) && fs::equivalent(fs::u8path(inPath), fs::u8path(outPath), ec)) { printf("the output is the input: give another name; nothing written\n"); return 1; } }
    InFile in; if (!in.open(inPath)) { printf("cannot read %s\n", inPath.c_str()); return 3; }

    // ---- structure
    auto rd = [&](uint64_t at, void* d, size_t k) { if (!in.read(at, d, k)) memset(d, 0, k); };
    const uint64_t s0 = laesflac::flacStart(rd, in.n); uint8_t mg[4] = {0};
    if (!in.read(s0, mg, 4)) { printf("%s: too short for a FLAC or WAV file\n", inPath.c_str()); return 3; }
    if (memcmp(mg, "fLaC", 4)) { lae_probe_info pi; char err[256] = {0}; const int pr = lae_probe_file(inPath.c_str(), &pi, err, (int)sizeof err);
        if (pr == 0 && pi.format != LAE_FORMAT_NONE) printf("%s is a %s file in the WAV form: nothing to repair (lae_udec verify checks it)\n", inPath.c_str(), lae_format_name(pi.format));
        else printf("%s is not a FLAC file: nothing to repair\n", inPath.c_str());
        return 1; }
    std::vector<Block> blocks; uint64_t p = s0 + 4; bool last = false;
    while (!last) { uint8_t h[4]; if (!in.read(p, h, 4)) { printf("metadata truncated\n"); return 3; } Block b; last = h[0] & 0x80; b.type = h[0] & 0x7F; b.len = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3]; b.at = p + 4;
        if (b.at + b.len > in.n) { printf("metadata truncated\n"); return 3; }
        if (b.type == 2 && b.len >= 4) { in.read(b.at, b.id, 4); }
        const bool laeBlock = b.type == 2 && (!memcmp(b.id, "LAE1", 4) || !memcmp(b.id, "LAE2", 4));
        if (!laeBlock) { b.data.resize(b.len); if (b.len && !in.read(b.at, b.data.data(), b.len)) { printf("cannot read %s\n", inPath.c_str()); return 3; } }
        if (blocks.empty() && b.type != 0) { printf("STREAMINFO is not the first metadata block: not a valid FLAC file\n"); return 3; }
        blocks.push_back(std::move(b)); p += 4 + blocks.back().len; }
    const uint64_t framesAt = p;
    laesflac::Info fi; { if (blocks[0].len != 34) { printf("bad STREAMINFO\n"); return 3; } laesflac::BR r{blocks[0].data.data(), 34}; fi.minBs = (int)r.get(16); fi.maxBs = (int)r.get(16); r.get(24); r.get(24);
        fi.rate = (int)r.get(20); fi.channels = (int)r.get(3) + 1; fi.bps = (int)r.get(5) + 1; fi.total = r.get(36); memcpy(fi.md5, blocks[0].data.data() + 18, 16); }
    int laeId = 0; bool riff = false, riffLae = false; int seekCue = 0;
    for (auto& b : blocks) { if (b.type == 2 && !memcmp(b.id, "LAE1", 4)) laeId |= 1; if (b.type == 2 && !memcmp(b.id, "LAE2", 4)) laeId |= 2;
        if (b.type == 2 && !memcmp(b.id, "riff", 4)) { riff = true; if (b.len >= 8 && !memcmp(b.data.data() + 4, "LAE ", 4)) riffLae = true; }
        if (b.type == 3 || b.type == 5) seekCue++; }
    if (laeId == 3) { printf("both LAE1 and LAE2 blocks: not a valid LAE file; nothing written\n"); return 2; }
    if (laeId && riff) { printf("LAE blocks and \"riff\" blocks in one file: not a case this tool knows; nothing written\n"); return 2; }
    if (!laeId && !riffLae) { printf("%s has no LAE data (%s): nothing to repair\n", inPath.c_str(), riff ? "its \"riff\" blocks hold no \"LAE \" chunk" : "no LAE1 / LAE2 block"); return 1; }
    const int want = laeId == 1 ? LAE_FORMAT_LAEY : laeId == 2 ? LAE_FORMAT_LAES : 0;
    if (fi.channels != 2 || (fi.bps != 16 && fi.bps != 24 && fi.bps != 32) || fi.total == 0 || fi.maxBs < 16 || fi.rate < 1) { printf("STREAMINFO is not that of an LAE core (2 channels, 16 / 24 / 32 bit, a sample count): nothing written\n"); return 2; }
    { bool zero = true; for (int i = 0; i < 16; i++) zero &= fi.md5[i] == 0; if (zero) { printf("STREAMINFO has no MD5: the audio cannot be checked against the file's records; nothing written\n"); return 2; } }

    // ---- case 2: the WAV chunks
    std::vector<const Block*> chunks; int dataIx = -1; int by = (fi.bps + 7) / 8;
    if (riff) { for (auto& b : blocks) if (b.type == 2 && !memcmp(b.id, "riff", 4)) chunks.push_back(&b);
        const auto& h = chunks[0]->data; if (h.size() != 16 || (memcmp(h.data() + 4, "RIFF", 4) && memcmp(h.data() + 4, "RF64", 4) && memcmp(h.data() + 4, "BW64", 4)) || memcmp(h.data() + 12, "WAVE", 4)) { printf("the first \"riff\" block is not a RIFF/WAVE header: nothing written\n"); return 2; }
        for (size_t i = 1; i < chunks.size(); i++) { const auto& d = chunks[i]->data; if (d.size() < 12) { printf("a \"riff\" block is too short: nothing written\n"); return 2; }
            if (!memcmp(d.data() + 4, "data", 4)) { if (dataIx >= 0 || d.size() != 12) { printf("the \"data\" chunk header is not as flac stores it: nothing written\n"); return 2; } dataIx = (int)i; }
            else if (!memcmp(d.data() + 4, "fmt ", 4)) { if (d.size() < 4 + 8 + 16) { printf("bad fmt chunk: nothing written\n"); return 2; } const uint8_t* f = d.data() + 12; const int nch = f[2] | f[3] << 8, align = f[12] | f[13] << 8;
                if (nch != 2 || align != 2 * by) { printf("the fmt chunk (%d channels, %d bytes per frame) does not fit the FLAC audio (2 channels, %d bit): nothing written\n", nch, align, fi.bps); return 2; } } }
        if (dataIx < 0) { printf("no \"data\" chunk header among the \"riff\" blocks: nothing written\n"); return 2; }
        const uint32_t dlen = le32(chunks[(size_t)dataIx]->data.data() + 8);
        if (dlen != 0xFFFFFFFFu && (uint64_t)dlen != fi.total * 2 * (uint64_t)by) { printf("the \"data\" chunk length (%u bytes) differs from the FLAC audio (%llu bytes): nothing written\n", dlen, (unsigned long long)(fi.total * 2 * (uint64_t)by)); return 2; } }

    // ---- the output (as <out>.part)
    { std::error_code ec;
        if (fs::exists(fs::u8path(outPath), ec) && !overwrite) { printf("%s already exists: give another name or --overwrite; nothing written\n", outPath.c_str()); return 1; }
        if (fs::exists(fs::u8path(part), ec)) { printf("%s exists and was not made by this run: remove it or choose another name; nothing written\n", part.c_str()); return 3; } }
    FILE* o = fopen(part.c_str(), "wb"); if (!o) { printf("cannot write %s\n", part.c_str()); return 3; }
    auto fail = [&](int code, const std::string& why) { if (o) fclose(o); o = nullptr; std::error_code ec; fs::remove(fs::u8path(part), ec); printf("%s; nothing written\n", why.c_str()); return code; };
    bool okw = true; std::vector<const Block*> keep; uint64_t siAt = 0;
    if (!riff) {   // front ID3v2 tags kept, "fLaC", STREAMINFO (patched at the end), the other blocks in order without SEEKTABLE / CUESHEET
        okw = copyRange(in, 0, s0, o) && fwrite("fLaC", 1, 4, o) == 4;
        for (size_t i = 1; i < blocks.size(); i++) if (blocks[i].type != 3 && blocks[i].type != 5) keep.push_back(&blocks[i]);
        putHeader(o, keep.empty(), 0, 34); siAt = s0 + 8; okw = okw && fwrite(blocks[0].data.data(), 1, 34, o) == 34;
        for (size_t i = 0; i < keep.size() && okw; i++) { const Block& b = *keep[i]; putHeader(o, i + 1 == keep.size(), b.type, b.len);
            okw = b.data.empty() && b.len ? copyRange(in, b.at, b.len, o) : fwrite(b.data.data(), 1, b.data.size(), o) == b.data.size(); } }
    else for (int i = 0; i <= dataIx && okw; i++) { const auto& d = chunks[(size_t)i]->data; okw = fwrite(d.data() + 4, 1, d.size() - 4, o) == d.size() - 4; }   // up to the "data" header
    if (!okw) return fail(3, "cannot write " + part);

    // ---- the audio: decoded in a window, coded again (case 1) or written as PCM (case 2)
    alignhash::Md5 md5; uint64_t have = 0, frameNo = 0, decorrelated = 0, outFrames = 0; uint32_t fmin = 0xFFFFFF, fmax = 0; const int64_t B = 4096;
    V acc[2]; std::vector<uint8_t> win; uint64_t winAt = framesAt; size_t wp = 0; std::vector<uint8_t> pcm;
    const int th = (int)std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    auto encodeSome = [&](bool final) -> std::string {   // codes the whole 4096-sample frames in acc (and the rest when final), in order
        const int64_t n = (int64_t)acc[0].size(); int64_t nf = n / B; if (final && n % B) nf++; if (!nf) return "";
        std::vector<std::vector<uint8_t>> fr((size_t)nf); std::vector<std::exception_ptr> xs((size_t)th); std::vector<std::thread> pool;
        auto range = [&](int t) { try { for (int64_t k = t; k < nf; k += th) fr[(size_t)k] = laesflac::encodeFrame(acc, k * B, std::min<int64_t>(B, n - k * B), outFrames + (uint64_t)k, fi.rate, fi.bps); } catch (...) { xs[(size_t)t] = std::current_exception(); } };
        for (int t = 1; t < th; t++) { try { pool.emplace_back(range, t); } catch (...) { range(t); } } range(0); for (auto& x : pool) x.join();
        for (auto& e : xs) if (e) { try { std::rethrow_exception(e); } catch (const std::exception& ex) { return std::string("the frame writer refused the audio: ") + ex.what(); } catch (...) { return "the frame writer refused the audio"; } }
        for (auto& f : fr) { fmin = std::min<uint32_t>(fmin, (uint32_t)f.size()); fmax = std::max<uint32_t>(fmax, (uint32_t)f.size()); if (fwrite(f.data(), 1, f.size(), o) != f.size()) return "cannot write " + part; }
        const int64_t used = std::min<int64_t>(n, nf * B); for (int c = 0; c < 2; c++) acc[c].erase(acc[c].begin(), acc[c].begin() + used); outFrames += (uint64_t)nf; return ""; };
    while (have < fi.total) {
        if (win.size() - wp < (8u << 20) && winAt + win.size() < in.n) {   // keep at least 8 MiB (or the rest of the file) ahead
            win.erase(win.begin(), win.begin() + (ptrdiff_t)wp); winAt += wp; wp = 0; const size_t k = (size_t)std::min<uint64_t>(32u << 20, in.n - (winAt + win.size()));
            const size_t old = win.size(); win.resize(old + k); if (!in.read(winAt + old, win.data() + old, k)) return fail(3, "cannot read " + inPath); }
        if (wp + 4 <= win.size()) { const int chc = win[wp + 3] >> 4; if (chc >= 8 && chc <= 10) decorrelated++; }
        V ch[2]; size_t used = 0; const std::string e = laesflac::decodeFrame(win.data() + wp, win.size() - wp, fi, frameNo, have, ch, used, true);
        if (!e.empty()) return fail(2, "audio frame " + std::to_string((unsigned long long)frameNo) + ": " + e);
        const size_t n = ch[0].size(); pcm.resize(n * 2 * (size_t)by); size_t q = 0;
        for (size_t i = 0; i < n; i++) for (int c = 0; c < 2; c++) { const uint64_t v = (uint64_t)ch[c][i]; for (int k = 0; k < by; k++) pcm[q++] = (uint8_t)(v >> (8 * k)); }
        md5.update(pcm.data(), pcm.size());
        if (riff) { if (fwrite(pcm.data(), 1, pcm.size(), o) != pcm.size()) return fail(3, "cannot write " + part); }
        else { for (int c = 0; c < 2; c++) acc[c].insert(acc[c].end(), ch[c].begin(), ch[c].end()); if ((int64_t)acc[0].size() >= B * 64 * th) { const std::string w = encodeSome(false); if (!w.empty()) return fail(3, w); } }
        wp += used; have += n; frameNo++; }
    const uint64_t framesEnd = winAt + wp;
    { const uint64_t tl = laesflac::trailerLen(rd, framesEnd, in.n); if (tl != in.n - framesEnd) return fail(2, "bytes after the last audio frame that are no APEv2 / ID3v1 tag"); }
    { uint8_t m[16]; const std::string h = md5.hex(); for (int i = 0; i < 16; i++) m[i] = (uint8_t)std::stoul(h.substr((size_t)i * 2, 2), nullptr, 16);
        if (memcmp(m, fi.md5, 16)) return fail(2, "the decoded audio differs from the MD5 in STREAMINFO: the audio is not the one the LAE data was made for"); }
    if (!riff && !decorrelated) { fclose(o); o = nullptr; std::error_code ec; fs::remove(fs::u8path(part), ec);
        printf("%s: every audio frame already has independent channels: nothing to repair (lae_udec verify checks it)\n", inPath.c_str()); return 1; }
    if (!riff) { const std::string w = encodeSome(true); if (!w.empty()) return fail(3, w);
        okw = copyRange(in, framesEnd, in.n - framesEnd, o);   // APEv2 / ID3v1 tags after the frames kept
        const uint32_t bs = fi.total >= (uint64_t)B ? (uint32_t)B : (uint32_t)std::max<uint64_t>(16, fi.total);
        laes::BitW si; si.put(bs, 16); si.put(bs, 16); si.put(fmin, 24); si.put(fmax, 24);
#ifdef _WIN32
        okw = okw && _fseeki64(o, (long long)siAt, SEEK_SET) == 0;
#else
        okw = okw && fseeko(o, (off_t)siAt, SEEK_SET) == 0;
#endif
        okw = okw && fwrite(si.b.data(), 1, 10, o) == 10; }
    else { if ((fi.total * 2 * (uint64_t)by) & 1) okw = fputc(0, o) != EOF;   // the pad byte of an odd chunk
        for (size_t i = (size_t)dataIx + 1; i < chunks.size() && okw; i++) { const auto& d = chunks[i]->data; okw = fwrite(d.data() + 4, 1, d.size() - 4, o) == d.size() - 4; } }
    okw = okw && fflush(o) == 0; okw = fclose(o) == 0 && okw; o = nullptr;
    if (!okw) return fail(3, "cannot write " + part + " (disk full?)");
    printf("%s: %s, %llu audio frames%s; audio matches the MD5 in STREAMINFO\n", inPath.c_str(), riff ? "WAV form packed by flac --keep-foreign-metadata" : "FLAC form re-encoded by another encoder",
        (unsigned long long)frameNo, riff ? "" : (" (" + std::to_string((unsigned long long)decorrelated) + " with inter-channel decorrelation)").c_str());
    if (!riff && seekCue) printf("left out: %d SEEKTABLE / CUESHEET block%s (the LAE forms have none)\n", seekCue, seekCue == 1 ? "" : "s");
    if (riff) { int other = 0; for (auto& b : blocks) if (!(b.type == 2 && !memcmp(b.id, "riff", 4)) && b.type != 0 && b.type != 1) other++;
        if (other) printf("left out: %d FLAC metadata block%s that flac added (the WAV file's own tags are in its chunks)\n", other, other == 1 ? "" : "s"); }

    // ---- the result checked by the players' readers
    int fmtWant = want; if (riff) { lae_probe_info pi; char err[256] = {0}; if (lae_probe_file(part.c_str(), &pi, err, (int)sizeof err) == 0) fmtWant = pi.format; }
    if (fmtWant == LAE_FORMAT_NONE) return fail(2, "the WAV file put together holds no LAE data the readers recognise");
    const std::string bad = verifyResult(part, fmtWant);
    if (!bad.empty()) return fail(2, "the result failed the check: " + bad);
    { std::error_code ec; if (overwrite) fs::remove(fs::u8path(outPath), ec); fs::rename(fs::u8path(part), fs::u8path(outPath), ec); if (ec) return fail(3, "cannot rename " + part + " to " + outPath); }
    printf("%s repair OK: %s -> %s | every check passed\n", lae_format_name(fmtWant), inPath.c_str(), outPath.c_str());
    return 0; }
