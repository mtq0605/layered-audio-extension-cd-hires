// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_abx - a stand-alone ABX listening test for the LAE demo files (Windows, x64).
//
// The kit folder holds lae_abx.exe, lae_udec.exe (the LAE decoder), abx_set.txt and the audio files. abx_set.txt lists the pairs:
//   <name> | <file A> | <pick A> | <file B> | <pick B>
// A pick is a choice name of lae_udec (e.g. master_1_352800_32, full_352800, pack1_full_176400) for an LAE file, or "-" for a plain
// WAV or an MP3 file. Both sources are decoded (LAE files by lae_udec, MP3 by Windows Media Foundation), brought to the test rate
// (48 or 44.1 kHz) by the same resampler, and, when one of them is an MP3 / AAC file, lined up (the lag is found by
// cross-correlation: those decoders add a delay; a lag other than 0 is shown during the test).
// The program assigns the two sources to the buttons A and B at random and does not tell which is which until the test ends.
// In every trial X and Y are A and B in a random order; the listener says whether X is A or X is B. At the end it shows the score,
// the one-sided binomial p-value and which source was A, and writes a log (with the SHA-256 of both source files) to results\.
// The ABX method itself goes back to Munson & Gardner (1950) and Clark (JAES 30(5), 1982).
//
// Command line (for checks, no window):  lae_abx --prepare <pair number, 1-based> <48000|44100> <out folder>
//   prepares the pair as the window would and writes A.wav and B.wav (32-bit float, the test rate) plus prepare.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <functional>
#include <mutex>
#include <numeric>
#include <shellapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const char* kVersion = "lae_abx 1.0 (2026-11-12)";

typedef std::function<void(const std::string&, double)> Say;   // stage text, overall fraction 0..1 (negative: unchanged)

// ---------------------------------------------------------------- small helpers
static std::wstring widen(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
static std::string narrow(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
static std::wstring exeDir() { wchar_t p[MAX_PATH * 4]; GetModuleFileNameW(nullptr, p, MAX_PATH * 4); PathRemoveFileSpecW(p); return p; }
static std::string trim(std::string s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
static uint32_t rnd32() { uint32_t v = 0; BCryptGenRandom(nullptr, (PUCHAR)&v, sizeof v, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return v; }
static std::string nowText() { SYSTEMTIME t; GetLocalTime(&t); char b[64]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b; }
static std::string nowStamp() { SYSTEMTIME t; GetLocalTime(&t); char b[64]; snprintf(b, sizeof b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b; }

struct Sha256 {   // Windows CNG
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() { BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0); BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0); }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void* p, size_t n) { BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex() { unsigned char d[32]; BCryptFinishHash(h, d, 32, 0); char s[65]; for (int i = 0; i < 32; i++) snprintf(s + 2 * i, 3, "%02x", d[i]); return s; } };
static std::string fileSha256(const std::wstring& path, const std::function<void(double)>& prog) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr); if (f == INVALID_HANDLE_VALUE) return "(cannot read)";
    LARGE_INTEGER size; GetFileSizeEx(f, &size); Sha256 h; std::vector<char> buf(1 << 22); DWORD got = 0; long long done = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) { h.add(buf.data(), got); done += got; if (prog) prog(size.QuadPart ? (double)done / size.QuadPart : 1); }
    CloseHandle(f); return h.hex(); }

// ---------------------------------------------------------------- WAV reading (PCM 16/24/32, float 32; RIFF or RF64)
struct WavIn {
    HANDLE f = INVALID_HANDLE_VALUE; int rate = 0, ch = 0, bits = 0; bool isFloat = false; long long dataOff = 0, frames = 0;
    bool open(const std::wstring& path, std::string& err) {
        f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr); if (f == INVALID_HANDLE_VALUE) { err = "cannot open the decoded WAV"; return false; }
        auto rd = [&](void* p, DWORD n) { DWORD g = 0; return ReadFile(f, p, n, &g, nullptr) && g == n; };
        char hdr[12]; if (!rd(hdr, 12) || (memcmp(hdr, "RIFF", 4) && memcmp(hdr, "RF64", 4)) || memcmp(hdr + 8, "WAVE", 4)) { err = "not a WAV file"; return false; }
        long long ds64data = -1, pos = 12;
        for (;;) { char ck[8]; if (!rd(ck, 8)) { err = "no data chunk"; return false; } uint32_t n; memcpy(&n, ck + 4, 4); pos += 8;
            if (!memcmp(ck, "ds64", 4)) { std::vector<char> b(n); if (!rd(b.data(), n)) { err = "bad ds64"; return false; } long long d; memcpy(&d, b.data() + 8, 8); ds64data = d; }
            else if (!memcmp(ck, "fmt ", 4)) { std::vector<char> b(n); if (!rd(b.data(), n)) { err = "bad fmt"; return false; } uint16_t tag; memcpy(&tag, b.data(), 2); uint16_t c; memcpy(&c, b.data() + 2, 2); uint32_t r; memcpy(&r, b.data() + 4, 4); uint16_t bp; memcpy(&bp, b.data() + 14, 2);
                ch = c; rate = (int)r; bits = bp; if (tag == 0xFFFE && n >= 40) memcpy(&tag, b.data() + 24, 2); isFloat = tag == 3; if (tag != 1 && tag != 3) { err = "unsupported WAV format"; return false; } }
            else if (!memcmp(ck, "data", 4)) { long long len = (n == 0xFFFFFFFFu && ds64data >= 0) ? ds64data : n; dataOff = pos; frames = len / (ch * (bits / 8)); return ch > 0 && rate > 0; }
            else { LARGE_INTEGER m; m.QuadPart = n + (n & 1); SetFilePointerEx(f, m, nullptr, FILE_CURRENT); }
            pos += n + (n & 1); if (memcmp(ck, "data", 4)) { LARGE_INTEGER m; m.QuadPart = pos; SetFilePointerEx(f, m, nullptr, FILE_BEGIN); } } }
    // reads up to n frames as float, channel-separated (first two channels; mono is duplicated)
    long long read(long long n, std::vector<float>& L, std::vector<float>& R) {
        const int bps = bits / 8; std::vector<unsigned char> b((size_t)n * ch * bps); DWORD got = 0; ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr);
        long long fr = got / (ch * bps); L.resize((size_t)fr); R.resize((size_t)fr);
        for (long long i = 0; i < fr; i++) for (int c = 0; c < 2; c++) { const unsigned char* p = b.data() + (i * ch + (c < ch ? c : 0)) * bps; float v;
            if (isFloat) memcpy(&v, p, 4);
            else if (bps == 2) { int16_t s; memcpy(&s, p, 2); v = s / 32768.0f; }
            else if (bps == 3) { int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; v = (float)(s / 8388608.0); }
            else { int32_t s; memcpy(&s, p, 4); v = (float)(s / 2147483648.0); }
            (c ? R : L)[(size_t)i] = v; }
        return fr; }
    ~WavIn() { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); } };

