// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// audiodecoder.lae: LAE decoder for Kodi (2026-11-03). Plays files with the format's own extension .lae: LAES (LAE Lossless, "LAE2") in the
// WAV / RF64 or FLAC form, and LAEY (LAE v5, "LAE1") WAV. Kodi gives a decoder add-on only the extensions it declares and does not let it
// hand a file back to Kodi's own decoder (CodecFactory, Kodi 21), so .flac / .wav stay with Kodi (which plays their core); rename an LAE
// file to .lae to play its masters here. Kodi's add-on headers (kodi-dev-kit, GPL-2.0-or-later) are compiled in: the add-on binary as a
// whole is distributed under GPL-3.0-or-later; this source file is Apache-2.0.
// LAES rules, as in the foobar2000 component: a master plays as that master only after a full run matched its SHA-256 (spec 1.3); the
// result is kept by the SHA-256 of the whole file (laes_verified.txt in the add-on's user folder), so an unchanged file plays at once the
// next time. The first time, every master of the file is checked in one pass with a progress window. Block positions for seeking are
// kept in laes_index\<file SHA-256>.idx (never written into the media file). What plays follows the add-on settings.
#include <kodi/addon-instance/AudioDecoder.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/gui/dialogs/ExtendedProgress.h>
#include <xmmintrin.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../../src/lae_api.h"
#include "../../../src/lae_flacmeta.h"   // the FLAC form of LAEY: structure and Vorbis comments (2026-11-10)
#include "../../../src/lae_wavtags.h"   // tags of the WAV form (LIST/INFO + id3), shared by the LAE player plugins

namespace {
// the decoders need round-to-nearest and no flush-to-zero (x64: SSE only); every call into them runs with this state
struct FpPin { unsigned mx; FpPin() : mx(_mm_getcsr()) { _mm_setcsr(0x1F80u); } ~FpPin() { _mm_setcsr(mx); } };

// a file read through Kodi's file system (local, network shares, ...)
struct Src { kodi::vfs::CFile f; std::mutex m; uint64_t size = 0;
    static int LAE_CALL read(void* ctx, uint64_t off, void* buf, size_t len) { Src* s = (Src*)ctx; std::lock_guard<std::mutex> g(s->m);
        if (s->f.Seek((int64_t)off, SEEK_SET) != (int64_t)off) return -1; uint8_t* p = (uint8_t*)buf;
        while (len) { const ssize_t k = s->f.Read(p, len); if (k <= 0) return -1; p += k; len -= (size_t)k; } return 0; } };

std::string hex(const unsigned char* p, int n) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
std::string userFile(const std::string& name) { return kodi::addon::GetUserPath(name); }

// On Windows the SHA-256 results and the block position records are kept in the folder shared by every LAE player of this user
// (%APPDATA%\LAE: foobar2000, DirectShow, Kodi), so a file checked in one player plays at once in the others; elsewhere in the add-on's
// own data folder. The results file is only appended to (players running at once keep each other's results).
#ifdef _WIN32
std::wstring sharedDir() { const wchar_t* a = _wgetenv(L"APPDATA"); std::wstring d = a ? a : L"."; d += L"\\LAE"; _wmkdir(d.c_str()); return d; }
std::vector<uint8_t> readFileW(const std::wstring& p) { std::vector<uint8_t> d; FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return d;
    _fseeki64(f, 0, SEEK_END); const int64_t n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET); if (n > 0 && n < (64ll << 20)) { d.resize((size_t)n); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); } fclose(f); return d; }
#endif
std::vector<uint8_t> readRecord(const std::string& sha) {
#ifdef _WIN32
    std::vector<uint8_t> d = readFileW(sharedDir() + L"\\laes_index\\" + std::wstring(sha.begin(), sha.end()) + L".idx"); if (!d.empty()) return d;
#else
    std::vector<uint8_t> d;
#endif
    kodi::vfs::CFile r; if (r.OpenFile(userFile("laes_index/" + sha + ".idx"))) { d.resize((size_t)std::max<int64_t>(0, r.GetLength())); if (!d.empty() && r.Read(d.data(), d.size()) != (ssize_t)d.size()) d.clear(); r.Close(); }
    return d; }
