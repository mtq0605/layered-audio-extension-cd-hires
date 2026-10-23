// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// foo_input_lae: LAE (Layered Audio Extension) decoder for foobar2000 1.4 / 1.5 (and the 32-bit foobar2000 2.x).
// Plays LAE v5 WAV files (.wav with an "LAE " chunk after the CD layer, or the same file named .lae) through liblae
// (src/lae_api.h, compiled into this DLL). WAV files without an LAE chunk are refused at once, so foobar2000 hands them to its
// standard WAV decoder. Rules of the LAE reader (spec/cdx/SPEC.md R17, R20): a track is verified completely before it
// plays in high resolution (chunk CRC, header and stream structure); when it cannot be, the whole track plays as the CD layer; no
// automatic switch between CD and extension inside a track (only the user's choice in the layer menu switches, at the current position).
// Built against the foobar2000 1.5 SDK (SDK-2019-12-27, BSD-style licence, not part of this repository; see README.md here).
// Layers: menu Playback > LAE (applies at once); the same settings in Preferences > Advanced > Decoding > LAE decoder.
// Texts are Chinese when the Windows display language is Chinese, English otherwise.
#include <foobar2000/SDK/foobar2000.h>
#include <xmmintrin.h>
#include <float.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include "lae_api.h"

namespace {
// ---- texts: Chinese when the Windows display language is Chinese
bool zh() { static const bool z = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE; return z; }
const char* T(const char* en, const char* cn) { return zh() ? cn : en; }
}

DECLARE_COMPONENT_VERSION(T("LAE decoder", "LAE 解码器"), "0.3",
    "LAE (Layered Audio Extension) decoder\n"
    "Plays LAE v5 WAV files: the CD layer plus the depth and frequency layers, up to the master rate (e.g. 352.8 kHz).\n"
    "Files without an LAE extension are left to the standard decoders.\n"
    "Decoder: liblae (Apache-2.0). foobar2000 SDK (c) Peter Pawlowski, BSD-style licence.");
VALIDATE_COMPONENT_FILENAME("foo_input_lae.dll");