// ---------------------------------------------------------------- resampler: rational L/M, Kaiser-windowed sinc, about 140 dB stop band,
// flat to 20 kHz; the same filter design for every source, so both sides of a pair go through the same processing
static double besselI0(double x) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-20 * s) break; } return s; }
struct Resampler {
    long long L = 1, M = 1, K = 0, D = 0; std::vector<float> h;   // h: K taps per phase, phase-major: h[phase * K + k]
    Resampler(int in, int out) {
        long long g = std::gcd((long long)in, (long long)out); L = out / g; M = in / g; if (L == 1 && M == 1) return;
        const double fsUp = (double)in * L, pass = 20000.0, stop = std::min(in, out) / 2.0, A = 140.0;
        const double dw = 2 * 3.14159265358979323846 * (stop - pass) / fsUp; long long N = (long long)std::ceil((A - 8) / (2.285 * dw)) + 1; if (N % 2 == 0) N++;
        const double beta = 0.1102 * (A - 8.7), fc = (pass + stop) / 2.0 / fsUp, i0b = besselI0(beta); K = (N + L - 1) / L; D = (N - 1) / 2;
        std::vector<double> full((size_t)(K * L), 0.0);
        for (long long n = 0; n < N; n++) { const double m = n - (N - 1) / 2.0, x = 2 * fc * m, sinc = m == 0 ? 2 * fc : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * m), r = 2.0 * n / (N - 1) - 1.0;
            full[(size_t)n] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b * L; }
        h.assign((size_t)(K * L), 0.0f); for (long long p = 0; p < L; p++) for (long long k = 0; k < K; k++) h[(size_t)(p * K + k)] = (float)full[(size_t)(p + k * L)]; }
    bool identity() const { return L == 1 && M == 1; }
    // whole channel in, whole channel out (out length = round(nIn * L / M)); threads share the outputs
    void run(const std::vector<float>& x, std::vector<float>& y, const std::function<void(double)>& prog = nullptr) const {
        if (identity()) { y = x; return; }
        const long long nIn = (long long)x.size(), nOut = (nIn * L + M / 2) / M; y.assign((size_t)nOut, 0.0f);
        const unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency())); std::vector<std::thread> th;
        for (unsigned t = 0; t < nt; t++) th.emplace_back([&, t]() {
            const long long n0 = (long long)t * nOut / nt, n1 = (long long)(t + 1) * nOut / nt;
            for (long long n = n0; n < n1; n++) {
                if (t == 0 && prog && ((n - n0) & 0xFFFF) == 0) prog((double)(n - n0) / std::max(1LL, n1 - n0));
                const long long tt = n * M + D, base = tt / L, ph = tt % L; const float* hp = h.data() + ph * K; double acc = 0;
                long long k0 = std::max(0LL, base - nIn + 1), k1 = std::min(K, base + 1);
                for (long long k = k0; k < k1; k++) acc += (double)hp[k] * x[(size_t)(base - k)];
                y[(size_t)n] = (float)acc; } });
        for (auto& q : th) q.join(); } };

// ---------------------------------------------------------------- one prepared source
struct Source { std::string name, file, pick, sha; std::vector<float> L, R; };

static bool runProcess(const std::wstring& cmd, const std::wstring& logPath, std::string& err, const std::function<void()>& tick = nullptr) {
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE }; HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{ sizeof si }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; si.hStdOutput = log; si.hStdError = log; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{}; std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { if (log != INVALID_HANDLE_VALUE) CloseHandle(log); err = "cannot start lae_udec.exe"; return false; }
    while (WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT) if (tick) tick();
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (code) { err = "lae_udec.exe failed (exit code " + std::to_string(code) + "), see " + narrow(logPath); return false; } return true; }

// reads a WAV in pieces, resamples each channel to the test rate; progress runs from base to base + span
static bool loadWav(const std::wstring& path, int rate, Source& s, std::string& err, const Say& say, double base, double span) {
    WavIn w; if (!w.open(path, err)) return false;
    std::vector<float> L, R, l, r; L.reserve((size_t)w.frames); R.reserve((size_t)w.frames); long long done = 0;
    while (done < w.frames) { long long n = w.read(std::min<long long>(1 << 20, w.frames - done), l, r); if (n <= 0) break; L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end()); done += n;
        const double f = (double)done / std::max(1LL, w.frames); say("reading the decoded audio  " + std::to_string((int)(f * 100)) + " %", base + span * 0.3 * f); }
    Resampler rs(w.rate, rate); const std::string what = "resampling " + std::to_string(w.rate) + " Hz to " + std::to_string(rate) + " Hz";
    rs.run(L, s.L, [&](double f) { say(what + " (left)  " + std::to_string((int)(f * 100)) + " %", base + span * (0.3 + 0.35 * f)); }); { std::vector<float>().swap(L); }
    rs.run(R, s.R, [&](double f) { say(what + " (right)  " + std::to_string((int)(f * 100)) + " %", base + span * (0.65 + 0.35 * f)); });
    return true; }

