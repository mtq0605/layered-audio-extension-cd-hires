// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
package com.lae.laey;

import com.lae.laes.LaesTags;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;

/**
 * Tags and pictures of LAEY files (port of lae_enc.cpp a85dd40, 2026-11-06), the same as laes: --tag=KEY=VALUE, --cover=&lt;image&gt;,
 * --picture=&lt;type&gt;:&lt;image&gt;, also without the leading "--". One set describes the whole file; a LIST/INFO and an "id3 " chunk go after
 * "fmt " and before "data". The CD audio and the "LAE " chunk do not change, and without tags the file is the same, byte for byte.
 * lae_enc tag &lt;in.wav&gt; &lt;out.wav&gt; [tags] writes a new file with new tags (none given = the tags are removed); the input is not changed.
 */
public final class LaeRetag {
    private LaeRetag() {}

    /** the encoder named in ISFT / TSSE */
    public static final String kLaeyEncoder = "lae_enc (LAE Lossy)";

    // port of lae_enc.cpp:isTagOpt
    public static boolean isTagOpt(String a) { return LaesTags.isTagArg(a) || a.startsWith("tag=") || a.startsWith("cover=") || a.startsWith("picture="); }

    // port of lae_enc.cpp:tagArgs: the tag options of argv[from..], each with its leading "--"; "" or the reason
    static String tagArgs(String[] argv, int from, LaesTags.Tags t) {
        List<String> v = new ArrayList<String>();
        for (int i = from; i < argv.length; i++) if (isTagOpt(argv[i])) v.add(argv[i].charAt(0) == '-' ? argv[i] : "--" + argv[i]);
        return LaesTags.parseArgs(v.toArray(new String[0]), t); }

    // The form of the output file (2026-11-10). WAV is the default form of LAEY; FLAC is a supported one: .flac = FLAC form (LaeyFlac); a name ending in
    // .lae = WAV form, or FLAC form with --form=flac; any other name = WAV form, as before. --form=flac / --form=wav (also without the
    // "--") must agree with a .flac / .wav name. Readers go by content, never by the name.
    // port of lae_enc.cpp:isFormOpt
    public static boolean isFormOpt(String a) { return a.startsWith("--form=") || a.startsWith("form="); }
    // port of lae_enc.cpp:nameEnds
    static boolean nameEnds(String s, String e) { final int n = e.length(); if (s.length() <= n) return false;
        for (int i = 0; i < n; i++) { char c = s.charAt(s.length() - n + i); if (c >= 'A' && c <= 'Z') c = (char) (c + 32); if (c != e.charAt(i)) return false; } return true; }
    // port of lae_enc.cpp:outputForm: 0 = WAV form, 1 = FLAC form, -1 = refused (reason printed)
    static int outputForm(EncEnv env, String out, String[] argv, int from) {
        String formArg = ""; for (int i = from; i < argv.length; i++) if (isFormOpt(argv[i])) formArg = argv[i].substring(argv[i].indexOf('=') + 1);
        if (!formArg.isEmpty() && !formArg.equals("flac") && !formArg.equals("wav")) { env.printf("--form must be flac or wav\n"); return -1; }
        final boolean laeName = nameEnds(out, ".lae"); final boolean asFlac = laeName ? formArg.equals("flac") : nameEnds(out, ".flac");
        if (!formArg.isEmpty() && !laeName && formArg.equals("flac") != asFlac) { env.printf("--form=%s does not match the name %s (use .lae to choose the form freely)\n", formArg, out); return -1; }
        return asFlac ? 1 : 0; }

    // port of lae_enc.cpp:m_tag::Ck
    static final class Ck { String id; long body; long len; }

