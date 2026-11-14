// win_utf8.h - on Windows, every string inside the tools is UTF-8 (2026-11-05; the approach of flac's win_utf8_io).
//
// Windows hands a narrow main() its arguments in the system code page (GBK on a Chinese system) and reads narrow file names
// in that code page too: a name GBK cannot hold (Japanese, Korean, rare characters) cannot be opened, and a tag typed on the
// command line was written as GBK bytes where FLAC, RIFF INFO and ID3 (encoding 3) say UTF-8. lae_u8::init(argc, argv),
// the first line of main(), changes that for the whole program:
//   - argv is replaced by the UTF-8 form of the wide command line, parsed by the C runtime itself (the same splitting as before);
//   - the C runtime's character type is set to UTF-8 (LC_CTYPE only: numbers keep the "C" format, so every text and file the
//     tools write stays as before), and from then on the C and C++ libraries read every narrow file name as UTF-8:
//     fopen, remove, rename, _popen and std::filesystem::path(std::string) included, path::string() gives UTF-8;
//   - environment variables with non-ASCII values are put in their UTF-8 form, so getenv() agrees with argv;
//   - a console showing the output is switched to UTF-8 while the tool runs and set back when it ends; output to a file or a
//     pipe is UTF-8 bytes. (Without the switch a Chinese console shows Korean as '?'.) A tool that ends through _exit calls
//     restoreConsole() first (lae_enc's emergency stop); init() after the tool's own signal() calls, so on Ctrl+C the console
//     is set back before the tool's handler runs.
// init() acts once per process: lae_enc runs the decoder's main inside itself with arguments of its own.
// With ASCII arguments, paths and tags nothing changes: the output files are byte-identical with the builds before.
// Other platforms: init() does nothing (their file names are bytes already, normally UTF-8).
// fromAnsi(): text that is not UTF-8 and comes from an older Windows program (a CUE sheet without a BOM, say), turned into
// UTF-8 from the system code page; the result goes to fs::u8path() or to fopen() after init().
#pragma once
#include <string>
#include <vector>
#if defined(_WIN32) && defined(_MSC_VER)   // the MSVC runtime (UCRT); other Windows compilers keep the system code page as before
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <io.h>
#include <corecrt_startup.h>   // _configure_wide_argv: the C runtime's own parse of the wide command line
// the few Win32 calls needed, declared here so that <windows.h> and its macros (min, max, ...) stay out of the tools
extern "C" {
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int, unsigned long, const char*, int, wchar_t*, int);
__declspec(dllimport) unsigned int __stdcall GetConsoleOutputCP(void);
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
__declspec(dllimport) int __stdcall SetConsoleCtrlHandler(int(__stdcall*)(unsigned long), int);
}
#endif

namespace lae_u8 {
#if defined(_WIN32) && defined(_MSC_VER)
inline std::string fromWide(const wchar_t* w) {
    const int n = WideCharToMultiByte(65001, 0, w, -1, nullptr, 0, nullptr, nullptr); if (n <= 1) return std::string();
    std::string s((size_t)n, '\0'); WideCharToMultiByte(65001, 0, w, -1, &s[0], n, nullptr, nullptr); s.resize((size_t)n - 1); return s; }
inline std::string fromAnsi(const std::string& a) {
    if (a.empty()) return a;
    const int n = MultiByteToWideChar(0, 0, a.data(), (int)a.size(), nullptr, 0); if (n <= 0) return a;   // 0 = CP_ACP
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(0, 0, a.data(), (int)a.size(), &w[0], n); return fromWide(w.c_str()); }
inline unsigned int& savedCP() { static unsigned int cp = 0; return cp; }
inline void restoreConsole() { if (savedCP()) { fflush(stdout); fflush(stderr); SetConsoleOutputCP(savedCP()); savedCP() = 0; } }
inline int __stdcall onConsoleEvent(unsigned long) { restoreConsole(); return 0; }   // Ctrl+C / close: set back, then the tool's own handling
inline void init(int& argc, char**& argv) {
    static std::vector<std::string> store; static std::vector<char*> ptrs;
    static bool done = false; if (done) return; done = true;   // once per process: lae_enc calls the decoder's main with arguments of its own
    if (!setlocale(LC_CTYPE, ".UTF8")) {   // no UTF-8 locale in this runtime (Windows before 10 version 1803): arguments and names stay in the system code page, as before
        fprintf(stderr, "warning: this Windows has no UTF-8 locale (Windows 10 version 1803 or later needed); file names and tags outside the system code page cannot be used\n"); return; }
    if (_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0 && __wargv) {
        for (int i = 0; i < __argc; i++) store.push_back(fromWide(__wargv[i]));
        for (auto& s : store) ptrs.push_back(&s[0]); ptrs.push_back(nullptr);
        argc = (int)store.size(); argv = ptrs.data(); }
    (void)_wgetenv(L"PATH");   // makes the C runtime build its wide environment (_wenviron); _putenv_s below
    // sets the process environment in UTF-8 (what getenv and child processes see); the copy in _wenviron goes through CP_ACP, unused
    if (wchar_t** env = _wenviron) {
        std::vector<std::pair<std::string, std::string>> put;
        for (; *env; env++) { bool wide = false; for (const wchar_t* c = *env; *c; c++) if (*c >= 0x80) { wide = true; break; }
            if (!wide) continue; const std::string kv = fromWide(*env); const size_t eq = kv.find('=', 1);
            if (eq != std::string::npos) put.push_back({kv.substr(0, eq), kv.substr(eq + 1)}); }
        for (auto& p : put) _putenv_s(p.first.c_str(), p.second.c_str()); }
    if ((_isatty(_fileno(stdout)) || _isatty(_fileno(stderr))) && GetConsoleOutputCP() != 65001) {
        savedCP() = GetConsoleOutputCP(); SetConsoleOutputCP(65001); std::atexit(restoreConsole); SetConsoleCtrlHandler(onConsoleEvent, 1); } }
#else
inline std::string fromAnsi(const std::string& a) { return a; }
inline void init(int&, char**&) {}
inline void restoreConsole() {}
#endif
}
