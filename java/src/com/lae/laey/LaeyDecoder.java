// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.AlignHash;
import com.lae.core.LaeException;
import com.lae.core.U;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Port of src/lae_api.cpp (liblae, the C interface of lae_api.h), the v5 WAV path: decoding LAE v5 (LAEY, extension LAE1) audio in
 * players. One object = one C {@code lae_track}. Not ported (out of scope, PORT_RULES.md): lae_open_pair, lae_open_pair_files and the
 * CD-Extra base layer + .lax reader (lae_cdx_read.h); from that header only parseWav (used by lae_open_memory) is ported here.
 * <p>
 * A track is opened from a v5 WAV (CD layer + "LAE " chunk). Then a route and an output rate are chosen and the output is read in order.
 * Output is always 2 channels, interleaved, as int32 values: 24-bit range for the routes CD24, 16 and FULL; 16-bit range for the
 * route CD. Threads: one track must not be used by two threads at once; different tracks may be decoded in different threads.
 * <p>
 * Java API mapping: functions that return NULL / -1 with a message in {@code err} (open, start) throw {@link LaeException} with the
 * same message; functions whose C version returns -1 without a message (read, seek, output_format, stats, stream_bytes) return -1.
 */
public final class LaeyDecoder {
    /** grows only when functions are added (C LAE_API_VERSION; the Java port has the v5 WAV functions of versions 1-3) */
    public static final int LAE_API_VERSION = 5;
    public static final int LAE_ROUTE_CD = 0;     /* the CD layer itself (16-bit, base rate) */
    public static final int LAE_ROUTE_CD24 = 1;   /* CD layer + depth layer (24-bit, base rate) */
    public static final int LAE_ROUTE_16 = 2;     /* CD layer + frequency layer (24-bit, master rate or an octave below) */
    public static final int LAE_ROUTE_FULL = 3;   /* all layers (24-bit, master rate or an octave below) */

    /** port of lae_api.h:lae_info */
    public static final class Info {
        public int master_rate, base_rate, stages, cutoff_hz;   /* cutoff_hz: CD cut-off (top of the CD band) */
        public long cd_frames, master_frames;                   /* lengths of the CD layer and of the master (uint64_t) */
        public String cd_md5 = "";                              /* whole-track MD5 of the CD layer (lower-case hex) */
        public int from_pair;                                   /* 1 when opened from a base layer + extension file (always 0 here) */
        Info copy() { Info i = new Info(); i.master_rate = master_rate; i.base_rate = base_rate; i.stages = stages; i.cutoff_hz = cutoff_hz;
            i.cd_frames = cd_frames; i.master_frames = master_frames; i.cd_md5 = cd_md5; i.from_pair = from_pair; return i; }
    }

    /** port of lae_api.h:lae_read_fn: fills buf[off .. off + len) with the len bytes at offset and returns 0, or returns -1 */
    public interface ReadFn { int read(long offset, byte[] buf, int off, int len); }

    // ---- struct lae_track
    CdPcm cd; byte[] lae; final Info info = new Info(); SharedFile file;   // file: Java only, the open file the CD layer is read from on demand (closed by the last lae_close of the track and its copies)
    boolean crcOk = false;   // Java only: lae passed the CRC check when the track was opened (later starts skip that pass over the whole chunk)
    // Java only: lae_dup's copy of the source's checkpoints (adopted by this copy's first start of the same pack)
    private LaeDecoder.Decoder.MarkSnap seed; private int seedPack = -1;
    /** Java only: an open file shared by a track and its lae_dup copies: reads are serialised, the last release closes it */
    static final class SharedFile { final RandomAccessFile f; private int refs = 1; SharedFile(RandomAccessFile x) { f = x; }
        synchronized int read(long offset, byte[] buf, int off, int len) { try { if (offset < 0) return -1; f.seek(offset); f.readFully(buf, off, len); return 0; } catch (IOException e) { return -1; } }
        synchronized void retain() { refs++; }
        synchronized void release() { if (--refs == 0) { try { f.close(); } catch (IOException e) { /* read-only file */ } } } }
    /** Java only: the default memory budget of lae_open_file for the CD layer read on demand (bytes; the decoder's other memory does not grow with the track length) */
    public static final long DEFAULT_CD_CACHE = 4L << 20;
    LaeDecoder.Decoder dec; int route = -1, outRate = 0, wrate = 0, bits = 0; long pos = 0, outFrames = 0; boolean err = false;
    private Throwable lastFailure;   // Java only: the exception a C++ catch (...) turned into -1 (for diagnosis; null if none)

