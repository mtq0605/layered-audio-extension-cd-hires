// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// laes_choices.h - the list of what an LAES file offers, in one place for the player (laes_play.h, the plug-ins) and `laes dec --list`
// (2026-11-11, spec/lossless/SPEC.md, Section 10). Order and names:
//   1. the core master ("core"); if its word length is above 16 bit, its derived 16-bit version right after it
//   2. per master, in the menu order (the publisher's marks; else the file order), one group, k = its place in that order: the master itself ("master_<k>_<rate>_<bits>"), its rate levels from high to low
//      ("master_<k>_level_<rate>_<bits>"), then its derived 16-bit versions (own rate first, then each level from high to low;
//      "derived16_<rate>_from_<source name>") - only for a master above 16 bit whose flag bit 0 (no derived versions) is clear
// Every master of every word length (16 / 24 / 32) is a master of its own; a derived version never replaces or hides one. The text
// shown ("what") carries the master's number and, when it has one, its name as the publisher wrote it; rate levels and derived versions
// carry the name of the master they come from. Names never select anything.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "laes_file.h"
namespace laesc {
struct Spec { int kind = 0; int m = -1, no = 0, level = 0, from = -1; int rate = 0, bits = 0; int64_t frames = 0; std::string name, what; };   // kind 0 core, 1 master, 2 rate level, 3 derived 16-bit
// the menu order (2026-11-11): the file indices in the order the publisher marked (menu positions), else the file order;
// the number shown for a master is its place in that order. The file order itself stays the encoder's (smallest file).
inline std::vector<size_t> menuOrder(const std::vector<laes::MasterInfo>& ms) { std::vector<size_t> o(ms.size()); for (size_t i = 0; i < o.size(); i++) o[i] = i;
    bool any = false; for (const auto& I : ms) any |= I.menu != 0; if (any) for (size_t i = 0; i < ms.size(); i++) if (ms[i].menu >= 1 && ms[i].menu <= (int)ms.size()) o[(size_t)ms[i].menu - 1] = i; return o; }
inline int displayNo(const std::vector<laes::MasterInfo>& ms, size_t i) { const std::vector<size_t> o = menuOrder(ms); for (size_t k = 0; k < o.size(); k++) if (o[k] == i) return (int)k + 1; return (int)i + 1; }
inline std::string masterLabel(size_t no, const laes::MasterInfo& I) { std::string s = "master " + std::to_string(no); if (!I.name.empty()) s += " \"" + I.name + "\""; return s; }
// offered[i] = false: master i is left out with its levels and derived versions (`laes dec` after a failed SHA-256); the player lists all.
// head: the core master's flags (bit 0: no derived version) and the default master (2026-11-12); null: neither.
// Wording (spec/lossless/SPEC.md, Section 10.3): only the core master and the masters themselves are "lossless"; rate levels and 16-bit versions are only
// "derived from" their master, a rate level being part of the master's stored data (obtained while decoding, no extra space), a 16-bit
// version made when it is played.
inline std::vector<Spec> build(int baseRate, int coreBits, int64_t coreFrames, const std::vector<laes::MasterInfo>& ms, const std::vector<bool>* offered = nullptr, const laes::Head* head = nullptr) {
    std::vector<Spec> ch;
    { Spec c; c.kind = 0; c.rate = baseRate; c.bits = coreBits; c.frames = coreFrames; c.name = "core"; c.what = "the core master (lossless)"; ch.push_back(c); }
    auto derived = [&](size_t from, const std::string& srcLabel) { const Spec& S = ch[from]; Spec d = S; d.kind = 3; d.from = (int)from; d.bits = 16;
        d.name = "derived16_" + std::to_string(S.rate) + "_from_" + S.name; d.what = "derived from " + srcLabel; ch.push_back(d); };
    if (coreBits > 16 && !(head && (head->coreFlags & laes::kNoDerive))) derived(0, "the core master");
    const std::vector<size_t> order = menuOrder(ms);
    for (size_t pos = 0; pos < order.size(); pos++) { const size_t i = order[pos]; if (offered && !(*offered)[i]) continue; const laes::MasterInfo& I = ms[i]; const int no = (int)pos + 1;
        const std::string mk = "master_" + std::to_string(no), lb = masterLabel((size_t)no, I);
        const size_t first = ch.size();
        { Spec c; c.kind = 1; c.m = (int)i; c.no = no; c.level = I.stages; c.rate = I.rate; c.bits = I.bits; c.frames = I.frames; c.name = mk + "_" + std::to_string(I.rate) + "_" + std::to_string(I.bits); c.what = lb + " (lossless)"; ch.push_back(c); }
        for (int k = I.stages - 1; k >= 0; k--) { Spec c; c.kind = 2; c.m = (int)i; c.no = no; c.level = k; c.rate = I.rate >> (I.stages - k); c.bits = I.bits; c.frames = laes::levLen(I.frames, I.stages - k);
            c.name = mk + "_level_" + std::to_string(c.rate) + "_" + std::to_string(c.bits); c.what = "derived from " + lb; ch.push_back(c); }
        const size_t last = ch.size();
        if (I.bits > 16 && !(I.flags & laes::kNoDerive)) for (size_t j = first; j < last; j++) derived(j, lb); }
    return ch; }
// the default choice (spec/lossless/SPEC.md, Section 10.3): the default master of the header (the master itself, never a level or a derived version), else the
// core master (0). A default master not in the list (left out after a failed check): the core master.
inline int defaultIndex(const std::vector<Spec>& ch, const laes::Head& h) { if (h.def > 0) for (size_t j = 0; j < ch.size(); j++) if (ch[j].kind == 1 && ch[j].m == h.def - 1) return (int)j; return 0; }
}  // namespace laesc
