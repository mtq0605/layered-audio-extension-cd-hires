// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import java.math.BigDecimal;
import java.math.RoundingMode;

/**
 * The C library functions laes_tool uses on its arguments and in its messages, with the C results (MSVC CRT): atof / atoi / atoll parse the
 * longest valid prefix (no error: 0 when there is none), and printf's "%.Nf" rounds the exact binary value (ties to even), where Java's
 * Double.parseDouble throws and String.format may round the shortest decimal string instead.
 */
public final class CStd {
    private CStd() {}
    static boolean space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == 11 || c == '\f' || c == '\r'; }
    // the C digits only ('0'-'9', and a-f / A-F in hex); Character.isDigit / digit also take full-width and other scripts' digits
    static boolean dig(char c) { return c >= '0' && c <= '9'; }
    static boolean hex(char c) { return dig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    // C atof (strtod): leading white space, sign, decimal or hexadecimal digits with an optional exponent, "inf" / "infinity" / "nan"
    public static double atof(String s) {
        int i = 0; final int n = s.length(); while (i < n && space(s.charAt(i))) i++;
        final int st = i; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
        final boolean neg = st < n && s.charAt(st) == '-';
        final String rest = s.substring(i).toLowerCase(java.util.Locale.ROOT);
        if (rest.startsWith("infinity") || rest.startsWith("inf")) return neg ? Double.NEGATIVE_INFINITY : Double.POSITIVE_INFINITY;
        if (rest.startsWith("nan")) return Double.NaN;
        if (rest.startsWith("0x")) { int k = i + 2; final int d0 = k; boolean any = false;
            while (k < n && hex(s.charAt(k))) { k++; any = true; }
            if (k < n && s.charAt(k) == '.') { k++; while (k < n && hex(s.charAt(k))) { k++; any = true; } }
            if (!any) return neg ? -0.0 : 0.0;   // "0x" alone: the "0" is the number
            int e = k; String exp = "p0";
            if (e < n && (s.charAt(e) == 'p' || s.charAt(e) == 'P')) { int q = e + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && dig(s.charAt(q))) q++; if (q > q0) { exp = s.substring(e, q); } }
            final String body = s.substring(d0, k); final double v = Double.parseDouble("0x" + (body.startsWith(".") ? "0" + body : body) + exp); return neg ? -v : v; }
        int k = i; boolean any = false; while (k < n && dig(s.charAt(k))) { k++; any = true; }
        if (k < n && s.charAt(k) == '.') { k++; while (k < n && dig(s.charAt(k))) { k++; any = true; } }
        if (!any) return 0.0;
        if (k < n && (s.charAt(k) == 'e' || s.charAt(k) == 'E')) { int q = k + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && dig(s.charAt(q))) q++; if (q > q0) k = q; }
        return Double.parseDouble(s.substring(st, k)); }
    // C strtod(s, &e) with the whole string required (*e == 0): the value, or null when no number starts there or anything is left over
    // (laes_tool.cpp masterArg). The same parse as atof; the end is where atof's number ends.
    public static Double strtodFull(String s) {
        int i = 0; final int n = s.length(); while (i < n && space(s.charAt(i))) i++;
        if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
        final String rest = s.substring(i).toLowerCase(java.util.Locale.ROOT); int end;
        if (rest.startsWith("infinity")) end = i + 8; else if (rest.startsWith("inf")) end = i + 3; else if (rest.startsWith("nan")) end = i + 3;
        else if (rest.startsWith("0x")) { int k = i + 2; boolean any = false; while (k < n && hex(s.charAt(k))) { k++; any = true; }
            if (k < n && s.charAt(k) == '.') { k++; while (k < n && hex(s.charAt(k))) { k++; any = true; } }
            if (!any) end = i + 1;
            else { end = k; if (k < n && (s.charAt(k) == 'p' || s.charAt(k) == 'P')) { int q = k + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && dig(s.charAt(q))) q++; if (q > q0) end = q; } } }
        else { int k = i; boolean any = false; while (k < n && dig(s.charAt(k))) { k++; any = true; }
            if (k < n && s.charAt(k) == '.') { k++; while (k < n && dig(s.charAt(k))) { k++; any = true; } }
            if (!any) return null;
            if (k < n && (s.charAt(k) == 'e' || s.charAt(k) == 'E')) { int q = k + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && dig(s.charAt(q))) q++; if (q > q0) k = q; }
            end = k; }
        if (end != n) return null; return atof(s); }
    // C atoll / atoi (strtol base 10; out of range: the limit, as the MSVC CRT)
    public static long atoll(String s) {
        int i = 0; final int n = s.length(); while (i < n && space(s.charAt(i))) i++;
        boolean neg = false; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) { neg = s.charAt(i) == '-'; i++; }
        BigDecimal v = BigDecimal.ZERO; boolean any = false; while (i < n && dig(s.charAt(i))) { v = v.multiply(BigDecimal.TEN).add(BigDecimal.valueOf(s.charAt(i) - '0')); i++; any = true; if (v.compareTo(BigDecimal.valueOf(Long.MAX_VALUE).add(BigDecimal.ONE)) > 0) break; }
        if (!any) return 0; if (neg) v = v.negate();
        if (v.compareTo(BigDecimal.valueOf(Long.MAX_VALUE)) > 0) return Long.MAX_VALUE; if (v.compareTo(BigDecimal.valueOf(Long.MIN_VALUE)) < 0) return Long.MIN_VALUE; return v.longValue(); }
    // C strtol(s, &e, 10) with the whole string required (*e == 0): the value, or 0 when anything is left over (callers refuse k < 1); overflow: Long.MAX_VALUE / MIN_VALUE
    public static long strtolFull(String s) { int i = 0; final int n = s.length(); while (i < n && (s.charAt(i) == ' ' || (s.charAt(i) >= 9 && s.charAt(i) <= 13))) i++;
        boolean neg = false; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) { neg = s.charAt(i) == '-'; i++; }
        final int d0 = i; long v = 0; boolean ovf = false; while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') { if (v > (Long.MAX_VALUE - (s.charAt(i) - '0')) / 10) ovf = true; else v = v * 10 + (s.charAt(i) - '0'); i++; }
        if (i == d0) return 0; if (i != n) return 0; if (ovf) return neg ? Long.MIN_VALUE : Long.MAX_VALUE; return neg ? -v : v; }
    public static int atoi(String s) { final long v = atoll(s); return v > Integer.MAX_VALUE ? Integer.MAX_VALUE : v < Integer.MIN_VALUE ? Integer.MIN_VALUE : (int) v; }
    // printf "%.<dec>f" of d (exact binary value, ties to even; inf / nan as the MSVC CRT)
    public static String f(double d, int dec) {
        if (Double.isNaN(d)) return (Double.doubleToRawLongBits(d) < 0 ? "-" : "") + "nan" + (Double.doubleToRawLongBits(d) < 0 ? "(ind)" : "");
        if (Double.isInfinite(d)) return d < 0 ? "-inf" : "inf";
        String r = new BigDecimal(d).setScale(dec, RoundingMode.HALF_EVEN).toPlainString();
        if (d < 0 || (d == 0 && Double.doubleToRawLongBits(d) < 0)) { if (!r.startsWith("-")) r = "-" + r; }   // -0.0 and values rounding to zero keep the sign
        return r; }
    // printf "%<w>.<dec>f" (right-aligned in w characters)
    public static String f(double d, int w, int dec) { String r = f(d, dec); while (r.length() < w) r = " " + r; return r; }
}