static bool loadMp3(const std::wstring& path, int rate, Source& s, std::string& err, const Say& say, double base, double span) {
    IMFSourceReader* rd = nullptr; if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd))) { err = "Media Foundation cannot open the file"; return false; }
    IMFMediaType* mt = nullptr; MFCreateMediaType(&mt); mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt); mt->Release();
    IMFMediaType* cur = nullptr; rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur); UINT32 sr = 0, nch = 0; cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr); cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nch); cur->Release();
    PROPVARIANT dv; PropVariantInit(&dv); double dur = 0; if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dv)) && dv.vt == VT_UI8) dur = (double)dv.uhVal.QuadPart; PropVariantClear(&dv);
    std::vector<float> L, R; int lastPct = -1;
    for (;;) { DWORD flags = 0; LONGLONG ts = 0; IMFSample* smp = nullptr; if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &smp))) break;
        if (smp) { IMFMediaBuffer* b = nullptr; smp->ConvertToContiguousBuffer(&b); BYTE* p = nullptr; DWORD len = 0; b->Lock(&p, nullptr, &len);
            const float* f = (const float*)p; const size_t fr = len / (4 * nch); for (size_t i = 0; i < fr; i++) { L.push_back(f[i * nch]); R.push_back(f[i * nch + (nch > 1 ? 1 : 0)]); }
            b->Unlock(); b->Release(); smp->Release(); }
        if (dur > 0) { const double fr = std::min(1.0, ts / dur); const int pct = (int)(fr * 100); if (pct != lastPct) { lastPct = pct; say("decoding the MP3  " + std::to_string(pct) + " %", base + span * 0.6 * fr); } }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break; }
    rd->Release(); if (L.empty()) { err = "no audio decoded from the MP3"; return false; }
    Resampler rs((int)sr, rate); const std::string what = "resampling the MP3 " + std::to_string(sr) + " Hz to " + std::to_string(rate) + " Hz";
    rs.run(L, s.L, [&](double f) { say(what + "  " + std::to_string((int)(f * 50)) + " %", base + span * (0.6 + 0.2 * f)); });
    rs.run(R, s.R, [&](double f) { say(what + "  " + std::to_string(50 + (int)(f * 50)) + " %", base + span * (0.8 + 0.2 * f)); });
    return true; }

// lag of b against a (b late by lag samples), searched over +-0.15 s in the 3 s window of a with the most energy (the middle may be
// silence). The highest correlation counts with its sign (an inverted copy is no match); among equal values the lag nearest 0 wins,
// and 0 stays when nothing correlates (digital silence): a steady periodic signal or silence must not shift the second source.
static long long findLag(const std::vector<float>& a, const std::vector<float>& b, int rate) {
    const long long n = (long long)std::min(a.size(), b.size()), W = 3LL * rate, R = rate * 15 / 100; if (n < W + 2 * R + 10) return 0;
    long long s = n / 2 - W / 2;
    { const long long hop = std::max(1, rate / 10), nb = n / hop, wb = W / hop; std::vector<double> e((size_t)nb, 0.0);   // energy per 0.1 s block, then the loudest run of wb blocks
        for (long long k = 0; k < nb; k++) { double acc = 0; for (long long i = k * hop; i < (k + 1) * hop; i++) acc += (double)a[(size_t)i] * a[(size_t)i]; e[(size_t)k] = acc; }
        double run = 0, bestRun = -1; for (long long k = 0; k < nb; k++) { run += e[(size_t)k]; if (k >= wb) run -= e[(size_t)(k - wb)];
            const long long st = (k - wb + 1) * hop; if (k + 1 >= wb && st >= R && st + W + R <= n && run > bestRun) { bestRun = run; s = st; } } }
    std::vector<double> score((size_t)(2 * R + 1));
    const unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency())); std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++) th.emplace_back([&, t]() { for (long long g = -R + (long long)t; g <= R; g += nt) { double acc = 0; for (long long i = 0; i < W; i++) acc += (double)a[(size_t)(s + i)] * b[(size_t)(s + i + g)]; score[(size_t)(g + R)] = acc; } });
    for (auto& q : th) q.join();
    long long best = 0; double bv = score[(size_t)R];
    for (long long g = -R; g <= R; g++) { const double v = score[(size_t)(g + R)]; if (v > bv || (v == bv && std::llabs(g) < std::llabs(best))) { bv = v; best = g; } }
    return bv > 0 ? best : 0; }

struct Pair { std::string name, fileA, pickA, fileB, pickB; };
static std::vector<Pair> readSet(const std::wstring& dir) {
    std::vector<Pair> v; FILE* f = _wfopen((dir + L"\\abx_set.txt").c_str(), L"rb"); if (!f) return v; std::string all; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) all.append(b, n); fclose(f);
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF) all = all.substr(3); size_t p = 0;
    while (p < all.size()) { size_t e = all.find('\n', p); std::string ln = trim(all.substr(p, e == std::string::npos ? std::string::npos : e - p)); p = e == std::string::npos ? all.size() : e + 1;
        if (ln.empty() || ln[0] == '#') continue; std::vector<std::string> c; size_t q = 0; for (;;) { size_t k = ln.find('|', q); c.push_back(trim(ln.substr(q, k == std::string::npos ? std::string::npos : k - q))); if (k == std::string::npos) break; q = k + 1; }
        if (c.size() == 5) v.push_back({ c[0], c[1], c[2], c[3], c[4] }); }
    return v; }