namespace {
// ---- settings
const GUID guid_branch = { 0x736992c4, 0x2a6d, 0x4b6c, { 0xbf, 0x6c, 0x89, 0x37, 0xb8, 0xfa, 0x64, 0x29 } };
const GUID guid_layers = { 0x86633863, 0xc826, 0x4270, { 0xb8, 0x3d, 0x6a, 0x17, 0xda, 0x17, 0xe2, 0x99 } };
const GUID guid_maxrate = { 0xdef0b7ec, 0xae79, 0x4b8b, { 0x96, 0xab, 0x8e, 0x46, 0x1f, 0x25, 0x3d, 0x77 } };
const GUID guid_autoorder = { 0xe1eb67b6, 0xd817, 0x412a, { 0xb9, 0x9f, 0x0f, 0x3a, 0x6e, 0x0d, 0x46, 0xfc } };
advconfig_branch_factory g_branch(T("LAE decoder", "LAE 解码器"), guid_branch, advconfig_branch::guid_branch_decoding, 0);
advconfig_integer_factory g_layers(T("Layers: 0 = all layers, 1 = CD layer + depth layer, 2 = CD layer only", "播放的层：0 = 全部层，1 = CD 层 + 深度层，2 = 只放 CD 层"),
    guid_layers, guid_branch, 0, 0, 0, 2);
advconfig_integer_factory g_maxrate(T("Highest output sample rate in Hz (0 = the master rate; all layers are decoded to the highest octave of the master rate "
    "not above it, but never below twice the CD rate)", "最高输出采样率 Hz（0 = 母带采样率；全部层解到不超过它的最高一档，但不低于 CD 采样率的 2 倍）"),
    guid_maxrate, guid_branch, 1, 0, 0, 1536000);
advconfig_checkbox_factory g_autoOrder(T("Put LAE ahead of decoders that also take .wav files (automatically)", "自动把 LAE 排在其他接收 .wav 的解码器前面"),
    guid_autoorder, guid_branch, 2, true);

// ---- the decoder's identity in foobar2000's decoder priority list. foobar2000 puts a decoder it has never seen at the top of a saved
// list, but builds the first list of a new profile in no fixed order. When a decoder that also takes .wav files is ahead of LAE and the
// user did not put it there (first start of a new profile, or a newly installed decoder), LAE moves on to its next GUID, which foobar2000
// then sees as new and puts at the top (after a restart). The old GUID stays at the bottom of the list as an unnamed old decoder.
const GUID guid_input[] = {
    { 0x5c5f5ced, 0xb09e, 0x4a4f, { 0xa1, 0xe0, 0x40, 0x10, 0x07, 0x74, 0x8f, 0x57 } },
    { 0x936d4fef, 0x1f61, 0x45ea, { 0x8d, 0x7e, 0x1f, 0xcf, 0x07, 0x51, 0x5e, 0x7b } },
    { 0x5c2425f9, 0x80b3, 0x4b50, { 0x91, 0x0d, 0x7f, 0x0d, 0xac, 0x04, 0xf2, 0x5f } },
    { 0x256b21b8, 0x3a2f, 0x4262, { 0x91, 0x9d, 0xe5, 0x58, 0xa4, 0x25, 0x5d, 0x74 } } };
const unsigned kInputGuids = sizeof guid_input / sizeof guid_input[0];
const GUID guid_cfg_slot = { 0x5f1dc4d6, 0x525a, 0x4b5d, { 0x90, 0x16, 0xdb, 0x41, 0x2d, 0x5e, 0x87, 0x7e } };
const GUID guid_cfg_promoted = { 0x108e268b, 0xf67e, 0x4000, { 0x88, 0x1a, 0xab, 0x59, 0x32, 0x0b, 0x54, 0x12 } };
const GUID guid_cfg_known = { 0xfb1a94ae, 0x3079, 0x4cb7, { 0xbd, 0xd7, 0x41, 0x24, 0xcd, 0x18, 0xcf, 0x6d } };
// the GUID slot: read once from the configuration and fixed for the session (foobar2000 knows a decoder by one GUID per session);
// a move is saved for the next start
class cfg_slot : public cfg_var {
public:
    explicit cfg_slot(const GUID& g) : cfg_var(g) {}
    t_uint32 current = 0, next = 0;
protected:
    void get_data_raw(stream_writer* s, abort_callback& a) { s->write_lendian_t(next, a); }
    void set_data_raw(stream_reader* s, t_size, abort_callback& a) { t_uint32 v = 0; s->read_lendian_t(v, a); current = next = std::min<t_uint32>(v, kInputGuids - 1); }
};
cfg_slot g_slot(guid_cfg_slot);
cfg_bool g_promoted(guid_cfg_promoted, false);   // LAE has moved itself at least once (a record; the decision uses g_known)
cfg_string g_known(guid_cfg_known, "");          // GUIDs of the decoders seen at the last start (to tell a newly installed one)

// ---- the decoder needs strict IEEE-754 double arithmetic: round to nearest, no flush-to-zero, 53-bit x87 precision (x86)
struct FpPin {   // x87 and SSE are saved and restored separately (_controlfp_s would report and write back a mix of the two)
    unsigned mx; unsigned x87 = 0;
    FpPin() : mx(_mm_getcsr()) {
#if defined(_M_IX86)
        unsigned cur; __control87_2(0, 0, &x87, nullptr); __control87_2(_RC_NEAR | _PC_53, _MCW_RC | _MCW_PC, &cur, nullptr);
#endif
        _mm_setcsr(0x1F80u); }
    ~FpPin() {
#if defined(_M_IX86)
        unsigned cur; __control87_2(x87, _MCW_RC | _MCW_PC, &cur, nullptr);
#endif
        _mm_setcsr(mx); }
};
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// liblae reads the file through foobar2000's file object (archives, abort); exceptions are carried across the C interface by hand
struct FileReader {
    service_ptr_t<file> f; abort_callback& abort; std::exception_ptr ep;
    static int LAE_CALL read(void* ctx, uint64_t offset, void* buf, size_t len) {
        FileReader* r = (FileReader*)ctx;
        try { r->f->seek(offset, r->abort); r->f->read_object(buf, len, r->abort); return 0; } catch (...) { r->ep = std::current_exception(); return -1; } }
};
std::string khz(int hz) { char s[32]; snprintf(s, sizeof s, hz % 1000 ? "%.1f kHz" : "%.0f kHz", hz / 1000.0); return s; }
}

