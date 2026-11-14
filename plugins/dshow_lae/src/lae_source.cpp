// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE source filter for DirectShow (2026-11-03): PotPlayer, MPC-BE, MPC-HC, Windows Media Player (legacy) and other DirectShow players.
// Opens .lae files (and any file it is given by a player's external-filter list): LAES (LAE Lossless; WAV / RF64 / FLAC form) and LAEY
// (LAE v5 WAV), and delivers uncompressed PCM (2 channels; 16-bit, or 24-bit and 32-bit samples in 32-bit containers, WAVE_FORMAT_EXTENSIBLE; 24-bit packed in 3 bytes
// was turned into noise by MPC-BE's audio path) on one output pin, with
// seeking. Written for this project from the DirectShow interface documentation (no base classes, no third-party code).
// LAES rules, as in the foobar2000 component: a master plays as that master only after a full run matched its SHA-256; the result is kept
// by the SHA-256 of the whole file (%APPDATA%\LAE\DirectShow\laes_verified.txt); the first time, every master is checked in one pass with a
// small progress window. Block positions for seeking: laes_index\<file SHA-256>.idx in the same folder (never written into the media file).
// What plays: HKCU\Software\LAE\DirectShow, DWORD values layers (0 all, 1 CD or core layer + depth layer, 2 CD or core layer only, 3 CD or core layer + frequency layer without the depth layer), maxrate (0 = no limit),
// maxbits (0 = no limit, 16, 24, 32).
#include <windows.h>
#include <commctrl.h>
#include <dshow.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <initguid.h>
#include <qnetwork.h>   // IAMMediaContent (tags for the player); after initguid.h, so that its IID is defined here
#include <xmmintrin.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../../src/lae_api.h"
#include "../../../src/lae_flacmeta.h"   // the FLAC form of LAEY: structure and Vorbis comments (2026-11-10)
#include "../../../src/lae_wavtags.h"   // tags of the WAV form (LIST/INFO + id3), shared by the LAE player plugins

// {5B3C6E8A-2F41-4C7D-9E15-8A0C3D6B7F21}
DEFINE_GUID(CLSID_LaeSource, 0x5b3c6e8a, 0x2f41, 0x4c7d, 0x9e, 0x15, 0x8a, 0x0c, 0x3d, 0x6b, 0x7f, 0x21);

namespace {
// texts: two standalone builds, English (the standard) and Chinese (/DLAE_LANG_ZH)
#ifdef LAE_LANG_ZH
const wchar_t* const kTitle = L"LAE 解码器：LAES"; const wchar_t* const kVerifyA = L"校验 "; const wchar_t* const kVerifyB = L" 个母带（SHA-256，仅首次播放）";
#else
const wchar_t* const kTitle = L"LAE decoder: LAES"; const wchar_t* const kVerifyA = L"Verifying "; const wchar_t* const kVerifyB = L" masters (SHA-256, first playback only)";
#endif
HMODULE g_module = nullptr; std::atomic<long> g_objects{0}, g_locks{0};
struct FpPin { unsigned mx; FpPin() : mx(_mm_getcsr()) { _mm_setcsr(0x1F80u); } ~FpPin() { _mm_setcsr(mx); } };

// the folder shared by every LAE player of this user (%APPDATA%\LAE): SHA-256 results and block position records
std::wstring dataDir() { wchar_t p[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"APPDATA", p, MAX_PATH); std::wstring d = (n && n < MAX_PATH) ? std::wstring(p) : L".";
    d += L"\\LAE"; CreateDirectoryW(d.c_str(), nullptr); return d; }
DWORD setting(const wchar_t* name, DWORD def) { DWORD v = def, sz = sizeof v; RegGetValueW(HKEY_CURRENT_USER, L"Software\\LAE\\DirectShow", name, RRF_RT_REG_DWORD, nullptr, &v, &sz); return v; }
std::string hex(const unsigned char* p, int n) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }

// one whole line of the results file, of any length, without its line end; false at the end of the file. A result counts only in exactly
// the form written ("<64 hex digits> m<number> ok|fail"): a long line of another player read in pieces could hold a piece that looks like one.
bool recordLine(FILE* f, std::string& s) { s.clear(); char b[512]; bool any = false;
    while (fgets(b, sizeof b, f)) { any = true; s += b; if (s.back() == '\n') break; }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return any; }
bool resultKey(const std::string& k) { if (k.size() < 67 || k.size() > 70 || k.compare(64, 2, " m")) return false;
    for (size_t i = 0; i < k.size(); i++) { const char c = k[i]; if (i == 64 || i == 65) continue;
        if (i < 64 ? !isxdigit((unsigned char)c) : !isdigit((unsigned char)c)) return false; } return true; }
// SHA-256 results per file content and master, shared with the other LAE players: "<file SHA-256> m<k> ok|fail". Lines are only appended
// (players running at once keep each other's results); a missing result is looked up again (another player may have checked it meanwhile).
// The 0.1 test builds kept the file in %APPDATA%\LAE\DirectShow: read as well.
class Verified {
public:
    int get(const unsigned char sha[32], int m) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1); auto it = map.find(k);
        if (it == map.end()) { loaded = false; load(); it = map.find(k); } return it == map.end() ? 0 : it->second ? 1 : -1; }
    void put(const unsigned char sha[32], int m, bool ok) { std::lock_guard<std::mutex> g(mx); load(); const std::string k = hex(sha, 32) + " m" + std::to_string(m + 1);
        auto it = map.find(k); if (it != map.end() && it->second == ok) return; map[k] = ok;
        FILE* f = _wfopen((dataDir() + L"\\laes_verified.txt").c_str(), L"ab"); if (!f) return; fprintf(f, "%s %s\n", k.c_str(), ok ? "ok" : "fail"); fclose(f); }