// the session's work folder in TEMP (decoded files while they are made, and the prepared sources kept for later pairs)
static std::wstring workDir() { static std::wstring w; if (w.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); w = std::wstring(tmp) + L"lae_abx_" + std::to_wstring(GetCurrentProcessId()); CreateDirectoryW(w.c_str(), nullptr); } return w; }
static void removeWorkDir() {
    const std::wstring w = workDir(); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((w + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((w + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
    RemoveDirectoryW(w.c_str()); }
static std::wstring cacheDir() { static std::wstring c; if (c.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); c = std::wstring(tmp) + L"lae_abx_cache"; CreateDirectoryW(c.c_str(), nullptr); } return c; }
static void pruneCache() {   // prepared sources older than 14 days
    const std::wstring c = cacheDir(); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((c + L"\\src_*.f32").c_str(), &fd); if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now; GetSystemTimeAsFileTime(&now); const unsigned long long n = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime, limit = 14ULL * 24 * 3600 * 10000000ULL;
    do { const unsigned long long t = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; if (n > t && n - t > limit) DeleteFileW((c + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
static std::wstring cachePath(const Source& x, int rate, const std::string& sha) { return cacheDir() + L"\\src_" + widen(sha.substr(0, 16)) + L"_" + widen(x.pick == "-" ? "plain" : x.pick) + L"_" + std::to_wstring(rate) + L".f32"; }
static bool cacheLoad(const std::wstring& path, Source& x) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr); if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz); const size_t n = (size_t)(sz.QuadPart / 8); x.L.resize(n); x.R.resize(n); DWORD got = 0; bool ok = true;
    for (int c = 0; c < 2 && ok; c++) { char* p = (char*)(c ? x.R.data() : x.L.data()); size_t left = n * 4; while (left > 0) { const DWORD part = (DWORD)std::min<size_t>(left, 1u << 30); if (!ReadFile(f, p, part, &got, nullptr) || got != part) { ok = false; break; } p += part; left -= part; } }
    CloseHandle(f); return ok && n > 0; }
static void cacheSave(const std::wstring& path, const Source& x) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (f == INVALID_HANDLE_VALUE) return; DWORD put = 0;
    for (int c = 0; c < 2; c++) { const char* p = (const char*)(c ? x.R.data() : x.L.data()); size_t left = x.L.size() * 4; while (left > 0) { const DWORD part = (DWORD)std::min<size_t>(left, 1u << 30); WriteFile(f, p, part, &put, nullptr); p += part; left -= part; } }
    CloseHandle(f); }
static long long fileSize(const std::wstring& path) { WIN32_FILE_ATTRIBUTE_DATA a; return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ? ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow : 0; }
// the size of the WAV lae_udec will write, from the line of the chosen choice it prints at its start ("> n  name  rate Hz bits-bit frames frames")
static long long expectedFromLog(const std::wstring& log, const std::string& pick) {
    FILE* f = _wfopen(log.c_str(), L"rb"); if (!f) return 0; std::string all; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0 && all.size() < 65536) all.append(b, n); fclose(f);
    const size_t k = all.find(" " + pick + " "); if (k == std::string::npos) return 0; long long rate = 0, bits = 0, frames = 0;
    return sscanf(all.c_str() + k + pick.size() + 2, " %lld Hz %lld-bit %lld frames", &rate, &bits, &frames) == 3 ? frames * 2 * (bits / 8) + 44 : 0; }

struct Job;
static long long riffTotal(const std::wstring& path) {   // the final file size a WAV header announces (RIFF size + 8; RF64: from ds64), 0 if not there yet
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr); if (f == INVALID_HANDLE_VALUE) return 0;
    unsigned char b[28] = {}; DWORD got = 0; ReadFile(f, b, sizeof b, &got, nullptr); CloseHandle(f); if (got < 12) return 0; uint32_t r32; memcpy(&r32, b + 4, 4);
    if (!memcmp(b, "RIFF", 4) && r32 > 36) return (long long)r32 + 8;
    if (!memcmp(b, "RF64", 4) && got >= 28 && !memcmp(b + 12, "ds64", 4)) { long long r64; memcpy(&r64, b + 20, 8); return r64 > 36 ? r64 + 8 : 0; } return 0; }
static std::string have_text(const Job& j);
struct Job { HANDLE proc = nullptr; std::wstring out, log; long long want = 0; bool done = false, ok = true; double frac = 0; };
static bool startDecode(const std::wstring& dir, const std::wstring& src, const std::string& pick, Job& j, std::string& err) {
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE }; HANDLE log = CreateFileW(j.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{ sizeof si }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; si.hStdOutput = log; si.hStdError = log; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{}; std::wstring c = L"\"" + dir + L"\\lae_udec.exe\" dec \"" + src + L"\" \"" + j.out + L"\" --pick=" + widen(pick);
    const BOOL ok = CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi); if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) { err = "cannot start lae_udec.exe"; return false; } CloseHandle(pi.hThread); j.proc = pi.hProcess; return true; }

static std::string have_text(const Job& j) { return j.want > 0 ? std::to_string((int)(j.frac * 100)) + " %" : std::string("reading and checking the file first"); }

