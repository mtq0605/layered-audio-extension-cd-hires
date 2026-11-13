// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laes;

import java.util.ArrayList;
import java.util.List;

/**
 * port of laes_choices.h - the list of what an LAES file offers, in one place for the player (Play) and `laes dec --list` (2026-11-11,
 * SPEC section 10). Order and names:
 *   1. the core master ("core"); if its word length is above 16 bit, its derived 16-bit version right after it
 *   2. per master, in the menu order (the publisher's marks; else the file order), one group, k = its place in that order: the master
 *      itself ("master_k_rate_bits"), its rate levels from high to low ("master_k_level_rate_bits"), then its derived 16-bit versions
 *      (own rate first, then each level from high to low; "derived16_rate_from_source") - only for a master above 16 bit whose flag
 *      bit 0 (no derived versions) is clear
 * Every master of every word length is a master of its own; a derived version never replaces or hides one. The text shown ("what")
 * carries the master's number and, when it has one, its name; rate levels and derived versions carry the name of their master.
 * Names never select anything.
 */
public final class Choices {
    private Choices() {}
    /** port of laesc::Spec  (kind 0 core, 1 master, 2 rate level, 3 derived 16-bit; m = file index; no = menu number) */
    public static final class Spec { public int kind = 0; public int m = -1, no = 0, level = 0, from = -1; public int rate = 0, bits = 0; public long frames = 0; public String name = "", what = "";
        Spec copy() { Spec c = new Spec(); c.kind = kind; c.m = m; c.no = no; c.level = level; c.from = from; c.rate = rate; c.bits = bits; c.frames = frames; c.name = name; c.what = what; return c; } }
    // port of laesc::menuOrder  (the file indices in the order the publisher marked, else the file order)
    public static int[] menuOrder(List<LaesFile.MasterInfo> ms) { final int[] o = new int[ms.size()]; for (int i = 0; i < o.length; i++) o[i] = i;
        boolean any = false; for (LaesFile.MasterInfo I : ms) any |= I.menu != 0;
        if (any) for (int i = 0; i < ms.size(); i++) { final int p = ms.get(i).menu; if (p >= 1 && p <= ms.size()) o[p - 1] = i; }
        return o; }
    // port of laesc::displayNo  (the menu number of file index i)
    public static int displayNo(List<LaesFile.MasterInfo> ms, int i) { final int[] o = menuOrder(ms); for (int k = 0; k < o.length; k++) if (o[k] == i) return k + 1; return i + 1; }
    // port of laesc::masterLabel
    public static String masterLabel(int no, LaesFile.MasterInfo I) { String s = "master " + no; if (I.name.length() != 0) s += " \"" + I.name + "\""; return s; }
    private static void derived(List<Spec> ch, int from, String srcLabel) { final Spec S = ch.get(from); final Spec d = S.copy(); d.kind = 3; d.from = from; d.bits = 16;
        d.name = "derived16_" + S.rate + "_from_" + S.name; d.what = "derived from " + srcLabel; ch.add(d); }
    /** port of laesc::build  (offered[i] = false: master i is left out with its levels and derived versions; null: all; head: the core master's flags, null: none) */
    public static List<Spec> build(int baseRate, int coreBits, long coreFrames, List<LaesFile.MasterInfo> ms, boolean[] offered) { return build(baseRate, coreBits, coreFrames, ms, offered, null); }
    public static List<Spec> build(int baseRate, int coreBits, long coreFrames, List<LaesFile.MasterInfo> ms, boolean[] offered, LaesFile.Head head) {
        final List<Spec> ch = new ArrayList<Spec>();
        { Spec c = new Spec(); c.kind = 0; c.rate = baseRate; c.bits = coreBits; c.frames = coreFrames; c.name = "core"; c.what = "the core master (lossless)"; ch.add(c); }
        if (coreBits > 16 && !(head != null && (head.coreFlags & LaesFile.NO_DERIVE) != 0)) derived(ch, 0, "the core master");
        final int[] order = menuOrder(ms);
        for (int pos = 0; pos < order.length; pos++) { final int i = order[pos]; if (offered != null && !offered[i]) continue; final LaesFile.MasterInfo I = ms.get(i); final int no = pos + 1;
            final String mk = "master_" + no, lb = masterLabel(no, I);
            final int first = ch.size();
            { Spec c = new Spec(); c.kind = 1; c.m = i; c.no = no; c.level = I.stages; c.rate = I.rate; c.bits = I.bits; c.frames = I.frames; c.name = mk + "_" + I.rate + "_" + I.bits; c.what = lb + " (lossless)"; ch.add(c); }
            for (int k = I.stages - 1; k >= 0; k--) { Spec c = new Spec(); c.kind = 2; c.m = i; c.no = no; c.level = k; c.rate = I.rate >> (I.stages - k); c.bits = I.bits; c.frames = LaesFile.levLen(I.frames, I.stages - k);
                c.name = mk + "_level_" + c.rate + "_" + c.bits; c.what = "derived from " + lb; ch.add(c); }
            final int last = ch.size();
            if (I.bits > 16 && (I.flags & LaesFile.NO_DERIVE) == 0) for (int j = first; j < last; j++) derived(ch, j, lb); }
        return ch; }
    // port of laesc::defaultIndex  (the header's default master itself, else the core master, 0)
    public static int defaultIndex(List<Spec> ch, LaesFile.Head h) { if (h != null && h.def > 0) for (int j = 0; j < ch.size(); j++) if (ch.get(j).kind == 1 && ch.get(j).m == h.def - 1) return j; return 0; }
}
