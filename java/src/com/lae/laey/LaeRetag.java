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

    // port of lae_enc.cpp:m_tag::retag; presetTags: tags the app gives as objects (added after those in argv; null = none)
    static int retag(EncEnv env, String[] argv, LaesTags.Tags presetTags) {
        if (argv.length < 4) { env.printf("usage: lae_enc tag <in.wav> <out.wav> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]   (no tags given: the tags are removed)\n"); return 1; }
        final String in = argv[2], out = argv[3], part = out + ".part";
        for (int i = 4; i < argv.length; i++) if (!isTagOpt(argv[i])) { env.printf("unknown option '%s': nothing written (options: --tag=KEY=VALUE --cover=<image> --picture=<type>:<image>)\n", argv[i]); return 1; }
        if (new File(out).exists()) { env.printf("%s already exists: choose another output name (the input is never changed); nothing written\n", out); return 1; }
        if (new File(part).exists()) { env.printf("%s already exists: remove it or choose another output name; nothing written\n", part); return 1; }
        LaesTags.Tags tags = new LaesTags.Tags(); { final String e = tagArgs(argv, 4, tags); if (!e.isEmpty()) { env.printf("%s: nothing written\n", e); return 1; } }
        if (presetTags != null) { tags.kv.addAll(presetTags.kv); tags.pics.addAll(presetTags.pics); final String e = LaesTags.checkAll(tags); if (!e.isEmpty()) { env.printf("%s: nothing written\n", e); return 1; } }
        final byte[] tc = LaesTags.wavChunks(tags, kLaeyEncoder);
        RandomAccessFile fi = null; boolean ok = false;
        try {
            try { fi = new RandomAccessFile(in, "r"); } catch (IOException e) { env.printf("cannot read %s\n", in); return 1; }
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
        if (ok) { ok = new File(part).renameTo(new File(out)); if (!ok) env.printf("cannot rename %s to %s\n", part, out); }
        if (!ok) { new File(part).delete(); env.printf("nothing written\n"); return 2; }
        env.printf("written: %s (%zu tag(s), %zu picture(s)); CD audio and LAE chunk byte-identical to %s\n", out, (long) tags.kv.size(), (long) tags.pics.size(), in); return 0; }
}