class input_lae : public input_stubs {
public:
    ~input_lae() {
        if (m_track) { FpPin pin; lae_close(m_track); }
        if (m_playback) { std::lock_guard<std::mutex> g(s_lock); s_live.erase(std::remove(s_live.begin(), s_live.end(), this), s_live.end()); } }
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
        if (p_reason == input_open_info_write) throw exception_io_unsupported_format();   // tags are not written
        m_file = p_filehint; input_open_file_helper(m_file, p_path, p_reason, p_abort); m_path = p_path;
        // cheap check first: a RIFF/WAVE whose chunk list contains "LAE " (anything else goes to the next decoder)
        const t_filesize size = m_file->get_size(p_abort);
        if (size == filesize_invalid || size < 12 + 8 + 24 || size > 0xFFFFFFFFull + 8) throw exception_io_unsupported_format();
        uint8_t h[12]; m_file->read_object(h, 12, p_abort); if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) throw exception_io_unsupported_format();
        t_filesize pos = 12, laeAt = 0;
        while (pos + 8 <= size) { uint8_t c[8]; m_file->seek(pos, p_abort); m_file->read_object(c, 8, p_abort); const t_filesize n = le32(c + 4);
            if (!memcmp(c, "LAE ", 4)) { laeAt = pos + 8; break; } pos += 8 + n + (n & 1); }
        if (!laeAt || laeAt + 24 > size) throw exception_io_unsupported_format();
        uint8_t hd[24]; m_file->seek(laeAt, p_abort); m_file->read_object(hd, 24, p_abort);
        if (le32(hd) != 0x3145414Cu || le32(hd + 4) != 5) throw exception_io_unsupported_format();   // not container v5
        m_master = (int)le32(hd + 8); m_base = (int)le32(hd + 12); m_stages = (int)le32(hd + 16); m_masterFrames = le32(hd + 20);
        if ((m_base != 44100 && m_base != 48000) || m_stages < 1 || m_stages > 3 || m_master != (m_base << m_stages)) throw exception_io_unsupported_format();
        m_size = size;
        if (p_reason == input_open_decode) load(p_abort);
    }
    void get_info(file_info& p_info, abort_callback&) {
        p_info.set_length((double)m_masterFrames / m_master);
        const bool cd = m_cdOnly || m_fallbackNow;   // known only to a decoding instance
        p_info.info_set_int("samplerate", cd ? m_base : outRate());
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", cd || layers() == 2 ? 16 : 24);
        p_info.info_set("encoding", "lossless");
        p_info.info_set("codec", "LAE");
        p_info.info_set("lae_master", pfc::string_formatter() << m_master << " Hz");
        p_info.info_set("lae_cd_layer", pfc::string_formatter() << "16 bit / " << m_base << " Hz");
        p_info.info_set("lae_layers", (std::string(T("CD layer 16 bit / ", "CD 层 16 bit / ")) + khz(m_base) + T(" + depth layer 24 bit + frequency layer up to ",
            " + 深度层 24 bit + 频率层 至 ") + khz(m_master)).c_str());
        p_info.info_set_bitrate((t_int64)((m_size * 8) / ((double)m_masterFrames / m_master) / 1000.0 + 0.5));   // the whole file's average
    }
    t_filestats get_file_stats(abort_callback& p_abort) { return m_file->get_stats(p_abort); }
    void decode_initialize(unsigned p_flags, abort_callback& p_abort) {
        if (!m_track && !m_cdOnly) load(p_abort);
        // integrity tests (input.h): a track that can only play as its CD layer fails, even when only the CD layer was asked for
        m_testing = (p_flags & input_flag_testing_integrity) != 0;
        if (m_testing && m_cdOnly) throw exception_io_data((std::string("LAE: ") + m_fallback).c_str());
        if ((p_flags & input_flag_playback) && !m_playback) { m_playback = true; std::lock_guard<std::mutex> g(s_lock); s_live.push_back(this); }
        m_pos = 0; m_fallbackNow = false;
        if (m_cdOnly) { m_rate = m_base; m_bits = 16; m_fallbackNow = true; m_route = LAE_ROUTE_CD; m_report = true; return; }
        startRoute(m_testing);
    }
    bool decode_run(audio_chunk& p_chunk, abort_callback&) {
        enum { block = 4096 };
        applyLayerChange();   // e.g. the next track was opened before the user changed the layers
        if (m_cdOnly) {   // the CD layer itself (the LAE extension could not be verified)
            const size_t frames = m_cd.size() / 2; if (m_pos >= frames) return false; const size_t n = (std::min)((size_t)block, frames - m_pos);
            p_chunk.set_data_fixedpoint(&m_cd[m_pos * 2], n * 4, m_rate, 2, 16, audio_chunk::channel_config_stereo); m_pos += n; return true; }
        int64_t got; { FpPin pin; m_buf.resize(2 * block); got = lae_read(m_track, m_buf.data(), block); }
        if (got < 0) throw exception_io_data("LAE: stream error (extension layer damaged)");   // never switch to the CD layer inside a track
        if (got == 0) return false;
        const int bps = m_bits / 8; m_bytes.resize((size_t)got * 2 * bps);
        for (size_t i = 0; i < (size_t)got * 2; i++) { const int32_t v = m_buf[i]; for (int k = 0; k < bps; k++) m_bytes[i * bps + k] = (uint8_t)(v >> (8 * k)); }
        p_chunk.set_data_fixedpoint(m_bytes.data(), m_bytes.size(), m_rate, 2, m_bits, audio_chunk::channel_config_stereo);
        m_pos += (size_t)got; countBitrate((uint64_t)got); return true;
    }
    void decode_seek(double p_seconds, abort_callback&) {
        applyLayerChange();   // the layer menu seeks to the current position, so the change is heard at once
        const uint64_t frame = audio_math::time_to_samples(p_seconds, m_rate);
        if (m_cdOnly) { m_pos = (size_t)(std::min)((uint64_t)(m_cd.size() / 2), frame); return; }
        FpPin pin; if (lae_seek(m_track, frame) != 0) throw exception_io_data("LAE: seek failed"); m_pos = (size_t)frame; resetBitrate();
    }
    bool decode_can_seek() { return true; }
    // the layers and format that actually play, once per start (the file's info describes what the settings ask for): codec "LAE" + the
    // layers is what the status bar shows; lae_route gives the reason when the CD layer plays instead. And the live bitrate, once a second.
    bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
        if (!m_report && !m_reportRate) return false;
        p_timestamp_delta = 0;
        if (m_report) {
            p_out.info_set("codec", routeLabel().c_str()); p_out.info_set_int("samplerate", m_rate); p_out.info_set_int("bitspersample", m_bits);
            if (m_fallbackNow) p_out.info_set("lae_route", (std::string(T("CD layer (", "CD 层（")) + m_fallback + T(")", "）")).c_str()); else p_out.info_remove("lae_route");
            if (m_route == LAE_ROUTE_CD) p_out.info_set_bitrate_vbr((t_int64)std::lround(cdKbps())); }
        if (m_reportRate) p_out.info_set_bitrate_vbr(m_kbps);
        m_report = m_reportRate = false; return true; }
    bool decode_get_dynamic_info_track(file_info&, double&) { return false; }
    void decode_on_idle(abort_callback& p_abort) { m_file->on_idle(p_abort); }
    void retag(const file_info&, abort_callback&) { throw exception_io_unsupported_format(); }
    void remove_tags(abort_callback&) { throw exception_io_unsupported_format(); }
    // a forward seek decodes the entropy streams up to the target (no seek table in v5): tell the host to avoid needless seeks
    size_t extended_param(const GUID& p_type, size_t, void*, size_t) { return p_type == input_params::seeking_expensive ? 1 : 0; }

    static bool g_is_our_content_type(const char*) { return false; }
    static bool g_is_our_path(const char*, const char* p_extension) { return stricmp_utf8(p_extension, "wav") == 0 || stricmp_utf8(p_extension, "lae") == 0; }
    static const char* g_get_name() { return T("LAE decoder", "LAE 解码器"); }
    static const GUID g_get_guid() { return guid_input[g_slot.current]; }
    static std::atomic<unsigned> s_layerGen;   // bumped by the layer menu
    // whether the playback decoder of this path would play other layers with the current settings (the layer menu seeks only then)
    static bool s_wouldChange(const char* path) {
        std::lock_guard<std::mutex> g(s_lock);
        for (const input_lae* p : s_live) if ((!path || !strcmp(p->m_path.c_str(), path)) && p->wouldChange()) return true;
        return false; }   // path null: any playback decoder (a cue-sheet track's path is the .cue, not the audio file)

