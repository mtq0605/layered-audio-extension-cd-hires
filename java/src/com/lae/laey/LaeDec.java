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
 * A write failure removes the output and returns 1, as lae_dec.cpp (F05-1).
 */
final class LaeDec {
    private LaeDec() {}

    // port of lae_dec.cpp:main   usage: dec <in.wav> <out.wav> [full|16|cd24] [output rate]
    static int main(EncEnv env, String[] argvIn, boolean[] streamErr) {
        // port of lae_dec.cpp:main, pack=<n> (extension pack, 2026-11-10): taken out of the arguments; 0 = the main body, as always
        int packArg = 0; java.util.ArrayList<String> rest = new java.util.ArrayList<String>();
        for (int i = 0; i < argvIn.length; i++) { final String a = argvIn[i]; if (i < 3 || !a.startsWith("pack=")) { rest.add(a); continue; }
            final String v = a.substring(5); boolean okv = v.length() == 1 && v.charAt(0) >= '0' && v.charAt(0) <= '8';
            if (!okv) { env.printf("pack=%s: give the pack's number (0 = the main body)\n", v); return 1; }
            packArg = v.charAt(0) - '0'; }
        final String[] argv = rest.toArray(new String[0]);
        streamErr[0] = false;
        if (argv.length < 3) { env.printf("usage: lae_dec <in.wav | in.flac | in.lae> <out.wav> [full|16|cd24] [output rate]\n"); return 1; }
        String mode = argv.length > 3 ? argv[3] : "full";
        if (!MdctTb.selftestOK()) { env.printf("decoder self-test FAILED: this build does not follow the IEEE-754 order the format requires (see docs/BUILD.md); refusing to decode\n"); return 4; }
        if (LaeyFlac.isFlacFile(argv[1])) {   // FLAC form (2026-11-10): the CD layer in the FLAC frames, the extension in "LAE1" APPLICATION blocks
            int[] frate = {0}; short[][] fpcm = {null}; byte[][] flae = {null}; boolean[] notLaey = {true};
            final String fe = LaeyFlac.readFile(argv[1], frate, fpcm, flae, notLaey);
            if (!fe.isEmpty()) { env.printf("%s\n", fe); return notLaey[0] ? 1 : 3; }
            return decode(env, argv, streamErr, packArg, mode, CdPcm.of(fpcm[0]), flae[0], frate[0]); }
        WavIo.Wav w = new WavIo.Wav(); if (!WavIo.readCD(argv[1], w, false) || w.bits != 16 || w.ch != 2) { env.printf("not a 16-bit stereo WAV\n"); return 1; }
        // Java only: the CD layer is read on demand from the file through a bounded cache (the C++ reads it whole); every value is the same
        final RandomAccessFile raf; try { raf = new RandomAccessFile(argv[1], "r"); } catch (IOException x) { env.printf("not a 16-bit stereo WAV\n"); return 1; }
        try { CdPcm cdp = CdPcm.reader(new LaeyDecoder.ReadFn() { @Override public int read(long offset, byte[] buf, int off, int len) { try { raf.seek(offset); raf.readFully(buf, off, len); return 0; } catch (IOException x) { return -1; } } }, w.dataOff, w.dataValues, CD_CACHE);
              final byte[] lae = w.lae; w.lae = null; return decode(env, argv, streamErr, packArg, mode, cdp, lae, 0); } finally { try { raf.close(); } catch (IOException x) { /* read-only file */ } } }
    private static BufferedOutputStream closeQuietly(BufferedOutputStream f) { if (f != null) try { f.close(); } catch (IOException x) { /* failure path */ } return null; }
    /** the CD layer's cache in the encoder's decode checks (bytes) */
    static final long CD_CACHE = 4L << 20;
    // port of lae_dec.cpp:main, after the file is read (the CD layer from raf)
    private static int decode(EncEnv env, String[] argv, boolean[] streamErr, int packArg, String mode, CdPcm cdp, byte[] lae, int flacRate) {   // flacRate: the STREAMINFO rate of the FLAC form, 0 for WAV
        LaeDecoder.Decoder D = new LaeDecoder.Decoder(); int[] code = {0}; int orate = argv.length > 4 ? (Fmt.atoi(argv[4]) > 0 ? Fmt.atoi(argv[4]) : -1) : 0;
        D.pack = packArg;   // extension pack: 0 = the main body, as always
        String e = D.openCd(cdp, lae, mode, orate, code); if (!e.isEmpty()) { env.printf("%s\n", e); return code[0]; }
        if (flacRate != 0 && flacRate != D.base) { env.printf("FLAC STREAMINFO gives %d Hz, the extension's base rate is %d Hz: refusing to decode; the CD audio itself still plays\n", flacRate, D.base); return 3; }   // FU11-9: spec/lossy 3.4
        final String leakOut = env.get("LEAKOUT"), clipMap = env.get("CLIPMAP");
        if (!D.packs.isEmpty() && leakOut == null && clipMap == null) {   // the file's extension packs; only the caller chooses one (pack=<n>)
            env.printf("extension packs: %d (decoding pack %d)\n  pack 0: the main body%s%s%s\n", D.packs.size() + 1, D.pack, D.mainName.isEmpty() ? "" : ", \"", D.mainName, D.mainName.isEmpty() ? "" : "\"");
            for (int k = 0; k < D.packs.size(); k++) { final LaeDecoder.Decoder.PackInfo q = D.packs.get(k);
                env.printf("  pack %d: %d Hz%s%s%s\n", k + 1, q.rate, q.name.isEmpty() ? "" : ", \"", q.name, q.name.isEmpty() ? "" : "\""); } }
        if (leakOut != null && D.haveF) {   // leak compensation (encoder only): the decoded frequency layer brought down to the base rate (int32, 1/16 LSB24, L/R interleaved)
            BufferedOutputStream fl; try { fl = new BufferedOutputStream(new FileOutputStream(leakOut), 1 << 16); } catch (IOException x) { env.printf("cannot write %s\n", leakOut); return 1; }   // FU21-3: the reason is said (the C++ returns 1 silently)
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
        } catch (IOException x) { fo = closeQuietly(fo); new File(argv[2]).delete(); env.printf("cannot write %s (disk full?): the output was removed\n", argv[2]); return 1; }   // FU21-3: as lae_dec.cpp (F05-1)
        finally { closeQuietly(fo); }
        streamErr[0] = D.err.v;
        // FU21-3: a corrupt stream is refused before the clip map is written, so no map is left beside a removed output (C++: lae_dec.cpp:62-63 writes it first)
        if (D.err.v) { new File(argv[2]).delete(); env.printf("corrupt stream (values out of range): refusing to decode\n"); return 3; }
        if (wantMap) { BufferedOutputStream fm = null; try { fm = new BufferedOutputStream(new FileOutputStream(clipMap)); for (int c = 0; c < 2; c++) for (int i = 0; i < clipPos[c].size(); i++) { int v = clipPos[c].get(i); fm.write(v); fm.write(v >> 8); fm.write(v >> 16); fm.write(v >> 24); } }   // channel 0 first, as before
            catch (IOException x) { /* the C++ writes the map only if it can open it and does not check the writes */ } finally { if (fm != null) try { fm.close(); } catch (IOException x) { /* as above */ } } }
        env.printf("LAE decode OK: %s -> %s | %s, %d Hz %s, %ld samples/ch | mode %s (share by %s) %s, CD cut-off %d Hz | frequency segments used %d of %d | hard-limited samples %ld | format LAEY (extension LAE1)\n",
                   argv[1], argv[2], mode.equals("16") ? "16-bit route" : (mode.equals("cd24") ? "CD + depth" : "24-bit route"), D.wrate, mode.equals("16") ? "16 bit (in 24-bit words)" : "24 bit", D.Lo, PartitionFormat.MODES[D.mi].name, D.wm == 2 ? "cells+energy" : (D.wm != 0 ? "energy" : "cells"), D.eq != 0 ? "equal" : "unequal", D.fc, D.nused, D.nseg, D.clip);
        return 0; }
}