// decodes, resamples and lines up both sources of a pair; s[0] is the first source of the line, s[1] the second.
// Progress: checksums 0-5 %, decoding (both sources at once) 5-65 %, reading and resampling 65-98 %, lining up 98-100 %.
static bool preparePair(const Pair& p, int rate, Source s[2], long long& lag, std::string& err, const Say& say) {
    const std::wstring dir = exeDir(), work = workDir(); std::wstring src[2], cache[2]; bool cached[2] = { false, false }, isMp3[2] = { false, false }; Job job[2];
    for (int i = 0; i < 2; i++) {
        Source& x = s[i]; x = Source(); x.file = i ? p.fileB : p.fileA; x.pick = i ? p.pickB : p.pickA; x.name = x.file + (x.pick == "-" ? "" : " [" + x.pick + "]");
        src[i] = dir + L"\\" + widen(x.file); if (!PathFileExistsW(src[i].c_str())) { err = "missing file: " + x.file + " (put it next to lae_abx.exe)"; return false; }
        x.sha = fileSha256(src[i], [&](double f) { say("checking " + x.file + "  " + std::to_string((int)(f * 100)) + " %", 0.025 * i + 0.025 * f); });
        std::string ext = x.file.substr(x.file.find_last_of('.') + 1); for (auto& ch : ext) ch = (char)tolower(ch); isMp3[i] = ext == "mp3" || ext == "m4a" || ext == "aac";
        cache[i] = cachePath(x, rate, x.sha); cached[i] = PathFileExistsW(cache[i].c_str()) != FALSE; }
    // decode the LAE sources that are not kept yet, both at once
    int running = 0; const DWORD t0 = GetTickCount();
    for (int i = 0; i < 2; i++) if (!cached[i] && !isMp3[i] && s[i].pick != "-") {
        job[i].out = work + L"\\dec" + std::to_wstring(i) + L".wav"; job[i].log = work + L"\\dec" + std::to_wstring(i) + L".log";
        if (!startDecode(dir, src[i], s[i].pick, job[i], err)) return false; running++; }
        else job[i].done = true;
    while (running > 0) {
        std::string line; double sum = 0; int parts = 0;
        for (int i = 0; i < 2; i++) { Job& j = job[i]; if (!j.proc) continue;
            if (!j.done && WaitForSingleObject(j.proc, 0) == WAIT_OBJECT_0) { DWORD code = 1; GetExitCodeProcess(j.proc, &code); j.done = true; j.ok = code == 0; j.frac = 1; running--;
                if (!j.ok) { err = "lae_udec.exe failed on " + s[i].file + " (exit code " + std::to_string(code) + "), see " + narrow(j.log); } }
            if (!j.done) { if (!j.want) j.want = riffTotal(j.out + L".part"); const long long have = std::max(fileSize(j.out + L".part"), fileSize(j.out));
                j.frac = j.want > 0 ? std::min(0.99, (double)have / j.want) : 0; }
            line += (line.empty() ? "" : "   |   ") + s[i].file + ": " + (j.done ? std::string("decoded") : have_text(j)); sum += j.frac; parts++; }
        const int secs = (int)((GetTickCount() - t0) / 1000);
        say("decoding  " + line + "   (" + std::to_string(secs) + " s)\r\nFirst time for this file: please wait, decoding takes about a minute. It is kept, so next time it is ready in seconds.", 0.05 + 0.60 * (parts ? sum / parts : 1));
        if (running > 0) Sleep(250); }
    for (int i = 0; i < 2; i++) if (job[i].proc) { CloseHandle(job[i].proc); if (!job[i].ok) { DeleteFileW(job[i].out.c_str()); DeleteFileW((job[i].out + L".part").c_str()); return false; } }
    // read, resample (or load the kept copy)
    for (int i = 0; i < 2; i++) {
        Source& x = s[i]; const double base = 0.65 + 0.165 * i, span = 0.165; const Say sub = [&](const std::string& m, double f) { say(x.file + ": " + m, f); };
        if (cached[i]) { say(x.file + ": prepared earlier, loading", base); if (!cacheLoad(cache[i], x)) { err = "cannot read the kept copy of " + x.file; return false; } continue; }
        if (isMp3[i]) { if (!loadMp3(src[i], rate, x, err, sub, base, span)) return false; }
        else if (x.pick == "-") { if (!loadWav(src[i], rate, x, err, sub, base, span)) return false; }
        else { const bool ok = loadWav(job[i].out, rate, x, err, sub, base, span); DeleteFileW(job[i].out.c_str()); DeleteFileW(job[i].log.c_str()); if (!ok) return false; }
        say(x.file + ": keeping the prepared copy for later pairs", base + span); cacheSave(cache[i], x); }
    lag = 0; if (isMp3[0] || isMp3[1]) { say("lining up the two sources", 0.985); lag = findLag(s[0].L, s[1].L, rate); }   // LAE decodes and WAV files start together; only MP3 / AAC decoders add a delay
    for (int c = 0; c < 2; c++) { std::vector<float>& b = c ? s[1].R : s[1].L; if (lag > 0) b.erase(b.begin(), b.begin() + std::min<long long>(lag, (long long)b.size())); else if (lag < 0) b.insert(b.begin(), (size_t)(-lag), 0.0f); }
    const size_t n = std::min(s[0].L.size(), s[1].L.size()); for (int i = 0; i < 2; i++) { s[i].L.resize(n); s[i].R.resize(n); }
    say("ready", 1.0); return n > 0; }

// ---------------------------------------------------------------- playback (WASAPI shared mode, float stereo at the test rate)
struct Player {
    const std::vector<float>* src[2][2] = {}; std::atomic<int> cur{ -1 }, want{ -1 }; std::atomic<long long> pos{ 0 }, from{ 0 }, to{ 0 }; std::atomic<bool> run{ false };
    std::thread th; int rate = 48000; long long len = 0;
    void set(Source s[2], int r) { stop(); for (int i = 0; i < 2; i++) { src[i][0] = &s[i].L; src[i][1] = &s[i].R; } rate = r; len = (long long)s[0].L.size(); pos = 0; from = 0; to = len; }
    void play(int which) { want = which; if (!run) { run = true; th = std::thread([this]() { loop(); }); } }
    void pause() { want = -1; }
    void stop() { run = false; if (th.joinable()) th.join(); cur = -1; want = -1; }
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en); if (en) en->GetDefaultAudioEndpoint(eRender, eConsole, &dev); if (dev) dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = rate; wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = rate * 8;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); UINT32 bufFrames = 0;
        if (ac && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 400000, 0, &wf, nullptr))) {
            ac->SetEventHandle(ev); ac->GetBufferSize(&bufFrames); ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc); ac->Start();
            const int ramp = rate / 200; int gainStep = 0; int playing = -1;   // 5 ms fade out / in when the source changes or playback pauses
            while (run) { WaitForSingleObject(ev, 200); UINT32 pad = 0; ac->GetCurrentPadding(&pad); UINT32 n = bufFrames - pad; if (!n) continue; BYTE* out = nullptr; if (FAILED(rc->GetBuffer(n, &out))) continue; float* o = (float*)out;
                for (UINT32 i = 0; i < n; i++) { const int w = want;
                    if (playing != w) { if (gainStep > 0) gainStep--; else { playing = w; cur = w; } } else if (playing >= 0 && gainStep < ramp) gainStep++;
                    float l = 0, r = 0; if (playing >= 0) { long long p = pos; if (p >= to || p < from) p = from; const float g = (float)gainStep / ramp; l = (*src[playing][0])[(size_t)p] * g; r = (*src[playing][1])[(size_t)p] * g; pos = p + 1; }
                    o[2 * i] = l; o[2 * i + 1] = r; }
                rc->ReleaseBuffer(n, 0); }
            ac->Stop(); }
        if (rc) rc->Release(); if (ac) ac->Release(); if (dev) dev->Release(); if (en) en->Release(); CloseHandle(ev); CoUninitialize(); } };

// ---------------------------------------------------------------- the test
static double pValue(int k, int n) { double s = 0; for (int i = k; i <= n; i++) { double c = 1; for (int j = 0; j < i; j++) c = c * (n - j) / (j + 1); s += c; } return s / std::pow(2.0, n); }
struct Test {
    Pair pair; int rate = 48000, trials = 16, done = 0, correct = 0; int aIs = 0;   // aIs: which source (0/1 = line order) is button A
    std::vector<int> xIsA; std::vector<std::string> lines; Source s[2]; long long lag = 0; std::string startedAt;
    int xSource(int t) const { return xIsA[(size_t)t] ? aIs : 1 - aIs; } };