void writeRecord(const std::string& sha, const std::vector<uint8_t>& b) {
#ifdef _WIN32
    const std::wstring dir = sharedDir() + L"\\laes_index"; _wmkdir(dir.c_str()); const std::wstring rec = dir + L"\\" + std::wstring(sha.begin(), sha.end()) + L".idx", tmp = rec + L".part";
    FILE* w = _wfopen(tmp.c_str(), L"wb"); if (!w) return; const bool ok = fwrite(b.data(), 1, b.size(), w) == b.size(); fclose(w);
    if (ok) { _wremove(rec.c_str()); if (_wrename(tmp.c_str(), rec.c_str()) != 0) _wremove(tmp.c_str()); } else _wremove(tmp.c_str());
#else
    kodi::vfs::CreateDirectory(userFile("laes_index")); kodi::vfs::CFile w; if (w.OpenFileForWrite(userFile("laes_index/" + sha + ".idx"), true)) { w.Write(b.data(), b.size()); w.Close(); }
#endif
}

// results of the SHA-256 check per file content and master: laes_verified.txt, lines "<file SHA-256> m<k> ok|fail" (as foo_input_lae)
class Verified {
public:
    int get(const unsigned char sha[32], int m) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1); auto it = map.find(k);
        if (it == map.end()) { loaded = false; load(); it = map.find(k); }   // another player may have checked it meanwhile
        return it == map.end() ? 0 : it->second ? 1 : -1; }
    void put(const unsigned char sha[32], int m, bool ok) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1);
        auto it = map.find(k); if (it != map.end() && it->second == ok) return; map[k] = ok;
#ifdef _WIN32
        FILE* f = _wfopen((sharedDir() + L"\\laes_verified.txt").c_str(), L"ab"); if (f) { fprintf(f, "%s %s\n", k.c_str(), ok ? "ok" : "fail"); fclose(f); }
#else
        kodi::vfs::CFile f; std::string all; for (auto& e : map) all += e.first + (e.second ? " ok\n" : " fail\n");
        if (f.OpenFileForWrite(userFile("laes_verified.txt"), true)) { f.Write(all.data(), all.size()); f.Close(); }
#endif
    }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, bool> map;
    void parse(const std::string& text) { size_t at = 0; while (at < text.size()) { size_t e = text.find('\n', at); if (e == std::string::npos) e = text.size();
            std::string line = text.substr(at, e - at); at = e + 1; while (!line.empty() && line.back() == '\r') line.pop_back(); const size_t a = line.rfind(' ');
            if (a != std::string::npos) map[line.substr(0, a)] = line.substr(a + 1) == "ok"; } }
    void load() { if (loaded) return; loaded = true;
        { kodi::vfs::CFile f; if (f.OpenFile(userFile("laes_verified.txt"))) { std::string line, all; while (f.ReadLine(line)) all += line + "\n"; parse(all); } }   // the add-on's own folder (0.1.0)
#ifdef _WIN32
        { const std::vector<uint8_t> d = readFileW(sharedDir() + L"\\laes_verified.txt"); parse(std::string(d.begin(), d.end())); }
#endif
    }
};
Verified g_verified;

int mastersOf(const laes_file* f) { laes_file_info fi; laes_get_info(f, &fi); int n = 0; for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(f, i, &c); if (c.kind == LAES_KIND_MASTER) n++; } return n; }

