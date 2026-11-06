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
    // C atof (strtod): leading white space, sign, decimal or hexadecimal digits with an optional exponent, "inf" / "infinity" / "nan"
    public static double atof(String s) {
        int i = 0; final int n = s.length(); while (i < n && space(s.charAt(i))) i++;
        final int st = i; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
        final boolean neg = st < n && s.charAt(st) == '-';
        final String rest = s.substring(i).toLowerCase(java.util.Locale.ROOT);
        if (rest.startsWith("infinity") || rest.startsWith("inf")) return neg ? Double.NEGATIVE_INFINITY : Double.POSITIVE_INFINITY;
        if (rest.startsWith("nan")) return Double.NaN;
        if (rest.startsWith("0x")) { int k = i + 2; final int d0 = k; boolean any = false;
            while (k < n && Character.digit(s.charAt(k), 16) >= 0) { k++; any = true; }
            if (k < n && s.charAt(k) == '.') { k++; while (k < n && Character.digit(s.charAt(k), 16) >= 0) { k++; any = true; } }
            if (!any) return neg ? -0.0 : 0.0;   // "0x" alone: the "0" is the number
            int e = k; String exp = "p0";
            if (e < n && (s.charAt(e) == 'p' || s.charAt(e) == 'P')) { int q = e + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && Character.isDigit(s.charAt(q))) q++; if (q > q0) { exp = s.substring(e, q); } }
            final String body = s.substring(d0, k); final double v = Double.parseDouble("0x" + (body.startsWith(".") ? "0" + body : body) + exp); return neg ? -v : v; }
        int k = i; boolean any = false; while (k < n && Character.isDigit(s.charAt(k))) { k++; any = true; }
        if (k < n && s.charAt(k) == '.') { k++; while (k < n && Character.isDigit(s.charAt(k))) { k++; any = true; } }
        if (!any) return 0.0;
        if (k < n && (s.charAt(k) == 'e' || s.charAt(k) == 'E')) { int q = k + 1; if (q < n && (s.charAt(q) == '+' || s.charAt(q) == '-')) q++; final int q0 = q; while (q < n && Character.isDigit(s.charAt(q))) q++; if (q > q0) k = q; }
        return Double.parseDouble(s.substring(st, k)); }
    // C atoll / atoi (strtol base 10; out of range: the limit, as the MSVC CRT)
    public static long atoll(String s) {
        int i = 0; final int n = s.length(); while (i < n && space(s.charAt(i))) i++;
        boolean neg = false; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) { neg = s.charAt(i) == '-'; i++; }
        BigDecimal v = BigDecimal.ZERO; boolean any = false; while (i < n && Character.isDigit(s.charAt(i))) { v = v.multiply(BigDecimal.TEN).add(BigDecimal.valueOf(s.charAt(i) - '0')); i++; any = true; if (v.compareTo(BigDecimal.valueOf(Long.MAX_VALUE).add(BigDecimal.ONE)) > 0) break; }
        if (!any) return 0; if (neg) v = v.negate();
        if (v.compareTo(BigDecimal.valueOf(Long.MAX_VALUE)) > 0) return Long.MAX_VALUE; if (v.compareTo(BigDecimal.valueOf(Long.MIN_VALUE)) < 0) return Long.MIN_VALUE; return v.longValue(); }
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
