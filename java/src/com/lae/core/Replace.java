// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

import java.io.Console;
import java.io.File;

/**
 * Decision 21 (2026-11-15), the same rule and texts as every C++ tool (lae_enc.cpp mayReplace, src/laes_pick.h mayReplace): an existing
 * output file is not replaced silently. With a console ({@code System.console() != null}) the person is asked twice and only two answers
 * "yes" replace it; without a console it is refused unless --overwrite was given. The prompts and the refusal are operating explanations:
 * English, then Chinese. A resume of an unfinished run is not a replacement (the callers do not ask then).
 */
public final class Replace {
    private Replace() {}

    /** where the texts go (the tool's own output) */
    public interface Out { void print(String s); }

    /** the option that allows a replacement without asking (lae_enc also takes it without the leading "--") */
    public static final String OPTION = "--overwrite";

    // one answer: spaces, tabs, CR, LF and a byte-order mark around it are not counted; only "yes" counts (end of input: no)
    static boolean yes(Console c) { final String a = c.readLine(); if (a == null) return false; int b = 0, e = a.length();
        while (b < e && (a.charAt(b) == ' ' || a.charAt(b) == '\t' || a.charAt(b) == '﻿')) b++;
        while (e > b && (a.charAt(e - 1) == ' ' || a.charAt(e - 1) == '\t' || a.charAt(e - 1) == '\r' || a.charAt(e - 1) == '\n')) e--;
        if (e - b != 3) return false;   // only the word yes, in any letter case (ASCII letters only, as the C++ tools)
        return (a.charAt(b) | 0x20) == 'y' && (a.charAt(b + 1) | 0x20) == 'e' && (a.charAt(b + 2) | 0x20) == 's'; }

    /** true: the file may be written (it does not exist, --overwrite was given, or replacing it was confirmed twice at a console) */
    public static boolean mayReplace(String path, boolean overwrite, Out o) {
        if (!new File(path).exists() || overwrite) return true;
        final Console c = System.console();
        if (c != null) {
            o.print("\"" + path + "\" already exists. Replace it? Type yes to continue: \n\"" + path + "\" 已经存在。要替换吗？输入 yes 继续：");
            if (yes(c)) { o.print("Really replace \"" + path + "\"? It cannot be undone. Type yes again: \n确定要替换 \"" + path + "\" 吗？替换后无法恢复。再输入一次 yes：");
                if (yes(c)) return true; } }
        o.print("\"" + path + "\" already exists; it was not replaced (give --overwrite to replace it).\n\"" + path + "\" 已经存在，没有替换（加 --overwrite 才替换）。\n");
        return false; }
}