    // port of lae_enc.cpp:m_tag::walk
    static boolean walk(RandomAccessFile f, List<Ck> cks, String[] why) throws IOException {
        byte[] h = new byte[12]; f.seek(0);
        if (f.read(h) != 12 || !id(h, 0).equals("RIFF") || !id(h, 8).equals("WAVE")) { why[0] = "not a RIFF WAVE file"; return false; }
        final long end = f.length(); long p = 12;
        while (p + 8 <= end) { byte[] c = new byte[8]; f.seek(p); if (f.read(c) != 8) { why[0] = "cannot read a chunk header"; return false; }
            Ck k = new Ck(); k.id = id(c, 0); k.len = (c[4] & 0xFFL) | (c[5] & 0xFFL) << 8 | (c[6] & 0xFFL) << 16 | (c[7] & 0xFFL) << 24; k.body = p + 8;
            if (k.body + k.len > end) { why[0] = "chunk '" + k.id + "' runs past the end of the file"; return false; }
            cks.add(k); p = k.body + k.len + (k.len & 1); }
        return true; }
    private static String id(byte[] b, int o) { return new String(new char[] {(char) (b[o] & 0xFF), (char) (b[o + 1] & 0xFF), (char) (b[o + 2] & 0xFF), (char) (b[o + 3] & 0xFF)}); }
    static Ck find(List<Ck> cks, String id) { for (Ck k : cks) if (k.id.equals(id)) return k; return null; }

    // port of lae_enc.cpp:m_tag::isOldTag: LIST of type INFO, or an id3 chunk
    static boolean isOldTag(RandomAccessFile f, Ck k) throws IOException {
        if (k.id.equals("id3 ") || k.id.equals("ID3 ")) return true;
        if (!k.id.equals("LIST") || k.len < 4) return false; byte[] t = new byte[4]; f.seek(k.body); return f.read(t) == 4 && id(t, 0).equals("INFO"); }

    // port of lae_enc.cpp:m_tag::copyBody: header, body, pad byte
    static void copyBody(RandomAccessFile in, Ck k, BufferedOutputStream out) throws IOException {
        byte[] h = new byte[8]; for (int i = 0; i < 4; i++) h[i] = (byte) k.id.charAt(i); for (int i = 0; i < 4; i++) h[4 + i] = (byte) (k.len >>> (8 * i)); out.write(h);
        in.seek(k.body); byte[] b = new byte[1 << 20]; long left = k.len;
        while (left > 0) { final int n = (int) Math.min(left, b.length); in.readFully(b, 0, n); out.write(b, 0, n); left -= n; }
        if ((k.len & 1) != 0) out.write(0); }

    // port of lae_enc.cpp:m_tag::sameBody
    static boolean sameBody(RandomAccessFile a, Ck ka, RandomAccessFile b, Ck kb) throws IOException {
        if (ka.len != kb.len) return false; a.seek(ka.body); b.seek(kb.body);
        byte[] x = new byte[1 << 20], y = new byte[1 << 20]; long left = ka.len;
        while (left > 0) { final int n = (int) Math.min(left, x.length); a.readFully(x, 0, n); b.readFully(y, 0, n);
            for (int i = 0; i < n; i++) if (x[i] != y[i]) return false; left -= n; }
        return true; }

