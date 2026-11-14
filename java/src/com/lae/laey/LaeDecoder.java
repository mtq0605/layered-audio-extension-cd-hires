// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import com.lae.core.Cv;
import com.lae.core.Halfband;
import com.lae.core.Le;
import com.lae.core.U;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Port of src/lae_decoder.h: the LAE v5 decoder as a library (lossy tier: routes full, 16, cd24), STREAMING: memory does not grow
 * with the track length. Used by {@link LaeyDecoder} (the port of lae_api.cpp).
 * Deterministic pipeline (identical on every platform): range decoding one row of roots (8 frames) at a time -> dequantise
 * -> textbook IMDCT frame by frame (mdct_tb.h, strict IEEE double, overlap-add in a 2N window) -> base in 1/16 LSB24 = 4096 x CD
 * - regenerated TPDF dither + 16 x depth -> integer half-band up-conversion (Q31, 64-bit, one rounding per output) -> + 16 x frequency
 * layer (time domain) -> optional integer half-band decimation to the chosen octave -> one final rounding -> hard limiter.
 * Every stage is pulled sample by sample through a short sliding window, so memory does not depend on the track length.
 * Callers must check MdctTb.selftestOK() once before decoding (it refuses builds that do not follow the IEEE-754 order).
 * <p>
 * Port notes: C++ {@code long} is 32-bit on MSVC x64; all indices here stay below 2^31 for every accepted header (Lm + 2P <= 2e9)
 * or, where an intermediate exceeds it (Cached::mirror, 2 * (n - 1) for n > 2^30), the result fits and the 32-bit wrap cancels
 * (measured with MSVC /O2, port_work/laey_dec/probe/mirror_probe.cpp), so 64-bit Java arithmetic gives the same values.
 * The build is the library build (LAE_LIBRARY): a window underrun sets the stream-error flag instead of ending the process.
 */
public final class LaeDecoder {
    private LaeDecoder() {}

    static final int N = MdctTb.N;

    // port of lae_decoder.h:tab (half-length returned in half[0])
    static int[] tab(int s, int[] half) { switch (s) { case 0: half[0] = Halfband.kHalfLen0; return Halfband.kHalf0; case 1: half[0] = Halfband.kHalfLen1; return Halfband.kHalf1;
        case 2: half[0] = Halfband.kHalfLen2; return Halfband.kHalf2; default: half[0] = Halfband.kHalfLen3; return Halfband.kHalf3; } }
    // port of lae_decoder.h:rq31
    static long rq31(long a) { return (a + (1L << 30)) >> 31; }

    /** port of lae_decoder.h:Rd (p + off = the C++ pointer; n, i are size_t values held as long) */
    static final class Rd { final byte[] p; final int off; final long n; long i = 0;
        Rd(byte[] p, int off, long n) { this.p = p; this.off = off; this.n = n; }
        // port of lae_decoder.h:Rd::i32v
        int i32v() { int v = 0; if (i + 4 <= n) v = Le.i32(p, off + (int) i); i += 4; return v; }
        // port of lae_decoder.h:Rd::take (returns the array offset of the C++ pointer)
        int take(long k) { int q = off + (int) i; i += k; return q; } }

    /** a growable int list (the C++ std::vector&lt;int32_t&gt; of the limiter map) */
    public static final class IntList { int[] a = new int[16]; int size;
        public void add(int v) { if (size == a.length) a = Arrays.copyOf(a, size * 2); a[size++] = v; }
        public int size() { return size; } public int get(int i) { return a[i]; } }