private:
    static std::mutex s_lock; static std::vector<const input_lae*> s_live;   // decoders opened for playback
    int layers() const { return (int)g_layers.get(); }
    int askedRoute() const { return layers() == 2 ? LAE_ROUTE_CD : layers() == 1 ? LAE_ROUTE_CD24 : LAE_ROUTE_FULL; }
    int outRate() const {   // all layers: the highest octave of the master not above the setting (and above the base rate)
        if (layers() != 0) return m_base;
        const int cap = (int)g_maxrate.get(); int r = m_master;
        while (cap > 0 && r > cap && (r >> 1) > m_base) r >>= 1;
        return r; }
    bool wouldChange() const {   // called under s_lock from the menu (main thread): reads only the atomics the decoding thread publishes
        if (m_cdOnly || m_fallbackNowA.load()) return false;   // a track that fell back to the CD layer keeps it (no second attempt mid-track)
        const int r = askedRoute();
        return r != m_routeNow.load() || (r == LAE_ROUTE_FULL && outRate() != m_rateNow.load()); }
    double cdKbps() const { return m_base * 32 / 1000.0; }   // the CD layer: 16 bit x 2 channels, uncompressed
    // starts the route the settings ask for. On failure the whole track plays as the CD layer, unless noFallback (an integrity test, or a
    // layer change after audio has played: then the track stops with an error, as on a stream error, rather than switching to CD)
    void startRoute(bool noFallback) {
        FpPin pin; char err[512] = ""; m_gen = s_layerGen.load(); m_fallbackNow = false;
        const int route = askedRoute(); m_route = route;
        if (lae_start(m_track, route, route == LAE_ROUTE_FULL ? outRate() : 0, err, sizeof err) != 0) {
            if (noFallback) throw exception_io_data((std::string("LAE: ") + err).c_str());
            console::formatter() << "LAE: " << err << " -> the whole track plays as the CD layer";
            m_fallback = err; m_fallbackNow = true; m_route = LAE_ROUTE_CD;
            if (lae_start(m_track, LAE_ROUTE_CD, 0, err, sizeof err) != 0) throw exception_io_data(err); }
        uint64_t frames = 0; lae_output_format(m_track, &m_rate, &m_bits, &frames);
        m_routeNow = m_route; m_rateNow = m_rate; m_fallbackNowA = m_fallbackNow; resetBitrate();
        m_report = true;   // the status bar shows the layers that actually play (decode_get_dynamic_info)
    }
    // the user chose other layers in the menu: a playback decoder continues at the same time position with them (a user's choice, not a
    // fallback). Conversion, ReplayGain scans and integrity tests keep the layers they started with.
    void applyLayerChange() {
        if (!m_playback || m_testing || m_cdOnly || !m_track || m_gen == s_layerGen.load()) return;
        const unsigned gen = s_layerGen.load();   // the generation before the settings are read (as in startRoute)
        if (!wouldChange()) { m_gen = gen; return; }
        const double t = m_rate > 0 ? (double)m_pos / m_rate : 0;
        startRoute(m_pos > 0);
        const uint64_t frame = audio_math::time_to_samples(t, m_rate);
        FpPin pin; if (frame && lae_seek(m_track, frame) != 0) throw exception_io_data("LAE: seek failed"); m_pos = (size_t)frame; resetBitrate();
    }
    // live bitrate: the CD layer's fixed rate + the extension bytes the decoder consumed per second of output. After a start or seek the
    // window begins after the first block (that block may include the entropy catch-up to the target: not this second's audio)
    void resetBitrate() { m_winFrames = 0; m_needBase = true; }
    void countBitrate(uint64_t frames) {
        if (m_route == LAE_ROUTE_CD || m_rate <= 0) return;
        if (m_needBase) { uint64_t b = 0; if (lae_stream_bytes(m_track, &b) == 0) { m_winBytes = b; m_winFrames = 0; m_needBase = false; } return; }
        m_winFrames += frames; if (m_winFrames < (uint64_t)m_rate) return;
        uint64_t b = 0; if (lae_stream_bytes(m_track, &b) != 0) return;
        if (b >= m_winBytes) { m_kbps = (t_int64)std::lround(cdKbps() + (double)(b - m_winBytes) * 8.0 * m_rate / (double)m_winFrames / 1000.0); m_reportRate = true; }
        m_winBytes = b; m_winFrames = 0; }
    std::string routeLabel() const {
        if (m_fallbackNow) return T("LAE CD layer (extension unusable)", "LAE CD 层（扩展层不可用）");
        if (m_route == LAE_ROUTE_CD) return T("LAE CD layer", "LAE CD 层");
        if (m_route == LAE_ROUTE_CD24) return T("LAE CD + depth layer", "LAE CD 层 + 深度层");
        return T("LAE all layers", "LAE 全部层"); }
    // the track, verified by liblae (read through the file object: only the CD layer and the LAE chunk are held, about the file size);
    // on failure the CD layer of the WAV plays (whole track)
    void load(abort_callback& p_abort) {
        char err[512] = "";
        { FpPin pin; if (!lae_selftest()) strcpy_s(err, "decoder self-test failed (floating-point state)");
          else { FileReader r{m_file, p_abort}; m_track = lae_open_reader(&FileReader::read, &r, m_size, err, sizeof err); if (r.ep) std::rethrow_exception(r.ep); } }
        if (m_track) return;
        console::formatter() << "LAE: " << err << " -> the whole track plays as the CD layer";
        m_cdOnly = true; m_fallback = err;
        // the CD layer, chosen by liblae's rules (the last 'fmt ' and the last 'data' chunk; sizes clamped to the file; 64-bit positions)
        t_filesize p = 12, dOff = 0, dLen = 0; bool haveFmt = false, haveData = false, pcm16 = false;
        while (m_size - p >= 8) { uint8_t c[8]; m_file->seek(p, p_abort); m_file->read_object(c, 8, p_abort);
            const t_filesize body = p + 8; t_filesize n = le32(c + 4); if (n > m_size - body) n = m_size - body;
            if (!memcmp(c, "fmt ", 4)) { if (n < 16) throw exception_io_data("LAE: bad fmt chunk");
                uint8_t f[26]; const size_t k = n >= 26 ? 26 : 16; m_file->read_object(f, k, p_abort);
                int fmt = f[0] | f[1] << 8; if (fmt == 0xFFFE && k == 26) fmt = f[24] | f[25] << 8;   // WAVE_FORMAT_EXTENSIBLE: the sub-format
                pcm16 = fmt == 1 && (f[2] | f[3] << 8) == 2 && (f[14] | f[15] << 8) == 16; haveFmt = true; }
            else if (!memcmp(c, "data", 4)) { dOff = body; dLen = n; haveData = true; }
            if (n + (n & 1) >= m_size - body) break; p = body + n + (n & 1); }
        if (!haveFmt || !haveData) throw exception_io_data("LAE: no audio data");
        if (!pcm16) throw exception_io_data("LAE: the CD layer is not 16-bit stereo PCM");
        const t_filesize len = dLen / 4 * 2;
        try { m_cd.resize((size_t)len); } catch (...) { throw exception_io_data("LAE: the file is too large for this decoder"); }
        m_file->seek(dOff, p_abort); m_file->read_object(m_cd.data(), (size_t)len * 2, p_abort);   // little-endian: the bytes are the samples
    }
    service_ptr_t<file> m_file; t_filesize m_size = 0; pfc::string8 m_path;
    int m_master = 0, m_base = 0, m_stages = 0, m_rate = 0, m_bits = 24, m_route = LAE_ROUTE_FULL; uint64_t m_masterFrames = 0;
    std::atomic<int> m_routeNow{LAE_ROUTE_FULL}, m_rateNow{0};   // m_route / m_rate published for the menu thread
    lae_track* m_track = nullptr; std::vector<int16_t> m_cd; size_t m_pos = 0;
    bool m_cdOnly = false;        // the extension could not be verified: the whole track plays from m_cd
    bool m_fallbackNow = false;   // the current decode plays the CD layer (m_cdOnly, or lae_start failed); m_fallback says why
    bool m_report = false, m_reportRate = false, m_playback = false, m_testing = false; unsigned m_gen = 0; std::string m_fallback;
    std::atomic<bool> m_fallbackNowA{false};   // m_fallbackNow published for the menu thread
    uint64_t m_winFrames = 0, m_winBytes = 0; t_int64 m_kbps = 0; bool m_needBase = true;
    std::vector<int32_t> m_buf; std::vector<uint8_t> m_bytes;
};
std::atomic<unsigned> input_lae::s_layerGen{0};
std::mutex input_lae::s_lock;
std::vector<const input_lae*> input_lae::s_live;