    // FLAC form (2026-11-10): "fLaC" | STREAMINFO | the new tags | every other block in its order (former VORBIS_COMMENT, PICTURE and
    // PADDING blocks are left out) | the audio frames, copied. Then the new file's "LAE1" blocks and audio frames are compared byte for
    // byte with the input's.
    // port of lae_enc.cpp:m_tag::Bk
    static final class Bk { int type; long body; long len; boolean lae1; }
    // port of lae_enc.cpp:m_tag::walkFlac
    static boolean walkFlac(RandomAccessFile f, List<Bk> bks, long[] audioOff, long[] end, String[] why) throws IOException {
        byte[] h = new byte[4]; end[0] = f.length(); f.seek(0);
        if (f.read(h) != 4 || !id(h, 0).equals("fLaC")) { why[0] = "not a FLAC file"; return false; }
        long p = 4; boolean last = false; byte[] b = new byte[4];
        while (!last) { if (p + 4 > end[0]) { why[0] = "FLAC metadata truncated"; return false; } f.seek(p); if (f.read(b) != 4) { why[0] = "FLAC metadata truncated"; return false; }
            Bk k = new Bk(); last = (b[0] & 0x80) != 0; k.type = b[0] & 0x7F; k.len = (b[1] & 0xFFL) << 16 | (b[2] & 0xFFL) << 8 | (b[3] & 0xFFL); k.body = p + 4; k.lae1 = false;
            if (k.body + k.len > end[0]) { why[0] = "FLAC metadata truncated"; return false; }
            if (k.type == 2 && k.len >= 4) { byte[] idb = new byte[4]; if (f.read(idb) != 4) { why[0] = "FLAC metadata truncated"; return false; } k.lae1 = id(idb, 0).equals("LAE1"); }
            bks.add(k); p = k.body + k.len; }
        audioOff[0] = p; return true; }
    // port of lae_enc.cpp:m_tag::copyRange
    static void copyRange(RandomAccessFile in, long off, long len, BufferedOutputStream out) throws IOException {
        in.seek(off); byte[] b = new byte[1 << 20];
        while (len > 0) { final int n = (int) Math.min(len, b.length); in.readFully(b, 0, n); out.write(b, 0, n); len -= n; } }
    // port of lae_enc.cpp:m_tag::sameRange
    static boolean sameRange(RandomAccessFile a, long offA, RandomAccessFile b, long offB, long len) throws IOException {
        a.seek(offA); b.seek(offB); byte[] x = new byte[1 << 20], y = new byte[1 << 20];
        while (len > 0) { final int n = (int) Math.min(len, x.length); a.readFully(x, 0, n); b.readFully(y, 0, n);
            for (int i = 0; i < n; i++) if (x[i] != y[i]) return false; len -= n; }
        return true; }
    // port of lae_enc.cpp:m_tag::retagFlac (fi is closed by the caller)
    static int retagFlac(EncEnv env, RandomAccessFile fi, String in, String out, String part, LaesTags.Tags tags) {
        boolean ok = false;
        try {
            List<Bk> bks = new ArrayList<Bk>(); long[] audioOff = {0}, end = {0}; String[] why = {""};
            if (!walkFlac(fi, bks, audioOff, end, why)) { env.printf("%s: %s; nothing written\n", in, why[0]); return 1; }
            boolean haveLae = false; for (Bk k : bks) haveLae = haveLae || k.lae1;
            if (bks.isEmpty() || bks.get(0).type != 0 || bks.get(0).len != 34 || !haveLae) { env.printf("%s is not an LAE file in the FLAC form (it needs STREAMINFO and \"LAE1\" APPLICATION blocks); nothing written\n", in); return 1; }
            List<Bk> keep = new ArrayList<Bk>(); for (int i = 1; i < bks.size(); i++) { final int t = bks.get(i).type; if (t != 0 && t != 1 && t != 4 && t != 6) keep.add(bks.get(i)); }
            final byte[] tb = LaesTags.flacBlocks(tags, false, kLaeyEncoder);
            try { BufferedOutputStream fo = new BufferedOutputStream(new FileOutputStream(part), 1 << 16);
                  try { final byte[] h = {'f', 'L', 'a', 'C', 0, 0, 0, 34}; fo.write(h); copyRange(fi, bks.get(0).body, 34, fo); if (tb.length != 0) fo.write(tb);
                        for (int i = 0; i < keep.size(); i++) { final Bk k = keep.get(i); final byte[] bh = {(byte) ((i + 1 == keep.size() ? 0x80 : 0) | k.type), (byte) (k.len >> 16), (byte) (k.len >> 8), (byte) k.len};
                            fo.write(bh); copyRange(fi, k.body, k.len, fo); }
                        copyRange(fi, audioOff[0], end[0] - audioOff[0], fo);
                        ok = true; }
                  finally { fo.close(); } }
            catch (IOException e) { ok = false; }
            if (ok) { RandomAccessFile fc = null;
                try { fc = new RandomAccessFile(part, "r"); List<Bk> b2 = new ArrayList<Bk>(); long[] a2 = {0}, e2 = {0}; String[] w2 = {""};
                      ok = walkFlac(fc, b2, a2, e2, w2) && e2[0] - a2[0] == end[0] - audioOff[0] && sameRange(fi, audioOff[0], fc, a2[0], end[0] - audioOff[0]);
                      List<Bk> l1 = new ArrayList<Bk>(), l2 = new ArrayList<Bk>(); for (Bk k : bks) if (k.lae1) l1.add(k); for (Bk k : b2) if (k.lae1) l2.add(k);
                      ok = ok && l1.size() == l2.size(); for (int i = 0; ok && i < l1.size(); i++) ok = l1.get(i).len == l2.get(i).len && sameRange(fi, l1.get(i).body, fc, l2.get(i).body, l1.get(i).len); }
                catch (IOException e) { ok = false; }
                finally { if (fc != null) try { fc.close(); } catch (IOException e) { /* read only */ } }
                if (!ok) env.printf("check FAILED: the new file's CD audio frames or LAE1 blocks differ from the input's\n"); }
            else env.printf("cannot write %s\n", part);
        } catch (IOException e) { ok = false; env.printf("cannot read %s\n", in); }
        if (ok) { ok = LaeEncoder.rename(part, out); if (!ok) env.printf("cannot rename %s to %s\n", part, out); }   // also replaces an output agreed to (decision 21)
        if (!ok) { new File(part).delete(); env.printf("nothing written\n"); return 2; }
        env.printf("written: %s (FLAC form, %zu tag(s), %zu picture(s)); CD audio frames and LAE1 blocks byte-identical to %s\n", out, (long) tags.kv.size(), (long) tags.pics.size(), in); return 0; }