    /** port of lae_decoder.h:PlaneStream: one range-coded stream, decoded one row of roots at a time; frame(m) gives the dequantised coefficients of frame m */
    public static final class PlaneStream {
        final PartStream.Err err;
        PartStream.Hdr h = new PartStream.Hdr(); PartStream.Dec dc = new PartStream.Dec(); PartStream.Walker w; public int[] rowBuf; long row0 = -1; int rows = 0;
        // checkpoints for seeking: the decoder state before the row at frame M0, kept every `every` rows (about 4 s of the stream).
        // Java: a Mark is never changed after it is made (seekTo copies its state), so decoders of the same stream share Mark objects.
        static final class Mark { final long M0; final PartStream.Dec dc; final PartStream.Walker w; Mark(long m, PartStream.Dec d, PartStream.Walker ww) { M0 = m; dc = d; w = ww; } }
        final List<Mark> marks = new ArrayList<Mark>(); long every = 1;
        // decision 34 (2026-11-15, F05-4): false = no checkpoints are kept (LaeDec converts the whole file and never seeks; the library keeps them)
        boolean keepMarks = true;
        // F05-2 (2026-11-14): the stream header's rate and K are the file's own statement and have no upper limit, so nothing that costs
        // memory or time follows them alone. srate = the sample rate of this stream's layer from the main header (base for the depth layer,
        // the master rate for the frequency layer): the checkpoint spacing is counted with it. lim = min(K, frames of the layer): rows from
        // frame lim on are never requested (spec 5.3) and are not decoded. The decoded samples of every file are the same as before.
        int srate = 0; long lim = 0;
        PartStream.Dec dc0;   // the state at the start of the stream (decision 10: the check decodes every stream from here)
        public PlaneStream(PartStream.Err e) { err = e; }
        // port of lae_decoder.h:PlaneStream::open (p[off ..] = the stream's bytes)
        public boolean open(byte[] p, int off, int nbytes, int layerRate, long layerFrames) { if (nbytes < 40) return false; Rd s = new Rd(p, off, nbytes); int[] f = new int[10]; for (int i = 0; i < 10; i++) f[i] = s.i32v();
            h = new PartStream.Hdr(); h.K = f[0]; h.N = f[1]; h.k0 = f[2]; h.k1 = f[3]; h.rate = f[4]; h.L = f[5]; h.cw = f[6]; h.lv = f[7]; h.eq = f[8];
            if (h.N != N || f[9] != nbytes - 40 || h.K <= 0 || h.k0 < 0 || h.k1 > N || h.k0 >= h.k1) return false;
            dc.p = p; dc.off = off + 40; dc.n = f[9]; dc.init(); dc0 = dc.copy(); w = new PartStream.Walker(h, err); rowBuf = new int[PartitionFormat.ROOTT * N];
            srate = layerRate; lim = Math.min(h.K, layerFrames);
            every = srate > 0 ? Math.max(1L, (long) Cv.d2i(4.0 * srate / ((double) PartitionFormat.ROOTT * N))) : 64; return true; }   // (long)double: 32-bit long on MSVC
        // port of lae_decoder.h:PlaneStream::frame (returns the offset of frame m in rowBuf, or -1 for the C++ nullptr)
        public int frame(long m) {   // frames are requested in increasing order (seekTo excepted)
            while (!(m >= row0 && m < row0 + rows)) { if (w.M0 >= lim) return -1;
                if (keepMarks && (w.M0 / PartitionFormat.ROOTT) % every == 0 && (marks.isEmpty() || marks.get(marks.size() - 1).M0 < w.M0)) marks.add(new Mark(w.M0, dc.copy(), new PartStream.Walker(w)));
                row0 = w.M0; rows = w.rowFrames(); Arrays.fill(rowBuf, 0); w.row(dc, rowBuf, row0); }
            return (int) ((m - row0) * N); }
        /** Java only (tests): the stream header fields K, k0, k1, cw, rate and the read position dc.i */
        public long[] info() { return new long[] {h.K, h.k0, h.k1, h.cw, h.rate, dc.i}; }
        // port of lae_decoder.h:PlaneStream::seekTo
        // the next frame(m') with m' >= m continues from the nearest checkpoint at or before m, or from the current state when that is closer
        void seekTo(long m) {
            final long cur = row0 >= 0 ? row0 : w.M0; final boolean forward = cur <= m; Mark best = null;
            for (Mark k : marks) { if (k.M0 > m) break; best = k; }
            if (best != null && (!forward || best.M0 > cur)) { dc = best.dc.copy(); w = new PartStream.Walker(h, err, best.w); row0 = -1; rows = 0; } } }   // Java: bound to this stream's header and error flag (the mark may come from another decoder)

    // port of lae_decoder.h:scanStream (decision 10, spec/lossy 6.7), without the saved checkpoints (the Java port keeps no player record):
    // decodes the whole stream from its start with its own walker and error flag; true when the stream ends exactly at its last byte with
    // no symbol out of range. The walk stops at the first byte asked for beyond the end, and a K within one row of 2^31 is refused before it.
    static boolean scanStream(PlaneStream ps) {
        if (ps.h.K > 2147483647L - PartitionFormat.ROOTT) return false;
        final PartStream.Err e = new PartStream.Err(); final PartStream.Dec d = ps.dc0.copy(); final PartStream.Walker w = new PartStream.Walker(ps.h, e);
        final int[] buf = new int[PartitionFormat.ROOTT * N];
        while (!w.done() && d.over == 0) w.row(d, buf, w.M0);
        return !e.v && d.i == d.n && d.over == 0; }

    /** port of lae_decoder.h:ImdctStream: IMDCT of a sum of planes (octave segments are disjoint), sample by sample: out(i) = llround(y[N + i]) as mdct::inverse */
    public static final class ImdctStream {
        final PartStream.Err err;
        public final List<PlaneStream> parts = new ArrayList<PlaneStream>(); long L = 0, K = 0, nextFrame = 0; double[] y; int[] outRing; long outHead = 0;   // signal samples [0, outHead) produced
        private final double[] c = new double[N], u = new double[N], re = new double[MdctTb.M], im = new double[MdctTb.M];
        public ImdctStream(PartStream.Err e) { err = e; }
        // port of lae_decoder.h:ImdctStream::init
        public void init(long len) { L = len; K = MdctTb.frames(L); y = new double[4 * N]; outRing = new int[1 << 15]; }
        // port of lae_decoder.h:ImdctStream::step
        void step() {   // add frame m; padded samples [mN, mN + N) become final
            long m = nextFrame++; for (int k = 0; k < N; k++) c[k] = 0;
            boolean have = false; for (PlaneStream ps : parts) { int f = ps.frame(m); if (f < 0) continue; have = true; int[] rb = ps.rowBuf; for (int k = 0; k < N; k++) c[k] += rb[f + k]; }
            long s = m * N; final double[] Y = y; final int W = 4 * N, Wm = W - 1; final double[] WIN = MdctTb.WIN;   // Java: x % W as x & (W - 1) (W = 2048, x >= 0)
            if (have && m < K) { MdctTb.dct4(c, u, re, im);
                for (int n = 0; n < N / 2; n++) { Y[(int) ((s + n) & Wm)] += u[N / 2 + n] * WIN[n]; Y[(int) ((s + N / 2 + n) & Wm)] += -u[N - 1 - n] * WIN[N / 2 + n];
                    Y[(int) ((s + N + n) & Wm)] += -u[N / 2 - 1 - n] * WIN[N + n]; Y[(int) ((s + 3 * N / 2 + n) & Wm)] += -u[n] * WIN[3 * N / 2 + n]; } }
            for (long p = s; p < s + N; p++) { int pi = (int) (p & Wm); double v = Y[pi]; Y[pi] = 0.0; long i = p - N; if (i >= 0 && i < L) { if (v > 1073741823.0 || v < -1073741823.0) { err.v = true; v = v > 0 ? 1073741823.0 : -1073741823.0; }   // beyond any valid signal: corrupt
                    outRing[(int) (outHead & (outRing.length - 1))] = (int) Cv.llround(v); outHead++; } } }
        // port of lae_decoder.h:ImdctStream::get
        public int get(long i) { if (i >= L) return 0; while (outHead <= i) step(); return outRing[(int) (i & (outRing.length - 1))]; }
        // port of lae_decoder.h:ImdctStream::resetAt
        // seeking: restart at frame ms with an empty overlap buffer. Output from signal index ms*N on is then exact (each sample is the sum
        // of the two frames that cover it, both added after the restart); the first stepped frame only finalises samples below ms*N
        void resetAt(long ms) { nextFrame = ms; Arrays.fill(y, 0.0); outHead = Math.max(0L, (ms - 1) * N); for (PlaneStream ps : parts) ps.seekTo(ms); } }