static input_singletrack_factory_t<input_lae> g_input_lae_factory;
DECLARE_FILE_TYPE("LAE files", "*.LAE");

namespace {
// ---- menu Playback > LAE: which layers play; applies at once (the playing LAE track switches at the same position)
struct Preset { int layers, maxrate; const char* en; const char* cn; GUID guid; };
const Preset kPresets[] = {
    { 0, 0,      "All layers (master rate)",            "全部层（母带采样率）",      { 0x166cde31, 0x21dd, 0x4611, { 0x98, 0x56, 0xb8, 0x64, 0x4a, 0xda, 0x01, 0x9b } } },
    { 0, 192000, "All layers, up to 192 kHz",           "全部层，最高 192 kHz",     { 0xed163c2d, 0x153a, 0x4c28, { 0xa4, 0x5a, 0x54, 0x4f, 0x9c, 0xee, 0x60, 0x9f } } },
    { 0, 96000,  "All layers, up to 96 kHz",            "全部层，最高 96 kHz",      { 0xbc3a37e8, 0x3753, 0x4e35, { 0xab, 0x9a, 0xbc, 0xfb, 0x0d, 0xde, 0x74, 0xfc } } },
    { 1, 0,      "CD layer + depth layer (24 bit)",     "CD 层 + 深度层（24 bit）", { 0xaf8688f3, 0x90df, 0x4278, { 0xb6, 0x13, 0xef, 0x14, 0x73, 0x5d, 0xd5, 0xaf } } },
    { 2, 0,      "CD layer only (16 bit)",              "只放 CD 层（16 bit）",     { 0x89ee1614, 0xb19d, 0x4977, { 0x82, 0x8c, 0xde, 0x84, 0xc4, 0xed, 0x26, 0xbb } } },
};
const unsigned kPresetCount = sizeof kPresets / sizeof kPresets[0];
const GUID guid_menu = { 0x52c9c179, 0xeccd, 0x44ec, { 0x89, 0x77, 0x66, 0x05, 0xd9, 0x6e, 0xe1, 0x83 } };
mainmenu_group_popup_factory g_menuGroup(guid_menu, mainmenu_groups::playback, mainmenu_commands::sort_priority_dontcare, "LAE");

int currentPreset() {
    const int l = (int)g_layers.get(), r = (int)g_maxrate.get();
    for (unsigned i = 0; i < kPresetCount; i++) if (kPresets[i].layers == l && (l != 0 || kPresets[i].maxrate == r)) return (int)i;
    return -1; }

class lae_menu : public mainmenu_commands {
public:
    t_uint32 get_command_count() { return kPresetCount; }
    GUID get_command(t_uint32 i) { return kPresets[i].guid; }
    void get_name(t_uint32 i, pfc::string_base& out) { out = T(kPresets[i].en, kPresets[i].cn); }
    bool get_description(t_uint32, pfc::string_base& out) { out = T("Layers LAE files play with (applies at once)", "LAE 文件播放哪些层（立即生效）"); return true; }
    GUID get_parent() { return guid_menu; }
    t_uint32 get_sort_priority() { return sort_priority_base; }   // the presets keep their order (equal priorities: same group order)
    bool get_display(t_uint32 i, pfc::string_base& text, t_uint32& flags) { get_name(i, text); flags = currentPreset() == (int)i ? flag_radiochecked : 0; return true; }
    void execute(t_uint32 i, service_ptr_t<service_base>) {
        g_layers.set(kPresets[i].layers); g_maxrate.set(kPresets[i].maxrate); input_lae::s_layerGen++;
        // when the audible track's playback decoder would play other layers: a seek to the current position empties the playback buffer
        // and reaches that decoder, which switches there (other LAE decoders switch on their next block; a non-LAE track is left alone)
        auto pc = playback_control::get(); metadb_handle_ptr np;
        if (!pc->get_now_playing(np) || !pc->playback_can_seek()) return;
        bool lae = false; { metadb_info_container::ptr ic; if (np->get_info_ref(ic)) { const char* c = ic->info().info_get("codec"); lae = c && !strcmp(c, "LAE"); } }
        if (input_lae::s_wouldChange(np->get_path()) || (lae && input_lae::s_wouldChange(nullptr))) pc->playback_seek(pc->playback_get_position());
    }
};
static mainmenu_commands_factory_t<lae_menu> g_lae_menu;

// ---- decoder order: a decoder ahead of LAE that also takes .wav files would get LAE .wav files first (and play their CD layer only)
std::set<std::string> splitKnown(const char* s) { std::set<std::string> r; std::string cur; for (const char* p = s; ; p++) { if (!*p || *p == ';') { if (!cur.empty()) r.insert(cur); cur.clear(); if (!*p) break; } else cur += *p; } return r; }
class lae_order : public initquit {
public:
    void on_init() {
        fb2k::inMainThread([] {
            // every installed decoder (to tell a newly installed one next time), and the enabled ones in the user's order
            std::set<std::string> installed; { service_enum_t<input_entry> e; input_entry::ptr p;
                while (e.next(p)) { input_entry_v2::ptr v2; if (p->service_query_t(v2)) installed.insert(pfc::print_guid(v2->get_guid()).get_ptr()); } }
            const std::set<std::string> known = splitKnown(g_known.get_ptr());
            { std::string s; for (const std::string& g : installed) { s += g; s += ';'; } g_known = s.c_str(); }
            if (!g_autoOrder.get()) return;
            input_manager_v3::ptr im; if (!input_manager::get()->service_query_t(im)) return;   // foobar2000 before 1.5
            pfc::list_t<input_entry::ptr> list; im->get_enabled_inputs(list);
            // the decoders ahead of LAE that also take .wav files: the first one's name, and whether any was installed since the last start
            const GUID mine = input_lae::g_get_guid(); bool found = false, ahead = false, fresh = false; pfc::string8 who;
            for (size_t i = 0; i < list.get_count(); i++) { input_entry_v2::ptr v2; const bool isV2 = list[i]->service_query_t(v2);
                if (isV2 && v2->get_guid() == mine) { found = true; break; }
                if (!list[i]->is_our_path("C:\\x.wav", "wav")) continue;
                ahead = true; if (who.is_empty() && isV2) who = v2->get_name();
                if (isV2 && !known.empty() && !known.count(pfc::print_guid(v2->get_guid()).get_ptr())) fresh = true; }
            if (!found || !ahead) return;   // LAE disabled by the user, or first among the decoders that take .wav
            // move up when LAE looks at this profile's order for the first time (the order foobar2000 made up, or the order before 0.3), or when
            // a decoder installed since the last start was put ahead; a known decoder ahead of LAE later on is the user's arrangement
            if (who.is_empty()) who = T("a decoder that takes .wav files", "一个接收 .wav 文件的解码器");
            if (!(known.empty() || fresh) || g_slot.current + 1 >= kInputGuids) {
                console::formatter() << T("LAE: a decoder ahead of LAE also takes .wav files: ", "LAE：排在 LAE 前面、也接收 .wav 的解码器：") << who; return; }
            g_slot.next = g_slot.current + 1; g_promoted = true; standard_commands::main_saveconfig();   // saved now: quitting may crash (other components)
            pfc::string8 msg;
            if (zh()) msg << "解码器列表里，“" << who << "”排在 LAE 前面并接收 .wav 文件，LAE 的 .wav 文件可能只能放出 CD 层。\n\n"
                             "已经把 LAE 调到列表最前面，重启 foobar2000 后生效。\n\n现在重启 foobar2000 吗？";
            else msg << "\"" << who << "\" is ahead of LAE in the decoder list and takes .wav files, so LAE .wav files may play as their CD layer only.\n\n"
                        "LAE has been moved to the top of the list; this takes effect when foobar2000 restarts.\n\nRestart foobar2000 now?";
            if (uMessageBox(core_api::get_main_window(), msg, T("LAE decoder", "LAE 解码器"), MB_YESNO | MB_ICONINFORMATION) == IDYES) standard_commands::main_restart();
        });
    }
};
static initquit_factory_t<lae_order> g_lae_order;
}