// what plays, from the settings (as the foobar2000 component): layers 0 = all, 1 = CD or core layer + depth (base rate), 2 = CD or core layer only, 3 = CD or core layer + frequency layer (no depth layer). LAES (SPEC 10.3): the file's default entry (a master, else the core master); of that master the highest rate up to maxrate; a word above maxbits: that entry's own derived 16-bit version. Never another master.
int pick(const laes_file* f, int layers, int maxrate, int maxbits) {
    laes_file_info fi; laes_get_info(f, &fi); if (!fi.payload_ok || layers == 2) return 0;
    const int capR = layers == 1 ? fi.base_rate : maxrate;
    auto capped = [&](int i) { laes_choice_info c; laes_get_choice(f, i, &c);   // word length above the maximum: this entry's own derived 16-bit version, if the file offers one
        if (maxbits > 0 && c.bits > maxbits) for (int d = 0; d < fi.choices; d++) if (laes_choice_from(f, d) == i) return d;
        return i; };
    // SPEC 10.3 (2026-11-12): what plays without a choice of the user is the file's default entry (set when the file was encoded: a
    // master itself, else the core master), never an entry picked by rate, size or device. The limits work inside that master only.
    int d = laes_default_choice(f); if (d < 0 || d >= fi.choices) d = 0;
    laes_choice_info dc; laes_get_choice(f, d, &dc); if (dc.kind != LAES_KIND_MASTER) return capped(0);
    for (int i = d; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(f, i, &c);
        if (i > d && (c.kind == LAES_KIND_MASTER || c.kind == LAES_KIND_CORE)) break;
        if (c.master != dc.master || c.kind == LAES_KIND_DERIVED16) continue;
        if (capR > 0 && c.rate > capR) continue;
        return capped(i); }
    return capped(0); }



class CLaeDecoder : public kodi::addon::CInstanceAudioDecoder {
public:
    explicit CLaeDecoder(const kodi::addon::IInstanceInfo& instance) : CInstanceAudioDecoder(instance) {}
    ~CLaeDecoder() override { close(); }

    bool Init(const std::string& filename, unsigned int, int& channels, int& samplerate, int& bitspersample, int64_t& totaltime, int& bitrate,
              AudioEngineDataFormat& format, std::vector<AudioEngineChannel>& channellist) override try {   // no C++ exception may reach Kodi (it would end Kodi): the file does not open instead
        FpPin pin; close(); m_src.reset(new Src()); if (!m_src->f.OpenFile(filename)) return false; m_src->size = (uint64_t)m_src->f.GetLength();
        char err[512] = "", mg[5] = "";
        m_laes = laes_open_reader(&Src::read, m_src.get(), m_src->size, 0, mg, err, sizeof err);
        if (m_laes) { if (!startLaes()) return false; }
        else if (!strcmp(mg, "LAE1")) { if (!startLaey()) return false; }
        else { kodi::Log(ADDON_LOG_INFO, "LAE: %s: %s", filename.c_str(), err); return false; }
        channels = 2; samplerate = m_rate; bitspersample = m_bits; totaltime = (int64_t)(m_frames * 1000 / (uint64_t)m_rate);
        bitrate = (int)((double)m_src->size * 8 / ((double)m_frames / m_rate) / 1000 + 0.5);
        format = m_bits == 16 ? AUDIOENGINE_FMT_S16NE : m_bits == 24 ? AUDIOENGINE_FMT_S24NE4MSB : AUDIOENGINE_FMT_S32NE;   // 24 bit: at the top of 32 (Kodi passes S24NE4 to FFmpeg as S32 without a shift)
        channellist = {AUDIOENGINE_CH_FL, AUDIOENGINE_CH_FR}; return true; }
    catch (...) { kodi::Log(ADDON_LOG_ERROR, "LAE: %s: not enough memory or internal error", filename.c_str()); close(); return false; }