// ---------------------------------------------------------------- window
enum { ID_PAIR = 100, ID_ALL, ID_NONE, ID_RATE48, ID_RATE44, ID_TRIALS, ID_PREPARE, ID_STATUS, ID_PA, ID_PB, ID_PX, ID_PY, ID_STOP, ID_XA, ID_XB, ID_POS, ID_SETSTART, ID_SETEND, ID_RESET, ID_RANGE };
static HWND gProgress; static std::atomic<int> gProg{ 0 };
static HWND gWnd, gPair, gAll, gNone, gR48, gR44, gTrials, gPrepare, gStatus, gBtn[5], gChoose[2], gPos, gRange, gRangeBtn[3]; static HFONT gFont;
static Player gPlayer; static Test gTest; static std::vector<Pair> gSet; static std::atomic<bool> gBusy{ false }; static bool gReady = false;
static std::mutex gMsgMu; static std::string gMsg;
static std::vector<int> gQueue; static size_t gQueuePos = 0; static std::vector<std::string> gSummary; static std::string gSessionStart;   // the pairs ticked for this session, in list order
#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_READY (WM_APP + 2)

static void setStatus(const std::string& s) { SetWindowTextW(gStatus, widen(s).c_str()); }
static std::string fmtTime(long long frames, int rate) { char b[32]; long long s = frames / rate; snprintf(b, sizeof b, "%lld:%02lld.%d", s / 60, s % 60, (int)(frames % rate * 10 / rate)); return b; }
static void enableTest(bool on) { for (auto h : gBtn) EnableWindow(h, on); for (auto h : gChoose) EnableWindow(h, on); EnableWindow(gPos, on); for (auto h : gRangeBtn) EnableWindow(h, on); }
static void showTrial() {
    char b[640]; snprintf(b, sizeof b, "Pair %zu of %zu: %s\r\nTrial %d of %d.  Listen to A, B, X and Y, then say whether X is A or X is B.\r\nCorrect so far is not shown until the end.", gQueuePos + 1, gQueue.size(), gTest.pair.name.c_str(), gTest.done + 1, gTest.trials);
    std::string st = b; if (gTest.lag) { char lg[160]; snprintf(lg, sizeof lg, "\r\nLined up: the second source of the pair is moved by %lld samples (%.1f ms).", gTest.lag, 1000.0 * (double)gTest.lag / gTest.rate); st += lg; }
    setStatus(st); }
static void finish() {
    gPlayer.pause(); enableTest(false); const double p = pValue(gTest.correct, gTest.trials);
    const Source& A = gTest.s[gTest.aIs]; const Source& B = gTest.s[1 - gTest.aIs];
    std::string log = std::string(kVersion) + "\r\n" + "test started " + gTest.startedAt + ", finished " + nowText() + "\r\n\r\n" + "pair: " + gTest.pair.name + "\r\n" +
        "button A was: " + A.name + "\r\n  SHA-256 " + A.sha + "\r\n" + "button B was: " + B.name + "\r\n  SHA-256 " + B.sha + "\r\n" +
        "test rate: " + std::to_string(gTest.rate) + " Hz, both sides through the same resampler; lag of the second source: " + std::to_string(gTest.lag) + " samples\r\n\r\n";
    for (auto& l : gTest.lines) log += l + "\r\n";
    char t[160]; snprintf(t, sizeof t, "\r\nTotal: %d/%d\r\np-value (one-sided binomial): %.4f (%.2f %%)\r\n", gTest.correct, gTest.trials, p, 100 * p); log += t;
    Sha256 h; h.add(log.data(), log.size()); const char salt[] = "lae_abx log 2026-11-12"; h.add(salt, sizeof salt - 1); log += "\r\nchecksum (shows accidental edits; not a signature): " + h.hex() + "\r\n";
    const std::wstring dir = exeDir() + L"\\results"; CreateDirectoryW(dir.c_str(), nullptr);
    std::string safe = gTest.pair.name; for (auto& c : safe) if (strchr("\\/:*?\"<>| ", c)) c = '_';
    const std::wstring path = dir + L"\\" + widen(safe) + L"_" + widen(nowStamp()) + L".txt"; FILE* f = _wfopen(path.c_str(), L"wb"); if (f) { fwrite(log.data(), 1, log.size(), f); fclose(f); }
    char b[1024]; snprintf(b, sizeof b, "Finished: %d of %d correct, p = %.4f (%s).\r\nA was: %s\r\nB was: %s\r\nLog: %s",
        gTest.correct, gTest.trials, p, p < 0.05 ? "below 5 %: a difference was heard" : "not below 5 %: no reliable difference", A.name.c_str(), B.name.c_str(), narrow(path).c_str());
    char sl[400]; snprintf(sl, sizeof sl, "%-48s %2d/%-2d  p = %.4f  (A was %s)", gTest.pair.name.c_str(), gTest.correct, gTest.trials, p, A.name.c_str()); gSummary.push_back(sl);
    std::string msg = b; gQueuePos++;
    if (gQueuePos < gQueue.size()) { msg += "\r\n\r\nNext: " + gSet[(size_t)gQueue[gQueuePos]].name + ".  Press \"Next pair\" when you are ready."; SetWindowTextW(gPrepare, (L"Next pair (" + std::to_wstring(gQueue.size() - gQueuePos) + L" left)").c_str()); }
    else {   // the whole session in one summary file
        std::string sum = std::string(kVersion) + "\r\nsession " + gSessionStart + " - " + nowText() + ", test rate " + std::to_string(gTest.rate) + " Hz\r\n\r\n"; for (auto& l : gSummary) sum += l + "\r\n";
        const std::wstring sp = dir + L"\\session_" + widen(nowStamp()) + L".txt"; FILE* g = _wfopen(sp.c_str(), L"wb"); if (g) { fwrite(sum.data(), 1, sum.size(), g); fclose(g); }
        msg = "All ticked pairs done (" + std::to_string(gSummary.size()) + "). Summary: " + narrow(sp) + "\r\nLast pair: " + msg; SetWindowTextW(gPrepare, L"Start ticked pairs"); gQueue.clear(); gQueuePos = 0; }
    setStatus(msg); }
static void choose(bool saysXisA) {
    if (!gReady || gTest.done >= gTest.trials) return;
    const bool truth = gTest.xIsA[(size_t)gTest.done] != 0, ok = truth == saysXisA; if (ok) gTest.correct++;
    char b[160]; snprintf(b, sizeof b, "%s trial %2d: answered \"X is %s\", X was %s: %s", nowText().c_str() + 11, gTest.done + 1, saysXisA ? "A" : "B", truth ? "A" : "B", ok ? "correct" : "wrong");
    gTest.lines.push_back(b); gTest.done++;
    if (gTest.done >= gTest.trials) finish(); else showTrial(); }