    private LaeyDecoder() {}

    // port of lae_api.cpp:md5hex: the whole-track MD5 over little-endian 16-bit samples
    static String md5hex(short[] cd) { return AlignHash.cdMd5(cd, cd.length / 2); }   // cd.length is even (whole frames)

    // port of lae_api.cpp:finish: checks the v5 pair (all decoder checks) and fills the information; takes cd and lae
    private static LaeyDecoder finish(short[] cd, byte[] lae, int pair) { return finish(CdPcm.of(cd), lae, pair); }
    // (Java only: the CD layer from a source, see CdPcm)
    private static LaeyDecoder finish(CdPcm cd, byte[] lae, int pair) {
        LaeyDecoder t = new LaeyDecoder(); t.cd = cd; t.lae = lae;
        { LaeDecoder.Decoder D = new LaeDecoder.Decoder(); int[] code = {0}; String e = D.openCd(t.cd, t.lae, "full", 0, code); if (!e.isEmpty()) throw new LaeException(e);
          Info i = t.info; i.master_rate = D.rate; i.base_rate = D.base; i.stages = D.stages; i.cutoff_hz = D.fc; i.cd_frames = D.ncd; i.master_frames = D.Lm;
          { PackInfo p0 = new PackInfo(); p0.rate = D.rate; p0.stages = D.stages; p0.name = D.mainName; t.packList.add(p0); }
          for (LaeDecoder.Decoder.PackInfo q : D.packs) { PackInfo p = new PackInfo(); p.rate = q.rate; p.stages = q.stages; p.name = q.name; t.packList.add(p); } }
        t.crcOk = true;   // openCd above checked the CRC of this array (t.lae is never changed afterwards)
        t.info.cd_md5 = t.cd.md5(); t.info.from_pair = pair; return t; }