    // ---- pulled signal stages with a cached sliding window (all values in 1/16 LSB24)
    /** port of lae_decoder.h:Src */
    public abstract static class Src { public long n = 0; public abstract long get(long i); }
    /** port of lae_decoder.h:Cached */
    public abstract static class Cached extends Src { final long[] ring = new long[1 << 14]; long lo = 0, hi = 0;   // computed [lo, hi)
        final PartStream.Err err;
        Cached(PartStream.Err e) { err = e; }
        protected abstract long compute(long i);
        // port of lae_decoder.h:Cached::get (library build: an underrun is reported as a stream error)
        @Override public long get(long i) { if (i < lo) { err.v = true; return 0; }   // a library must never end its host process: reported as a stream error
            final int mask = ring.length - 1;
            while (hi <= i) { ring[(int) (hi & mask)] = compute(hi); hi++; if (hi - lo > ring.length) lo = hi - ring.length; }
            return ring[(int) (i & mask)]; }
        // port of lae_decoder.h:Cached::mirror
        static long mirror(Src in, long i) { long n = in.n; if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; if (i < 0) i = 0; if (i > n - 1) i = n - 1; return in.get(i); }
        // port of lae_decoder.h:Cached::resetAt
        void resetAt(long s) { lo = hi = s; } }   // seeking: the next value computed is index s (compute(i) depends only on the inputs at fixed offsets of i)

    // xorshift32 (13, 17, 5) advanced by `steps` steps in O(log steps): the step is linear over GF(2), so it is applied as a 32 x 32 bit matrix power
    // port of lae_decoder.h:xsApply
    static int xsApply(int[] col, int v) { int r = 0; for (int j = 0; j < 32; j++) if (((v >>> j) & 1) != 0) r ^= col[j]; return r; }
    // port of lae_decoder.h:xsJump (steps: uint64_t)
    public static int xsJump(int s, long steps) {
        int[] M = new int[32], M2 = new int[32]; for (int j = 0; j < 32; j++) { int v = 1 << j; v ^= v << 13; v ^= v >>> 17; v ^= v << 5; M[j] = v; }
        while (steps != 0) { if ((steps & 1) != 0) s = xsApply(M, s); for (int j = 0; j < 32; j++) M2[j] = xsApply(M, M[j]); System.arraycopy(M2, 0, M, 0, 32); steps >>>= 1; }
        return s; }

    /** port of lae_decoder.h:BaseSrc: 4096 x CD - dither + 16 x depth, base rate, padded length nb */
    static final class BaseSrc extends Cached {
        CdPcm cd; byte[] edgeB; int edgeOff; long pb, ncd; int ch; byte[] dflB; int dflOff; long nbk; ImdctStream depth; final int[] st = {0x12345678, 0x12345678 + 0x9E3779B9};
        BaseSrc(PartStream.Err e) { super(e); }
        private int edge(long k) { return Le.s16(edgeB, edgeOff + (int) (2 * k)); }   // the C++ int16_t* edge (little-endian bytes of the chunk)
        // port of lae_decoder.h:BaseSrc::sample
        int sample(long i) { return i < pb ? edge(i * 2 + ch) : (i < pb + ncd ? cd.get((i - pb) * 2 + ch) : edge(2 * pb + (i - pb - ncd) * 2 + ch)); }
        // port of lae_decoder.h:BaseSrc::compute
        @Override protected long compute(long i) { long d16 = 0;
            for (int cc = 0; cc < 2; cc++) { int s = st[cc]; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u1 = (s >>> 20) & 0xFFF; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; int u2 = (s >>> 20) & 0xFFF; st[cc] = s;
                if (cc == ch && nbk != 0 && dflB[dflOff + (int) (cc * nbk + i / 4096)] != 0) d16 = u1 + u2 - 4096; }
            return 4096L * sample(i) - d16 + (depth != null ? 16L * depth.get(i) : 0); }
        // port of lae_decoder.h:BaseSrc::ditherAt
        void ditherAt(long i) { st[0] = xsJump(0x12345678, 2L * i); st[1] = xsJump(0x12345678 + 0x9E3779B9, 2L * i); } }   // the state after i samples (2 steps per sample)

