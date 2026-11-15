// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

/**
 * The C library's printf / atoi / atof as the encoder uses them (MSVC UCRT). Java's String.format rounds the decimal expansion half
 * up and depends on the locale; the UCRT rounds the exact binary value half to even (measured: printf("%.0f", 16537.5) = "16538",
 * 0.5 = "0", 2.5 = "2", 3.5 = "4", "%.1f" of 0.35 = "0.3"). Some of these strings decide the output file (the cut-off written as
 * "%.0f" and read back, the K0FRAC "%.6f"), so they are formatted from the exact binary value here.
 * Supported conversions: %d %i %u (with l, ll, z), %s, %c, %f %e %g with flags + - space 0 and width / precision, %%.
 */
final class Fmt {
    private Fmt() {}

    // port of the C library's printf conversions (UCRT semantics, see the class comment)
    static String c(String f, Object... a) {
        StringBuilder o = new StringBuilder(); int ai = 0;
        for (int i = 0; i < f.length(); i++) { char ch = f.charAt(i);
            if (ch != '%') { o.append(ch); continue; }
            i++; if (f.charAt(i) == '%') { o.append('%'); continue; }
            boolean plus = false, minus = false, space = false, zero = false;
            for (;; i++) { char g = f.charAt(i); if (g == '+') plus = true; else if (g == '-') minus = true; else if (g == ' ') space = true; else if (g == '0') zero = true; else break; }
            int width = 0; while (Character.isDigit(f.charAt(i))) width = width * 10 + (f.charAt(i++) - '0');
            int prec = -1; if (f.charAt(i) == '.') { i++; prec = 0; while (Character.isDigit(f.charAt(i))) prec = prec * 10 + (f.charAt(i++) - '0'); }
            while ("lhzjtL".indexOf(f.charAt(i)) >= 0) i++;
            char cv = f.charAt(i); String s;
            if (cv == 'd' || cv == 'i' || cv == 'u') { long v = ((Number) a[ai++]).longValue(); s = Long.toString(Math.abs(v)); if (v == Long.MIN_VALUE) s = s.substring(1);
                if (v < 0) s = "-" + s; else if (plus) s = "+" + s; else if (space) s = " " + s; }
            else if (cv == 's') { Object x = a[ai++]; s = String.valueOf(x); if (prec >= 0 && s.length() > prec) s = s.substring(0, prec); }
            else if (cv == 'c') { Object x = a[ai++]; s = x instanceof Character ? x.toString() : String.valueOf((char) ((Number) x).intValue()); }
            else if (cv == 'f' || cv == 'e' || cv == 'g') { double v = ((Number) a[ai++]).doubleValue(); s = fp(v, cv, prec < 0 ? 6 : prec);
                if (!s.startsWith("-")) { if (plus) s = "+" + s; else if (space) s = " " + s; } }
            else throw new IllegalArgumentException("format: %" + cv);
            if (s.length() < width) { StringBuilder p = new StringBuilder();
                if (minus) { p.append(s); while (p.length() < width) p.append(' '); }
                else if (zero && cv != 's' && cv != 'c') { int sg = (s.startsWith("-") || s.startsWith("+") || s.startsWith(" ")) ? 1 : 0; p.append(s, 0, sg); for (int k = s.length(); k < width; k++) p.append('0'); p.append(s.substring(sg)); }
                else { for (int k = s.length(); k < width; k++) p.append(' '); p.append(s); }
                s = p.toString(); }
            o.append(s); }
        return o.toString(); }

        // %f / %e / %g of v with the given precision, rounded half to even from the exact binary value (as the UCRT)
    static String fp(double v, char cv, int prec) {
        if (v != v) return "nan"; if (Double.isInfinite(v)) return v > 0 ? "inf" : "-inf";
        boolean neg = v < 0 || (v == 0 && 1 / v < 0); String s;
        Exact x = Exact.of(Math.abs(v));
        if (cv == 'f') s = fixed(x, prec);
        else if (cv == 'e') s = sci(x, prec);
        else { int P = prec == 0 ? 1 : prec;
            int X; if (x.zero()) X = 0; else { Exact r = x.round(P); X = r.ip - 1; }
            if (P > X && X >= -4) s = fixed(x, P - 1 - X); else s = sci(x, P - 1);
            s = stripZeros(s); }
        return neg ? "-" + s : s; }
    // fixed-point text with prec fraction digits
    private static String fixed(Exact x, int prec) {
        StringBuilder o = new StringBuilder(); Exact r = x.zero() ? x : x.round(x.ip + prec);
        String d = r.d; int ip = r.ip;
        if (r.zero() || ip <= 0) { o.append('0'); if (prec > 0) { o.append('.'); for (int i = 0; i < prec; i++) { int k = i + ip; o.append(!r.zero() && k >= 0 && k < d.length() ? d.charAt(k) : '0'); } } return o.toString(); }
        for (int i = 0; i < ip; i++) o.append(i < d.length() ? d.charAt(i) : '0');
        if (prec > 0) { o.append('.'); for (int i = 0; i < prec; i++) { int k = ip + i; o.append(k < d.length() ? d.charAt(k) : '0'); } }
        return o.toString(); }
    // scientific text d.ddd e+XX with prec fraction digits
    private static String sci(Exact x, int prec) {
        int X = 0; StringBuilder m = new StringBuilder();
        if (x.zero()) { m.append('0'); if (prec > 0) { m.append('.'); for (int i = 0; i < prec; i++) m.append('0'); } }
        else { Exact r = x.round(prec + 1); X = r.ip - 1; m.append(r.d.charAt(0)); if (prec > 0) { m.append('.'); for (int i = 1; i <= prec; i++) m.append(i < r.d.length() ? r.d.charAt(i) : '0'); } }
        String e = Integer.toString(Math.abs(X)); if (e.length() < 2) e = "0" + e;
        return m + "e" + (X < 0 ? "-" : "+") + e; }