private:
    std::mutex mx; bool loaded = false; std::map<std::string, bool> map;
    void load() { if (loaded) return; loaded = true;
        for (const std::wstring& pth : {dataDir() + L"\\DirectShow\\laes_verified.txt", dataDir() + L"\\laes_verified.txt"}) { FILE* f = _wfopen(pth.c_str(), L"rb"); if (!f) continue; std::string s;
            while (recordLine(f, s)) { const size_t a = s.rfind(' '); if (a == std::string::npos) continue; const std::string k = s.substr(0, a), v = s.substr(a + 1);
                if (resultKey(k) && (v == "ok" || v == "fail")) map[k] = v == "ok"; } fclose(f); } }
};
Verified g_verified;

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



// a small window with one progress bar, on a thread of its own (the first check of a file's masters)
class ProgressWindow {
public:
    explicit ProgressWindow(const std::wstring& text) { m_thread = std::thread([this, text] { run(text); }); while (!m_ready) Sleep(5); }
    ~ProgressWindow() { if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0); if (m_thread.joinable()) m_thread.join(); }
    void set(int percent) { if (m_bar) PostMessageW(m_bar, PBM_SETPOS, (WPARAM)percent, 0); }
private:
    std::thread m_thread; std::atomic<bool> m_ready{false}; HWND m_hwnd = nullptr, m_bar = nullptr;
    void run(const std::wstring& text) {
        INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic);
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", text.c_str(), WS_POPUP | WS_CAPTION | WS_VISIBLE | SS_CENTER, CW_USEDEFAULT, CW_USEDEFAULT, 460, 90, nullptr, nullptr, g_module, nullptr);
        if (m_hwnd) { RECT r; GetClientRect(m_hwnd, &r); m_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 10, r.bottom - 26, r.right - 20, 18, m_hwnd, nullptr, g_module, nullptr);
            SendMessageW(m_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100)); SetWindowTextW(m_hwnd, kTitle); SetWindowPos(m_hwnd, HWND_TOPMOST, 200, 200, 0, 0, SWP_NOSIZE); }
        m_ready = true; MSG msg; while (m_hwnd && GetMessageW(&msg, nullptr, 0, 0) > 0) { if (msg.message == WM_CLOSE && msg.hwnd == m_hwnd) { DestroyWindow(m_hwnd); break; } TranslateMessage(&msg); DispatchMessageW(&msg); } }
};