    /** port of lae_decoder.h:UpSrc */
    public static final class UpSrc extends Cached { final Src in; final int[] h; final int half;
        public UpSrc(Src x, int s, PartStream.Err e) { super(e); in = x; int[] hf = new int[1]; h = tab(s, hf); half = hf[0]; n = 2 * x.n; }
        // port of lae_decoder.h:UpSrc::compute
        @Override protected long compute(long o) { if ((o & 1) == 0) return in.get(o / 2); long a = 0;
            final long[] rg = interior(in, (o - half) / 2, (o + half) / 2);
            if (rg != null) { final int mask = rg.length - 1;   // Java fast form (2026-11-04): the same values read from the input's window directly
                for (int j = 1, t = 0; j <= half; j += 2, t++) a += (long) h[t] * (rg[(int) (((o - j) / 2) & mask)] + rg[(int) (((o + j) / 2) & mask)]);
                return rq31(2 * a); }
            for (int j = 1, t = 0; j <= half; j += 2, t++) a += (long) h[t] * (mirror(in, (o - j) / 2) + mirror(in, (o + j) / 2));
            return rq31(2 * a); } }
    // Java (2026-11-04, speed): the ring of input x when every index in [i0, i1] is inside the signal (mirror leaves it as it is) and, once
    // x has computed up to i1 (in order, as get(i1) does), still inside x's window: then x.get(i) == ring[i & mask] for each of them and no
    // get() would report an underrun. Otherwise null (the caller takes the get() path, which also reports underruns as before).
    static long[] interior(Src x, long i0, long i1) {
        if (!(x instanceof Cached) || i0 < 0 || i1 > x.n - 1) return null; final Cached c = (Cached) x;
        if (i1 + 1 - i0 > c.ring.length) return null; if (c.hi <= i1) c.get(i1);   // computes [hi, i1] in order; the window then still holds i0 unless i0 < lo already
        return i0 >= c.lo && i1 < c.hi ? c.ring : null; }

    /** port of lae_decoder.h:MixSrc: up(base) truncated / zero-padded to Lp, + 16 x frequency layer (L/R from M/S) */
    static final class MixSrc extends Cached { Src up; ImdctStream mt, st; int ch;
        MixSrc(PartStream.Err e) { super(e); }
        // port of lae_decoder.h:MixSrc::compute
        @Override protected long compute(long i) { long u = i < up.n ? up.get(i) : 0; if (mt == null) return u; long m = mt.get(i), s = st.get(i); long r = m - (s >> 1); return u + 16 * (ch != 0 ? r : r + s); } }   // 64-bit: no overflow on any input

    /** port of lae_decoder.h:FreqSrc: frequency layer alone (L/R), 1/16 LSB24 (leak compensation, LEAKOUT) */
    static final class FreqSrc extends Cached { ImdctStream mt, st; int ch;
        FreqSrc(PartStream.Err e) { super(e); }
        // port of lae_decoder.h:FreqSrc::compute
        @Override protected long compute(long i) { long m = mt.get(i), s = st.get(i), r = m - (s >> 1); return 16 * (ch != 0 ? r : r + s); } }

    /** port of lae_decoder.h:DecSrc */
    public static final class DecSrc extends Cached { final Src in; final int[] h; final int half;
        public DecSrc(Src x, int s, PartStream.Err e) { super(e); in = x; int[] hf = new int[1]; h = tab(s, hf); half = hf[0]; n = x.n / 2; }
        // port of lae_decoder.h:DecSrc::compute
        @Override protected long compute(long i) { long c = 2 * i; long a = in.get(c) * (1L << 30);
            final long[] rg = interior(in, c - half, c + half);
            if (rg != null) { final int mask = rg.length - 1;   // Java fast form (2026-11-04), as UpSrc
                for (int j = 1, t = 0; j <= half; j += 2, t++) a += (long) h[t] * (rg[(int) ((c - j) & mask)] + rg[(int) ((c + j) & mask)]);
                return rq31(a); }
            for (int j = 1, t = 0; j <= half; j += 2, t++) a += (long) h[t] * (mirror(in, c - j) + mirror(in, c + j));
            return rq31(a); } }

    /** port of lae_decoder.h:Decoder: one track: open (all checks, in the order of the command-line decoder), then read the output frames in order */
    public static final class Decoder {
        /** the stream-error flag of this decoder (C++ thread_local g_streamErr) */
        public final PartStream.Err err = new PartStream.Err();
        CdPcm cdOwn; byte[] laeOwn;   // used by open(): the CD layer (interleaved L/R) and the "LAE " chunk body (with its CRC)
        /** Java only (2026-11-12): set before open() when this same chunk array already passed the CRC check in this process (the caller
         *  holds it unchanged): open() then skips the CRC pass over the whole chunk (every pack), which was the whole cost of lae_start */
        public boolean crcChecked = false;
        public String mode; public int rate = 0, base = 0, stages = 0, P = 0, fc = 0, mi = 0, eq = 0, wm = 0, nseg = 0, nused = 0, d = 0, wrate = 0;
        public long Lm = 0, nb = 0, pb = 0, ncd = 0, Lp = 0, Po = 0, Lo = 0, pos = 0, clip = 0;
        final PlaneStream[] dS = {new PlaneStream(err), new PlaneStream(err)}; PlaneStream[] fS = new PlaneStream[0];
        final ImdctStream[] dI = {new ImdctStream(err), new ImdctStream(err)}; final ImdctStream mI = new ImdctStream(err), sI = new ImdctStream(err); public boolean haveF = false;
        final Src[] outSrc = {null, null};
        final BaseSrc[] baseSrc = {null, null}; final MixSrc[] mix = {null, null};
        final UpSrc[][] ups = {new UpSrc[0], new UpSrc[0]}; final DecSrc[][] decs = {new DecSrc[0], new DecSrc[0]};   // the output chain (seek)