    int ReadPCM(uint8_t* buffer, size_t size, size_t& actualsize) override try {
        FpPin pin; actualsize = 0; const int by = m_bits == 16 ? 2 : 4; const int64_t want = (int64_t)(size / (size_t)(2 * by)); if (want <= 0) return AUDIODECODER_READ_SUCCESS;
        m_buf.resize((size_t)want * 2); const int64_t k = m_play ? laes_read(m_play, m_buf.data(), want) : m_track ? lae_read(m_track, m_buf.data(), want) : -1;
        if (k < 0) return AUDIODECODER_READ_ERROR; if (k == 0) return AUDIODECODER_READ_EOF;
        if (by == 2) { int16_t* o = (int16_t*)buffer; for (int64_t i = 0; i < k * 2; i++) o[i] = (int16_t)m_buf[(size_t)i]; }
        else if (m_bits == 24) { uint32_t* o = (uint32_t*)buffer; for (int64_t i = 0; i < k * 2; i++) o[i] = (uint32_t)m_buf[(size_t)i] << 8; }   // S24NE4MSB
        else memcpy(buffer, m_buf.data(), (size_t)k * 8);   // S32NE as is
        actualsize = (size_t)k * 2 * (size_t)by; return AUDIODECODER_READ_SUCCESS; }
    catch (...) { actualsize = 0; return AUDIODECODER_READ_ERROR; }

    int64_t Seek(int64_t time) override {
        if (time < 0) time = 0;   // a negative time is the start (as an unsigned frame number it was the end)
        FpPin pin; const uint64_t frame = std::min<uint64_t>(m_frames, (uint64_t)(time * (int64_t)m_rate / 1000)); char err[256];
        if (m_laes) {   // every choice starts at the frame, a derived 16-bit version too (its dither state is moved there, bit-exact; 2026-11-12)
            // the new decoder first: when it cannot start there, the playing one goes on where it was and the seek fails
            laes_play* np = laes_start_at(m_laes, m_choice, frame, 0, err, sizeof err); if (!np) return -1;
            if (m_play) laes_stop(m_play); m_play = np; return time; }
        if (m_track) return lae_seek(m_track, frame) == 0 ? time : -1;
        return -1; }