static void playButton(int b) {   // 0 A, 1 B, 2 X, 3 Y
    if (!gReady) return; int s; const int t = std::min(gTest.done, gTest.trials - 1);
    if (b == 0) s = gTest.aIs; else if (b == 1) s = 1 - gTest.aIs; else { const int x = gTest.xSource(t); s = b == 2 ? x : 1 - x; }
    gPlayer.play(s); }
static void updateRange() { char b[96]; snprintf(b, sizeof b, "Range: %s - %s", fmtTime(gPlayer.from, gTest.rate).c_str(), fmtTime(gPlayer.to, gTest.rate).c_str()); SetWindowTextA(gRange, b); }

static void startPrepare() {
    if (gBusy) return;
    if (gQueue.empty() || gQueuePos >= gQueue.size()) {   // a new session from the pairs ticked in the list
        gQueue.clear(); gQueuePos = 0; gSummary.clear(); gSessionStart = nowText();
        for (int i = 0; i < (int)gSet.size(); i++) if (SendMessageW(gPair, LB_GETSEL, i, 0) > 0) gQueue.push_back(i);
        if (gQueue.empty()) { setStatus("Tick one or more pairs in the list first (click to tick, click again to untick)."); return; } }
    else if (gReady && gTest.done < gTest.trials) return;   // a pair is still being tested
    const int idx = gQueue[gQueuePos];
    gPlayer.stop(); gReady = false; enableTest(false); EnableWindow(gPrepare, FALSE); gBusy = true;
    gTest = Test(); gTest.pair = gSet[(size_t)idx]; gTest.rate = SendMessageW(gR44, BM_GETCHECK, 0, 0) == BST_CHECKED ? 44100 : 48000;
    wchar_t tb[16]; GetWindowTextW(gTrials, tb, 16); gTest.trials = std::max(5, std::min(64, _wtoi(tb) ? _wtoi(tb) : 16));
    std::thread([]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION); std::string err;
        gProg = 0;
        auto say = [](const std::string& m, double f) { if (f >= 0) gProg = (int)(f * 1000); { std::lock_guard<std::mutex> g(gMsgMu); gMsg = "Preparing " + gTest.pair.name + "  (" + std::to_string(gProg / 10) + " %)\r\n" + m; } PostMessageW(gWnd, WM_APP_STATUS, 0, 0); };
        const bool ok = preparePair(gTest.pair, gTest.rate, gTest.s, gTest.lag, err, say);
        MFShutdown(); CoUninitialize();
        { std::lock_guard<std::mutex> g(gMsgMu); gMsg = ok ? "" : "Could not prepare: " + err; } PostMessageW(gWnd, WM_APP_READY, ok, 0); }).detach(); }

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND: switch (LOWORD(wp)) {
        case ID_PREPARE: startPrepare(); return 0;
        case ID_ALL: case ID_NONE: if (!gBusy && (gQueue.empty() || gQueuePos >= gQueue.size())) SendMessageW(gPair, LB_SETSEL, LOWORD(wp) == ID_ALL, -1); return 0;
        case ID_PA: playButton(0); return 0; case ID_PB: playButton(1); return 0; case ID_PX: playButton(2); return 0; case ID_PY: playButton(3); return 0;
        case ID_STOP: gPlayer.pause(); return 0;
        case ID_XA: choose(true); return 0; case ID_XB: choose(false); return 0;
        case ID_SETSTART: if (gReady) { gPlayer.from = std::min<long long>(gPlayer.pos, gPlayer.to - gTest.rate / 2); updateRange(); } return 0;
        case ID_SETEND: if (gReady) { gPlayer.to = std::max<long long>(gPlayer.pos, gPlayer.from + gTest.rate / 2); updateRange(); } return 0;
        case ID_RESET: if (gReady) { gPlayer.from = 0; gPlayer.to = gPlayer.len; updateRange(); } return 0; } break;
    case WM_HSCROLL: if ((HWND)lp == gPos && gReady) { const long long v = SendMessageW(gPos, TBM_GETPOS, 0, 0); gPlayer.pos = gPlayer.len * v / 1000; } return 0;
    case WM_TIMER: if (gReady) { SendMessageW(gPos, TBM_SETPOS, TRUE, (LPARAM)(gPlayer.len ? gPlayer.pos * 1000 / gPlayer.len : 0)); } return 0;
    case WM_APP_STATUS: { std::lock_guard<std::mutex> g(gMsgMu); setStatus(gMsg); } SendMessageW(gProgress, PBM_SETPOS, gProg, 0); return 0;
    case WM_APP_READY: gBusy = false; EnableWindow(gPrepare, TRUE); SendMessageW(gProgress, PBM_SETPOS, wp ? 1000 : 0, 0);
        if (!wp) { std::lock_guard<std::mutex> g(gMsgMu); setStatus(gMsg); return 0; }
        gTest.aIs = (int)(rnd32() & 1); gTest.xIsA.clear(); for (int i = 0; i < gTest.trials; i++) gTest.xIsA.push_back((int)(rnd32() & 1)); gTest.startedAt = nowText();
        gPlayer.set(gTest.s, gTest.rate); gReady = true; enableTest(true); updateRange(); showTrial(); return 0;
    case WM_DESTROY: gPlayer.stop(); removeWorkDir(); PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, m, wp, lp); }

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cw, int chh, int id) {
    const UINT dpi = GetDpiForWindow(gWnd); auto s = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(cw), s(chh), gWnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)gFont, TRUE); return h; }