// ---- the decoder side: an opened file and what plays
struct Decoder {
    HANDLE h = INVALID_HANDLE_VALUE; std::mutex m; uint64_t size = 0; laes_file* laes = nullptr; laes_play* play = nullptr; lae_track* track = nullptr;
    int choice = 0, rate = 0, bits = 16; uint64_t frames = 0, pos = 0; std::thread indexer; volatile int cancel = 0;
    lae_wavtags::List tags;   // the file's tags for IAMMediaContent: WAV form from LIST/INFO + id3, FLAC form from its Vorbis comments
    static int LAE_CALL read(void* ctx, uint64_t off, void* buf, size_t len) { Decoder* d = (Decoder*)ctx; std::lock_guard<std::mutex> g(d->m);
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off; if (!SetFilePointerEx(d->h, li, nullptr, FILE_BEGIN)) return -1; uint8_t* p = (uint8_t*)buf;
        while (len) { DWORD k = 0; const DWORD want = (DWORD)(len < (1u << 30) ? len : (1u << 30)); if (!ReadFile(d->h, p, want, &k, nullptr) || k == 0) return -1; p += k; len -= k; } return 0; }
    ~Decoder() { close(); }
    void close() { cancel = 1; if (indexer.joinable()) indexer.join(); cancel = 0; FpPin pin;
        if (play) { laes_stop(play); play = nullptr; } if (laes) { laes_close(laes); laes = nullptr; } if (track) { lae_close(track); track = nullptr; }
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
    HRESULT open(const wchar_t* path) {
        FpPin pin; close(); h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr); if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER li; GetFileSizeEx(h, &li); size = (uint64_t)li.QuadPart; char err[512] = "", mg[5] = "";
        tags = lae_wavtags::read([this](uint64_t off, void* buf, size_t n) { return read(this, off, buf, n) == 0; }, size);
        laes = laes_open_reader(&Decoder::read, this, size, 0, mg, err, sizeof err);
        if (laes && tags.empty()) for (int k = 0; ; k++) { const char* key; const char* val; if (laes_get_tag(laes, k, &key, &val) != 0) break; tags.push_back({key, val}); }
        if (laes) return startLaes() ? S_OK : E_FAIL;
        if (!strcmp(mg, "LAE1")) {   // LAEY, WAV form or FLAC form (liblae tells them apart); the FLAC form's tags are its Vorbis comments
            if (tags.empty()) { const laeyflac::ReadFn rd = [this](uint64_t off, void* buf, size_t n) { return read(this, off, buf, n) == 0; }; laeyflac::Layout L; if (laeyflac::layout(rd, size, L).empty()) tags = laeyflac::comments(rd, L); }
            return startLaey() ? S_OK : E_FAIL; }
        return VFW_E_UNSUPPORTED_STREAM; }
    void checkAll() {
        laes_file_info fi; laes_get_info(laes, &fi); int first = -1, nm = 0;
        for (int i = 0; i < fi.choices; i++) { laes_choice_info c; laes_get_choice(laes, i, &c); if (c.kind == LAES_KIND_MASTER) { nm++; if (first < 0) first = i; } }
        char err[256]; laes_play* p = laes_start_all(laes, first, 0, err, sizeof err); if (!p) return; laes_choice_info c; laes_get_choice(laes, first, &c);
        ProgressWindow w(kVerifyA + std::to_wstring(nm) + kVerifyB);
        std::vector<int32_t> b(65536 * 2); uint64_t got = 0; int64_t k;
        while ((k = laes_read(p, b.data(), 65536)) > 0) { got += (uint64_t)k; w.set((int)(100.0 * (double)got / (double)(c.frames ? c.frames : 1))); }
        if (laes_verified(p) >= 0) for (int m = 0; m < nm; m++) { const int r = laes_master_ok(p, m); if (r >= 0) g_verified.put(fi.file_sha256, m, r == 1); }
        laes_stop(p); }
    bool startLaes() {
        laes_file_info fi; laes_get_info(laes, &fi); choice = pick(laes, (int)setting(L"layers", 0), (int)setting(L"maxrate", 0), (int)setting(L"maxbits", 0));
        laes_choice_info c; laes_get_choice(laes, choice, &c);
        if (c.master >= 0) { if (g_verified.get(fi.file_sha256, c.master) == 0) checkAll(); if (g_verified.get(fi.file_sha256, c.master) != 1) { choice = 0; laes_get_choice(laes, 0, &c); } }
        rate = c.rate; bits = c.bits; frames = c.frames; if (!seek(0)) return false;
        const std::wstring rec = dataDir() + L"\\laes_index\\" + std::wstring(pfcw(hex(fi.file_sha256, 32))) + L".idx"; laes_file* f = laes;
        indexer = std::thread([this, rec, f] { FpPin pin2; std::vector<uint8_t> d; FILE* r = _wfopen(rec.c_str(), L"rb");
            if (r) { _fseeki64(r, 0, SEEK_END); const int64_t n = _ftelli64(r); _fseeki64(r, 0, SEEK_SET); if (n > 0 && n < (64ll << 20)) { d.resize((size_t)n); if (fread(d.data(), 1, d.size(), r) != d.size()) d.clear(); } fclose(r); }
            if (!d.empty() && laes_index_load(f, d.data(), (int64_t)d.size()) == 0) return;
            laes_index_layers(f, &cancel, 0); if (cancel) return; const int64_t n = laes_index_save(f, nullptr, 0); if (n <= 0) return;
            std::vector<uint8_t> b((size_t)n); if (laes_index_save(f, b.data(), n) != n) return; CreateDirectoryW((dataDir() + L"\\laes_index").c_str(), nullptr);
            const std::wstring tmp = rec + L".part"; FILE* w = _wfopen(tmp.c_str(), L"wb"); if (!w) return; const bool ok = fwrite(b.data(), 1, b.size(), w) == b.size(); fclose(w);
            if (!ok || !MoveFileExW(tmp.c_str(), rec.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str()); });
        return true; }
    static std::wstring pfcw(const std::string& s) { return std::wstring(s.begin(), s.end()); }
    bool startLaey() {
        char err[512] = ""; if (!lae_selftest()) return false; track = lae_open_reader(&Decoder::read, this, size, err, sizeof err); if (!track) return false;
        // LAEY extension packs: the pack number the user set (registry value "pack", 0 = the main body); a file without that pack plays pack 0
        { const int pk = (int)setting(L"pack", 0); if (pk > 0 && pk < lae_pack_count(track)) lae_select_pack(track, pk); }
        const DWORD layers = setting(L"layers", 0); int route = layers == 2 ? LAE_ROUTE_CD : layers == 1 ? LAE_ROUTE_CD24 : layers == 3 ? LAE_ROUTE_16 : LAE_ROUTE_FULL;
        lae_info li; lae_get_info(track, &li); int out = 0; const int cap = (int)setting(L"maxrate", 0); if ((route == LAE_ROUTE_FULL || route == LAE_ROUTE_16) && cap > 0) { out = li.master_rate; while (out > cap && out > li.base_rate) out /= 2;
            if (out == li.base_rate) { route = route == LAE_ROUTE_FULL ? LAE_ROUTE_CD24 : LAE_ROUTE_CD; out = 0; } }   // the cap reaches the base rate: all layers = CD layer + depth layer (24-bit); route 16 has no depth layer
        if (lae_start(track, route, out, err, sizeof err) != 0 && lae_start(track, LAE_ROUTE_CD, 0, err, sizeof err) != 0) return false;
        int r, b; uint64_t fr; lae_output_format(track, &r, &b, &fr); rate = r; bits = b == 16 ? 16 : 24; frames = fr; pos = 0; return true; }
    bool seek(uint64_t frame) { FpPin pin; char err[256]; if (frame > frames) frame = frames;
        if (laes) {   // every choice starts at the frame, a derived 16-bit version too (its dither state is moved there, bit-exact; 2026-11-12)
            if (play) laes_stop(play); play = frame ? laes_start_at(laes, choice, frame, 0, err, sizeof err) : laes_start_at(laes, choice, 0, 0, err, sizeof err);
            if (!play) play = laes_start(laes, choice, 0, err, sizeof err), frame = 0; pos = frame; return play != nullptr; }
        if (track) { if (lae_seek(track, frame) != 0) return false; pos = frame; return true; }
        return false; }
    int64_t readFrames(int32_t* out, int64_t n) { FpPin pin; const int64_t k = play ? laes_read(play, out, n) : track ? lae_read(track, out, n) : -1; if (k > 0) pos += (uint64_t)k; return k; }
};

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
const REFERENCE_TIME kUnits = 10000000;   // 100 ns per second

class LaeSource;
// ---- the output pin: PCM, pushed by a thread of its own; seeking (the renderer passes IMediaSeeking upstream to it)
class LaePin : public IPin, public IMediaSeeking {
public:
    explicit LaePin(LaeSource* f) : m_filter(f) {}
    // IUnknown: the pin lives as long as its filter
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER; if (riid == IID_IUnknown || riid == IID_IPin) *ppv = static_cast<IPin*>(this); else if (riid == IID_IMediaSeeking) *ppv = static_cast<IMediaSeeking*>(this); else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK; }
    STDMETHODIMP_(ULONG) AddRef() override; STDMETHODIMP_(ULONG) Release() override;
    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }   // an output pin
    STDMETHODIMP Disconnect() override { std::lock_guard<std::recursive_mutex> g(m_lock); if (!m_peer) return S_FALSE; stopThread(); if (m_alloc) m_alloc->Decommit(); release(m_alloc); release(m_input); release(m_peer); return S_OK; }
    STDMETHODIMP ConnectedTo(IPin** pPin) override { if (!pPin) return E_POINTER; std::lock_guard<std::recursive_mutex> g(m_lock); *pPin = m_peer; if (!m_peer) return VFW_E_NOT_CONNECTED; m_peer->AddRef(); return S_OK; }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override { if (!pmt) return E_POINTER; std::lock_guard<std::recursive_mutex> g(m_lock); if (!m_peer) { ZeroMemory(pmt, sizeof *pmt); return VFW_E_NOT_CONNECTED; } return fillType(pmt) ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override { if (!pPinDir) return E_POINTER; *pPinDir = PINDIR_OUTPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR* Id) override { if (!Id) return E_POINTER; *Id = (LPWSTR)CoTaskMemAlloc(sizeof L"Audio"); if (!*Id) return E_OUTOFMEMORY; wcscpy_s(*Id, 6, L"Audio"); return S_OK; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override { AM_MEDIA_TYPE mt; if (!fillType(&mt)) return E_OUTOFMEMORY; const bool same = pmt && pmt->majortype == mt.majortype && pmt->subtype == mt.subtype && pmt->formattype == mt.formattype; freeType(&mt); return same ? S_OK : S_FALSE; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
    // IMediaSeeking: time format TIME_FORMAT_MEDIA_TIME only
    STDMETHODIMP GetCapabilities(DWORD* pCapabilities) override { if (!pCapabilities) return E_POINTER; *pCapabilities = AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanSeekForwards | AM_SEEKING_CanSeekBackwards | AM_SEEKING_CanGetDuration | AM_SEEKING_CanGetCurrentPos | AM_SEEKING_CanGetStopPos; return S_OK; }
    STDMETHODIMP CheckCapabilities(DWORD* pCapabilities) override { if (!pCapabilities) return E_POINTER; DWORD have; GetCapabilities(&have); const DWORD want = *pCapabilities; *pCapabilities &= have; return *pCapabilities == want ? S_OK : *pCapabilities ? S_FALSE : E_FAIL; }
    STDMETHODIMP IsFormatSupported(const GUID* pFormat) override { return pFormat && *pFormat == TIME_FORMAT_MEDIA_TIME ? S_OK : S_FALSE; }
    STDMETHODIMP QueryPreferredFormat(GUID* pFormat) override { if (!pFormat) return E_POINTER; *pFormat = TIME_FORMAT_MEDIA_TIME; return S_OK; }
    STDMETHODIMP GetTimeFormat(GUID* pFormat) override { return QueryPreferredFormat(pFormat); }
    STDMETHODIMP IsUsingTimeFormat(const GUID* pFormat) override { return IsFormatSupported(pFormat); }
    STDMETHODIMP SetTimeFormat(const GUID* pFormat) override { return pFormat && *pFormat == TIME_FORMAT_MEDIA_TIME ? S_OK : E_INVALIDARG; }
    STDMETHODIMP GetDuration(LONGLONG* pDuration) override;
    STDMETHODIMP GetStopPosition(LONGLONG* pStop) override { return GetDuration(pStop); }
    STDMETHODIMP GetCurrentPosition(LONGLONG* pCurrent) override;
    STDMETHODIMP ConvertTimeFormat(LONGLONG* pTarget, const GUID* pTargetFormat, LONGLONG Source, const GUID* pSourceFormat) override {
        if (!pTarget) return E_POINTER; if ((pTargetFormat && *pTargetFormat != TIME_FORMAT_MEDIA_TIME) || (pSourceFormat && *pSourceFormat != TIME_FORMAT_MEDIA_TIME)) return E_INVALIDARG; *pTarget = Source; return S_OK; }
    STDMETHODIMP SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags, LONGLONG* pStop, DWORD dwStopFlags) override;
    STDMETHODIMP GetPositions(LONGLONG* pCurrent, LONGLONG* pStop) override { if (pCurrent) GetCurrentPosition(pCurrent); if (pStop) GetDuration(pStop); return S_OK; }
    STDMETHODIMP GetAvailable(LONGLONG* pEarliest, LONGLONG* pLatest) override { if (pEarliest) *pEarliest = 0; return pLatest ? GetDuration(pLatest) : S_OK; }
    STDMETHODIMP SetRate(double dRate) override { return dRate == 1.0 ? S_OK : E_INVALIDARG; }
    STDMETHODIMP GetRate(double* pdRate) override { if (!pdRate) return E_POINTER; *pdRate = 1.0; return S_OK; }
    STDMETHODIMP GetPreroll(LONGLONG* pllPreroll) override { if (!pllPreroll) return E_POINTER; *pllPreroll = 0; return S_OK; }

    bool fillType(AM_MEDIA_TYPE* pmt);
    static void freeType(AM_MEDIA_TYPE* pmt) { if (pmt->cbFormat) CoTaskMemFree(pmt->pbFormat); pmt->pbFormat = nullptr; pmt->cbFormat = 0; release(pmt->pUnk); }
    void startThread(); void stopThread();
    bool connected() { std::lock_guard<std::recursive_mutex> g(m_lock); return m_peer != nullptr; }
private:
    LaeSource* m_filter; std::recursive_mutex m_lock; IPin* m_peer = nullptr; IMemInputPin* m_input = nullptr; IMemAllocator* m_alloc = nullptr;
    std::thread m_thread; std::atomic<bool> m_stop{false}; REFERENCE_TIME m_segStart = 0; uint64_t m_segFrame = 0;
    void push();
};

class LaeSource : public IBaseFilter, public IFileSourceFilter, public IAMFilterMiscFlags, public IAMMediaContent {
public:
    LaeSource() : m_pin(this) { g_objects++; }
    ~LaeSource() { m_pin.stopThread(); release(m_clock); g_objects--; }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter || riid == IID_IPersist) *ppv = static_cast<IBaseFilter*>(this);
        else if (riid == IID_IFileSourceFilter) *ppv = static_cast<IFileSourceFilter*>(this); else if (riid == IID_IAMFilterMiscFlags) *ppv = static_cast<IAMFilterMiscFlags*>(this);
        else if (riid == IID_IMediaSeeking) *ppv = static_cast<IMediaSeeking*>(&m_pin);
        else if (riid == IID_IAMMediaContent || riid == IID_IDispatch) *ppv = static_cast<IAMMediaContent*>(this); else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK; }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++m_ref; }
    STDMETHODIMP_(ULONG) Release() override { const long r = --m_ref; if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP GetClassID(CLSID* pClassID) override { if (!pClassID) return E_POINTER; *pClassID = CLSID_LaeSource; return S_OK; }
    STDMETHODIMP Stop() override { m_pin.stopThread(); m_state = State_Stopped; return S_OK; }
    STDMETHODIMP Pause() override { if (m_state == State_Stopped && m_pin.connected()) m_pin.startThread(); m_state = State_Paused; return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME tStart) override { if (m_state == State_Stopped) Pause(); m_start = tStart; m_state = State_Running; return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* State) override { if (!State) return E_POINTER; *State = m_state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override { if (pClock) pClock->AddRef(); release(m_clock); m_clock = pClock; return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override { if (!pClock) return E_POINTER; *pClock = m_clock; if (m_clock) m_clock->AddRef(); return S_OK; }
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override { if (!ppPin) return E_POINTER; if (Id && !wcscmp(Id, L"Audio")) { *ppPin = &m_pin; m_pin.AddRef(); return S_OK; } *ppPin = nullptr; return VFW_E_NOT_FOUND; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override { if (!pInfo) return E_POINTER; wcscpy_s(pInfo->achName, L"LAE source"); pInfo->pGraph = m_graph; if (m_graph) m_graph->AddRef(); return S_OK; }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) override { m_graph = pGraph; return S_OK; }   // not AddRef'd (the graph holds the filter)
    STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }
    // IAMMediaContent (2026-11-06): title, artist, copyright and comment for players that ask the source filter (MPC-BE, MPC-HC); the
    // IDispatch part is not offered (scripting only)
    STDMETHODIMP GetTypeInfoCount(UINT* n) override { if (n) *n = 0; return S_OK; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT tagOut(const char* key, BSTR* out) { if (!out) return E_POINTER; *out = nullptr;
        for (auto& kv : dec.tags) if (lae_wavtags::sameName(kv.first, key)) { const int n = MultiByteToWideChar(CP_UTF8, 0, kv.second.c_str(), -1, nullptr, 0); if (n <= 1) return VFW_E_NOT_FOUND;
            std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, kv.second.c_str(), -1, &w[0], n); *out = SysAllocString(w.c_str()); return *out ? S_OK : E_OUTOFMEMORY; }
        return VFW_E_NOT_FOUND; }
    STDMETHODIMP get_AuthorName(BSTR* s) override { return tagOut("ARTIST", s); }
    STDMETHODIMP get_Title(BSTR* s) override { return tagOut("TITLE", s); }
    STDMETHODIMP get_Rating(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_Description(BSTR* s) override { return tagOut("COMMENT", s); }
    STDMETHODIMP get_Copyright(BSTR* s) override { return tagOut("COPYRIGHT", s); }
    STDMETHODIMP get_BaseURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_LogoURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_LogoIconURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_WatermarkURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_MoreInfoURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_MoreInfoBannerImage(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_MoreInfoBannerURL(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    STDMETHODIMP get_MoreInfoText(BSTR* s) override { if (s) *s = nullptr; return E_NOTIMPL; }
    // IFileSourceFilter
    STDMETHODIMP Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE*) override { if (!pszFileName) return E_POINTER; m_name = pszFileName; return dec.open(pszFileName); }
    STDMETHODIMP GetCurFile(LPOLESTR* ppszFileName, AM_MEDIA_TYPE* pmt) override {
        if (!ppszFileName) return E_POINTER; *ppszFileName = (LPOLESTR)CoTaskMemAlloc((m_name.size() + 1) * sizeof(wchar_t)); if (!*ppszFileName) return E_OUTOFMEMORY;
        wcscpy_s(*ppszFileName, m_name.size() + 1, m_name.c_str()); if (pmt) { ZeroMemory(pmt, sizeof *pmt); pmt->majortype = MEDIATYPE_Stream; } return S_OK; }
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    Decoder dec; LaePin m_pin; IFilterGraph* m_graph = nullptr;
private:
    std::atomic<long> m_ref{0}; FILTER_STATE m_state = State_Stopped; IReferenceClock* m_clock = nullptr; REFERENCE_TIME m_start = 0; std::wstring m_name;
};

STDMETHODIMP_(ULONG) LaePin::AddRef() { return m_filter->AddRef(); }
STDMETHODIMP_(ULONG) LaePin::Release() { return m_filter->Release(); }
STDMETHODIMP LaePin::QueryPinInfo(PIN_INFO* pInfo) { if (!pInfo) return E_POINTER; pInfo->pFilter = m_filter; m_filter->AddRef(); pInfo->dir = PINDIR_OUTPUT; wcscpy_s(pInfo->achName, L"Audio"); return S_OK; }
STDMETHODIMP LaePin::GetDuration(LONGLONG* pDuration) { if (!pDuration) return E_POINTER; const Decoder& d = m_filter->dec; *pDuration = d.rate ? (LONGLONG)(d.frames * (uint64_t)kUnits / (uint64_t)d.rate) : 0; return S_OK; }
STDMETHODIMP LaePin::GetCurrentPosition(LONGLONG* pCurrent) { if (!pCurrent) return E_POINTER; const Decoder& d = m_filter->dec; *pCurrent = d.rate ? (LONGLONG)(d.pos * (uint64_t)kUnits / (uint64_t)d.rate) : 0; return S_OK; }

bool LaePin::fillType(AM_MEDIA_TYPE* pmt) {
    const Decoder& d = m_filter->dec; ZeroMemory(pmt, sizeof *pmt); const int by = d.bits == 16 ? 2 : 4;
    WAVEFORMATEXTENSIBLE* w = (WAVEFORMATEXTENSIBLE*)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)); if (!w) return false; ZeroMemory(w, sizeof *w);
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w->Format.nChannels = 2; w->Format.nSamplesPerSec = (DWORD)d.rate; w->Format.wBitsPerSample = (WORD)(by * 8);
    w->Format.nBlockAlign = (WORD)(2 * by); w->Format.nAvgBytesPerSec = (DWORD)d.rate * w->Format.nBlockAlign; w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = (WORD)d.bits; w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; w->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    pmt->majortype = MEDIATYPE_Audio; pmt->subtype = MEDIASUBTYPE_PCM; pmt->bFixedSizeSamples = TRUE; pmt->lSampleSize = w->Format.nBlockAlign;
    pmt->formattype = FORMAT_WaveFormatEx; pmt->cbFormat = sizeof(WAVEFORMATEXTENSIBLE); pmt->pbFormat = (BYTE*)w; return true; }

class EnumTypes : public IEnumMediaTypes {   // the one media type of the pin
public:
    explicit EnumTypes(LaePin* p, ULONG at = 0) : m_pin(p), m_at(at) { m_pin->AddRef(); }
    virtual ~EnumTypes() { m_pin->Release(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { if (!ppv) return E_POINTER; if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) { *ppv = this; AddRef(); return S_OK; } *ppv = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++m_ref; }
    STDMETHODIMP_(ULONG) Release() override { const long r = --m_ref; if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE** out, ULONG* got) override { ULONG k = 0;
        while (k < n && m_at < 1) { AM_MEDIA_TYPE* t = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)); if (!t || !m_pin->fillType(t)) { if (t) CoTaskMemFree(t); break; } out[k++] = t; m_at++; }
        if (got) *got = k; return k == n ? S_OK : S_FALSE; }
    STDMETHODIMP Skip(ULONG n) override { m_at += n; return m_at <= 1 ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() override { m_at = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp) override { if (!pp) return E_POINTER; *pp = new EnumTypes(m_pin, m_at); (*pp)->AddRef(); return S_OK; }
private:
    std::atomic<long> m_ref{0}; LaePin* m_pin; ULONG m_at;
};
STDMETHODIMP LaePin::EnumMediaTypes(IEnumMediaTypes** ppEnum) { if (!ppEnum) return E_POINTER; *ppEnum = new EnumTypes(this); (*ppEnum)->AddRef(); return S_OK; }

class EnumPinsImpl : public IEnumPins {   // the one pin of the filter
public:
    explicit EnumPinsImpl(LaeSource* f, ULONG at = 0) : m_f(f), m_at(at) { m_f->AddRef(); }
    virtual ~EnumPinsImpl() { m_f->Release(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { if (!ppv) return E_POINTER; if (riid == IID_IUnknown || riid == IID_IEnumPins) { *ppv = this; AddRef(); return S_OK; } *ppv = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++m_ref; }
    STDMETHODIMP_(ULONG) Release() override { const long r = --m_ref; if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP Next(ULONG n, IPin** out, ULONG* got) override { ULONG k = 0; while (k < n && m_at < 1) { out[k++] = &m_f->m_pin; m_f->m_pin.AddRef(); m_at++; } if (got) *got = k; return k == n ? S_OK : S_FALSE; }
    STDMETHODIMP Skip(ULONG n) override { m_at += n; return m_at <= 1 ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() override { m_at = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp) override { if (!pp) return E_POINTER; *pp = new EnumPinsImpl(m_f, m_at); (*pp)->AddRef(); return S_OK; }
private:
    std::atomic<long> m_ref{0}; LaeSource* m_f; ULONG m_at;
};
STDMETHODIMP LaeSource::EnumPins(IEnumPins** ppEnum) { if (!ppEnum) return E_POINTER; *ppEnum = new EnumPinsImpl(this); (*ppEnum)->AddRef(); return S_OK; }

STDMETHODIMP LaePin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER; std::lock_guard<std::recursive_mutex> g(m_lock); if (m_peer) return VFW_E_ALREADY_CONNECTED;
    if (!m_filter->dec.rate) return VFW_E_NO_ACCEPTABLE_TYPES;   // no file loaded
    if (pmt && pmt->majortype != GUID_NULL && QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
    AM_MEDIA_TYPE mt; if (!fillType(&mt)) return E_OUTOFMEMORY; HRESULT hr = pReceivePin->ReceiveConnection(this, &mt); freeType(&mt); if (FAILED(hr)) return VFW_E_NO_ACCEPTABLE_TYPES;
    IMemInputPin* in = nullptr; hr = pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&in); if (FAILED(hr)) { pReceivePin->Disconnect(); return hr; }
    // the allocator: the input pin's, or a standard one; buffers of about 50 ms
    IMemAllocator* a = nullptr; if (FAILED(in->GetAllocator(&a))) hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&a);
    ALLOCATOR_PROPERTIES want{}, got{}; in->GetAllocatorRequirements(&want); const Decoder& d = m_filter->dec; const int by = d.bits == 16 ? 2 : 4;
    want.cBuffers = std::max<long>(want.cBuffers, 8); want.cbBuffer = std::max<long>(want.cbBuffer, (long)(d.rate / 20) * 2 * by); if (want.cbAlign <= 0) want.cbAlign = 1;
    if (!a || FAILED(hr = a->SetProperties(&want, &got)) || FAILED(hr = in->NotifyAllocator(a, FALSE))) { release(a); in->Release(); pReceivePin->Disconnect(); return FAILED(hr) ? hr : E_FAIL; }
    m_peer = pReceivePin; m_peer->AddRef(); m_input = in; m_alloc = a; return S_OK; }

void LaePin::startThread() {
    stopThread(); std::lock_guard<std::recursive_mutex> g(m_lock); if (!m_peer || !m_alloc) return;
    m_alloc->Commit(); m_stop = false; const Decoder& d = m_filter->dec; m_segFrame = d.pos; m_segStart = d.rate ? (REFERENCE_TIME)(d.pos * (uint64_t)kUnits / (uint64_t)d.rate) : 0;
    m_peer->NewSegment(m_segStart, (REFERENCE_TIME)(d.frames * (uint64_t)kUnits / (uint64_t)std::max(1, d.rate)), 1.0);
    m_thread = std::thread([this] { push(); }); }
void LaePin::stopThread() { m_stop = true; IMemAllocator* a = nullptr; { std::lock_guard<std::recursive_mutex> g(m_lock); a = m_alloc; if (a) a->AddRef(); }
    if (a) { a->Decommit(); a->Release(); }   // a wait in GetBuffer returns
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) m_thread.join(); }
// delivers PCM in buffers of about 50 ms; time stamps relative to the segment start (the position of the last start or seek)
void LaePin::push() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); Decoder& d = m_filter->dec; const int by = d.bits == 16 ? 2 : 4; std::vector<int32_t> buf;
    for (;;) { if (m_stop) break; IMediaSample* s = nullptr; if (FAILED(m_alloc->GetBuffer(&s, nullptr, nullptr, 0)) || !s) break;
        BYTE* p = nullptr; s->GetPointer(&p); const int64_t n = (int64_t)(s->GetSize() / (2 * by)); buf.resize((size_t)n * 2);
        const uint64_t first = d.pos; const int64_t k = d.readFrames(buf.data(), n);
        if (k <= 0) { s->Release(); if (!m_stop && m_peer) { m_peer->EndOfStream();
                if (k < 0) {   // a decoding or read error is not the end of the track: the graph is told (EC_ERRORABORT), as a source filter does
                    IMediaEventSink* sink = nullptr; if (m_filter->m_graph && SUCCEEDED(m_filter->m_graph->QueryInterface(IID_IMediaEventSink, (void**)&sink)) && sink) { sink->Notify(EC_ERRORABORT, E_FAIL, 0); sink->Release(); } } }
            break; }
        for (int64_t i = 0; i < k * 2; i++) { const int32_t v = buf[(size_t)i]; const uint32_t u = d.bits == 24 ? (uint32_t)v << 8 : (uint32_t)v; for (int b = 0; b < by; b++) p[i * by + b] = (BYTE)(u >> (8 * b)); }
        s->SetActualDataLength((long)(k * 2 * by));
        REFERENCE_TIME t0 = (REFERENCE_TIME)((first - m_segFrame) * (uint64_t)kUnits / (uint64_t)d.rate), t1 = (REFERENCE_TIME)((first + (uint64_t)k - m_segFrame) * (uint64_t)kUnits / (uint64_t)d.rate);
        s->SetTime(&t0, &t1); s->SetSyncPoint(TRUE); s->SetDiscontinuity(first == m_segFrame);
        const HRESULT hr = m_input->Receive(s); s->Release(); if (hr != S_OK) break; }
    CoUninitialize(); }