        // port of lae_decoder.h:Decoder::PackInfo, pack, packs (extension packs, 2026-11-10: one core and several mutually independent
        // extensions; the main body is pack 0, each further pack one "LPAK" record between the main body's last stream record and the CRC)
        public static final class PackInfo { public String name = ""; public int off = 0, len = 0, rate = 0, stages = 0; }
        public int pack = 0; public final java.util.ArrayList<PackInfo> packs = new java.util.ArrayList<PackInfo>();   // pack: set before open() (0 = main body, k = packs.get(k - 1))
        // port of lae_decoder.h:Decoder::bodyEnd (returns the end offset, or -1 where the C++ returns false)
        static long bodyEnd(byte[] p, long n) {
            if (n < 56) return -1; final int pb = Le.i32(p, 48); if (pb < 0 || pb > 65536) return -1;
            long i = 52 + 8L * pb; if (i > n || n - i < 4) return -1;
            final int ndf = Le.i32(p, (int) i); i += 4; if (ndf < 0 || ndf > n - i) return -1; i += ndf;
            for (int k = 0; k < 2; k++) { if (n - i < 4) return -1; final int nb = Le.i32(p, (int) i); i += 4; if (nb < 40 || nb > n - i) return -1; i += nb; }
            if (n - i < 4) return -1; final int ns = Le.i32(p, (int) i); i += 4; if (ns < 1 || ns > 8) return -1;
            for (int k = 0; k < 2 * ns; k++) { if (n - i < 4) return -1; final int nb = Le.i32(p, (int) i); i += 4; if (nb < 40 || nb > n - i) return -1; i += nb; }
            return i; }
        // port of lae_decoder.h:Decoder::scanPacks
        void scanPacks(byte[] p, long n) { packs.clear(); long e = bodyEnd(p, n); if (e < 0) return;
            while (packs.size() < 8 && n - e >= 9 && p[(int) e] == 'L' && p[(int) e + 1] == 'P' && p[(int) e + 2] == 'A' && p[(int) e + 3] == 'K') {
                final long len = Le.i32(p, (int) e + 4) & 0xFFFFFFFFL; if (len < 1 || len > n - e - 8) break;
                final int nl = p[(int) e + 8] & 0xFF; if (1 + nl + 52 > len) break; PackInfo q = new PackInfo();
                try { q.name = new String(p, (int) e + 9, nl, "UTF-8"); } catch (java.io.UnsupportedEncodingException x) { q.name = ""; }
                q.off = (int) e + 9 + nl; q.len = (int) (len - 1 - nl);
                if (Le.i32(p, q.off) != 0x3145414C || Le.i32(p, q.off + 4) != 5) break;
                if (Le.i32(p, q.off + 12) != Le.i32(p, 12)) break;   // F05-5 (C++ 47c58c4): spec 5.6, a pack has the main body's base (the same CD / core layer)
                q.rate = Le.i32(p, q.off + 8); q.stages = Le.i32(p, q.off + 16); packs.add(q); e += 8 + len; }
            // the name of the main body: one optional record after the last pack record, "LNAM" | 1 byte length | the name (UTF-8); display only
            mainName = ""; if (n - e >= 5 && p[(int) e] == 'L' && p[(int) e + 1] == 'N' && p[(int) e + 2] == 'A' && p[(int) e + 3] == 'M' && (p[(int) e + 4] & 0xFF) <= n - e - 5) {
                try { mainName = new String(p, (int) e + 5, p[(int) e + 4] & 0xFF, "UTF-8"); } catch (java.io.UnsupportedEncodingException x) { mainName = ""; } } }
        public String mainName = "";   // the main body's name ("" = none stored)
        private boolean opened = false;   // FU21-2: set by the first openCd (open / openRef included)
        private String fail(int[] code, int c, String m) { if (code != null) code[0] = c; return m; }
        // port of lae_decoder.h:Decoder::open
        // returns "" when ready; otherwise the message and code[0] (1 = not this format / bad arguments, 3 = corrupt: the CD audio still plays)
        public String open(short[] cdLayer, byte[] chunk, String route, int outRate, int[] code) {
            cdOwn = CdPcm.of(cdLayer); laeOwn = chunk; return openCd(cdOwn, laeOwn, route, outRate, code); }
        // port of lae_decoder.h:Decoder::openRef
        // as open(), but the caller keeps cd and lae alive and unchanged while this decoder is used
        public String openRef(final short[] cd, final byte[] lae, String route, int outRate, int[] code) { return openCd(CdPcm.of(cd), lae, route, outRate, code); }
        // Java only: as openRef, with the CD layer from a source (an array, or read on demand from the file with a bounded cache: {@link CdPcm})
        public String openCd(final CdPcm cd, final byte[] lae, String route, int outRate, int[] code) {
            // FU21-2: one object, one open. A second open would add the depth streams again and keep the former file's streams and
            // checkpoints in the lists (the C++ clears only fS); every caller makes a new Decoder for every open
            if (opened) return fail(code, 1, "internal error: this decoder was opened before (use a new one for every open)");
            opened = true;
            mode = route;
            if (!mode.equals("full") && !mode.equals("16") && !mode.equals("cd24")) return fail(code, 1, "route must be full, 16 or cd24");
            if (lae.length == 0) return fail(code, 1, "no LAE chunk: plain CD audio");
            if (lae.length < 8) return fail(code, 3, "corrupt LAE chunk");
            // the chunk's first 4 bytes name the format: "LAE1" = LAEY (lossy, this decoder), "LAE2" = LAES (lossless, decoded by laes)
            if (lae[0] == 'L' && lae[1] == 'A' && lae[2] == 'E' && lae[3] == '2') return fail(code, 1, "this is an LAES file (lossless, extension LAE2): decode it with laes; this decoder reads LAEY (extension LAE1)");
            if (!crcChecked) { int stored = Le.i32(lae, lae.length - 4); if (PartStream.crc32(lae, 0, lae.length - 4) != stored) return fail(code, 3, "LAE chunk damaged (CRC mismatch): refusing to decode; the CD audio itself still plays"); }
            int bp = 0; long bn = lae.length - 4;   // the payload to decode: the main body (pack 0), exactly as before
            scanPacks(lae, lae.length - 4);
            if (pack != 0) { if (pack < 0 || pack > packs.size()) return fail(code, 1, "this file has no extension pack of that number");
                bp = packs.get(pack - 1).off; bn = packs.get(pack - 1).len; }   // a further pack: its own complete v5 payload, decoded by the same steps
            Rd r = new Rd(lae, bp, bn);
            if (r.i32v() != 0x3145414C || r.i32v() != 5) return fail(code, 1, "unknown LAE chunk version (this decoder reads v5)");
            rate = r.i32v(); base = r.i32v(); stages = r.i32v(); Lm = r.i32v(); P = r.i32v(); fc = r.i32v(); mi = r.i32v(); eq = r.i32v(); wm = r.i32v(); nb = r.i32v(); pb = r.i32v();
            // header consistency: every field is checked before it is used to address anything
            boolean hdrOk = (base == 44100 || base == 48000) && stages >= 1 && stages <= 3 && rate == (base << stages) && P > 0 && P <= 65536 && P % (1 << stages) == 0
                         && pb == (P >> stages) && Lm > 0 && Lm <= 2000000000L - 2L * P && nb == ((Lm + 2L * P) >> stages) && mi >= 0 && mi < PartitionFormat.NMODES;
            if (!hdrOk || U.gt(8 * pb, r.n - r.i)) return fail(code, 3, "corrupt LAE header: refusing to decode");   // size_t comparison
            final int edge = r.take(8 * pb); int ndf = r.i32v();
            if (ndf != 2 * ((nb + 4095) / 4096) || r.i > r.n || U.gt(ndf, r.n - r.i)) return fail(code, 3, "corrupt dither flags: refusing to decode");
            final int dfl = r.take(ndf);
            ncd = cd.size() / 2; if (ncd != nb - 2 * pb || r.i > r.n || mi < 0 || mi >= PartitionFormat.NMODES || stages < 1 || stages > 3) return fail(code, 3, "CD audio does not match the LAE chunk (edited or misaligned): refusing to decode");
            int orate = outRate != 0 ? outRate : rate; if (orate <= 0) return fail(code, 1, "output rate must be a positive number");
            d = 0; while (d <= stages && (rate >> d) > orate) d++;
            if (!mode.equals("cd24") && ((rate >> d) != orate || d >= stages)) { if (code != null) code[0] = 1; return "output rate must be " + rate + " / 2^n and above " + base; }
            // streams: depth L, depth R, then n segments for M, n for S
            for (int c = 0; c < 2; c++) if (!take(r, dS[c], !mode.equals("16"), base, MdctTb.frames(nb))) return fail(code, 3, "corrupt depth stream");
            nseg = r.i32v(); if (nseg < 1 || nseg > 8) return fail(code, 3, "corrupt frequency layer");
            final int kcut = (int) Cv.llround((double) N * (orate / 2.0) / (rate / 2.0)); nused = 0;   // C++ (int)lround: 32-bit long, the value is <= N here
            fS = new PlaneStream[2 * nseg]; for (int k = 0; k < fS.length; k++) fS[k] = new PlaneStream(err); boolean[] used = new boolean[2 * nseg];
            for (int c = 0; c < 2; c++) for (int sg = 0; sg < nseg; sg++) { int k0s = 0; if (r.i + 4 + 12 <= r.n) k0s = Le.i32(lae, r.off + (int) (r.i + 4 + 8));   // header field 3 = start bin
                boolean use = !mode.equals("cd24") && k0s < kcut;
                used[c * nseg + sg] = use; if (use && c == 0) nused++;
                if (!take(r, fS[c * nseg + sg], use, rate, MdctTb.frames(Lm + 2L * P))) return fail(code, 3, "corrupt frequency stream"); }
            Lp = Lm + 2 * P;
            for (int c = 0; c < 2; c++) { dI[c].init(nb); if (!mode.equals("16")) dI[c].parts.add(dS[c]); }
            mI.init(Lp); sI.init(Lp); for (int sg = 0; sg < nseg; sg++) { if (used[sg]) mI.parts.add(fS[sg]); if (used[nseg + sg]) sI.parts.add(fS[nseg + sg]); }
            haveF = !mI.parts.isEmpty();
            for (int c = 0; c < 2; c++) {
                BaseSrc b = new BaseSrc(err); b.n = nb; b.cd = cd; b.edgeB = lae; b.edgeOff = edge; b.pb = pb; b.ncd = ncd; b.ch = c; b.dflB = lae; b.dflOff = dfl; b.nbk = ndf / 2; b.depth = !mode.equals("16") ? dI[c] : null; baseSrc[c] = b;
                if (mode.equals("cd24")) { outSrc[c] = b; continue; }
                Src s = b; ups[c] = new UpSrc[stages]; for (int k = 0; k < stages; k++) { UpSrc u = new UpSrc(s, k, err); ups[c][k] = u; s = u; }
                MixSrc mx = new MixSrc(err); mx.n = Lp; mx.up = s; mx.mt = haveF ? mI : null; mx.st = haveF ? sI : null; mx.ch = c; mix[c] = mx; s = mx;
                decs[c] = new DecSrc[d]; for (int k = 0; k < d; k++) { DecSrc dd = new DecSrc(s, stages - 1 - k, err); decs[c][k] = dd; s = dd; }
                outSrc[c] = s; }
            if (mode.equals("cd24")) { Po = pb; Lo = nb - 2 * pb; wrate = base; } else { Po = P >> d; Lo = Lm >> d; wrate = orate; }
            pos = 0; clip = 0; if (code != null) code[0] = 0; return ""; }
        // port of lae_decoder.h:Decoder::openRef lambda take
        private static boolean take(Rd r, PlaneStream ps, boolean use, int layerRate, long layerFrames) { int nbytes = r.i32v(); long start = r.i; if (nbytes < 40 || start + nbytes > r.n) return false;
            if (use && !ps.open(r.p, r.off + (int) start, nbytes, layerRate, layerFrames)) return false; r.i = start + nbytes; return true; }

