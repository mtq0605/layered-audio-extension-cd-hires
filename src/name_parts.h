// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// The five positions of a master's name (2026-11-18): name, part2, part3, part4, part5.
// Each is optional free text of the publisher; no position has a fixed meaning. The encoders join the filled ones in this fixed order
// into the one name a file already stores (LAEY pack record / "LNAM", LAES master record):
//   "<name> - <part2>, <part3>, <part4>, <part5>"   (empty positions left out; without a name the rest alone, joined by ", ")
// The file holds only the joined text: no position markers, no new field. Decoders and players show it as it is and never
// split it. Giving only a name writes exactly what was written before.
#pragma once
#include <cstring>
#include <string>

namespace nameparts {

constexpr int kCount = 5;
inline const char* key(int i) { static const char* const k[kCount] = {"name", "part2", "part3", "part4", "part5"}; return k[i]; }

// the position whose key starts s and is followed by '=' (for example "part3=text"): its index, and *rest = the text after '='; else -1
inline int match(const char* s, const char** rest) {
    for (int i = 0; i < kCount; i++) { const size_t n = strlen(key(i)); if (!strncmp(s, key(i), n) && s[n] == '=') { *rest = s + n + 1; return i; } }
    return -1; }

struct Parts {
    std::string v[kCount]; bool given[kCount] = {false, false, false, false, false};
    bool anyGiven() const { for (bool g : given) if (g) return true; return false; }
    bool suffixGiven() const { for (int i = 1; i < kCount; i++) if (given[i]) return true; return false; }
    std::string join() const {
        std::string rest; for (int i = 1; i < kCount; i++) if (!v[i].empty()) { if (!rest.empty()) rest += ", "; rest += v[i]; }
        if (v[0].empty()) return rest; if (rest.empty()) return v[0]; return v[0] + " - " + rest; }
    // "" when the joined name fits in 255 bytes, else the reason (who: for example "master2" or "master 3")
    std::string tooLong(const std::string& who) const {
        const size_t n = join().size(); if (n <= 255) return "";
        return who + ": the name and its positions together are " + std::to_string(n) + " bytes, at most 255 (" + std::to_string(n - 255) + " bytes too many)"; }
};

}  // namespace nameparts
