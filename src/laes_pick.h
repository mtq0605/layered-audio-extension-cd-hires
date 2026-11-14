// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// laes: the tick list for choosing among several masters that fit one --pick: they are ticked, or numbers and ranges are typed below the list.
// Each candidate is shown with its number, rate, word length and name, and a box to tick. Keys: Up / Down move, Space ticks or unticks,
// Enter is done, Esc cancels. Below the list a line takes numbers and ranges ("6,12", "6-12", "2-4,9"); Enter ticks the candidates whose
// numbers they hold (the boxes are set to exactly those), a second Enter is done. Only in a console (stdin and stdout are terminals);
// in a script the candidates are listed and the tool stops (exit code 1), and --pick takes the numbers or ranges directly.
// Test hook: LAES_TEST_KEYS=<keys> runs the list without a console: U / D = Up / Down, space = Space, | = Enter, ! = Esc, < = Backspace,
// any other character is typed into the line.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
// the three Win32 calls needed, declared here so that <windows.h> stays out of the tool (as in win_utf8.h)
extern "C" __declspec(dllimport) void* __stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall GetConsoleMode(void*, unsigned long*);
extern "C" __declspec(dllimport) int __stdcall SetConsoleMode(void*, unsigned long);
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

namespace laespick {

// "6,12" / "6-12" / "2-4, 9": every number in them (a-b with a <= b, both >= 1); false: not such a list
inline bool parseRanges(const std::string& s, std::vector<long>& out) {
    out.clear(); size_t i = 0; bool any = false;
    auto skip = [&] { while (i < s.size() && s[i] == ' ') i++; };
    for (;;) { skip(); if (i >= s.size()) return any;
        if (s[i] < '0' || s[i] > '9') return false; long a = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') { a = a * 10 + (s[i] - '0'); if (a > 100000) return false; i++; }
        long b = a; skip();
        if (i < s.size() && s[i] == '-') { i++; skip(); if (i >= s.size() || s[i] < '0' || s[i] > '9') return false; b = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') { b = b * 10 + (s[i] - '0'); if (b > 100000) return false; i++; } skip(); }
        if (a < 1 || b < a) return false; for (long v = a; v <= b; v++) out.push_back(v); any = true;
        if (i >= s.size()) return true; if (s[i] != ',') return false; i++; } }

struct Item { long no; std::string line; };   // no: the number the user gives (list entry, or master k); line: "176400 Hz 24-bit  master 2 \"name\""

enum Key { kUp, kDown, kSpace, kEnter, kEsc, kBack, kChar, kNone };

class Input {
public:
    Input() { const char* t = getenv("LAES_TEST_KEYS"); if (t) { test = true; keys = t; }
#ifndef _WIN32
        else if (isatty(0)) { tcgetattr(0, &old); termios r = old; r.c_lflag &= ~(ICANON | ECHO | ISIG);   // ISIG off (FU13-9): Ctrl-C arrives as byte 3 = Esc, and the terminal is set back
            r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &r); raw = true; }
#endif
    }
    ~Input() {
#ifndef _WIN32
        if (raw) tcsetattr(0, TCSANOW, &old);
#endif
    }
    Key get(char& ch) {
        ch = 0;
        if (test) { if (at >= keys.size()) return kEsc; const char c = keys[at++];
            if (c == 'U') return kUp; if (c == 'D') return kDown; if (c == ' ') return kSpace; if (c == '|') return kEnter; if (c == '!') return kEsc; if (c == '<') return kBack; ch = c; return kChar; }
#ifdef _WIN32
        const int c = _getch();
        if (c == 0 || c == 0xE0) { const int d = _getch(); return d == 72 ? kUp : d == 80 ? kDown : kNone; }
        if (c == ' ') return kSpace; if (c == '\r' || c == '\n') return kEnter; if (c == 27 || c == 3) return kEsc; if (c == 8) return kBack;
        ch = (char)c; return kChar;
#else
        unsigned char c = 0; if (read(0, &c, 1) != 1) return kEsc;
        if (c == 27) { unsigned char q[2]; fd_set fs; FD_ZERO(&fs); FD_SET(0, &fs); timeval tv{0, 50000};
            if (select(1, &fs, nullptr, nullptr, &tv) > 0 && read(0, q, 2) == 2 && q[0] == '[') return q[1] == 'A' ? kUp : q[1] == 'B' ? kDown : kNone;
            return kEsc; }
        if (c == ' ') return kSpace; if (c == '\r' || c == '\n') return kEnter; if (c == 3) return kEsc; if (c == 127 || c == 8) return kBack;
        ch = (char)c; return kChar;
#endif
    }
    bool test = false;
private:
    std::string keys; size_t at = 0;
#ifndef _WIN32
    termios old{}; bool raw = false;
#endif
};

// a console on both sides, or the test hook
inline bool interactive() {
    if (getenv("LAES_TEST_KEYS")) return true;
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(0) && isatty(1);
#endif
}

// DEC 21 (2026-11-15): an output file that exists is not replaced silently. In a console the user is asked twice and only two "yes"
// answers replace it; not in a console it is refused unless --overwrite was given (overwrite = true). The texts are the same in every
// LAE tool (an operating explanation: English, then Chinese). true: the file may be written (it does not exist, or replacing it is agreed).
inline bool mayReplace(const std::string& path, bool overwrite) {
    FILE* t = fopen(path.c_str(), "rb"); if (!t) return true; fclose(t); if (overwrite) return true;
    auto refuse = [&]() { printf("\"%s\" already exists; it was not replaced (give --overwrite to replace it).\n", path.c_str()); printf(u8"\"%s\" 已经存在，没有替换（加 --overwrite 才替换）。\n", path.c_str()); return false; };
    if (!interactive()) return refuse();
    auto yes = []() { char a[64] = {0}; if (!fgets(a, sizeof a, stdin)) return false; std::string s(a); size_t p = 0; while (p < s.size() && !isalpha((unsigned char)s[p])) p++;   // skips a byte-order mark or spaces
        s = s.substr(p); while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back(); return s == "yes"; };
    printf("\"%s\" already exists. Replace it? Type yes to continue: \n", path.c_str()); printf(u8"\"%s\" 已经存在。要替换吗？输入 yes 继续：", path.c_str()); fflush(stdout);
    if (!yes()) return refuse();
    printf("Really replace \"%s\"? It cannot be undone. Type yes again: \n", path.c_str()); printf(u8"确定要替换 \"%s\" 吗？替换后无法恢复。再输入一次 yes：", path.c_str()); fflush(stdout);
    if (!yes()) return refuse();
    return true; }

// returns the ticked items' numbers in list order; empty: cancelled
inline std::vector<long> tick(const std::string& title, const std::vector<Item>& items, const std::string& numberHint) {
#ifdef _WIN32
    { void* h = GetStdHandle((unsigned long)-11); unsigned long m = 0; if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | 0x0004); }   // ENABLE_VIRTUAL_TERMINAL_PROCESSING: the redraw below
#endif
    if (items.empty()) return {};   // FU13-9: nothing to tick (Space would touch on[0])
    Input in; size_t cur = 0; std::vector<bool> on(items.size(), false); std::string typed, msg; int drawn = 0;
    for (;;) {
        if (drawn && !in.test) printf("\x1b[%dA", drawn);
        int n = 0; auto line = [&](const std::string& s) { if (!in.test) printf("\x1b[2K"); printf("%s\n", s.c_str()); n++; };
        line(title);
        for (size_t k = 0; k < items.size(); k++) { char b[32]; snprintf(b, sizeof b, "%s [%s] %4ld  ", k == cur ? ">" : " ", on[k] ? "x" : " ", items[k].no); line(b + items[k].line); }
        line("Up / Down: move   Space: tick   Enter: done   Esc: cancel");
        line("Or type numbers or ranges (" + numberHint + "), then Enter to tick them:");
        line("Numbers: " + typed); line(msg); fflush(stdout); drawn = n; msg.clear();
        char ch = 0; const Key k = in.get(ch);
        if (k == kUp) { if (cur > 0) cur--; }
        else if (k == kDown) { if (cur + 1 < items.size()) cur++; }
        else if (k == kSpace) on[cur] = !on[cur];
        else if (k == kBack) { if (!typed.empty()) typed.pop_back(); }
        else if (k == kChar) { if ((ch >= '0' && ch <= '9') || ch == '-' || ch == ',') typed += ch; }
        else if (k == kEsc) { printf("cancelled: nothing written\n"); return {}; }
        else if (k == kEnter) {
            if (!typed.empty()) { std::vector<long> v; if (!parseRanges(typed, v)) { msg = "'" + typed + "' is not a list of numbers or ranges"; typed.clear(); continue; }
                std::vector<bool> nw(items.size(), false); bool any = false;
                for (size_t j = 0; j < items.size(); j++) for (long x : v) if (x == items[j].no) { nw[j] = true; any = true; }
                if (!any) { msg = "none of " + typed + " is one of the numbers above"; typed.clear(); continue; }
                on = nw; typed.clear(); msg = "ticked; Enter again to write them"; continue; }
            std::vector<long> r; for (size_t j = 0; j < items.size(); j++) if (on[j]) r.push_back(items[j].no);
            if (r.empty()) { msg = "nothing ticked: tick at least one, or Esc to cancel"; continue; }
            return r; } }
}

}  // namespace laespick
