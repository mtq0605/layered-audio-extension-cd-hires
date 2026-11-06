// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.AlignHash;
import com.lae.core.LaeException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The CD layer of a v5 file (interleaved L/R int16, the plain PCM of the WAV 'data' chunk) as the decoder reads it, sample by sample.
 * Java only (no C++ counterpart): the C++ decoder holds the whole CD layer in memory (std::vector&lt;int16_t&gt;); on a phone that is too
 * much next to an app (about 106 MB for 10 min at 44.1 kHz), so the layer can also be read on demand from the file through a bounded
 * cache of blocks. Every value is the same int16 either way, so every output is the same.
 * One source serves one track; it is not thread-safe (as the decoder: one thread at a time).
 */
public abstract class CdPcm {
    /** value i (interleaved: frame i / 2, channel i % 2), as a signed int16 */
    public abstract int get(long i);
    /** number of values (2 x frames) */
    public abstract long size();
    /** the whole-track MD5 over the little-endian values (alignhash::cdMd5) */
    public abstract String md5();
    /** releases what the source holds (a file reader: nothing to close here; the owner closes its file) */
    public void close() { }

    /** the values held in an array (the C++ layout) */
    public static CdPcm of(final short[] s) { return new CdPcm() {
        @Override public int get(long i) { return s[(int) i]; }
        @Override public long size() { return s.length; }
        @Override public String md5() { return AlignHash.cdMd5(s, s.length / 2); } }; }

    /** the values as little-endian bytes b[off .. off + 2 n) (a whole file already in memory: no second copy) */
    public static CdPcm ofBytes(final byte[] b, final int off, final long n) { return new CdPcm() {
        @Override public int get(long i) { int o = off + (int) (2 * i); return (short) ((b[o] & 0xFF) | b[o + 1] << 8); }
        @Override public long size() { return n; }
        @Override public String md5() { AlignHash.Md5 m = new AlignHash.Md5(); long len = 2 * n; for (long i = 0; i < len; i += 1 << 20) m.update(b, off + (int) i, (int) Math.min(1 << 20, len - i)); return m.hex(); } }; }

    /** values per cache block */
    public static final int BLOCK_VALUES = 1 << 15;   // 64 KiB per block

    /**
     * the values read on demand through rd from the bytes at off (n values), held in a cache of at most cacheBytes (at least 4 blocks of
     * 64 KiB). A failed read throws "cannot read the file" (the decoder's read then returns -1, as on a stream error).
     */
    public static CdPcm reader(final LaeyDecoder.ReadFn rd, final long off, final long n, long cacheBytes) {
        final int maxBlocks = (int) Math.max(4, Math.min(1 << 20, cacheBytes / (2L * BLOCK_VALUES)));
        return new CdPcm() {
            final Map<Long, short[]> cache = new LinkedHashMap<Long, short[]>(16, 0.75f, true) { private static final long serialVersionUID = 1L;
                @Override protected boolean removeEldestEntry(Map.Entry<Long, short[]> e) { return size() > maxBlocks; } };
            long lastK = -1; short[] last; byte[] buf = new byte[2 * BLOCK_VALUES];
            @Override public int get(long i) { final long k = i / BLOCK_VALUES; if (k != lastK) { last = block(k); lastK = k; } return last[(int) (i - k * BLOCK_VALUES)]; }
            short[] block(long k) { short[] s = cache.get(k); if (s != null) return s;
                final long a = k * BLOCK_VALUES; final int cnt = (int) Math.min(BLOCK_VALUES, n - a); if (cnt <= 0) throw new LaeException("cannot read the file");
                if (rd.read(off + 2 * a, buf, 0, 2 * cnt) != 0) throw new LaeException("cannot read the file");
                s = new short[cnt]; for (int j = 0; j < cnt; j++) s[j] = (short) ((buf[2 * j] & 0xFF) | buf[2 * j + 1] << 8); cache.put(k, s); return s; }
            @Override public long size() { return n; }
            @Override public String md5() { AlignHash.Md5 m = new AlignHash.Md5(); final long len = 2 * n; byte[] b = new byte[1 << 20];
                for (long i = 0; i < len; i += b.length) { int c = (int) Math.min(b.length, len - i); if (rd.read(off + i, b, 0, c) != 0) throw new LaeException("cannot read the file"); m.update(b, 0, c); }
                return m.hex(); }
            @Override public void close() { cache.clear(); last = null; lastK = -1; } }; }
}