    // Extension packs (2026-11-10, spec/lossy/SPEC.md, Section 5.6): one core, several mutually independent extensions. Pack 0 is the
    // main body; the others carry the name their publisher gave. Only the caller chooses a pack - the app's menu or setting; nothing
    // here chooses one by itself, and the choice is never written into the file. The C interface (lae_api.h) gets the same three calls.
    public static final class PackInfo { public int rate, stages; public String name = ""; }
    private final java.util.ArrayList<PackInfo> packList = new java.util.ArrayList<PackInfo>(); private int pack = 0, decPack = 0;
    /** the number of extension packs in the file, the main body included (1 = an ordinary file) */
    public int lae_pack_count() { return packList.size(); }
    /** pack k's master rate, octave count and name (k = 0: the main body; its name is that of the optional "LNAM" record, else ""); null if there is no such pack */
    public PackInfo lae_pack_info(int k) { if (k < 0 || k >= packList.size()) return null; PackInfo s = packList.get(k), r = new PackInfo(); r.rate = s.rate; r.stages = s.stages; r.name = s.name; return r; }
    /** the pack the next lae_start decodes (0 = the main body, the default). The track is unstarted afterwards and lae_get_info describes
     *  the chosen pack. Throws if the file has no such pack. */
    public void lae_select_pack(int k) {
        if (k < 0 || k >= packList.size()) throw new LaeException("this file has no extension pack of that number");
        try { LaeDecoder.Decoder D = new LaeDecoder.Decoder(); D.pack = k; D.crcChecked = crcOk; int[] code = {0}; String e = D.openCd(cd, lae, "full", 0, code); if (!e.isEmpty()) throw new LaeException(e);
            info.master_rate = D.rate; info.stages = D.stages; info.cutoff_hz = D.fc; info.master_frames = D.Lm; }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); }
        pack = k; dec = null; route = -1; err = false; pos = 0; }

    // port of lae_cdx_read.h:parseWav (the part lae_open_memory uses: 16-bit stereo PCM and the last "LAE " chunk; lae[0] receives it)
    static String parseWav(byte[] b, long size, int[] rate, short[][] pcm, byte[][] lae) {
        if (size < 12 || !tag(b, 0, "RIFF") || !tag(b, 8, "WAVE")) return "not a WAV file";
        long p = 12; int fmt = 0, ch = 0, bits = 0; long data = -1; long dlen = 0; boolean haveFmt = false;
        while (size - p >= 8) { long body = p + 8, n = u32(b, (int) (p + 4)); if (n > size - body) n = size - body;   // a truncated last chunk is tolerated
        if (cd == null) throw new LaeException("the track is closed");   // FU21-6: as lae_dup (was a NullPointerException)
            if (tag(b, (int) p, "fmt ")) { if (n < 16) return "bad fmt chunk"; fmt = u16(b, (int) body); ch = u16(b, (int) body + 2); rate[0] = (int) u32(b, (int) body + 4); bits = u16(b, (int) body + 14); if (fmt == 0xFFFE && n >= 26) fmt = u16(b, (int) body + 24); haveFmt = true; }
            else if (tag(b, (int) p, "data")) { data = body; dlen = n; }
            else if (tag(b, (int) p, "LAE ") && lae != null) { lae[0] = new byte[(int) n]; System.arraycopy(b, (int) body, lae[0], 0, (int) n); }
            if (n + (n & 1) >= size - body) break; p = body + n + (n & 1); }
        if (!haveFmt || data < 0) return "not a PCM WAV file";
        if (fmt != 1 || ch != 2 || bits != 16) return "not 16-bit stereo PCM";
        if (dlen / 4 * 2 > Integer.MAX_VALUE - 8) return "the file is too large for this decoder";   // Java only: an array holds < 2^31 values
        short[] s = new short[(int) (dlen / 4 * 2)]; for (int i = 0; i < s.length; i++) s[i] = (short) ((b[(int) data + i * 2] & 0xFF) | b[(int) data + i * 2 + 1] << 8);
        pcm[0] = s;
        return ""; }
    private static boolean tag(byte[] b, int o, String t) { for (int k = 0; k < 4; k++) if (b[o + k] != t.charAt(k)) return false; return true; }
    private static int u16(byte[] q, int o) { return (q[o] & 0xFF) | (q[o + 1] & 0xFF) << 8; }
    private static long u32(byte[] q, int o) { return ((q[o] & 0xFF) | (q[o + 1] & 0xFF) << 8 | (q[o + 2] & 0xFF) << 16 | (long) (q[o + 3] & 0xFF) << 24); }

    // port of lae_api.cpp:openFlac: the FLAC form (LaeyFlac): the CD layer from the FLAC audio frames, the extension from the "LAE1"
    // APPLICATION blocks; checked against STREAMINFO (sample count, MD5). From there on the track is the same as one opened from a WAV.
    // (The C++ also takes the file's SHA-256 here, the key of the player's checkpoint record; the Java port keeps no such record.)
    private static LaeyDecoder openFlac(ReadFn rd, long size) {
        int[] rate = {0}; short[][] pcm = {null}; byte[][] lae = {null}; boolean[] notLaey = {true};
        final String e = LaeyFlac.read(rd, size, rate, pcm, lae, notLaey); if (!e.isEmpty()) throw new LaeException(e);
        return finish(pcm[0], lae[0], 0); }

    // port of lae_api.cpp:openBytes
    private static LaeyDecoder openBytes(final byte[] b, final long len) {
        if (len >= 4 && tag(b, 0, "fLaC")) return openFlac(new ReadFn() { @Override public int read(long offset, byte[] buf, int off, int n) {   // FLAC form
            if (offset < 0 || offset > len || n > len - offset) return -1; System.arraycopy(b, (int) offset, buf, off, n); return 0; } }, len);
        int[] rate = {0}; short[][] pcm = {null}; byte[][] lae = {new byte[0]}; String e = parseWav(b, len, rate, pcm, lae); if (!e.isEmpty()) throw new LaeException(e);
        if (lae[0].length == 0) throw new LaeException("no LAE chunk: plain CD audio");
        return finish(pcm[0], lae[0], 0); }

    // port of lae_api.cpp:openReader
    // the same RIFF walk as cdxr::parseWav (the last 'fmt ', 'data' and "LAE " chunk count; a truncated last chunk is tolerated), but through the
    // host's reader: only the 'data' and "LAE " bodies are read, straight into the track's own arrays
    private static LaeyDecoder openReader(ReadFn rd, long size) { return openReader(rd, size, 0); }
    // (Java only: cdCache > 0 reads the CD layer on demand through rd, with at most cdCache bytes of it held; 0: the whole layer is read, as the C++)
    private static LaeyDecoder openReader(ReadFn rd, long size, long cdCache) {
        byte[] h = new byte[12]; if (U.lt(size, 12)) throw new LaeException("not a WAV file");
        if (rd.read(0, h, 0, 12) != 0) throw new LaeException("cannot read the file");
        if (tag(h, 0, "fLaC")) return openFlac(rd, size);   // FLAC form (2026-11-10)
        if (!tag(h, 0, "RIFF") || !tag(h, 8, "WAVE")) throw new LaeException("not a WAV file");
        long p = 12, dOff = 0, dLen = 0, lOff = 0, lLen = 0; int fmt = 0, ch = 0, bits = 0; boolean haveFmt = false, haveData = false, haveLae = false;
        byte[] c = new byte[8];
        while (U.ge(size - p, 8)) { if (rd.read(p, c, 0, 8) != 0) throw new LaeException("cannot read the file");
            final long body = p + 8; long n = u32(c, 4); if (U.gt(n, size - body)) n = size - body;
            if (tag(c, 0, "fmt ")) { if (n < 16) throw new LaeException("bad fmt chunk");
                byte[] f = new byte[26]; final int k = n >= 26 ? 26 : 16; if (rd.read(body, f, 0, k) != 0) throw new LaeException("cannot read the file");
                fmt = u16(f, 0); ch = u16(f, 2); bits = u16(f, 14); if (fmt == 0xFFFE && k == 26) fmt = u16(f, 24); haveFmt = true; }
            else if (tag(c, 0, "data")) { dOff = body; dLen = n; haveData = true; }
            else if (tag(c, 0, "LAE ")) { lOff = body; lLen = n; haveLae = true; }
            if (U.ge(n + (n & 1), size - body)) break; p = body + n + (n & 1); }
        if (!haveFmt || !haveData) throw new LaeException("not a PCM WAV file");
        if (fmt != 1 || ch != 2 || bits != 16) throw new LaeException("not 16-bit stereo PCM");
        if (!haveLae || lLen == 0) throw new LaeException("no LAE chunk: plain CD audio");
        // C++: sizes above (size_t)-1 / 2; Java: an array holds fewer than 2^31 values (a 16-bit stereo WAV below 4 GiB always fits)
        if (dLen / 4 * 2 > Integer.MAX_VALUE - 8 || lLen > Integer.MAX_VALUE - 8) throw new LaeException("the file is too large for this decoder");
        if (cdCache > 0) { byte[] lae = new byte[(int) lLen]; if (!bulk(rd, lOff, lae, lLen)) throw new LaeException("cannot read the file"); return finish(CdPcm.reader(rd, dOff, dLen / 4 * 2, cdCache), lae, 0); }
        short[] pcm = new short[(int) (dLen / 4 * 2)]; byte[] lae = new byte[(int) lLen];
        if (!bulkPcm(rd, dOff, pcm, dLen / 4 * 4) || !bulk(rd, lOff, lae, lLen)) throw new LaeException("cannot read the file");
        return finish(pcm, lae, 0); }
    // port of lae_api.cpp:openReader lambda bulk: in blocks, so that the host can abort between them
    private static final long STEP = (long) 4 << 20;
    private static boolean bulk(ReadFn rd, long off, byte[] q, long n) {
        for (long i = 0; i < n; i += STEP) if (rd.read(off + i, q, (int) i, (int) Math.min(STEP, n - i)) != 0) return false; return true; }
    // the same blocks for the 'data' body, converted from little-endian bytes into the int16 array (the C++ reads into the vector's bytes)
    private static boolean bulkPcm(ReadFn rd, long off, short[] pcm, long n) {
        byte[] blk = new byte[(int) Math.min(STEP, Math.max(n, 1))];
        for (long i = 0; i < n; i += STEP) { int k = (int) Math.min(STEP, n - i); if (rd.read(off + i, blk, 0, k) != 0) return false;
            int s0 = (int) (i / 2); for (int j = 0; j < k / 2; j++) pcm[s0 + j] = (short) ((blk[2 * j] & 0xFF) | blk[2 * j + 1] << 8); }
        return true; }

    // port of lae_api.cpp:lae_api_version
    public static int lae_api_version() { return LAE_API_VERSION; }
    // port of lae_api.cpp:lae_selftest: 1 when this runtime decodes correctly
    public static int lae_selftest() { try { return MdctTb.selftestOK() ? 1 : 0; } catch (RuntimeException e) { return 0; } }

    // port of lae_api.cpp:lae_open_file (the C++ catch (...) of bad_alloc maps to the same message)
    public static LaeyDecoder lae_open_file(String path) { return lae_open_file(path, DEFAULT_CD_CACHE); }
    /**
     * Java only: lae_open_file with a memory budget for the CD layer: cdCacheBytes &gt; 0 reads it on demand from the file (the file stays open
     * until lae_close; at most about cdCacheBytes of it are held, at least 256 KiB); 0 reads it whole, as the C++. The output is the same.
     */
    public static LaeyDecoder lae_open_file(String path, long cdCacheBytes) {
        RandomAccessFile f = null; boolean keep = false;
        try { if (path == null) throw new LaeException("cannot read the file");
            try { f = new RandomAccessFile(new File(path), "r"); } catch (IOException e) { throw new LaeException("cannot read the file", e); }
            long n; try { n = f.length(); } catch (IOException e) { throw new LaeException("cannot read the file", e); }
            final SharedFile ff = new SharedFile(f);
            LaeyDecoder t = openReader(new ReadFn() {   // port of lae_api.cpp:readFile: lae_read_fn over a file (Java: serialised, lae_dup copies share the file)
                @Override public int read(long offset, byte[] buf, int off, int len) { return ff.read(offset, buf, off, len); } }, n, cdCacheBytes);
            if (cdCacheBytes > 0) { t.file = ff; keep = true; }
            return t; }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); }
        finally { if (f != null && !keep) { try { f.close(); } catch (IOException e) { /* read-only file: closing cannot lose data (C++ ignores fclose here too) */ } } } }
    // port of lae_api.cpp:lae_open_memory: the bytes of a whole v5 WAV file (wav[0 .. len))
    public static LaeyDecoder lae_open_memory(byte[] wav, long len) {
        try { if (wav == null) throw new LaeException("no input"); return openBytes(wav, Math.min(len, wav.length)); }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); } }
    // port of lae_api.cpp:lae_open_reader: a v5 WAV file of `size` bytes read through the host
    public static LaeyDecoder lae_open_reader(ReadFn read, long size) {
        try { if (read == null) throw new LaeException("no input"); return openReader(read, size); }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); } }
    /** Java only: lae_open_reader with a memory budget for the CD layer (see lae_open_file(String, long)); the host keeps read valid until lae_close */
    public static LaeyDecoder lae_open_reader(ReadFn read, long size, long cdCacheBytes) {
        try { if (read == null) throw new LaeException("no input"); return openReader(read, size, cdCacheBytes); }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); } }

    // port of lae_api.cpp:lae_get_info
    public Info lae_get_info() { return info.copy(); }

    // port of lae_api.cpp:lae_start
    // out_rate 0 = master rate (routes 16, FULL) or base rate (CD, CD24). Starts at output frame 0. Throws with the message on failure;
    // after a failure the track is not started (lae_read and lae_seek return -1 until a successful lae_start).
    public void lae_start(int route, int out_rate) {
        try { LaeDecoder.Decoder old = dec; dec = null; this.route = -1; err = false; pos = 0;   // a failed start leaves the track unstarted (read/seek refuse it)
            if (route == LAE_ROUTE_CD) {
                if (old != null) { seed = old.snapMarks(); seedPack = decPack; }   // FU21-6: the 24-bit route's checkpoints are kept for a later return to it (as lae_dup's seed)
                if (out_rate != 0 && out_rate != info.base_rate) throw new LaeException("the CD route plays at the base rate only");
                this.route = route; outRate = 0; wrate = info.base_rate; bits = 16; outFrames = info.cd_frames; return; }
            String m = route == LAE_ROUTE_CD24 ? "cd24" : route == LAE_ROUTE_16 ? "16" : route == LAE_ROUTE_FULL ? "full" : null;
            if (m == null) throw new LaeException("unknown route");
            if (!MdctTb.selftestOK()) throw new LaeException("decoder self-test failed in this thread (floating-point state): play the CD layer");
            LaeDecoder.Decoder D = new LaeDecoder.Decoder(); int[] code = {0}; D.pack = pack; D.crcChecked = crcOk;   // the chosen extension pack (0 = the main body)
            String e = D.openCd(cd, lae, m, route == LAE_ROUTE_CD24 ? 0 : out_rate, code); if (!e.isEmpty()) throw new LaeException(e);
            if (route == LAE_ROUTE_CD24 && out_rate != 0 && out_rate != D.wrate) throw new LaeException("the CD24 route plays at the base rate only");
            if (old != null && decPack == pack) D.adoptMarks(old);   // a restart or another route keeps the seek checkpoints decoded so far (of the same pack only)
            else if (seed != null && seedPack == pack) D.adoptMarks(seed);   // Java only: a lae_dup copy starts with its source's checkpoints
        if (cd == null) throw new LaeException("the track is closed");   // FU21-6: as lae_dup (was a NullPointerException)
            decPack = pack;
            this.route = route; outRate = out_rate; wrate = D.wrate; bits = 24; outFrames = D.Lo; dec = D; }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); } }

    // port of lae_api.cpp:lae_output_format: fmt[0] = rate, fmt[1] = bits; returns the frames, or -1 when not started
    public long lae_output_format(int[] fmt) {
        if (route < 0) return -1; if (fmt != null && fmt.length >= 2) { fmt[0] = wrate; fmt[1] = bits; } return outFrames; }

    // port of lae_api.cpp:lae_read
    // next frames in order into out[0 .. 2 * frames); returns frames written (0 at the end) or -1 on a stream error
    public long lae_read(int[] out, long frames) {
        if (route < 0 || out == null || frames < 0 || err) return -1;
        if (frames > out.length / 2) return -1;   // Java only: the C++ would write past the caller's buffer
        try {
            if (route == LAE_ROUTE_CD) { long n = U.min(frames, outFrames - pos);
                for (long i = 0; i < n * 2; i++) out[(int) i] = cd.get(pos * 2 + i); pos += n; return n; }
            if (dec == null) return -1;
            dec.err.v = false;   // the flag belongs to this track's decoder (the C++ saves and restores its per-thread flag around the call)
            long done = 0; while (done < frames) { long chunk = Math.min(frames - done, 1 << 20); long n = dec.read(out, (int) (done * 2), chunk, null); if (n <= 0) break; done += n; }
            boolean e = dec.err.v; dec.err.v = false; pos += done; if (e) { err = true; return -1; } return done; }
        catch (RuntimeException e) { lastFailure = e; err = true; return -1; }   // C++ catch (...)
        catch (OutOfMemoryError e) { lastFailure = e; err = true; return -1; } }

    // port of lae_api.cpp:lae_seek
    // to an output frame: forward decodes and discards, backward restarts from the beginning. Returns 0 or -1.
    public int lae_seek(long frame) {
        try { if (route < 0) return -1; if (U.gt(frame, outFrames)) frame = outFrames;
            if (route == LAE_ROUTE_CD) { pos = frame; return 0; }
            if (dec == null) return -1;
            if (frame != pos && dec.seek(frame)) { pos = frame; return 0; }   // bit-exact fast seek (checkpoints + warm restart)
            if (frame < pos) { try { lae_start(route, outRate); } catch (LaeException e) { lastFailure = e; return -1; } }   // near the start: restart and decode forward
            int[] scratch = new int[2 * 8192]; while (pos < frame) { long n = lae_read(scratch, Math.min(8192, frame - pos)); if (n <= 0) return -1; }
            return 0; }
        catch (RuntimeException e) { lastFailure = e; return -1; }
        catch (OutOfMemoryError e) { lastFailure = e; return -1; } }

    // port of lae_api.cpp:lae_stats: since lae_start: r[0] = samples held by the decoder's hard limiter, r[1] = 1 if a stream error occurred
    public int lae_stats(long[] r) { if (r != null && r.length >= 2) { r[0] = dec != null ? dec.clip : 0; r[1] = err ? 1 : 0; } return 0; }

    // port of lae_api.cpp:lae_stream_bytes: the read position summed over the entropy-coded extension streams the route decodes (bytes;
    // 0 on the CD route), or -1 when not started
    public long lae_stream_bytes() { if (route < 0) return -1; return dec != null ? dec.streamBytes() : 0; }

    /**
     * Java only (2026-11-12, for a route change in a player without a gap): a second track over the same opened file. It shares what lae_open_*
     * read and checked (the CD layer, the "LAE " chunk, the information with cd_md5; nothing is read or hashed again) and has its own
     * route, pack choice and decoding state. It starts unstarted, with this track's pack chosen, and its first lae_start of that pack
     * takes over this track's seek checkpoints decoded so far (output unchanged: a seek from a checkpoint gives the samples of a decode
     * from the start). The two tracks may then be decoded in two threads at once; each must still be used by one thread at a time.
     * Call lae_dup in the thread that uses this track. A file opened with lae_open_file stays open until both are closed; with
     * lae_open_reader and a CD cache, the host's read function must accept calls from both threads. Throws when the track is closed.
     */
    public LaeyDecoder lae_dup() {
        if (cd == null) throw new LaeException("the track is closed");
        try { LaeyDecoder t = new LaeyDecoder(); t.cd = cd.share(); t.lae = lae; t.crcOk = crcOk;
            Info s = info, i = t.info; i.master_rate = s.master_rate; i.base_rate = s.base_rate; i.stages = s.stages; i.cutoff_hz = s.cutoff_hz;
            i.cd_frames = s.cd_frames; i.master_frames = s.master_frames; i.cd_md5 = s.cd_md5; i.from_pair = s.from_pair;
            t.packList.addAll(packList); t.pack = pack;   // PackInfo entries are never changed after open (lae_pack_info returns copies)
            if (dec != null) { t.seed = dec.snapMarks(); t.seedPack = decPack; } else if (seed != null) { t.seed = seed; t.seedPack = seedPack; }
            if (file != null) { file.retain(); t.file = file; }
            return t; }
        catch (OutOfMemoryError e) { throw new LaeException("out of memory or internal error", e); } }

    // port of lae_api.cpp:lae_close
    public void lae_close() { dec = null; seed = null; if (cd != null) cd.close(); cd = null; route = -1;
        if (file != null) { file.release(); file = null; } }

    /** Java only: the exception that a C++ catch (...) would have turned into the -1 of the last failed read or seek (null if none) */
    public Throwable lastFailure() { return lastFailure; }
}