    /**
     * the exact decimal expansion of a non-negative double: value = 0.d x 10^ip (d without leading zeros; "" for zero). A double is
     * mant x 2^e: for e &lt; 0 its digits are those of mant x 5^-e with the point -e places from the right. Base-10^9 limbs.
     */
    static final class Exact { final String d; final int ip;
        Exact(String dd, int i) { d = dd; ip = i; }
        boolean zero() { return d.isEmpty(); }
        static Exact of(double v) {
            if (v == 0) return new Exact("", 0);
            long bits = Double.doubleToRawLongBits(v); int be = (int) ((bits >>> 52) & 0x7FF); long mant = bits & ((1L << 52) - 1);
            int e2; if (be == 0) e2 = -1074; else { mant |= 1L << 52; e2 = be - 1075; }
            int[] L = new int[(Math.abs(e2) * 7 / 20) + 8]; int n = 0; long m = mant; while (m > 0) { L[n++] = (int) (m % 1000000000L); m /= 1000000000L; }
            int sc = 0;
            if (e2 >= 0) { for (int k = e2; k > 0; k -= 29) n = mul(L, n, 1 << Math.min(29, k)); }
            else { sc = -e2; for (int k = sc; k > 0; k -= 12) { int p = 1; for (int i = 0; i < Math.min(12, k); i++) p *= 5; n = mul(L, n, p); } }
            StringBuilder s = new StringBuilder(Integer.toString(L[n - 1])); for (int i = n - 2; i >= 0; i--) { String t = Integer.toString(L[i]); for (int k = t.length(); k < 9; k++) s.append('0'); s.append(t); }
            int len = s.length(); int z = len; while (z > 1 && s.charAt(z - 1) == '0') z--;
            return new Exact(s.substring(0, z), len - sc); }
        private static int mul(int[] L, int n, int f) { long c = 0; for (int i = 0; i < n; i++) { long t = (long) L[i] * f + c; L[i] = (int) (t % 1000000000L); c = t / 1000000000L; } while (c > 0) { L[n++] = (int) (c % 1000000000L); c /= 1000000000L; } return n; }
        // rounded half to even to the first `cut` digits of d (cut may be <= 0: then the result is 0 or one unit of the next higher place)
        Exact round(int cut) {
            if (cut >= d.length()) return this;
            if (cut < 0) return new Exact("", 0);
            char r0 = d.charAt(cut); boolean rest = false; for (int i = cut + 1; i < d.length(); i++) if (d.charAt(i) != '0') { rest = true; break; }
            int last = cut > 0 ? d.charAt(cut - 1) - '0' : 0;
            boolean up = r0 > '5' || (r0 == '5' && (rest || (last & 1) == 1));
            char[] k = d.substring(0, cut).toCharArray(); int ipn = ip;
            if (up) { int i = k.length - 1; while (i >= 0 && k[i] == '9') { k[i] = '0'; i--; }
                if (i >= 0) k[i]++; else { char[] k2 = new char[k.length + 1]; k2[0] = '1'; System.arraycopy(k, 0, k2, 1, k.length); k = k2; ipn++; } }
            String s = new String(k); int z = s.length(); while (z > 0 && s.charAt(z - 1) == '0') z--; s = s.substring(0, z);
            return s.isEmpty() ? new Exact("", 0) : new Exact(s, ipn); } }
    private static String stripZeros(String s) {
        int e = s.indexOf('e'); String m = e >= 0 ? s.substring(0, e) : s, t = e >= 0 ? s.substring(e) : "";
        if (m.indexOf('.') >= 0) { int k = m.length(); while (m.charAt(k - 1) == '0') k--; if (m.charAt(k - 1) == '.') k--; m = m.substring(0, k); }
        return m + t; }

    // port of the C library's atoi (leading white space, optional sign, digits; anything else ends the number; 0 if none)
    static int atoi(String s) { return (int) atoll(s); }
    // port of the C library's atoll
    static long atoll(String s) {
        int i = 0, n = s.length(); while (i < n && Character.isWhitespace(s.charAt(i))) i++;
        boolean neg = false; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) neg = s.charAt(i++) == '-';
        long v = 0; while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') v = v * 10 + (s.charAt(i++) - '0');
        return neg ? -v : v; }
    // C digits only (Character.isDigit took full-width digits such as U+FF11, then Double.parseDouble threw)
    private static boolean dig(char c) { return c >= '0' && c <= '9'; }
    // port of the C library's atof (the longest prefix that is a decimal floating-point number; 0 if none; correctly rounded as the UCRT strtod)
    static double atof(String s) {
        int i = 0, n = s.length(); while (i < n && Character.isWhitespace(s.charAt(i))) i++;
        int st = i; if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
        int d0 = i; while (i < n && dig(s.charAt(i))) i++; int nd = i - d0;
        if (i < n && s.charAt(i) == '.') { i++; int f0 = i; while (i < n && dig(s.charAt(i))) i++; nd += i - f0; }
        if (nd == 0) return 0.0;
        int end = i;
        if (i < n && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) { int j = i + 1; if (j < n && (s.charAt(j) == '+' || s.charAt(j) == '-')) j++; int e0 = j; while (j < n && dig(s.charAt(j))) j++; if (j > e0) end = j; }
        String t = s.substring(st, end); if (t.endsWith(".")) t = t + "0"; if (t.startsWith(".") || t.startsWith("+.") || t.startsWith("-.")) t = t.replace(".", "0.");
        return Double.parseDouble(t); }
}