        // port of lae_decoder.h:Decoder::adoptMarks
        // the entropy checkpoints of another decoder of the same chunk (another route): a stream's checkpoints depend only on its bytes, so
        // they are valid here and a route change seeks as fast as the old decoder would have (output unchanged: a seek from a checkpoint
        // gives the samples of a decode from the start)
        public void adoptMarks(Decoder o) { adoptMarks(o.snapMarks()); }
        /** Java only (2026-11-12): the checkpoints of every stream as they are now (the lists are copied, the Mark objects shared: they are
         *  never changed). Taken in the thread that uses this decoder; the result may then be adopted by a decoder in another thread. */
        public static final class MarkSnap { final List<List<PlaneStream.Mark>> dS = new ArrayList<List<PlaneStream.Mark>>(), fS = new ArrayList<List<PlaneStream.Mark>>(); }
        public MarkSnap snapMarks() { MarkSnap m = new MarkSnap();
            for (int c = 0; c < 2; c++) m.dS.add(dS[c].w != null ? new ArrayList<PlaneStream.Mark>(dS[c].marks) : null);
            for (PlaneStream p : fS) m.fS.add(p.w != null ? new ArrayList<PlaneStream.Mark>(p.marks) : null); return m; }
        public void adoptMarks(MarkSnap o) {
            for (int c = 0; c < 2; c++) copyMarks(dS[c], o.dS.get(c));
            for (int k = 0; k < fS.length && k < o.fS.size(); k++) copyMarks(fS[k], o.fS.get(k)); }
        // port of lae_decoder.h:Decoder::adoptMarks lambda copy (Java: the Mark objects are shared, not copied; seekTo copies the state it restores)
        private static void copyMarks(PlaneStream to, List<PlaneStream.Mark> from) { if (to.w == null || from == null || !to.marks.isEmpty()) return; to.marks.addAll(from); }
        // port of lae_decoder.h:Decoder::allStreams: every entropy stream of the chunk in a fixed order (depth L, depth R, then the
        // frequency segments); unopened ones have no walker
        List<PlaneStream> allStreams() { List<PlaneStream> v = new ArrayList<PlaneStream>(); v.add(dS[0]); v.add(dS[1]); for (PlaneStream f : fS) v.add(f); return v; }
        // port of lae_decoder.h:Decoder::noSeekMarks (decision 34): no checkpoints are kept while decoding (a whole-file conversion that never seeks: LaeDec)
        public void noSeekMarks() { for (PlaneStream p : allStreams()) p.keepMarks = false; }
        /** port of lae_decoder.h:Decoder::seekThreads */
        public int seekThreads = Math.max(1, Math.min(16, Runtime.getRuntime().availableProcessors()));
        // port of lae_decoder.h:Decoder::checkAll (decision 10, 2026-11-15): every stream decoded from start to end in parallel (route full at
        // the master rate opens all of them); returns "" or the reason the file fails the check. Java: no checkpoints are saved.
        public String checkAll(int threads) {
            final List<PlaneStream> ps = allStreams(); final boolean[] ok = new boolean[ps.size()];
            for (PlaneStream p : ps) if (p.w == null) return "the check needs every stream (route full at the master rate)";
            runParallel(ps.size(), threads, new Job() { @Override public void run(int i) { ok[i] = scanStream(ps.get(i)); } });
            for (int i = 0; i < ps.size(); i++) if (!ok[i]) return "LAE chunk damaged (entropy stream " + i + " does not end at its last byte): refusing to decode; the CD audio itself still plays";
            return ""; }
        // port of lae_decoder.h:runParallel: job(i) for i < n, one thread each, at most `threads` at once (a thread's failure goes to the caller
        // after the others are joined: an OutOfMemoryError stays one, anything else becomes a RuntimeException)
        interface Job { void run(int i); }
        static void runParallel(int n, int threads, final Job job) {
            if (n <= 1 || threads <= 1) { for (int i = 0; i < n; i++) job.run(i); return; }
            final Throwable[] thrown = new Throwable[n];
            for (int i0 = 0; i0 < n; i0 += threads) { final int i1 = Math.min(n, i0 + threads); final Thread[] th = new Thread[i1 - i0];
                for (int i = i0; i < i1; i++) { final int k = i; th[i - i0] = new Thread(new Runnable() { @Override public void run() { try { job.run(k); } catch (Throwable x) { thrown[k] = x; } } }); th[i - i0].start(); }
                for (Thread t : th) { boolean done = false; while (!done) { try { t.join(); done = true; } catch (InterruptedException x) { /* the job ends on its own */ } } } }
            for (Throwable x : thrown) if (x != null) { if (x instanceof OutOfMemoryError) throw (OutOfMemoryError) x; if (x instanceof RuntimeException) throw (RuntimeException) x; throw new RuntimeException(x); } }
        // port of lae_decoder.h:Decoder::streamBytes
        // the read position summed over the streams this route decodes (bytes; it moves back when a seek restores a checkpoint, and two
        // calls with only reads in between differ by the bytes those reads consumed: the extension layers' share of the bitrate)
        public long streamBytes() { long n = 0; for (int c = 0; c < 2; c++) for (PlaneStream p : dI[c].parts) n += p.dc.i;
            for (PlaneStream p : mI.parts) n += p.dc.i; for (PlaneStream p : sI.parts) n += p.dc.i; return n; }
        // port of lae_decoder.h:Decoder::read
        // the next frames of the output (24-bit values, interleaved L/R, after the hard limiter, written from out[outOff]); returns the
        // number of frames written. clipPos (optional, encoder's limiter map): positions of limited samples per channel, in master-rate
        // frames of the padded signal.
        public long read(int[] out, int outOff, long frames, IntList[] clipPos) {
            long n = Math.min(frames, Lo - pos);
            for (long k = 0; k < n; k++, pos++) for (int c = 0; c < 2; c++) { long v = (outSrc[c].get(Po + pos) + 8) >> 4;
                if (v > 8388607 || v < -8388608) { if (clipPos != null) clipPos[c].add((int) (pos + P)); clip++; v = v > 0 ? 8388607 : -8388608; }   // hard limiter
                out[outOff + (int) (k * 2 + c)] = (int) v; }
            return n; }
        // port of lae_decoder.h:Decoder::seek
        // to output frame T, bit-exact (the samples read() gives when it reaches T from the start). The entropy streams continue from their
        // last checkpoint at or before the frames needed, or decode forward to them; every signal stage restarts a little before the first
        // index it must produce (each stage's value at index i depends only on its inputs at fixed offsets of i; the dither generator is
        // advanced to the index; each IMDCT sample is the sum of the two frames that cover it). Returns false near the start of the track,
        // where the caller simply restarts (just as fast).
        public boolean seek(long T) {
            if (T < 0) T = 0; if (T > Lo) T = Lo;
            final long margin = 4096; long s = Po + T - margin; if (s < 8 * margin) return false;
            long[] sDec = new long[decs[0].length], sUp = new long[ups[0].length]; long sMix = 0, sBase;
            if (mode.equals("cd24")) sBase = s;
            else { for (int k = decs[0].length - 1; k >= 0; k--) { sDec[k] = s; s = 2 * s - decs[0][k].half; }
                sMix = s;
                for (int k = ups[0].length - 1; k >= 0; k--) { sUp[k] = s; s = (s - ups[0][k].half) / 2; }
                sBase = s; }
            if (sBase < margin) return false;
            for (int c = 0; c < 2; c++) { baseSrc[c].resetAt(sBase); baseSrc[c].ditherAt(sBase);
                for (int k = 0; k < ups[c].length; k++) ups[c][k].resetAt(sUp[k]);
                if (mix[c] != null) mix[c].resetAt(sMix);
                for (int k = 0; k < decs[c].length; k++) decs[c][k].resetAt(sDec[k]); }
            if (!mode.equals("16")) for (int c = 0; c < 2; c++) dI[c].resetAt(sBase / N);
            if (!mode.equals("cd24") && haveF) { mI.resetAt(sMix / N); sI.resetAt(sMix / N); }
            pos = T; return true; }
        // port of lae_decoder.h:Decoder::writeLeak
        // the frequency layer alone at the base rate (encoder's leak compensation): writes int32 L/R pairs; returns base-rate frames
        public long writeLeak(OutputStream fl) {
            Src[] lk = new Src[2]; for (int c = 0; c < 2; c++) { FreqSrc fs = new FreqSrc(err); fs.n = Lp; fs.mt = mI; fs.st = sI; fs.ch = c; Src s = fs;
                for (int k = 0; k < stages; k++) { DecSrc dd = new DecSrc(s, stages - 1 - k, err); s = dd; } lk[c] = s; }
            long nlk = lk[0].n; byte[] buf = new byte[8 * 4096]; int fill = 0;
            try {
                for (long i = 0; i < nlk; i++) { for (int c = 0; c < 2; c++) { int v = (int) lk[c].get(i); Le.put32(buf, fill, v); fill += 4; }
                    if (fill == buf.length) { fl.write(buf, 0, fill); fill = 0; } }
                fl.write(buf, 0, fill);
            } catch (IOException e) { throw new com.lae.core.LaeException("cannot write the leak file", e); }
            return nlk; }
    }
}
