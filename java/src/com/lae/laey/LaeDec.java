// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.laey;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * Port of src/lae_dec.cpp compiled with LAE_ENCODER_HOOKS (namespace m_dec of lae_enc.cpp): the standalone decoder the encoder runs on
 * its own output, with the two exports only the encoder uses (LEAKOUT: the decoded frequency layer at the base rate for the leak
 * compensation; CLIPMAP: the positions of the samples the hard limiter changed). The decoding itself is the verified Java port of
 * lae_decoder.h ({@link LaeDecoder.Decoder}).
 * C++ thread_local g_streamErr: the decoder's own flag ({@link LaeDecoder.Decoder#err}), returned in streamErr[0].
 * Difference to the C++: the C++ does not check its fwrite results here; a Java I/O error throws (the module then fails).
 */
final class LaeDec {
    private LaeDec() {}

    // port of lae_dec.cpp:main   usage: dec <in.wav> <out.wav> [full|16|cd24] [output rate]
    static int main(EncEnv env, String[] argv, boolean[] streamErr) {
        streamErr[0] = false;
        if (argv.length < 3) { env.printf("usage: lae_dec <in.wav> <out.wav> [full|16|cd24] [output rate]\n"); return 1; }
        String mode = argv.length > 3 ? argv[3] : "full";
        if (!MdctTb.selftestOK()) { env.printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the format requires (see docs/BUILD.md); refusing to decode\n"); return 4; }
        WavIo.Wav w = new WavIo.Wav(); if (!WavIo.readCD(argv[1], w, false) || w.bits != 16 || w.ch != 2) { env.printf("not a 16-bit stereo WAV\n"); return 1; }
        // Java only: the CD layer is read on demand from the file through a bounded cache (the C++ reads it whole); every value is the same
        final RandomAccessFile raf; try { raf = new RandomAccessFile(argv[1], "r"); } catch (IOException x) { env.printf("not a 16-bit stereo WAV\n"); return 1; }
        try { return decode(env, argv, streamErr, mode, w, raf); } finally { try { raf.close(); } catch (IOException x) { /* read-only file */ } } }
    /** the CD layer's cache in the encoder's decode checks (bytes) */
    static final long CD_CACHE = 4L << 20;
    // port of lae_dec.cpp:main, after the file is read (the CD layer from raf)
    private static int decode(EncEnv env, String[] argv, boolean[] streamErr, String mode, WavIo.Wav w, final RandomAccessFile raf) {
        LaeDecoder.Decoder D = new LaeDecoder.Decoder(); int[] code = {0}; int orate = argv.length > 4 ? (Fmt.atoi(argv[4]) > 0 ? Fmt.atoi(argv[4]) : -1) : 0;
        CdPcm cdp = CdPcm.reader(new LaeyDecoder.ReadFn() { @Override public int read(long offset, byte[] buf, int off, int len) { try { raf.seek(offset); raf.readFully(buf, off, len); return 0; } catch (IOException x) { return -1; } } }, w.dataOff, w.dataValues, CD_CACHE);
        String e = D.openCd(cdp, w.lae, mode, orate, code); w.lae = null; if (!e.isEmpty()) { env.printf("%s\n", e); return code[0]; }
        final String leakOut = env.get("LEAKOUT"), clipMap = env.get("CLIPMAP");
        if (leakOut != null && D.haveF) {   // leak compensation (encoder only): the decoded frequency layer brought down to the base rate (int32, 1/16 LSB24, L/R interleaved)
            BufferedOutputStream fl; try { fl = new BufferedOutputStream(new FileOutputStream(leakOut), 1 << 16); } catch (IOException x) { return 1; }
            long nlk; try { nlk = D.writeLeak(fl); } finally { try { fl.close(); } catch (IOException x) { /* the C++ fclose result is not checked; the encoder checks the file size */ } }
            streamErr[0] = D.err.v; env.printf("leak written: %ld base-rate samples/ch\n", nlk); return 0; }
        if (!WavIo.fits32(D.Lo * 6, 0)) { env.printf("output would exceed the 4 GB limit of WAV: choose a lower output rate\n"); return 1; }
        BufferedOutputStream fo; try { fo = new BufferedOutputStream(new FileOutputStream(argv[2]), 1 << 16); } catch (IOException x) { env.printf("cannot write %s\n", argv[2]); return 1; }
        boolean wantMap = clipMap != null && D.d == 0 && !mode.equals("cd24");
        LaeDecoder.IntList[] clipPos = wantMap ? new LaeDecoder.IntList[] {new LaeDecoder.IntList(), new LaeDecoder.IntList()} : null;
        try {
            fo.write(WavIo.hdr(D.wrate, 2, 24, D.Lo * 6, 0));
            int[] blk = new int[2 * 4096]; byte[] buf = new byte[2 * 4096 * 3];
            for (long got; (got = D.read(blk, 0, 4096, clipPos)) > 0;) { env.checkCancel(); int n = 0;
                for (int k = 0; k < got * 2; k++) { int v = blk[k]; buf[n++] = (byte) v; buf[n++] = (byte) (v >> 8); buf[n++] = (byte) (v >> 16); }
                fo.write(buf, 0, n); }
            if (((D.Lo * 6) & 1) != 0) fo.write(0);
            fo.close(); fo = null;
            if (wantMap) { BufferedOutputStream fm = null; try { fm = new BufferedOutputStream(new FileOutputStream(clipMap)); for (int c = 0; c < 2; c++) for (int i = 0; i < clipPos[c].size(); i++) { int v = clipPos[c].get(i); fm.write(v); fm.write(v >> 8); fm.write(v >> 16); fm.write(v >> 24); } }   // channel 0 first, as before
                catch (IOException x) { /* the C++ writes the map only if it can open it and does not check the writes */ } finally { if (fm != null) try { fm.close(); } catch (IOException x) { /* as above */ } } }
        } catch (IOException x) { throw EncIo.fail("write failed (disk full?): " + argv[2]); }
        finally { if (fo != null) try { fo.close(); } catch (IOException x) { /* failure path */ } }
        streamErr[0] = D.err.v;
        if (D.err.v) { new File(argv[2]).delete(); env.printf("corrupt stream (values out of range): refusing to decode\n"); return 3; }
        env.printf("LAE decode OK: %s -> %s | %s, %d Hz 24 bit, %ld samples/ch | mode %s (share by %s) %s, CD cut-off %d Hz | frequency segments used %d of %d | hard-limited samples %ld | format LAEY (extension LAE1)\n",
                   argv[1], argv[2], mode.equals("16") ? "16-bit route" : (mode.equals("cd24") ? "CD + depth" : "24-bit route"), D.wrate, D.Lo, PartitionFormat.MODES[D.mi].name, D.wm == 2 ? "cells+energy" : (D.wm != 0 ? "energy" : "cells"), D.eq != 0 ? "equal" : "unequal", D.fc, D.nused, D.nseg, D.clip);
        return 0; }
}