STDMETHODIMP LaePin::SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags, LONGLONG*, DWORD) {
    if (!pCurrent || (dwCurrentFlags & AM_SEEKING_PositioningBitsMask) == AM_SEEKING_NoPositioning) return S_OK;
    Decoder& d = m_filter->dec; LONGLONG t = *pCurrent; if ((dwCurrentFlags & AM_SEEKING_PositioningBitsMask) == AM_SEEKING_RelativePositioning) { LONGLONG c; GetCurrentPosition(&c); t += c; }
    const uint64_t frame = t <= 0 ? 0 : (uint64_t)t * (uint64_t)d.rate / (uint64_t)kUnits;
    const bool running = m_thread.joinable() && !m_stop;
    if (running && m_peer) m_peer->BeginFlush(); stopThread(); if (m_peer && running) m_peer->EndFlush();
    if (!d.seek(frame)) return E_FAIL;
    if (running) startThread(); return S_OK; }

// ---- COM server
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { if (!ppv) return E_POINTER; if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; } *ppv = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; } STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override { if (outer) return CLASS_E_NOAGGREGATION; LaeSource* f = new (std::nothrow) LaeSource(); if (!f) return E_OUTOFMEMORY;
        f->AddRef(); const HRESULT hr = f->QueryInterface(riid, ppv); f->Release(); return hr; }
    STDMETHODIMP LockServer(BOOL lock) override { if (lock) g_locks++; else g_locks--; return S_OK; }
};
Factory g_factory;
}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) { g_module = h; DisableThreadLibraryCalls(h); } return TRUE; }
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) { if (rclsid != CLSID_LaeSource) return CLASS_E_CLASSNOTAVAILABLE; return g_factory.QueryInterface(riid, ppv); }
STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
// registration as standard DirectShow filters do it: through HKEY_CLASSES_ROOT (players such as PotPlayer capture these writes when they
// add an external filter): the COM class, the source for the .lae extension and the filter catalogue entry (legacy filter category, by the
// system's filter mapper). Without administrator rights those writes are refused; then the same keys go to the current user's classes
// (HKEY_CLASSES_ROOT redirected to HKCU\Software\Classes during the call).
static const wchar_t* kClsid = L"{5B3C6E8A-2F41-4C7D-9E15-8A0C3D6B7F21}";
static HRESULT setKey(const std::wstring& key, const wchar_t* name, const std::wstring& value) { HKEY k; if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return E_ACCESSDENIED;
    const LSTATUS r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t))); RegCloseKey(k); return r == ERROR_SUCCESS ? S_OK : E_ACCESSDENIED; }
