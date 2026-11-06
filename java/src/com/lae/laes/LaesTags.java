// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laes;

import com.lae.core.LaeException;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.UnsupportedEncodingException;
import java.util.ArrayList;
import java.util.List;

/**
 * Port of laes_tags.h (2026-11-05): tags and pictures of a
 * release file. Only the standard structures: FLAC VORBIS_COMMENT + PICTURE blocks; RIFF LIST/INFO + an "id3 " chunk holding an ID3v2.4 tag.
 *   --tag=KEY=VALUE        Vorbis comment names (TITLE, ARTIST, ALBUM, DATE, COPYRIGHT, COMMENT, ...); a key may repeat
 *   --cover=&lt;image&gt;        the front cover (picture type 3)
 *   --picture=&lt;type&gt;:&lt;image&gt;   any other picture (4 back cover, 5 leaflet, 6 medium, 8 artist, ...); JPEG or PNG, stored byte for byte
 * One set describes the core and every master. Placement: FLAC after STREAMINFO and before every "LAE2" block; WAV after "fmt "
 * and before "data". Nothing here touches the samples or the payload; without tags nothing is written.
 * Java: text is written as UTF-8 (the C++ writes the bytes of its argv); bytes are handled as unsigned (&amp; 0xFF) where the C++ uses uint8_t.
 */
public final class LaesTags {
    private LaesTags() {}

    /** port of laes_tags.h:Picture */
    public static final class Picture { public int type = 3; public String mime = ""; public long w = 0, h = 0, depth = 0, colors = 0; public byte[] data = new byte[0]; }
    /** a tag: key (upper case) and value */
    public static final class KV { public final String key, value; public KV(String k, String v) { key = k; value = v; } }
    /** port of laes_tags.h:Tags */
    public static final class Tags { public final List<KV> kv = new ArrayList<KV>(); public final List<Picture> pics = new ArrayList<Picture>();
        public boolean empty() { return kv.isEmpty() && pics.isEmpty(); } }
    /** the vendor string / ISFT / TSSE: fixed, so that the same input gives the same file */
    public static final String kEncoder = "laes (LAE Lossless)";

    static byte[] utf8(String s) { try { return s.getBytes("UTF-8"); } catch (UnsupportedEncodingException e) { throw new LaeException("UTF-8 not available", e); } }
    private static long be32(byte[] d, int o) { return (long) (d[o] & 0xFF) << 24 | (d[o + 1] & 0xFF) << 16 | (d[o + 2] & 0xFF) << 8 | (d[o + 3] & 0xFF); }
    private static boolean eq(byte[] d, int o, byte[] s) { if (o + s.length > d.length) return false; for (int i = 0; i < s.length; i++) if (d[o + i] != s[i]) return false; return true; }
    private static final byte[] PNG_SIG = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, IHDR = {'I', 'H', 'D', 'R'}, PLTE = {'P', 'L', 'T', 'E'};

    // port of laes_tags.h:readImage, on the image's bytes: width, height, colour depth (bits per pixel) and palette size from the image header;
    // "" or the reason (name: for the messages)
    public static String readImage(String name, byte[] d, Picture p) {
        p.data = d; final int n = d.length;
        if (n >= 33 && eq(d, 0, PNG_SIG) && eq(d, 12, IHDR)) {
            p.mime = "image/png"; p.w = be32(d, 16); p.h = be32(d, 20); final int bitDepth = d[24] & 0xFF, colorType = d[25] & 0xFF;
            final int channels = colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : 1; p.depth = (long) bitDepth * channels;
            if (colorType == 3) for (long o = 8; o + 12 <= n;) { final long len = be32(d, (int) o); if (eq(d, (int) o + 4, PLTE)) { p.colors = len / 3; break; } if (len > n - o - 12) break; o += 12 + len; }
            return ""; }
        if (n >= 4 && (d[0] & 0xFF) == 0xFF && (d[1] & 0xFF) == 0xD8) {   // JPEG: the first SOF marker (C0..CF without C4 DHT, C8 JPG, CC DAC) gives size and components
            p.mime = "image/jpeg"; long o = 2;
            while (o + 4 <= n) { final int oi = (int) o; if ((d[oi] & 0xFF) != 0xFF) return name + ": JPEG marker expected"; final int m = d[oi + 1] & 0xFF; if (m == 0xFF) { o++; continue; }
                if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { o += 2; continue; }
                final long len = (long) (d[oi + 2] & 0xFF) << 8 | (d[oi + 3] & 0xFF); if (len < 2 || o + 2 + len > n) break;
                if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { if (len < 8) break;
                    p.h = (long) (d[oi + 5] & 0xFF) << 8 | (d[oi + 6] & 0xFF); p.w = (long) (d[oi + 7] & 0xFF) << 8 | (d[oi + 8] & 0xFF); p.depth = (long) (d[oi + 4] & 0xFF) * (d[oi + 9] & 0xFF); return ""; }
                if (m == 0xDA || m == 0xD9) break; o += 2 + len; }
            return name + ": no JPEG frame header (SOF) found"; }
        return name + ": not a JPEG or PNG image"; }

