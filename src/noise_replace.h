// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Decision 21 (2026-11-15) for the LAE Noise Marker and the small tools: an output file that exists is not replaced silently.
// On a console (input and output both a console) the person is asked twice and only two answers "yes" replace it; without a console
// it is refused unless --overwrite was given. The texts are those of every LAE tool (an operating explanation: English, then Chinese).
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <io.h>
extern "C" __declspec(dllimport) void* __stdcall GetStdHandle(unsigned long);   // kernel32, declared here to keep <windows.h> out
extern "C" __declspec(dllimport) int __stdcall GetConsoleMode(void*, unsigned long*);
#else
#include <unistd.h>
#endif

namespace nrep {
inline bool onConsole() {   // a person at a console on both ends (not a pipe, a file or NUL)
#ifdef _WIN32
    unsigned long m; return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout)) && GetConsoleMode(GetStdHandle((unsigned long)-10), &m) && GetConsoleMode(GetStdHandle((unsigned long)-11), &m);
#else
    return isatty(fileno(stdin)) && isatty(fileno(stdout));
#endif
}
inline bool answeredYes() { char b[256]; if (!fgets(b, sizeof b, stdin)) return false; std::string s(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t k = 0; while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++; return s.substr(k) == "yes"; }
// an output that can be opened is there (as in laes_pick.h; no <filesystem>, so that the C++14 tools build); file names are UTF-8
// in lae_noise (win_utf8.h sets the UTF-8 locale) and ASCII in the generators
inline bool exists(const std::string& path) { FILE* t = fopen(path.c_str(), "rb"); if (!t) return false; fclose(t); return true; }
// true: the file may be written (it does not exist, --overwrite was given, or two "yes" at the console); false: refused, message printed
inline bool mayReplace(const std::string& path, bool overwrite) {
    if (!exists(path) || overwrite) return true;
    const char* f = path.c_str();
    if (onConsole()) {
        printf("\"%s\" already exists. Replace it? Type yes to continue: \n\"%s\" %s", f, f, u8"已经存在。要替换吗？输入 yes 继续："); fflush(stdout);
        if (answeredYes()) { printf("Really replace \"%s\"? It cannot be undone. Type yes again: \n%s\"%s\" %s", f, u8"确定要替换 ", f, u8"吗？替换后无法恢复。再输入一次 yes："); fflush(stdout);
            if (answeredYes()) return true; } }
    printf("\"%s\" already exists; it was not replaced (give --overwrite to replace it).\n\"%s\" %s\n", f, f, u8"已经存在，没有替换（加 --overwrite 才替换）。");
    return false; }
inline bool& overwriteFlag() { static bool given = false; return given; }   // --overwrite was given (set by takeOverwrite)
// takes "--overwrite" out of the arguments (anywhere after the program name) and sets overwriteFlag(); true when it was there
inline bool takeOverwrite(int& argc, char** argv) {
    bool found = false;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--overwrite")) { found = true; for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1]; argc--; i--; }
    if (found) overwriteFlag() = true; return found; }
inline bool mayReplace(const std::string& path) { return mayReplace(path, overwriteFlag()); }
}