    // the length, format and tags for Kodi's library: LAES from the structure (no check, no decoding); LAEY from its "LAE " chunk header.
    // Tags: the WAV form (LAEY, LAES WAV) from LIST/INFO + id3 (src/lae_wavtags.h), the FLAC form from its Vorbis comments.
    static void putTag(kodi::addon::AudioDecoderInfoTag& tag, const char* key, const char* val) { std::string K = key; for (char& ch : K) ch = (char)toupper((unsigned char)ch);
        if (K == "TITLE") tag.SetTitle(val); else if (K == "ARTIST") tag.SetArtist(val); else if (K == "ALBUM") tag.SetAlbum(val);
        else if (K == "ALBUMARTIST" || K == "ALBUM ARTIST") tag.SetAlbumArtist(val); else if (K == "GENRE") tag.SetGenre(val); else if (K == "TRACKNUMBER") tag.SetTrack(atoi(val));
        else if (K == "DISCNUMBER") tag.SetDisc(atoi(val)); else if (K == "DATE") tag.SetReleaseDate(val); else if (K == "COMMENT") tag.SetComment(val); }
    bool ReadTag(const std::string& file, kodi::addon::AudioDecoderInfoTag& tag) override try {
        FpPin pin; Src s; if (!s.f.OpenFile(file)) return false; s.size = (uint64_t)s.f.GetLength(); char err[256], mg[5] = "";
        const lae_wavtags::List wt = lae_wavtags::read([&s](uint64_t off, void* buf, size_t n) { return Src::read(&s, off, buf, n) == 0; }, s.size);
        laes_file* q = laes_open_reader(&Src::read, &s, s.size, 1, mg, err, sizeof err);
        if (q) { laes_file_info fi; laes_get_info(q, &fi); const int i = pick(q, kodi::addon::GetSettingInt("layers"), rateSetting(), bitsSetting()); laes_choice_info c; laes_get_choice(q, i, &c);
            tag.SetDuration((int)(c.frames / (uint64_t)c.rate)); tag.SetSamplerate(c.rate); tag.SetChannels(2);
            if (!wt.empty()) for (auto& kv : wt) putTag(tag, kv.first.c_str(), kv.second.c_str());
            else for (int k = 0; ; k++) { const char* key; const char* val; if (laes_get_tag(q, k, &key, &val) != 0) break; putTag(tag, key, val); }
            laes_close(q); return true; }
        // LAEY (container v5): the master rate and length from the "LAE " chunk header (magic LAE1, version 5, master rate, base rate, stages, frames)
        // the same 24 header bytes from the FLAC form (2026-11-10): the start of the "LAE1" APPLICATION data; tags from the Vorbis comments
        auto fromHeader = [&](const uint8_t* hd, const lae_wavtags::List& tg) { auto u32 = [&](int o) { return (uint32_t)hd[o] | (uint32_t)hd[o + 1] << 8 | (uint32_t)hd[o + 2] << 16 | (uint32_t)hd[o + 3] << 24; };
            if (u32(0) != 0x3145414Cu || u32(4) != 5) return false; const uint32_t master = u32(8), base = u32(12), frames = u32(20); if (!master || !base) return false;
            const int ly = kodi::addon::GetSettingInt("layers"), cap = rateSetting(); int out = ly == 1 || ly == 2 ? (int)base : (int)master;   // as Init picks the route
            if ((ly == 0 || ly == 3) && cap > 0) while (out > cap && out > (int)base) out /= 2;
            tag.SetDuration((int)(frames / master)); tag.SetSamplerate(out); tag.SetChannels(2);
            for (auto& kv : tg) putTag(tag, kv.first.c_str(), kv.second.c_str());
            return true; };
        uint8_t h[12]; if (s.size < 12 || Src::read(&s, 0, h, 12) != 0) return false;
        if (!memcmp(h, "fLaC", 4)) { const laeyflac::ReadFn rd = [&s](uint64_t off, void* buf, size_t n) { return Src::read(&s, off, buf, n) == 0; }; laeyflac::Layout L; bool notLaey = true; uint8_t hd[24];
            if (!laeyflac::layout(rd, s.size, L).empty() || !laeyflac::check(L, notLaey).empty() || !laeyflac::head(rd, L, hd, 24)) return false;
            return fromHeader(hd, laeyflac::comments(rd, L)); }
        if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
        uint64_t laeAt = 0, laeLen = 0;   // the last "LAE " chunk counts, as in liblae (spec: a reader uses the last one)
        for (uint64_t pos = 12; pos + 8 <= s.size;) { uint8_t c[8]; if (Src::read(&s, pos, c, 8) != 0) return false; const uint64_t n = (uint64_t)c[4] | (uint64_t)c[5] << 8 | (uint64_t)c[6] << 16 | (uint64_t)c[7] << 24;
            if (!memcmp(c, "LAE ", 4)) { laeAt = pos + 8; laeLen = n; }
            pos += 8 + n + (n & 1); }
        if (laeAt) { uint8_t hd[24]; if (laeLen < 24 || Src::read(&s, laeAt, hd, 24) != 0) return false; auto u32 = [&](int o) { return (uint32_t)hd[o] | (uint32_t)hd[o + 1] << 8 | (uint32_t)hd[o + 2] << 16 | (uint32_t)hd[o + 3] << 24; };
            if (u32(0) != 0x3145414Cu || u32(4) != 5) return false; const uint32_t master = u32(8), base = u32(12), frames = u32(20); if (!master || !base) return false;
            const int ly = kodi::addon::GetSettingInt("layers"), cap = rateSetting(); int out = ly == 1 || ly == 2 ? (int)base : (int)master;   // as Init picks the route
            if ((ly == 0 || ly == 3) && cap > 0) while (out > cap && out > (int)base) out /= 2;
            tag.SetDuration((int)(frames / master)); tag.SetSamplerate(out); tag.SetChannels(2);
            for (auto& kv : wt) putTag(tag, kv.first.c_str(), kv.second.c_str());
            return true; }
        return false; }
    catch (...) { return false; }   // memory: no tags for this file (nothing may reach Kodi)

private:
    std::unique_ptr<Src> m_src; laes_file* m_laes = nullptr; laes_play* m_play = nullptr; lae_track* m_track = nullptr; int m_choice = 0, m_rate = 0, m_bits = 16; uint64_t m_frames = 0;
    std::vector<int32_t> m_buf; std::thread m_indexer; volatile int m_cancel = 0;
    static int rateSetting() { const int r[] = {0, 192000, 96000}; const int i = kodi::addon::GetSettingInt("maxrate"); return i >= 0 && i < 3 ? r[i] : 0; }
    static int bitsSetting() { const int b[] = {0, 16, 24, 32}; const int i = kodi::addon::GetSettingInt("maxbits"); return i >= 0 && i < 4 ? b[i] : 0; }
    void close() { m_cancel = 1; if (m_indexer.joinable()) m_indexer.join(); m_cancel = 0;
        FpPin pin; if (m_play) { laes_stop(m_play); m_play = nullptr; } if (m_laes) { laes_close(m_laes); m_laes = nullptr; } if (m_track) { lae_close(m_track); m_track = nullptr; } }