static HRESULT registerAll(bool add) {
    const std::wstring clsid = kClsid; HRESULT hr = S_OK;
    if (add) { wchar_t path[MAX_PATH]; GetModuleFileNameW(g_module, path, MAX_PATH);
        hr = setKey(L"CLSID\\" + clsid, nullptr, L"LAE source");
        if (SUCCEEDED(hr)) hr = setKey(L"CLSID\\" + clsid + L"\\InprocServer32", nullptr, path);
        if (SUCCEEDED(hr)) hr = setKey(L"CLSID\\" + clsid + L"\\InprocServer32", L"ThreadingModel", L"Both");
        if (SUCCEEDED(hr)) hr = setKey(L"Media Type\\Extensions\\.lae", L"Source Filter", clsid);
        if (FAILED(hr)) return hr; }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); IFilterMapper2* fm = nullptr;
    HRESULT hm = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&fm);
    if (SUCCEEDED(hm)) {
        if (add) { REGPINTYPES type = {&MEDIATYPE_Audio, &MEDIASUBTYPE_PCM}; REGFILTERPINS2 pin{}; pin.cInstances = 1; pin.nMediaTypes = 1; pin.lpMediaType = &type;
            REGFILTER2 rf{}; rf.dwVersion = 2; rf.dwMerit = MERIT_NORMAL; rf.cPins2 = 1; rf.rgPins2 = &pin;
            hm = fm->RegisterFilter(CLSID_LaeSource, L"LAE source", nullptr, &CLSID_LegacyAmFilterCategory, nullptr, &rf); }
        else fm->UnregisterFilter(&CLSID_LegacyAmFilterCategory, nullptr, CLSID_LaeSource);
        fm->Release(); }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!add) { RegDeleteTreeW(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str()); RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Media Type\\Extensions\\.lae"); return S_OK; }
    return hm; }
static HRESULT inUserClasses(bool add) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr) != ERROR_SUCCESS) return E_FAIL;
    HRESULT hr = RegOverridePredefKey(HKEY_CLASSES_ROOT, k) == ERROR_SUCCESS ? registerAll(add) : E_FAIL; RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr); RegCloseKey(k); return hr; }
STDAPI DllRegisterServer() { HRESULT hr = registerAll(true); if (FAILED(hr)) hr = inUserClasses(true); return hr; }
STDAPI DllUnregisterServer() { registerAll(false); inUserClasses(false); return S_OK; }