// ---------------------------------------------------------------- command-line preparation (checks)
static int cliPrepare(int idx, int rate, const std::wstring& outDir) {
    gSet = readSet(exeDir()); if (idx < 1 || idx > (int)gSet.size()) { printf("pair %d not in abx_set.txt (%zu pairs)\n", idx, gSet.size()); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION); Source s[2]; long long lag = 0; std::string err;
    const bool ok = preparePair(gSet[(size_t)idx - 1], rate, s, lag, err, [](const std::string& m, double f) { static int last = -1; const int pct = f >= 0 ? (int)(f * 100) : last; if (pct != last) { last = pct; printf("  %3d %%  %s\n", pct, m.c_str()); } });
    MFShutdown(); removeWorkDir(); if (!ok) { printf("failed: %s\n", err.c_str()); return 1; }
    CreateDirectoryW(outDir.c_str(), nullptr);
    for (int i = 0; i < 2; i++) { const std::wstring p = outDir + (i ? L"\\B.wav" : L"\\A.wav"); FILE* f = _wfopen(p.c_str(), L"wb"); const uint32_t n = (uint32_t)s[i].L.size(), bytes = n * 8;
        fwrite("RIFF", 1, 4, f); uint32_t v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f); uint16_t t = 3, c = 2; fwrite(&t, 2, 1, f); fwrite(&c, 2, 1, f);
        v = (uint32_t)rate; fwrite(&v, 4, 1, f); v = (uint32_t)rate * 8; fwrite(&v, 4, 1, f); t = 8; fwrite(&t, 2, 1, f); t = 32; fwrite(&t, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
        for (uint32_t k = 0; k < n; k++) { fwrite(&s[i].L[k], 4, 1, f); fwrite(&s[i].R[k], 4, 1, f); } fclose(f); }
    FILE* f = _wfopen((outDir + L"\\prepare.txt").c_str(), L"wb"); fprintf(f, "pair: %s\nrate: %d\nlag of B: %lld\nframes: %zu\nA: %s sha256 %s\nB: %s sha256 %s\n", gSet[(size_t)idx - 1].name.c_str(), rate, lag, s[0].L.size(), s[0].name.c_str(), s[0].sha.c_str(), s[1].name.c_str(), s[1].sha.c_str()); fclose(f);
    printf("prepared: lag %lld, %zu frames at %d Hz\n", lag, s[0].L.size(), rate); return 0; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 5 && !wcscmp(argv[1], L"--prepare")) { if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); } return cliPrepare(_wtoi(argv[2]), _wtoi(argv[3]), argv[4]); }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{ sizeof ic, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS }; InitCommonControlsEx(&ic);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"lae_abx"; RegisterClassW(&wc);
    gWnd = CreateWindowExW(0, L"lae_abx", L"LAE ABX listening test", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    const UINT dpi = GetDpiForWindow(gWnd); RECT rc{ 0, 0, MulDiv(640, (int)dpi, 96), MulDiv(610, (int)dpi, 96) }; AdjustWindowRectExForDpi(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    SetWindowPos(gWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    NONCLIENTMETRICSW nm{ sizeof nm }; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0, dpi); gFont = CreateFontIndirectW(&nm.lfMessageFont);
    mk(L"STATIC", L"Tick the pairs you want to test (click to tick or untick); they run one after the other:", 0, 12, 10, 616, 20, -1);
    gPair = mk(L"LISTBOX", L"", LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 12, 32, 616, 140, ID_PAIR);
    gR48 = mk(L"BUTTON", L"48 kHz", BS_AUTORADIOBUTTON | WS_GROUP, 56, 184, 80, 22, ID_RATE48); gR44 = mk(L"BUTTON", L"44.1 kHz", BS_AUTORADIOBUTTON, 140, 184, 90, 22, ID_RATE44); SendMessageW(gR48, BM_SETCHECK, BST_CHECKED, 0);
    mk(L"STATIC", L"Trials:", 0, 250, 187, 46, 20, -1); gTrials = mk(L"EDIT", L"16", WS_BORDER | ES_NUMBER, 298, 184, 40, 22, ID_TRIALS);
    gAll = mk(L"BUTTON", L"Tick all", BS_PUSHBUTTON, 348, 182, 66, 26, ID_ALL); gNone = mk(L"BUTTON", L"Untick all", BS_PUSHBUTTON, 418, 182, 66, 26, ID_NONE);
    gPrepare = mk(L"BUTTON", L"Start ticked pairs", BS_PUSHBUTTON | WS_TABSTOP, 488, 182, 140, 26, ID_PREPARE);
    gStatus = mk(L"STATIC", L"Tick pairs and press \"Start ticked pairs\". The first time, a pair is decoded first (about one minute); prepared files are kept, so a file used again (later pairs, or the next time you start the program) is ready in seconds. After each pair you see its result and go on with \"Next pair\".", SS_LEFT | WS_BORDER, 12, 220, 616, 84, ID_STATUS);
    gProgress = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 12, 310, 616, 18, -1); SendMessageW(gProgress, PBM_SETRANGE32, 0, 1000);
    const wchar_t* bn[5] = { L"Play A", L"Play B", L"Play X", L"Play Y", L"Stop" }; for (int i = 0; i < 5; i++) gBtn[i] = mk(L"BUTTON", bn[i], BS_PUSHBUTTON, 12 + i * 124, 342, 116, 32, ID_PA + i);
    gChoose[0] = mk(L"BUTTON", L"X is A  (Y is B)", BS_PUSHBUTTON, 12, 386, 304, 36, ID_XA); gChoose[1] = mk(L"BUTTON", L"X is B  (Y is A)", BS_PUSHBUTTON, 324, 386, 304, 36, ID_XB);
    gPos = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 12, 436, 616, 30, ID_POS); SendMessageW(gPos, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    gRange = mk(L"STATIC", L"Range: -", 0, 12, 476, 300, 20, ID_RANGE);
    const wchar_t* rn[3] = { L"Set start", L"Set end", L"Whole piece" }; for (int i = 0; i < 3; i++) gRangeBtn[i] = mk(L"BUTTON", rn[i], BS_PUSHBUTTON, 320 + i * 104, 470, 98, 28, ID_SETSTART + i);
    mk(L"STATIC", L"A and B are assigned at random; the program tells which was which only at the end. X and Y are A and B in a random order in every trial. "
                  L"Switching keeps the position. Results are written to the results folder next to this program.", SS_LEFT, 12, 512, 616, 60, -1);
    enableTest(false); gSet = readSet(exeDir()); pruneCache();
    for (auto& p : gSet) SendMessageW(gPair, LB_ADDSTRING, 0, (LPARAM)widen(p.name).c_str());
    if (gSet.empty()) setStatus("abx_set.txt was not found next to lae_abx.exe, or it lists no pairs.");
    SetTimer(gWnd, 1, 200, nullptr); ShowWindow(gWnd, show); MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { if (!IsDialogMessageW(gWnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0; }