    // the first play of a file content with a master: every master is decoded and checked in one pass, with a progress window
    void checkAll() {
        laes_file_info fi; laes_get_info(m_laes, &fi); const int nm = mastersOf(m_laes); int first = -1;
        for (int i = 0; i < fi.choices && first < 0; i++) { laes_choice_info c; laes_get_choice(m_laes, i, &c); if (c.kind == LAES_KIND_MASTER) first = i; }
        char err[256]; laes_play* p = laes_start_all(m_laes, first, 0, err, sizeof err); if (!p) return;
        laes_choice_info c; laes_get_choice(m_laes, first, &c);
        kodi::gui::dialogs::CExtendedProgress dlg(kodi::addon::GetLocalizedString(30100)); dlg.SetText(kodi::addon::GetLocalizedString(30101) + " (" + std::to_string(nm) + ")");
        std::vector<int32_t> b(65536 * 2); uint64_t got = 0; int last = -1; int64_t k;
        while ((k = laes_read(p, b.data(), 65536)) > 0) { got += (uint64_t)k; const int pc = (int)(100.0 * (double)got / (double)std::max<uint64_t>(1, c.frames)); if (pc != last) { dlg.SetPercentage((float)pc); last = pc; } }
        if (laes_verified(p) >= 0) for (int m = 0; m < nm; m++) { const int r = laes_master_ok(p, m); if (r >= 0) g_verified.put(fi.file_sha256, m, r == 1); }
        dlg.MarkFinished(); laes_stop(p); }