    // port of laes_tags.h:readImage (the file read)
    public static String readImage(String path, Picture p) {
        byte[] d; InputStream in = null;
        try { in = new FileInputStream(path); ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] b = new byte[1 << 16]; int k; while ((k = in.read(b)) > 0) bo.write(b, 0, k); d = bo.toByteArray(); }
        catch (IOException e) { return "cannot read " + path; }
        finally { if (in != null) try { in.close(); } catch (IOException ig) { /* read only */ } }
        return readImage(path, d, p); }

    // port of laes_tags.h:upperKey (ASCII a-z only)
    public static String upperKey(String k) { StringBuilder u = new StringBuilder(k.length()); for (int i = 0; i < k.length(); i++) { char c = k.charAt(i); u.append(c >= 'a' && c <= 'z' ? (char) (c - 32) : c); } return u.toString(); }

    /** a key as the C++ accepts it: ASCII 0x20..0x7D without '=' (a byte >= 0x80 is a negative char there, so refused); "" or the reason */
    public static String checkKey(String key, String arg) { for (int i = 0; i < key.length(); i++) { final char c = key.charAt(i); if (c < 0x20 || c > 0x7D || c == '=') return "--tag: a key is ASCII 0x20..0x7D without '=': " + arg; } return ""; }

    /** the size checks of parseArgs for one picture ("" or the reason) */
    public static String checkPicture(String name, Picture p) { if (32L + utf8(p.mime).length + p.data.length >= (1L << 24)) return name + ": too large for a FLAC metadata block (16 MB)"; return ""; }
    /** the size check of parseArgs over all pictures ("" or the reason) */
    public static String checkAll(Tags t) { long all = 0; for (Picture p : t.pics) all += p.data.length + 64; if (all >= (1L << 28) - 4096) return "the pictures together are too large for an ID3v2 tag (256 MB)"; return ""; }

    // port of laes_tags.h:parseArgs: --tag / --cover / --picture from the command line; "" or the reason. Checked before any work is done.
    // args: the C++ argv without argv[0] (the C++ loop starts at argv[1], so every argument is looked at)
    public static String parseArgs(String[] args, Tags t) {
        for (String a : args) {
            if (a.startsWith("--tag=")) { final String kv = a.substring(6); final int eq = kv.indexOf('=');
                if (eq < 0 || eq == 0) return "--tag needs KEY=VALUE: " + a;
                final String key = upperKey(kv.substring(0, eq)); final String e = checkKey(key, a); if (!e.isEmpty()) return e;
                t.kv.add(new KV(key, kv.substring(eq + 1))); }
            else if (a.startsWith("--cover=") || a.startsWith("--picture=")) { Picture p = new Picture(); String path;
                if (a.charAt(2) == 'c') { p.type = 3; path = a.substring(8); }
                else { final String v = a.substring(10); final int c = v.indexOf(':'); if (c < 0 || c == 0 || c > 2) return "--picture needs <type>:<image>: " + a;
                    for (int q = 0; q < c; q++) if (v.charAt(q) < '0' || v.charAt(q) > '9') return "--picture: the type is a number 0..20: " + a;
                    p.type = Integer.parseInt(v.substring(0, c)); path = v.substring(c + 1); if (p.type > 20) return "--picture: the type is a number 0..20: " + a; }
                String e = readImage(path, p); if (!e.isEmpty()) return e;
                e = checkPicture(path, p); if (!e.isEmpty()) return e;
                t.pics.add(p); } }
        return checkAll(t); }

    // port of laes_tags.h:isTagArg: true when the argument is one of ours
    public static boolean isTagArg(String a) { return a.startsWith("--tag=") || a.startsWith("--cover=") || a.startsWith("--picture="); }

    static void le32(ByteArrayOutputStream o, long v) { for (int i = 0; i < 4; i++) o.write((int) (v >>> (8 * i)) & 0xFF); }
    static void be32(ByteArrayOutputStream o, long v) { for (int i = 3; i >= 0; i--) o.write((int) (v >>> (8 * i)) & 0xFF); }
    static void add(ByteArrayOutputStream o, byte[] s) { o.write(s, 0, s.length); }

    // port of laes_tags.h:flacBlocks: VORBIS_COMMENT (type 4) then one PICTURE (type 6) per picture; lastBlock: the final one carries the
    // "last" flag. Empty for no tags.
    public static byte[] flacBlocks(Tags t, boolean lastBlock) {
        final List<byte[]> bl = new ArrayList<byte[]>(); final List<Integer> types = new ArrayList<Integer>();
        if (!t.kv.isEmpty()) { ByteArrayOutputStream b = new ByteArrayOutputStream(); final byte[] ven = utf8(kEncoder); le32(b, ven.length); add(b, ven); le32(b, t.kv.size());
            for (KV kv : t.kv) { final byte[] s = utf8(kv.key + "=" + kv.value); le32(b, s.length); add(b, s); }
            bl.add(b.toByteArray()); types.add(4); }
        for (Picture p : t.pics) { ByteArrayOutputStream b = new ByteArrayOutputStream(); final byte[] mime = utf8(p.mime); be32(b, p.type); be32(b, mime.length); add(b, mime); be32(b, 0);   // no description
            be32(b, p.w); be32(b, p.h); be32(b, p.depth); be32(b, p.colors); be32(b, p.data.length); add(b, p.data);
            bl.add(b.toByteArray()); types.add(6); }
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        for (int i = 0; i < bl.size(); i++) { final long L = bl.get(i).length;
            o.write((lastBlock && i + 1 == bl.size() ? 0x80 : 0) | types.get(i)); o.write((int) (L >> 16) & 0xFF); o.write((int) (L >> 8) & 0xFF); o.write((int) L & 0xFF);
            add(o, bl.get(i)); }
        return o.toByteArray(); }

    // port of laes_tags.h:infoId: Vorbis name -> RIFF INFO id; "" = none
    static String infoId(String k) {
        final String[][] m = {{"TITLE", "INAM"}, {"ARTIST", "IART"}, {"ALBUM", "IPRD"}, {"DATE", "ICRD"}, {"COPYRIGHT", "ICOP"}, {"COMMENT", "ICMT"},
                              {"GENRE", "IGNR"}, {"TRACKNUMBER", "ITRK"}, {"ENGINEER", "IENG"}, {"DESCRIPTION", "ISBJ"}};
        for (String[] e : m) if (k.equals(e[0])) return e[1]; return ""; }
    // port of laes_tags.h:id3Frame: Vorbis name -> ID3v2.4 text frame; "" = none (ID3: TXXX)
    static String id3Frame(String k) {
        final String[][] m = {{"TITLE", "TIT2"}, {"ARTIST", "TPE1"}, {"ALBUM", "TALB"}, {"ALBUMARTIST", "TPE2"}, {"DATE", "TDRC"}, {"COPYRIGHT", "TCOP"},
                              {"GENRE", "TCON"}, {"TRACKNUMBER", "TRCK"}, {"DISCNUMBER", "TPOS"}, {"COMPOSER", "TCOM"}, {"LYRICIST", "TEXT"}, {"ISRC", "TSRC"}};
        for (String[] e : m) if (k.equals(e[0])) return e[1]; return ""; }
    // port of laes_tags.h:syncsafe
    static void syncsafe(ByteArrayOutputStream o, long v) { for (int i = 3; i >= 0; i--) o.write((int) ((v >>> (7 * i)) & 0x7F)); }

    static String join(List<String> v, String sep) { StringBuilder s = new StringBuilder(); for (int i = 0; i < v.size(); i++) { if (i != 0) s.append(sep); s.append(v.get(i)); } return s.toString(); }

    // port of laes_tags.h:wavChunks: a LIST/INFO chunk (UTF-8 text, NUL-terminated, even sizes) and an "id3 " chunk (ID3v2.4, UTF-8). Empty for no tags.
    public static byte[] wavChunks(Tags t) { return wavChunks(t, kEncoder); }
    // enc: the encoder named in ISFT / TSSE (lae_enc names itself; 2026-11-06, a85dd40)
    public static byte[] wavChunks(Tags t, String enc) {
        ByteArrayOutputStream o = new ByteArrayOutputStream(); if (t.empty()) return o.toByteArray();
        // a repeated key: one INFO entry / text frame (INFO "; ", ID3v2.4 NUL-separated values), in the order of the first appearance
        final List<String> mk = new ArrayList<String>(); final List<List<String>> mv = new ArrayList<List<String>>();
        for (KV kv : t.kv) { boolean found = false; for (int i = 0; i < mk.size(); i++) if (mk.get(i).equals(kv.key)) { mv.get(i).add(kv.value); found = true; }
            if (!found) { mk.add(kv.key); List<String> l = new ArrayList<String>(); l.add(kv.value); mv.add(l); } }
        {   ByteArrayOutputStream info = new ByteArrayOutputStream(); add(info, utf8("INFO"));
            for (int i = 0; i < mk.size(); i++) if (!infoId(mk.get(i)).isEmpty()) putInfo(info, infoId(mk.get(i)), join(mv.get(i), "; "));
            putInfo(info, "ISFT", enc);
            add(o, utf8("LIST")); le32(o, info.size()); add(o, info.toByteArray()); }
        {   ByteArrayOutputStream fr = new ByteArrayOutputStream();
            for (int i = 0; i < mk.size(); i++) { ByteArrayOutputStream b = new ByteArrayOutputStream(); b.write(3);   // 3 = UTF-8
                final String key = mk.get(i); final String v = join(mv.get(i), "\0");
                if (key.equals("COMMENT")) { add(b, utf8("XXX")); b.write(0); add(b, utf8(join(mv.get(i), "\n"))); frame(fr, "COMM", b.toByteArray()); }   // COMM holds one text
                else if (!id3Frame(key).isEmpty()) { add(b, utf8(v)); frame(fr, id3Frame(key), b.toByteArray()); }
                else { add(b, utf8(key)); b.write(0); add(b, utf8(v)); frame(fr, "TXXX", b.toByteArray()); } }
            { ByteArrayOutputStream b = new ByteArrayOutputStream(); b.write(3); add(b, utf8(enc)); frame(fr, "TSSE", b.toByteArray()); }
            for (Picture p : t.pics) { ByteArrayOutputStream b = new ByteArrayOutputStream(); b.write(3); add(b, utf8(p.mime)); b.write(0); b.write(p.type & 0xFF); b.write(0); add(b, p.data); frame(fr, "APIC", b.toByteArray()); }
            ByteArrayOutputStream tag = new ByteArrayOutputStream(); add(tag, utf8("ID3")); tag.write(4); tag.write(0); tag.write(0); syncsafe(tag, fr.size()); add(tag, fr.toByteArray());
            add(o, utf8("id3 ")); le32(o, tag.size()); add(o, tag.toByteArray()); if ((tag.size() & 1) != 0) o.write(0); }
        return o.toByteArray(); }

    // port of laes_tags.h:wavChunks lambda put (INFO entry: id, size including the NUL, text, NUL, pad to even)
    private static void putInfo(ByteArrayOutputStream info, String id, String v) { final byte[] s = utf8(v); add(info, utf8(id)); le32(info, s.length + 1); add(info, s); info.write(0); if (((s.length + 1) & 1) != 0) info.write(0); }
    // port of laes_tags.h:wavChunks lambda frame (ID3v2.4 frame: id, syncsafe size, two flag bytes, body)
    private static void frame(ByteArrayOutputStream fr, String id, byte[] body) { add(fr, utf8(id)); syncsafe(fr, body.length); fr.write(0); fr.write(0); add(fr, body); }
}