    // std::filesystem::equivalent: both exist and are the same file (Java: the same canonical path)
    static boolean sameFile(String a, String b) { final File x = new File(a), y = new File(b); if (!x.exists() || !y.exists()) return false;
        try { return x.getCanonicalFile().equals(y.getCanonicalFile()); } catch (IOException e) { return false; } }
    // port of lae_enc.cpp:m_tag::retag; presetTags: tags the app gives as objects (added after those in argv; null = none)
    static int retag(EncEnv env, String[] argv, LaesTags.Tags presetTags) {
        if (argv.length < 4) { env.printf("usage: lae_enc tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...] [--overwrite]   (WAV or FLAC form, the output keeps the input's form; no tags given: the tags are removed)\n"); return 1; }
        final String in = argv[2], out = argv[3], part = out + ".part";
        boolean overwrite = false;
        for (int i = 4; i < argv.length; i++) { if (LaeEncoder.isOverwriteOpt(argv[i])) { overwrite = true; continue; }
            if (!isTagOpt(argv[i])) { env.printf("unknown option '%s': nothing written (options: --tag=KEY=VALUE --cover=<image> --picture=<type>:<image> --overwrite)\n", argv[i]); return 1; } }
        if (sameFile(in, out)) { env.printf("%s is the input: choose another output name (the input is never changed); nothing written\n", out); return 1; }
        // decision 21 (2026-11-15, as lae_enc.cpp): an existing output only after two "yes" at a console, or with --overwrite
        if (!com.lae.core.Replace.mayReplace(out, overwrite, new com.lae.core.Replace.Out() { @Override public void print(String s) { env.print(s); } })) return 1;
        if (new File(part).exists()) { env.printf("%s already exists: remove it or choose another output name; nothing written\n", part); return 1; }
        LaesTags.Tags tags = new LaesTags.Tags(); { final String e = tagArgs(argv, 4, tags); if (!e.isEmpty()) { env.printf("%s: nothing written\n", e); return 1; } }
        if (presetTags != null) { tags.kv.addAll(presetTags.kv); tags.pics.addAll(presetTags.pics); final String e = LaesTags.checkAll(tags); if (!e.isEmpty()) { env.printf("%s: nothing written\n", e); return 1; } }
        final byte[] tc = LaesTags.wavChunks(tags, kLaeyEncoder);
        RandomAccessFile fi = null; boolean ok = false;
        try {
            try { fi = new RandomAccessFile(in, "r"); } catch (IOException e) { env.printf("cannot read %s\n", in); return 1; }
            { byte[] h4 = new byte[4]; final boolean inFlac = fi.read(h4) == 4 && id(h4, 0).equals("fLaC");   // the output keeps the input's form; its name must not say the other one
              if (inFlac ? nameEnds(out, ".wav") : nameEnds(out, ".flac")) { env.printf("%s is in the %s form: give the output a %s name (lae_enc tag does not change the form); nothing written\n", in, inFlac ? "FLAC" : "WAV", inFlac ? ".flac or .lae" : ".wav or .lae"); return 1; }
              if (inFlac) return retagFlac(env, fi, in, out, part, tags); }
            List<Ck> cks = new ArrayList<Ck>(); String[] why = {""};
            if (!walk(fi, cks, why)) { env.printf("%s: %s; nothing written\n", in, why[0]); return 1; }
            final Ck fmt = find(cks, "fmt "), dat = find(cks, "data"), lae = find(cks, "LAE ");
            if (fmt == null || dat == null || lae == null) { env.printf("%s is not an LAE file (it needs \"fmt \", \"data\" and \"LAE \" chunks); nothing written\n", in); return 1; }
            List<Ck> keep = new ArrayList<Ck>(); keep.add(fmt); for (Ck k : cks) if (k != fmt && !isOldTag(fi, k)) keep.add(k);
            long riff = 4 + tc.length; for (Ck k : keep) riff += 8L + k.len + (k.len & 1);
            if (riff > 0xFFFFFFFFL) { env.printf("the new file would exceed the 4 GB limit of WAV; nothing written\n"); return 1; }
            try { BufferedOutputStream fo = new BufferedOutputStream(new FileOutputStream(part), 1 << 16);
                  try { byte[] h = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}; for (int i = 0; i < 4; i++) h[4 + i] = (byte) (riff >>> (8 * i)); fo.write(h);
                        for (int i = 0; i < keep.size(); i++) { copyBody(fi, keep.get(i), fo); if (i == 0 && tc.length != 0) fo.write(tc); }
                        ok = true; }
                  finally { fo.close(); } }
            catch (IOException e) { ok = false; }
            if (ok) { RandomAccessFile fc = null;
                try { fc = new RandomAccessFile(part, "r"); List<Ck> c2 = new ArrayList<Ck>(); String[] w2 = {""};
                      ok = walk(fc, c2, w2) && find(c2, "data") != null && find(c2, "LAE ") != null && sameBody(fi, dat, fc, find(c2, "data")) && sameBody(fi, lae, fc, find(c2, "LAE ")); }
                catch (IOException e) { ok = false; }
                finally { if (fc != null) try { fc.close(); } catch (IOException e) { /* read only */ } }
                if (!ok) env.printf("check FAILED: the new file's CD audio or LAE chunk differs from the input's\n"); }
            else env.printf("cannot write %s\n", part);
        } catch (IOException e) { ok = false; env.printf("cannot read %s\n", in);
        } finally { if (fi != null) try { fi.close(); } catch (IOException e) { /* read only */ } }
        if (ok) { ok = LaeEncoder.rename(part, out); if (!ok) env.printf("cannot rename %s to %s\n", part, out); }   // also replaces an output agreed to (decision 21)
        if (!ok) { new File(part).delete(); env.printf("nothing written\n"); return 2; }
        env.printf("written: %s (%zu tag(s), %zu picture(s)); CD audio and LAE chunk byte-identical to %s\n", out, (long) tags.kv.size(), (long) tags.pics.size(), in); return 0; }
}