    bool startLaes() {
        laes_file_info fi; laes_get_info(m_laes, &fi); m_choice = pick(m_laes, kodi::addon::GetSettingInt("layers"), rateSetting(), bitsSetting());
        laes_choice_info c; laes_get_choice(m_laes, m_choice, &c);
        if (c.master >= 0) { if (g_verified.get(fi.file_sha256, c.master) == 0) checkAll();
            // the core master plays instead of a master that did not match its SHA-256 (spec 1.3), and the user is told which and why
            const int v = g_verified.get(fi.file_sha256, c.master);
            if (v != 1) { const int num = laes_master_number(m_laes, c.master); std::string msg = kodi::addon::GetLocalizedString(v < 0 ? 30102 : 30103);
                const size_t at = msg.find("%d"); if (at != std::string::npos) msg.replace(at, 2, std::to_string(num));
                kodi::Log(ADDON_LOG_WARNING, "LAES: master %d %s: the core master plays", num, v < 0 ? "did not pass its SHA-256 check" : "could not be checked");
                kodi::QueueNotification(QUEUE_WARNING, kodi::addon::GetLocalizedString(30100), msg); m_choice = 0; laes_get_choice(m_laes, 0, &c); } }
        char err[256]; m_play = laes_start_at(m_laes, m_choice, 0, 0, err, sizeof err); if (!m_play) m_play = laes_start(m_laes, m_choice, 0, err, sizeof err);
        if (!m_play) { kodi::Log(ADDON_LOG_ERROR, "LAES: %s", err); return false; }
        m_rate = c.rate; m_bits = c.bits; m_frames = c.frames;
        kodi::Log(ADDON_LOG_INFO, "LAES: plays %s", c.name);
        // block positions for seeking: the saved record, else one entropy-decoding pass on a thread of its own, then saved
        const std::string sha = hex(fi.file_sha256, 32); laes_file* f = m_laes;
        m_indexer = std::thread([this, sha, f] { FpPin pin; try {   // an exception here (memory) would end Kodi: without the record a seek decodes from the start
            const std::vector<uint8_t> d = readRecord(sha);
            if (!d.empty() && laes_index_load(f, d.data(), (int64_t)d.size()) == 0) return;
            laes_index_layers(f, &m_cancel, 0); if (m_cancel) return;
            const int64_t n = laes_index_save(f, nullptr, 0); if (n <= 0) return; std::vector<uint8_t> b((size_t)n); if (laes_index_save(f, b.data(), n) != n) return;
            writeRecord(sha, b); } catch (...) {} });
        return true; }

    bool startLaey() {
        char err[512] = ""; if (!lae_selftest()) { kodi::Log(ADDON_LOG_ERROR, "LAE: decoder self-test failed"); return false; }
        m_track = lae_open_reader(&Src::read, m_src.get(), m_src->size, err, sizeof err); if (!m_track) { kodi::Log(ADDON_LOG_ERROR, "LAE: %s", err); return false; }
        // LAEY extension packs: the pack number the user set (0 = the main body); a file without that pack plays pack 0. Never chosen automatically.
        // The log names a pack as the settings list does: "Pack n" = pack number n - 1 (Pack 1 = the main body).
        { const int pk = kodi::addon::GetSettingInt("pack"); if (pk > 0 && pk < lae_pack_count(m_track) && lae_select_pack(m_track, pk) != 0) kodi::Log(ADDON_LOG_WARNING, "LAE: Pack %d cannot be used; Pack 1 plays", pk + 1); }
        const int layers = kodi::addon::GetSettingInt("layers"); const int route = layers == 2 ? LAE_ROUTE_CD : layers == 1 ? LAE_ROUTE_CD24 : layers == 3 ? LAE_ROUTE_16 : LAE_ROUTE_FULL;
        lae_info li; lae_get_info(m_track, &li); int out = 0; const int cap = rateSetting(); if ((route == LAE_ROUTE_FULL || route == LAE_ROUTE_16) && cap > 0) { out = li.master_rate; while (out > cap && out > li.base_rate) out /= 2; }
        if (lae_start(m_track, route, out, err, sizeof err) != 0 && lae_start(m_track, LAE_ROUTE_CD, 0, err, sizeof err) != 0) { kodi::Log(ADDON_LOG_ERROR, "LAE: %s", err); return false; }
        int rate, bits; uint64_t frames; lae_output_format(m_track, &rate, &bits, &frames); m_rate = rate; m_bits = bits == 16 ? 16 : 24; m_frames = frames; return true; }
};

class CLaeAddon : public kodi::addon::CAddonBase {
public:
    ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance, KODI_ADDON_INSTANCE_HDL& hdl) override {
        if (instance.IsType(ADDON_INSTANCE_AUDIODECODER)) { hdl = new CLaeDecoder(instance); return ADDON_STATUS_OK; } return ADDON_STATUS_UNKNOWN; }
};
}  // namespace

ADDONCREATOR(CLaeAddon)
