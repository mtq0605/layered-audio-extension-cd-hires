# LAE: A Layered High-Resolution Extension of the Compact Disc

**English** | [中文](README.zh.md)

**Status:** this is the working paper and repository front page for container version 5. The format family is named **LAE** (**Layered Audio Extension**). This document describes the lossy tier, **LAE Lossy** (short name **LAEY**). For the lossless tier, **LAE Lossless** (short name **LAES**), a design document is published ([spec/lossless/DESIGN.md](spec/lossless/DESIGN.md)); its normative specification is not yet written.

This revision of the repository adds the following: alignment of an independent CD in the encoder; a whole-disc planner; the CD-Extra encapsulation tool `lae_cdx`; a decoder library with a C interface; and a foobar2000 component. The container remains version 5. The bitstream, the decoding process and the lossy specification are unchanged.

**Normative text:** the bitstream and the decoding process are specified in [spec/lossy/SPEC.md](spec/lossy/SPEC.md), which is currently a draft for public review. Where SPEC.md is silent or ambiguous, the reference decoder is normative. This document is informative.

The encapsulation of the extension on CD-Extra, in separate files and in streams is outside the v5 container. Its specification is to be published, as a draft, as [spec/cdx/SPEC.md](spec/cdx/SPEC.md).

**Further documents:**

- Building and verified builds: [docs/BUILD.md](docs/BUILD.md)
- Conformance vectors: [tests/vectors/MANIFEST.md](tests/vectors/MANIFEST.md)
- Measurement data: [data/](data/), described in [data/README.md](data/README.md)
- CD-Extra and file encapsulation (draft): [spec/cdx/SPEC.md](spec/cdx/SPEC.md)
- foobar2000 component: [plugins/foo_input_lae/README.md](plugins/foo_input_lae/README.md)
- Licences: [LICENSE](LICENSE). Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Patent pledge: [PATENTS.md](PATENTS.md). Commercial encoder licence: [COMMERCIAL.md](COMMERCIAL.md)

**Identifiers and files:**

- The executables are `lae_enc`, `lae_dec` and `lae_cdx`.
- The decoder library is `src/lae_decoder.h`. Its C interface is `liblae` (`src/lae_api.h`).
- The foobar2000 component is `foo_input_lae`.
- The RIFF chunk ID is `LAE ` (with a trailing space). The chunk begins with the magic `LAE1`.
- LAE files are distributed with the extension `.wav`, so that every player plays the CD layer.
- The extension file of the encapsulation specification has the extension `.lax` (`.LAX` on a CD). It ends with an 80-byte trailer that begins with the identifier `LTVT` (Section 10.6).
- `.lae` is the format's own file extension. The layout of a `.lae` file is not specified for container version 5.

---

## Abstract

LAE lets a single file carry a high-resolution master, up to 24 bit and up to 352.8/384 kHz, together with a 16-bit Compact Disc layer. Every existing WAV player reproduces the CD layer bit for bit.

- **44.1 kHz family.** CD-Extra encapsulation with the tool `lae_cdx` (at present it produces a CD-Extra disc image). The disc carries the CD layer unchanged in the audio session of the disc and the extension in the data session, so that every CD player reproduces the CD layer.
- **48 kHz family (96/192/384 kHz masters).** These masters have a 48 kHz base layer intended for DVD, Blu-ray and file distribution.
- **Files and streams.** Outside the CD, a track can also be distributed as a base-layer file (FLAC or WAV) with an extension file beside it (defined by the specification; `lae_cdx` reads WAV base-layer files so far).

The high-resolution information is stored entirely outside the PCM signal, in a separate extension. The extension is organised as a three-level pyramid:

- the **CD layer**, derived deterministically from the master or supplied independently;
- a **depth layer**, which refines the CD band from 16 bits to about 18 effective bits (by default) at the base sampling rate;
- a **frequency layer**, which codes everything above the cut-off of the CD as one MDCT stream per octave.

Decoding is purely additive in the time domain. Four decode routes are possible:

- CD only;
- CD plus frequency layer;
- CD plus depth layer;
- all three layers.

The extension is coded with three main tools:

- a shape-adaptive time-frequency partition;
- explicit precision tiers whose quantisation step is never coarser than one 16-bit LSB;
- an adaptive binary range coder.

The decoder is deterministic across platforms. The resampling filters use integer arithmetic, and the inverse MDCT is evaluated in a fixed order in strict IEEE-754 binary64 arithmetic. A start-up self-test checks this.

At the default operating point, the median CD-band error of the full 24-bit route is about −111 dBFS on real recordings. The optional 20-bit depth layer (`keep20`) lowers this to about −124 dBFS.

The extension size was measured during development (2026-10-16) on the v5 release candidate (Section 11.2). On real recordings in mode 3, its median size was about 0.24 to 1.23 times that of the uncompressed CD layer, depending on sampling rate and scheme.

The encoder can combine a master with an independently produced CD. It locates the CD against the master by waveform comparison, checks the waveform residual at three fixed places, and shifts the master, never the CD, to the CD's position with sub-sample precision: the whole part by placement, the part below one sample by band-limited interpolation. The waveform is unchanged: the error is below −140 dBFS, below the physical limits of reproduction, a precision that modern recordings rarely reach. Every position is confirmed by the user before anything is encoded. A whole-disc planner applies the same procedure to a publisher's complete disc. The result is an ordinary container-v5 file, which every v5 decoder decodes unchanged.

The reading side of `lae_cdx` restores the original LAE files from rips made with any drive offset. Each track is located on its own, and it is accepted only when the MD5 over all of its CD samples matches. A decoder library with the C interface `liblae` offers bit-exact fast seeking, and a foobar2000 component built on it plays LAE files up to the master rate.

The author has not filed, and will not file, any patent application on LAE, and makes a patent non-assertion pledge for implementations of the specification (Section 15).

---

## 1 Introduction

### 1.1 Problem

Recordings are increasingly mastered at 24 bit and at sampling rates of 88.2 to 384 kHz. The Compact Disc (16 bit, 44.1 kHz) nonetheless remains the format that plays everywhere: on legacy players, in cars, in every software player and as a physical product.

A publisher who wants to offer both has two options today:

- distribute two separate products;
- adopt a format whose high-resolution content is unavailable to legacy equipment.

Some existing approaches alter the PCM signal itself to carry additional data.

LAE addresses this problem under one constraint: **the CD layer is an ordinary CD, unaltered.** Everything else is additional information stored beside it.

### 1.2 Goals

1. **Bit-exact CD layer.** The 16-bit CD layer is stored exactly.
   - In the 44.1 kHz family, it is a 44.1 kHz layer that plays on any CD, WAV or FLAC player.
   - The 48 kHz family has a 48 kHz base layer (CD band 0–24 kHz), intended for DVD, Blu-ray and file distribution.
   - CD-Extra applies to the 44.1 kHz family only.

2. **Extension outside the PCM.** The extension restores up to 24 bit, and up to 352.8 kHz (44.1 kHz family) or 384 kHz (48 kHz family). The amount of extension follows the amount of genuine information in the master, so a master with more information produces a larger file.

3. **Reduce precision, not content (LAE Lossy).** LAE Lossy may represent content with reduced precision. It must not discard meaningful content.
   - Sustained tones, transients, musical tones, harmonics and reverberation tails are all retained.
   - Noise present in the master is treated as part of the music and coded like any other content. This includes the noise floor, inaudible random noise and deliberately added noise.
   - Only content that is certainly not information is zeroed automatically: digital silence, and rounding-level residue below one 24-bit LSB.

   A planned export interface will let the user mark noise regions for removal (Section 17.2).

4. **Both sampling-rate families.** One algorithm serves the 44.1 kHz and 48 kHz families. Only the octave rate tiers are fixed (88.2/176.4/352.8 kHz and 96/192/384 kHz); everything else is adaptive.

5. **Deterministic decoding.** All conformant decoders produce byte-identical output on every platform.

6. **Openness.** The format is published under open licences (Section 16). The author makes a patent non-assertion pledge (Section 15).

**Non-goals.**

- Perceptual coding of the audible band. The audible band is the CD layer plus a waveform-coded refinement.
- Disc-capacity planning by the encoder. The encoder compresses only. The publisher chooses scheme, mode and output range, and is responsible for whether the result fits a given medium. The packaging tool `lae_cdx` checks whether a disc image fits and stops if it does not; it never removes a track or an octave automatically (Section 10.9).

### 1.3 Principal features

- **Pyramid decomposition.** The signal is decomposed into CD, depth and frequency targets. These recombine by plain time-domain addition. The CD layer may be derived from the master or supplied independently.
- **Alignment of an independent CD.** A supplied CD is located against the master by two waveform searches, one whitened and one plain. The waveform residual is checked in three blocks, including a check for drift. The master is then shifted onto the CD's time axis with sub-sample precision (the whole part by placement, the part below one sample by band-limited interpolation; the waveform is unchanged, the error below −140 dBFS), and the CD's samples remain unchanged. Every position is confirmed by the user (Section 4.3).
- **Whole-disc planner.** `lae_enc plan` matches a master, given as one or more files, against a publisher's complete disc. It moves the master before cutting it at the CD's track markers (Section 4.5).
- **Per-recording frequency-layer start.** The frequency layer begins at the detected cut-off of the actual CD, rather than at a fixed fraction of the CD Nyquist frequency.
- **Encoder-only leakage compensation.** Quantisation noise of the frequency layer can leak into the CD band through the MDCT window. The depth layer cancels this leakage, and the decoder needs no change for it.
- **Precision rules with a 16-bit backstop.** Explicit precision tiers (B/C/D) are defined relative to a per-leaf exponent and governed by three precision rules. Under all three rules, no quantisation step is coarser than one 16-bit LSB.
- **Global rate–precision search.** A track-wide search chooses a shape-adaptive binary time-frequency partition. The search meets retention proportions for the highest-precision content at minimum total bytes.
- **Determinism and conformance.** The decoding process is deterministic under strict IEEE-754 arithmetic. A start-up self-test fingerprint and 16 conformance vectors check this.
- **CD-Extra encapsulation and reading.** `lae_cdx` writes a two-session CD-Extra image and reads rips of it back. Each track is located by content and accepted only when the MD5 over all of its CD samples matches. A closed-loop self-check runs before an image is delivered (Sections 10.5–10.9).
- **Decoder library and player component.** The decoder is available as a library with the C interface `liblae`. The library provides bit-exact fast seeking. The foobar2000 component `foo_input_lae` is built on the library (Sections 13.6 and 13.7).
- **Open licensing and a patent non-assertion pledge.** The specification, the reference decoder and the reference encoder are published under the licences of Section 16.

---

## 2 Related Work

**FLAC** [1] is a lossless audio format.

- The CD layer of LAE is an ordinary PCM signal, so it can be carried in an ordinary FLAC stream. The encapsulation specification carries only the CD layer (the base layer) in FLAC; the extension resides in a separate extension file beside it and never inside the FLAC file (Section 10.7).
- The whole-track MD5 of the encapsulation specification is defined in the same way as the audio MD5 of the FLAC STREAMINFO block for 16-bit stereo audio.
- Two aspects of the LAE encoder were inspired by FLAC:
  - the operating modes are numbered like compression levels;
  - the partition search chooses partitions by actual coded size, modelled on FLAC's power-of-two partitioning.

**MPEG-4 Audio BSAC and Scalable Lossless Coding (SLS)** [2] are earlier examples of layered audio coding.

**CD-Extra** [4] (the Blue Book, 1995) defines a multisession disc: an audio session (Red Book [3]) followed by a data session.

- LAE uses CD-Extra as its physical-disc container for the 44.1 kHz family (Section 10.5).
- The CD layer occupies the audio session unchanged, and the extension files reside in the data session, in an ISO 9660 [5] file system with Joliet extensions [15].
- Use of the "CD EXTRA" logo requires a licence.

**Time-delay estimation and fractional delay.** The alignment of an independent CD (Section 4.3) uses cross-correlation in two forms: plain, and whitened by the phase transform of the generalized cross-correlation [12]. The fractional move of the master uses band-limited windowed-sinc interpolation [13], a standard form of fractional-delay filter [14].

---

## 3 System Overview

### 3.1 Notation

The following symbols are used throughout.

| Symbol | Meaning |
|---|---|
| $R_0$ | Base sampling rate, 44.1 kHz or 48 kHz. $B = R_0/2$ is the base Nyquist frequency. |
| $R = R_0 \cdot 2^n$ | Master sampling rate, $n \in \lbrace 1, 2, 3 \rbrace$ (88.2/176.4/352.8 kHz or 96/192/384 kHz). |
| $x$ | Master signal at rate $R$, in 24-bit integer units. |
| $\mathcal{D}$, $\mathcal{U}$ | Decimator and interpolator: $n$ cascaded half-band stages using the same coefficient tables. |
| $b = \mathcal{D}(x)$ | 24-bit base signal at rate $R_0$. It is *not* clipped to full scale. |
| $c$ | 16-bit CD signal (CD16). |
| $d$ | Regenerated TPDF dither. It is non-zero only in blocks whose dither flag is set. |
| $v = 256 c - d$ | The CD path in 24-bit units. Any decoder can compute it from the CD layer and the dither flags alone. |
| $f_c$ | Detected cut-off frequency of the CD. |

The input master must be a stereo integer PCM WAV file with 16, 24 or 32 bits per sample.

Container v5 carries at most 24 bits. For 32-bit input, the encoder proceeds as follows:

- if the lowest 8 bits of every sample are zero, it encodes the upper 24 bits, and nothing is lost;
- otherwise it explains the problem and offers two choices: end the task without writing any file, or use the upper 24 bits, which the final report then states;
- in an interactive console the encoder asks. When it is called by a script, no one can answer, so it ends the task unless the option `bits=24` is given.

### 3.2 The pyramid

The encoder forms two targets:

$$T = \mathrm{LP}_{f_c}\left(b - v\right) \qquad \text{(depth target, base rate)}$$

$$F = x - \mathcal{U}\left(v + T\right) \qquad \text{(frequency target, master rate)}$$

$T$ and $F$ exist only in the encoder.

- The depth layer carries a coded approximation $\hat T$ of $T$. In v5, $\hat T$ also includes the leakage compensation term of Section 6.5.
- The frequency layer carries a coded approximation $\hat F$ of $F$.

By construction, $x = \mathcal{U}(v + T) + F$: the three parts add back to the master in the time domain.

```
                                master x  (rate R = R0 * 2^n, 24 bit)
                                   |
                     +-------------+----------------------------+
                     |                                          |
            half-band decimation D (n stages)                   |
                     |                                          |
            24-bit base b (rate R0, unclipped)                  |
                     |                                          |
     +---------------+------------------+                       |
     |                                  |                       |
  TPDF dither (flagged blocks only),    |                       |
  round, clip to 16 bit                 |                       |
     |                                  |                       |
  CD16 c ------> v = 256c - d           |                       |
     |               |                  |                       |
     |               +----> T = LP_fc(b - v) ---> depth layer   |
     |               |                  (L/R, base rate, MDCT)  |
     |               |                                          |
     |               +--> U(v + T) -----------> F = x - U(v+T) -+
     |                                               |
     |                                       frequency layer
     |                               (M/S, master rate, MDCT,
     |                                one stream per octave)
     v
  WAV "data" chunk (playable everywhere)    "LAE " chunk (extension)
```

### 3.3 Layers

| Layer | Content | Band | Rate | Precision (loud blocks) | Played by |
|---|---|---|---|---|---|
| CD | 16-bit PCM in the WAV `data` chunk | 0–22.05 kHz (48 kHz family: 0–24 kHz) | 44.1 / 48 kHz | 16 bit; TPDF dither only where needed, never on digital silence | Any player |
| Depth | Residual of the 24-bit base relative to the CD path; one stream per channel (L, R) | 0–22.05 kHz (48 kHz family: 0–24 kHz) | 44.1 / 48 kHz | Default B18/C20/D24; keep20 B20/C22/D24 | LAE decoder |
| Frequency | All content above the CD cut-off; M/S channels; one stream per octave segment per channel | From 1 kHz below $f_c$ to the master Nyquist frequency | Master rate | Scheme 1 B18/C20/D24; scheme 2 B16/C20/D24 | LAE decoder |

The depth layer and the frequency layer are independent. Together they form the complete high-resolution signal. The resolution of the output depends on which of them is decoded.

### 3.4 Decode routes

| Route (`lae_dec` option) | Layers used | Output | Reconstruction |
|---|---|---|---|
| CD (any player) | CD | 44.1/48 kHz, 16 bit | $c$ |
| `16` | CD + frequency | Master rate or a lower octave; CD band at 16-bit precision | $\mathcal{U}(v) + \hat F$ |
| `cd24` | CD + depth | 44.1/48 kHz, 24 bit | $v + \hat T$ |
| `full` (default) | CD + depth + frequency | Master rate or a lower octave, 24 bit | $\mathcal{U}(v + \hat T) + \hat F$ |

```
           CD (16 bit, R0) ............................ any player
            |
            +--- + depth layer ----------------------> cd24 (24 bit, R0)
            |
            +--- + frequency layer ------------------> 16-bit route (R0*2^k)
            |
            +--- + depth layer + frequency layer ----> full (24 bit, R0*2^k)
```

Output resolution is chosen along two independent axes:

- output octave: 88.2/176.4/352.8 kHz or 96/192/384 kHz;
- depth: 16 or 24 bit.

All three decoder routes write 24-bit PCM files. The 16-bit route differs in precision, not in file format ([SPEC.md](spec/lossy/SPEC.md), Section 11.8).

The same four routes are offered by the decoder library `liblae` (Section 13.6) and by the foobar2000 component (Section 13.7).

The operating mode is fixed at encoding time; the decoder does not offer a choice of mode at playback.

When a lower output octave is selected, the decoder:

- neither reads nor decodes the frequency-layer streams above that octave;
- reduces the result to the selected rate with the same half-band filters that derive the CD layer.

Samples outside the 24-bit range in the final output are hard-limited (Section 12.4).

---

## 4 CD Layer

### 4.1 Derived CD

By default the encoder derives the CD layer deterministically from the master, in four steps.

1. **Decimation.** $n$ cascaded half-band decimate-by-two stages reduce the master from $R$ to $R_0$.
   - The filters are integer FIR filters with fixed coefficient tables, stored in the reference source (`src/halfband.h`).
   - The filters are symmetric, odd-length (zero-phase) half-band filters with coefficients in Q31. Only the odd taps are stored.
   - The centre tap is $2^{30}$, and the stored taps of each table sum to exactly $2^{29}$, so the DC gain is exactly 1.
   - Accumulation is in 64-bit integers, with one rounding per output sample ([SPEC.md](spec/lossy/SPEC.md), Section 10).
   - Decimated output sample $m$ is centred on input sample $2m$. Input indices outside the signal are mirrored about the end samples ([SPEC.md](spec/lossy/SPEC.md), Sections 10.2 and 10.4).
   - The interpolator $\mathcal{U}$ uses the same coefficient tables.
   - Measured during development, the pass-band deviation of the half-band chain is below −120 dB from 0 to 19.8 kHz.

2. **No intermediate clipping.** No stage clips the 24-bit base $b$ to full scale. Band-limited overshoot above full scale, such as that produced by square waves and strong transients, remains in $b$ and is carried by the depth target $T$.

3. **Adaptive TPDF dither.** Dither is added only in blocks where the recording's own noise is below 0.5 LSB16.
   - The dither is TPDF and is generated by two xorshift32 generators, one per channel. The left generator is seeded with `0x12345678` and the right with `0xB06BD031`. The seeds are format constants and are not transmitted.
   - Each generator advances by two draws for every base-rate sample, including samples in undithered blocks.
   - The top 12 bits of the two draws, $u_1$ and $u_2$, give the dither value $u_1 + u_2 - 4096$ in units of 1/16 LSB24, about ±1 LSB16 ([SPEC.md](spec/lossy/SPEC.md), Section 11.2).
   - No noise shaping is applied.
   - Blocks that are entirely zero (digital silence) never receive dither, so digital silence in the master remains exact zero in the CD layer.

4. **Rounding and clipping.** The dithered base is rounded to 16 bit and clipped to the 16-bit range. This is the only point at which the CD path is clipped.

### 4.2 Dither flags and regenerated dither

The container stores one flag byte per channel for every 4096 base-rate samples. A decoder proceeds as follows:

1. it regenerates the dither sequence from the fixed seeds;
2. it applies the dither in the flagged blocks;
3. it forms $v = 256 c - d$.

Every decoder therefore knows the CD path exactly from the CD layer and the flags alone. No dither values are transmitted.

### 4.3 Independent CD

With the encoder option `cd=<file>`, an existing CD master becomes the CD layer bit for bit.

- **Format.** The CD must be supplied as a 16-bit stereo WAV at the base rate of the master's family: 44.1 kHz for the 44.1 kHz family, 48 kHz for the 48 kHz family.
- **Coding.** The extension codes the master, as placed on the time axis of the CD, minus the CD path. The pyramid of Section 3.2 is unchanged; only $c$ is no longer derived by the encoder.
- **Dither.** The encoder adds no dither to a supplied CD, so all dither flags are 0 in this mode.
- **Level and equalisation.** The encoder neither equalises the CD nor compensates its gain.
- **Size.** The closer the CD is to the master, the smaller the extension. A CD that was separately limited, equalised or level-adjusted produces a markedly larger extension.
- **Cut-off detection.** In this mode, the cut-off is detected from the coherence between master and CD, which makes the detection insensitive to level differences (Section 6.3).

The publisher decides which CD and which master are combined.

A 48 kHz family master can be combined with a 44.1 kHz CD only after resampling. This is a mastering task: the encoder performs no rational-ratio resampling.

**Alignment: purpose and principle.** A publisher's CD is rarely sample-aligned with the master. It may start at a different time, it may be offset by a fraction of a master sample, and it may have more or less digital silence at either end. Any offset that remains appears in the extension as a difference between master and CD, and it changes the size of the extension. When `cd=` is given, the encoder therefore performs the following steps:

1. it locates the CD in the master;
2. it checks the waveform residual at three fixed places;
3. it measures the remaining fraction of a master sample;
4. it places the **master** on the time axis of the CD and moves it by that fraction.

The CD's samples are never changed, and the CD layer remains bit-exact. Because the master is moved before the layers are derived, the result is an ordinary container-v5 file. Nothing about the alignment is stored in the file, and every v5 decoder decodes it unchanged; the decoded signal is the moved master.

**Search.** The search compares waveforms only. It follows a fixed coarse-to-fine cascade.

1. **Level 0.** The CD's sound is split into two halves at 1/16 of the CD rate. The whole allowed range is scanned by cross-correlation, and the range around the peak is scanned once more.
2. **Further levels.** Each further level halves the blocks and doubles the rate. It searches a fixed small range around the previous peak: ±32 level samples at the first such level, ±8 at the others.
3. **Last level.** The last level compares, at the master rate, the whole master samples within two CD samples of the previous peak, by waveform residual.

A peak on the edge of a level's range ends the search with "alignment failed". Such a peak indicates a CD of another version, an edited CD or a different speed.

**Two searches.** The search runs twice: once with a whitened cross-correlation (generalized cross-correlation with phase transform, GCC-PHAT [12]), and once with a plain, unweighted, energy-normalised cross-correlation. Whitening is not always the better choice. On a 2 s excerpt of a slow 79–182 Hz sweep, for example, only the plain correlation found the position: the whitened search failed, and the plain search reported +0.3000226 master samples against a true offset of +0.3.

**Check.** The waveform residual is checked on three blocks of the CD's sound, all with the same offset.

- **Sound of 40 s or more.** Three blocks of 5 s: the first starts 10 s after the first sound, the last ends 10 s before the last sound, and the third is centred.
- **Sound of 3 to 40 s.** The middle of each third, at most 5 s.
- **Sound under 3 s.** One check over the whole sound.

For each block, the encoder computes two quantities:

- the waveform residual after one least-squares gain per channel;
- the remaining time difference against the piece's single fractional best fit.

A block whose residual is above −3 dB does not match.

**Drift check.** Each block is also compared at two CD samples on either side of the offset, with the gains kept. A better fit at either place indicates a different speed, an edit or another take, and the encoder stops. This comparison only stops the encoder; it never moves anything.

The thresholds are defined in `src/cd_align.cpp` (structure `Th`).

**Bands.** The level of the remaining time difference determines the outcome.

| Remaining time difference | Outcome |
|---|---|
| ≤ −40 dB | Aligned |
| −40 to −33 dB | Confirmation required |
| above −33 dB | Confirmation required, with a warning that the file may become much larger |
| No fit (a check block whose residual is above −3 dB re the CD; drift, a better fit two CD samples to either side; or no position found) | Never encoded; cannot be overridden |

The confirmation is handled as follows:

- in an interactive console the encoder asks whether to end the task or continue;
- when it is called by a script, it stops unless the option `cdaccept` is given;
- after a confirmation, the last line of the output states `CONTINUED AFTER CONFIRMATION`.

A channel swap or inverted polarity needs the user's confirmation, as in the −40 … −33 dB band (`cdaccept`); the extension then codes the difference, and it becomes much larger.

**User confirmation of the position.** Every position is confirmed by the user. Without the option `cdalign=`, the encoder proceeds as follows:

1. it performs both searches;
2. for each result, it measures the fraction, tries the placement including the move, and checks again exactly as for an offset given by hand;
3. it prints both results, each as "aligned at `cdalign=<offset>`", "not aligned within the limit at …" or "alignment failed …";
4. it prints the line `ALIGN_SUGGEST whitened=<status>:<offset> plain=<status>:<offset>`;
5. it stops without writing anything, with exit code 1 (exit code 0 with `cdcheck`).

The encoder does not compare the two results and does not choose between them; the user does, and then runs the encoder again with `cdalign=<offset>`. Confirmation by the user is required because strictly periodic signals can be matched one whole period away (Section 17.2).

**Offset given by hand.** With `cdalign=<offset>`, the search is skipped.

- The offset is in master samples; a positive value means that the CD starts later. It may be a whole number, or carry up to 9 decimals, for example `cdalign=3.4999828`. A whole number means a fraction of 0.
- The encoder checks at the nearest whole position, places and moves the master by the given value, and checks again.
- If a better whole position lies nearby, the confirmation prompt reports the remaining shift and suggests a corrected value.
- An offset printed by the encoder and given back yields the same file; this was tested byte for byte.

**Measurement of the fraction.** After the whole offset $t$ has been found, the encoder measures the fraction once per piece, on the same check blocks.

- The descent is fixed: steps of 1/8, 1/16, … of a master sample, three points per step, down to one unit of $2^{-22}$ CD sample.
- The measured fraction is at most half a master sample.

**Placement.** The master is placed on the time axis of the CD as follows.

- **Whole samples.** The master is moved by whole master samples, and only across digital silence (all-zero samples).
- **Length matching.** Whichever side is longer at the start plays first, and whichever side is longer at the end ends later. The shorter side receives digital silence. Neither side loses a non-silent sample.
- **Sub-sample lead.** If the master's sound starts less than one CD sample before the CD's, one CD frame of digital silence is put in front of the CD layer. The CD's own samples are unchanged.
- **Unmatched sound.** Sound that only one side has, of one CD sample or more, is not supported, and the encoder stops with the time range.

**The move of the master.** After placement by the whole offset $t$, the encoder moves the master once by `move` master samples, $|\mathrm{move}| \le 0.5$.

- Each output sample $i$ is the band-limited value of the placed master at $i + \mathrm{move}$. It is computed with a 128-tap Kaiser-windowed sinc interpolator ($\beta = 10$) [13][14], whose taps are computed once from `move` by a closed formula.
- Samples outside the master count as digital silence.
- The result is rounded to 24 bit. Samples beyond full scale are hard-clipped, with a warning and a count.

The total offset is $t - \mathrm{move}$. The layers are then derived as usual and checked again at offset 0, with the same bands. The encoder prints a line `PHASE t=<whole> phi=0 move=<move> offset=<offset>`; the field `phi` is always 0.

**Digital silence around the move.** A fractional move carries a few dozen samples past the first and the last sample of the master. These samples are kept as follows:

1. before the move, 128 CD frames of digital silence are added at both ends of the placed master and of the CD layer;
2. after the move, at each end only what is still digital silence is removed again, at most the added frames and never a frame of the given CD.

The effect depends on the ends of the master:

- a master that starts and ends in digital silence gives a file byte-identical to one encoded without this step;
- a master cut in the middle of the sound keeps a few frames of digital silence at the ends of the CD layer: 8 at 352.8 kHz, up to 32 at 88.2 kHz.

Nothing about this step is stored, and decoders never discard a sample. When such frames are kept, the cut-off detection can change slightly. In two development excerpts, the extension changed by +1.5 % and −0.4 %, and the decoded quality was unchanged.

**Movement report.** After a successful encode, the encoder writes `<output>.align.txt` beside the output file. It is plain text for the user's records and contains:

- the whole offset;
- the fraction moved, in master samples and in microseconds;
- the total offset;
- which side starts earlier;
- the digital silence added and removed;
- the result of the check after the move.

No program reads the report. It is not part of the LAE file or of a disc.

**Table 4.3 — Alignment measurements during development (10-second excerpts unless noted).** Tracks A and B are two tracks of one commercial recording; the dual-search cases use two further test recordings.

| Test | Result |
|---|---|
| Fraction measurement: CD derived from the master delayed by 0.1, 0.25 and 0.5 master samples (Kaiser-windowed sinc), at 352.8, 176.4 and 88.2 kHz, tracks A and B | Error within ±2.3 × 10⁻⁵ master samples (≤ 108 ps); remaining time difference after alignment −99 dB (the printed floor) in all 18 cases |
| The move alone, track A, +0.3 and then −0.3 master samples | 0–20 kHz error −140 dBFS; ultrasonic error −147 to −166 dBFS. Without the added digital silence, up to −38.5 dBFS over a few dozen samples at hard-cut edges (sound at the first or last master sample); with it, −66 dBFS at such edges. Masters that start and end in digital silence are not affected. |
| Track A, CD derived from the master delayed by 3.5 master samples | Measured offset +3.4999828; re-check residual −73.1 dB; extension 1,671,417 B, against 1,807,763 B with the whole offset only (−7.5 %); decoded 0–20 kHz error −112 dBFS against the independently placed and moved master (whole-offset encode: −113 dBFS) |
| Track B, CD derived from the master delayed by 0.5 master samples | Measured offset +0.4999905; re-check residual −77.3 dB; extension 2,638,186 B, against 3,169,780 B with the whole offset only (−16.8 %); decoded 0–20 kHz error −111 dBFS, the codec's normal level with the 18-bit depth layer. |
| Tracks A and B: output of an unmodified v5 decoder | Identical to the output of the new decoder |
| Dual search with user confirmation: 9 cases at 352.8, 176.4 and 88.2 kHz | Nothing written without `cdalign=`; both results reported; with the reported offset, files byte-identical to those of the previous encoder |
| Single-piece alignment suite: placement, stops, drift, offsets given by hand, bands, fraction | 106 of 106 (track A) and 105 of 105 (track B), with a development build that preceded the dual search; the path for an offset given by hand is unchanged since (byte-identical files) |
| Conformance vectors | The released encoder writes all 16 v5 vectors byte for byte; its decoder gives all 64 outputs identically |

Only CDs derived from the master were tested, with equalisation, limiting, gain changes, delay, noise reduction, gating, edits and drift applied. Tests with real independent CD and master pairs from the market are open (Section 17.3). The known limits of the search are listed in Section 17.2.

### 4.4 Bit-exactness

The CD layer is the audio content of the output WAV file. It is therefore byte-identical to the derived CD. For an independent CD, it contains the samples of the supplied CD unchanged; in the cases of Section 4.3 it may additionally carry frames of digital silence at its ends.

Before writing any file, the encoder decodes every layer and every segment again and verifies that all coefficient planes match. After encoding, it also decodes the final file completely, and it writes no file that cannot be decoded.

The decoder refuses a file whose CD audio length does not match the extension chunk (Section 10.4). An alteration of the CD audio that leaves its length unchanged is not detected ([SPEC.md](spec/lossy/SPEC.md), Section 3.2). Outside the WAV file, the encapsulation specification protects the CD audio with a whole-track MD5 (Section 10.6).

### 4.5 Whole-disc planner

The planner `lae_enc plan` combines a master with a publisher's complete disc. It reads the publisher's disc as a CUE sheet with BIN or WAV audio. The master may be given as one file or as several files.

**Order of operations.** The planner proceeds in a fixed order:

1. **Join.** Several master files are joined sample by sample in the given order. Their own split points are not used; when they differ from the CD's, the planner prints "the master is re-split at the CD's positions".
2. **Check silence and coverage.** Silence and coverage are checked on the master as it is, before any move.
3. **Align.** Each piece, that is each track of the CD, is searched in the master around its expected place, within ± max(60 s, piece length). A piece that is digital silence on the CD is not searched, and its master range must be digital silence. Between pieces, master and CD may differ only in digital silence.
4. **Confirm.** The user confirms every position.
5. **Move.** Each group (below) is moved within the joined master.
6. **Cut.** Only then is the master cut at the CD's track markers.

**Groups.** Pieces whose master ranges touch at a track marker of the CD form a group: the CD splits continuous master material there. A group has one whole offset and one fraction, measured as one piece; other pieces form groups of their own. Each group is moved within the joined master with its real neighbouring samples and is cut only afterwards. Pieces that join without a gap are therefore joined in the output exactly as the whole group moved once; this was tested bit for bit.

**User confirmation.** Without `cdalign=`, the planner performs both searches, each through the whole plan. It prints the positions found and a line `PLAN_SUGGEST whitened=<status>:<positions> plain=<status>:<positions>`, writes nothing and exits with code 1.

With `cdalign=<p1>,<p2>,…`, one value is given per piece:

- the value is the master sample at which the piece's CD frame 0 lies, and a fraction is allowed;
- `-` is given for a piece that is digital silence on the CD;
- values given by hand always take precedence;
- different fractions given for pieces of one group stop the planner.

After cutting, each piece is checked again by the encoder's rules. Pieces that fall into the confirmation band are listed, and the planner writes files only after confirmation, either in the console or with `cdaccept`. These pieces must then also be encoded with `cdaccept`.

**Samples without a piece.** A move can carry samples to where no piece takes master samples: the very start or end of the master, or a gap that only the master has. The planner reports where this happens and how many samples are concerned. It also states that adding at least 1 ms (44 CD frames) of digital silence at that place to both the master and the CD, with the later track markers moving with it, keeps these samples. The user decides.

If the master's sound starts less than one CD sample before track 1, one frame of digital silence is put in front of the CD layer of track 1. This frame falls in the pregap, and session 1 of the disc is unchanged.

**Stops.** The planner stops and reports the time range in any of the following cases:

- the master has sound that the CD lacks;
- two pieces have sound in the same master range;
- the order of the pieces differs;
- the CD carries the pre-emphasis flag (FLAGS PRE);
- track 1 has sound in INDEX 00;
- any stop condition of the single-piece alignment (Section 4.3) occurs.

**Output.** For each piece, the planner writes:

- `TRACKnn.master.wav`: the moved master, 24 bit;
- `TRACKnn.cd.wav`: the publisher's samples unchanged, from the piece's INDEX 01 to the next INDEX 01.

The move is reported in `plan.align.txt`, a plain-text file that no program reads.

**Chain.** A complete disc is produced in three steps:

1. `lae_enc plan` matches and cuts;
2. `lae_enc` encodes each piece with `cdalign=0`;
3. `lae_cdx image … layout=<publisher.cue>` writes the CD-Extra image, whose session 1 is the publisher's programme sample for sample (Section 10.5).

The commands are given in Section 14.5.

**Measured.** The planner was tested on whole-disc cases in which the publisher's CD was derived from the master with noise reduction, gating and minimum-phase equalisation.

- With the current build (positions confirmed, the master moved before cutting): a disc of three tracks that the CD splits from continuous material, made through a publisher's chain with noise reduction, gating and minimum-phase EQ, passed 13 of 13 checks; its pieces, encoded with `cdalign=0`, have a remaining time difference of −67.6, −69.0 and −67.5 dB. Two pieces split at a CD track marker and joined again are bit-identical to their group moved once; two groups separated by digital silence each get their own fraction (remaining time difference −99 dB).
- With the released build, one case was repeated with the positions printed by the planner and passed 11 of 11 checks.

---

## 5 Depth Layer

### 5.1 Content

The depth layer codes $T$ at the base rate. $T$ is the low-passed difference between the unclipped 24-bit base and the CD path. The layer has one stream for the left channel and one for the right. In v5, the coded target is $T$ minus the leakage compensation of Section 6.5.

The depth layer always covers the whole base band, 0–22.05 kHz (0–24 kHz in the 48 kHz family), even though $T$ is low-passed at $f_c$. Cells above the cut-off are mostly class A and cost almost nothing.

### 5.2 Purpose and principle

The CD band carries most of the musical content, and the depth layer is the most cost-effective way to improve it.

Its precision is governed by precision rule 1 by default, and by rule 3 in keep20 (Section 8):

- in quiet blocks, coefficients are represented to 2, 4 or 8 bits below the block peak, which preserves waveform shape;
- in loud blocks, the step reaches 18/20/24-bit resolution under rule 1, or 20/22/24-bit resolution under rule 3.

### 5.3 Overshoot

The base $b$ is unclipped, but the CD path is clipped. Any band-limited overshoot above full scale therefore appears in $T$.

The 24-bit routes can thus restore content that the CD must clip; only their final output is hard-limited. The 16-bit route contains no $T$ and cannot restore such content (Section 12.3).

### 5.4 The keep20 depth layer

Mode `keep20` codes the depth layer with precision rule 3 (B20/C22/D24 in loud blocks). When keep20 is selected, the encoder prints a notice of its effect and cost. The measured values are:

- **CD-band error.** The error improves from about −111 to about −124 dBFS (Table 12.2).
- **Size.** The depth layer grows by about 84 MB per hour of music in the 44.1 kHz family and about 94 MB in the 48 kHz family.
- **Disc time.** On a CD-Extra disc, the extra size corresponds to about 9 disc minutes per hour of music.
- **Playing time.** In the measurements of Table 11.5, the median 24-bit-route playing time of an 80-minute disc fell by 2.0 to 4.8 minutes.

---

## 6 Frequency Layer

### 6.1 Transform and octave streams

The frequency target $F$ is transformed at the master rate by a single MDCT over the whole signal (Section 7.1).

The spectral lines are divided into segments, one per octave above the base band. For a 352.8 kHz master, for example, the segments are nominally 22.05–44.1, 44.1–88.2 and 88.2–176.4 kHz. Each segment is stored as an independent stream.

In this organisation, each octave is a segment of spectral lines. It needs no band-splitting filters, and no part of it enters the PCM.

A decoder that outputs a lower octave skips the streams above it.

### 6.2 Mid/side

The frequency layer is coded in mid/side form, with separate M and S streams for each segment. The depth layer is coded in left/right form.

M/S is formed by integer lifting. Here $\lfloor\cdot\rfloor$ is realised as an arithmetic right shift. The encoder computes

$$S = L - R, \qquad M = R + \lfloor S/2 \rfloor ;$$

and the decoder applies the inverse ([SPEC.md](spec/lossy/SPEC.md), Section 11.4):

$$R = M - \lfloor S/2 \rfloor, \qquad L = R + S .$$

The inverse restores $L$ and $R$ exactly from $M$ and $S$.

### 6.3 Cut-off detection and start line

The bandwidth of a CD varies from one production to another. The frequency layer therefore starts just below the actual cut-off of the CD in use, not at a fixed fraction of the base Nyquist frequency.

- **Derived CD.** The encoder searches downward from the base Nyquist frequency for the highest frequency at which the CD still follows the master to within 0.1 dB. Frequency bins in which the master has almost no energy are skipped.
- **Independent CD.** The cut-off is detected from the coherence between master and CD, independently of level. A CD that is offset from the master by a fraction of a sample lowers the detected cut-off; the alignment of Section 4.3 removes this effect (Table 4.3).

The frequency layer begins **1 kHz below the detected cut-off**.

- Its start and end spectral lines are written into each stream header.
- The detected cut-off frequency is also stored in the container header, for display only.

### 6.4 Precision

| Scheme | Frequency-layer rule | Loud blocks | Quiet blocks |
|---|---|---|---|
| Scheme 1 (high quality, `s1`, default) | Rule 1 | B18 / C20 / D24 | 2/4/8 bits below block peak |
| Scheme 2 (high compression, `s2`) | Rule 2 | B16 / C20 / D24 | as rule 1 |

Both schemes use the same depth-layer rule: rule 1, or rule 3 in keep20. Their CD band is therefore the same, and they differ only in the ultrasonic region.

### 6.5 Leakage compensation (encoder only)

Quantisation noise in the frequency layer leaks into the CD band through the side lobes of the sine window. The effect grows with the strength of the ultrasonic content: it is pronounced on synthetic material and affects only a few real recordings.

Measurements during development attribute this CD-band error to the frequency layer:

- decoding without the frequency layer returned the CD-band error to −111 dBFS;
- the error spectrum rose with frequency, from −119 dBFS at low frequencies to −100 dBFS at 18 kHz;
- the pass-band deviation of the half-band filters is below −120 dB.

The v5 encoder applies the following procedure:

1. After the frequency layer is final, decode it and form its quantisation error.
2. Reduce that error to the base rate.
3. Extract the part below the frequency-layer start with a 2047-tap low-pass filter whose cut-off lies 150 Hz below the start frequency.
4. Subtract this part from the depth target, then code the depth layer.

Content above the start frequency is coded by the frequency layer itself and is not compensated a second time. The procedure changes neither the decoder nor the format.

The effect of the compensation depends on the route:

- **`full`.** Both layers are decoded, and the compensation is effective.
- **`16`.** The route has no depth layer, so the compensation cannot act.
- **`cd24`.** The depth layer, and with it the compensation term, is decoded without the frequency layer whose leakage the term cancels. The effect of the term on the `cd24` output has not been measured; this measurement is future work (Section 17.3).

Measured during development:

- on the synthetic `electronic` excerpt, the CD-band error improved from −96 to −109 dBFS;
- real recordings were unchanged;
- extension size grew by 0–1 %.

Section 12.5 reports the effect averaged over 12 sources.

---

## 7 Time–Frequency Partition and Classification

### 7.1 Transform

Both coded layers use the same MDCT structure, implemented in textbook order.

| Direction | Steps |
|---|---|
| Forward | window → fold (add) → DCT-IV, computed through a complex FFT with pre- and post-twiddles |
| Inverse | DCT-IV → unfold → window → overlap-add |

**Window.** The window is the sine window.

**Size.** Every stream header carries the transform size $N$. In container v5, $N$ must be 512.

**Twiddles.** The pre- and post-twiddles are

$$w_n = \exp\left(-i \frac{2\pi (n + 1/8)}{2N}\right), \qquad n = 0, \dots, N/2 - 1.$$

This is the twiddle form used in FFmpeg `libavcodec/mdct.c` [6]. The bit-reversal permutation of the FFT uses a generated table, `BITREV`.

**Boundary extension.** The signal is mirror-extended at both ends. The extension length is recorded in the header (Section 10).

### 7.2 Cells, root blocks and leaves

The geometry that a decoder needs is defined in `src/partition_format.h`. The encoder's search is in `src/partition.h`.

**Cells.** The coefficient plane of one channel of one stream is divided into cells. A cell is a group of adjacent spectral lines in one frame. The stream header signals the cell width; in container v5 it is always 8 lines.

**Root blocks.** Cells are organised in root blocks of 16 cells × 8 frames. Root blocks form a fixed regular grid and are processed in time-major order. The root-block geometry is the same in every mode.

**Partition tree.** Each root block is partitioned by a binary tree.

- A node is either a leaf, or it is split into two children by one cut, in frequency or in time.
- The minimum block size is 1 cell × 1 frame.

**Cut positions.** Two cut-position schemes exist. The encoder flag `equal` selects between them, and the header records the choice.

- **Equal cuts:** halving only; no position is transmitted.
- **Unequal cuts (default):** the cut position may fall anywhere on the minimum grid. It is transmitted as a bypass-coded integer of fixed length, in grid units.

**Leaf parameters.** All coding parameters are carried at the leaves: class, exponent, zero-level index and coefficients. Intermediate nodes carry only split information.

### 7.3 Classes

Each leaf is assigned one of four classes.

| Class | Meaning | Coded |
|---|---|---|
| A | Certainly not information: digital silence, and rounding-level residue below one 24-bit LSB. Planned: noise explicitly removed by the user. | Nothing; reconstructed as exact zero |
| B | Content requiring the lowest precision tier | Exponent + coefficients at TB = 2 |
| C | Intermediate | TB = 4 |
| D | Content requiring the highest depth | TB = 8 |

The following rules govern class assignment.

- **Noise is coded.** Noise in the master is not class A. It is coded like other content, mostly at class B and sometimes at class C.
- **A is determined first.** Class A is fixed before the first compression pass and does not change afterwards.
- **Mixed blocks.** A block can be class A only if all of its cells are class A. Any block containing retained content is at least class B. Mixing A cells into a B leaf costs B bytes, so the partition search separates A from retained content of its own accord.
- **Spanning cells.** A cell that spans two analysis groups is class A only if both groups are A. In that case, the lower noise floor of the two is used.
- **Noise-floor estimate.** The noise floor of a cell group is the smaller of two values: the 10th percentile over time, and the spectral median across the cells of the group. Sustained tones are therefore not mistaken for a noise floor.

### 7.4 Labelling and closed loop (encoder)

The encoder first scans the whole track and labels every non-A cell with the depth it requires:

- the cells that need the most depth are labelled D;
- the next are labelled C;
- the remaining non-A cells are labelled B.

Two detectors, a sustained-tone detector and a transient detector, ensure that detected structure is labelled at least B.

The labels are then refined in a closed loop:

1. the encoder codes with every cell at its own label;
2. it decodes the result;
3. it runs the detectors on the error;
4. it raises only the cells that are still damaged at their own tier.

The sustained-tone detector (`src/tone_scan.cpp`) has no sensitivity setting. Its thresholds follow from the noise statistics of averaged periodograms:

- **Periodograms.** They come from a Hann-windowed long FFT with 50 % overlap. The FFT length is the smallest power of two that is not shorter than 0.09 s.
- **Time scales.** The periodograms are averaged over 1, 2, 4, 8 and 16 long-FFT frames, so that structures are tested at five time scales.
- **Thresholds.** The threshold at each scale is a Gamma-distribution quantile at a fixed false-alarm rate of about $10^{-9}$ per test.
- **Noise floor.** The local noise floor is taken from medians of neighbouring 32-bin chunks. In closed-loop error checks, it can be taken from the periodograms of a reference signal.
- **Detections.** A detection requires at least two consecutive windows at its scale, allowing a drift of ±1 bin. Detections within 3 bins of each other that overlap in time are merged.

A description of the statistics of the transient detector is future work (Section 17.2).

Structure-preservation results for v5 have not been published. Re-validation on v5 with injected ground truth (tones and clicks) is future work (Section 17.2).

### 7.5 Mode-driven global selection (encoder)

An operating mode does not prescribe a block size. Instead, it prescribes the proportion of D-labelled and C-labelled content that must remain at its own tier, measured over the whole track. The remaining C and D content may be demoted.

For each mode, the encoder chooses the partition and leaf classes over the whole track that minimise total coded bytes subject to these retention proportions.

- **Demotion cost.** The cost of demotion is expressed by a Lagrange multiplier λ. It is found by bisection, first for C and then for D, in one pass.
- **Search within a root block.** A dynamic programme over all sub-rectangles evaluates every allowed cut. A root block of 16 cells × 8 frames has 136 × 36 = 4,896 sub-rectangles, and each is tried with up to 22 cut positions.
- **Size evaluation.** Sizes are evaluated with the same probability model as the real range coder. A final pass codes with the real coder.

The search is constrained only by precision; it imposes no byte limit per segment or packet. The multiplier λ and the proportion targets exist only in the encoder; the decoder only reads the tree.

Proportions are measured by cell count by default:

- option `energy` measures them by energy;
- mode `max` always uses energy;
- modes `keep` and `keep20` use both cell count and energy.

The search over root blocks runs in parallel. The number of threads does not affect the output: 16-thread and 1-thread encodes are byte-identical.

---

## 8 Quantisation

### 8.1 Step size

Each non-A leaf carries an exponent $E$ and a tier $t \in \lbrace B, C, D \rbrace$:

- $E$ is the bit length of the largest coefficient magnitude in the leaf, in 24-bit units;
- the tier bit counts are $\mathrm{TB}(B) = 2$, $\mathrm{TB}(C) = 4$ and $\mathrm{TB}(D) = 8$.

The quantisation step is $\Delta = 2^s$ in 24-bit units. The exponent $s$ is given by one of three **precision rules**. Every stream header records the rule number.

| Rule | Used for | Formula | Loud blocks | Quiet blocks |
|---|---|---|---|---|
| 1 | Scheme 1 (both layers); depth layer of scheme 2 | $s = \max(0, \min(E, 8) - \mathrm{TB})$ | B18 / C20 / D24 | TB bits below block peak |
| 2 | Frequency layer of scheme 2 | $s = \max(0, \min(E - \mathrm{TB}, 8 - \lbrace 0, 4, 8 \rbrace))$ | B16 / C20 / D24 | as rule 1 |
| 3 | Depth layer of keep20 | $s = \max(0, \min(E, 6) - \mathrm{TB})$ | B20 / C22 / D24 | TB bits below block peak |

In rule 2, the second term is 8, 4 and 0 for B, C and D respectively.

- **Quiet leaf** ($E$ below the cap). The step is set relative to the leaf peak. The coefficients then keep a fixed number of significant bits, which preserves waveform similarity regardless of absolute level.
- **Loud leaf.** The cap fixes an absolute resolution in bits. Rule 1, for example, gives B leaves 16 + 2 = 18 bits.

**16-bit backstop.** Every rule gives $s \le 8$, so no step is coarser than one 16-bit LSB (256 in 24-bit units), the resolution of the CD layer. All content above the 16-bit level is therefore waveform-coded.

The cap is a fixed format constant.

**Range.** The theoretical upper limit of the format is 144 dB, the range of 24-bit samples. The design target for the principal content is 120 dB.

### 8.2 Dead zone

The encoder uses a dead-zone scalar quantiser with parameter θ = 0.30:

$$|q| = \left\lfloor \frac{|X|}{\Delta} + 0.5 - 0.30 \right\rfloor ,$$

where $q$ takes the sign of the coefficient $X$. The dead zone is an encoder parameter; the decoder needs only the reconstruction rule below.

Smaller dead zones (0.15 and 0) were evaluated during development. They improved errors by about 2–3 dB, at a cost of +3 % and +6 % in total size respectively (Table 12.5). They were not adopted.

### 8.3 Reconstruction offset

A non-zero quantisation index $q$ is reconstructed as

$$\hat X = \operatorname{sign}(q)\left(|q| + 0.20\right)\Delta .$$

The offset 0.20 is a format constant. In the development measurements of Table 12.5, compared with leakage compensation alone, the offset:

- left extension size unchanged;
- improved the average ultrasonic error by 1.8 dB;
- improved the average peak error by 2.0 dB with the 18-bit depth layer and by 2.8 dB with the 20-bit depth layer.

### 8.4 Zero coefficients and the optional zero level

**Zero coefficients.** By default, a zero quantisation index is reconstructed as exactly zero. The effect of this rule is small:

- in the CD band, the CD layer underlies the extension, so no spectral holes arise;
- in the ultrasonic band, only components below 0.5–0.8 of a step are missing;
- in development measurements, the ultrasonic level of the decoded signal was 0.3–1.4 dB below that of the master.

**Optional zero level (`zs`, off by default).** With `zs`, the encoder transmits the true sign of each coefficient that quantises to zero. Such a coefficient is then reconstructed as sign × leaf level × step.

- Each leaf carries a 3-bit index $k$.
- For $k = 0$, zero coefficients reconstruct to zero.
- For $k > 0$, every zero coefficient in the leaf is followed by one bypass-coded bit, its true sign. It is reconstructed as $\pm \mathrm{ZC}[k] \Delta$.

The level table is

$$\mathrm{ZC} = \lbrace 0, 1/32, 1/16, 3/32, 1/8, 3/16, 1/4, 3/8 \rbrace .$$

The encoder selects $k$ for each leaf from the statistics of that leaf. Without the option, the encoder writes $k = 0$ in every leaf.

The following effects of `zs` were measured during development (10-second excerpts, mode 3).

| Quantity | Effect |
|---|---|
| CD-band error (24-bit route) | from about −111.5 to about −115 dBFS (about 3.5 dB better) |
| Ultrasonic error | 1–5 dB better |
| Peak error | 1–7 dB better |
| Depth layer size | +23–28 % |
| Frequency layer size | +4 % to +67 %; the weaker the ultrasonic content, the larger the increase |
| CD-Extra playing time, scheme 2, DXD recording with weak ultrasonics | may fall from about 45 to about 36 min |
| Determinism | O2 and Od decodes byte-identical |

The option remains off by default.

### 8.5 Class A

Class-A leaves carry no coefficients. All of their coefficients are reconstructed as exact zero.

---

## 9 Entropy Coding

All leaf data are coded with an adaptive binary range coder. The full binarisation is specified in [SPEC.md](spec/lossy/SPEC.md), Sections 6–8. This section summarises its structure.

**Probability adaptation.** Every adaptive binary model updates with a single fixed shift of 5.

**Binarisation.** Multi-valued symbols are binarised by fixed binary trees:

- coefficient bit lengths use a fixed 5-bit tree;
- the leaf zero-level index uses a 3-bit tree.

**Coefficient contexts.** The context of a coefficient is derived from the class of its leaf and the leaf exponent.

**Side-information contexts.**

- **Class symbol.** It is coded with a context given by the class of the previous leaf in the bitstream.
- **Exponent.** It is coded as a difference from the exponent of the previous non-empty leaf.
- **Zero-level index.** It is coded with a context given by the class symbol.

**Tree bits.**

- Split and direction bits are bypass bits.
- Cut positions are bypass-coded integers of fixed length.
- The signs transmitted under `zs` are bypass bits.

**Model state.** Each stream starts from the default model state. Version 5 defines no random-access points ([SPEC.md](spec/lossy/SPEC.md), Section 6.8). The fast seek of the decoder library (Section 13.6) uses checkpoints that the decoder records while decoding; nothing is added to the bitstream.

---

## 10 Container (v5)

This section is a summary. The normative description is given in [SPEC.md](spec/lossy/SPEC.md), Sections 3–5.

### 10.1 Carriage

In container v5, the LAE file is a RIFF/WAV file:

- its `data` chunk is the CD layer;
- the extension resides in a separate chunk with chunk ID `LAE ` (with a trailing space).

Players ignore the unknown chunk and play the CD layer. Files are distributed with the extension `.wav`. `.lae` is the format's own extension; the layout of a `.lae` file is not specified for container v5.

Encapsulation beyond the WAV file is defined outside the v5 container, by the encapsulation specification (Sections 10.5–10.9):

- **CD-Extra.** Packaging onto CD-Extra (44.1 kHz family) is implemented in the tool `lae_cdx`. The audio session holds the CD layer, and the data session holds the extension.
- **Separate files and streams.** Distribution as a base-layer file (FLAC or WAV) with an extension file beside it, and the LAE stream, are defined there as well. The extension is not carried inside a FLAC file.

### 10.2 Extension chunk layout

All header integer fields are little-endian 32-bit integers. The edge samples are little-endian 16-bit integers.

| Field group | Content |
|---|---|
| Identification | Magic `LAE1`; container version 5 |
| Rates and lengths | Master rate; base rate; number of octave levels; master length; padding length P per side at the master rate |
| Informational | CD cut-off; mode index (0 max, 1 "1", 2 "3", 3 "5", 4 keep, 5 keep20); equal-cut flag; proportion metric. These fields are for display only and do not affect decoding; the mode index must nevertheless lie in 0–5. |
| Boundary | Length nb of the extended base layer; number pb of extension samples at each end; pb stereo CD samples for each end |
| Dither flags | Number of flag bytes; then 1 byte per 4096 base-rate samples per channel, all left-channel flags first |
| Depth layer | L stream, R stream |
| Frequency layer | Number of segments n; n M streams; n S streams |
| Integrity | CRC-32 [9] over all preceding content |

### 10.3 Stream layout

Each stream consists of a byte count, ten header fields and the range-coded data. The header fields are:

- K;
- N;
- start spectral line;
- end spectral line;
- sampling rate;
- length;
- cell width + 256 × precision rule number;
- mode;
- equal-cut flag;
- number of range-coded bytes.

The precision rule numbers are:

- 1: B18/C20/D24;
- 2: B16/C20/D24;
- 3: keep20 depth layer, B20/C22/D24.

K is the number of MDCT frames in the plane. It must be positive and should equal $\lceil L_s/512 \rceil + 1$. Here $L_s$ is the padded base-rate length nb for depth streams, and the padded master length for frequency streams. If K is smaller, the missing frames decode as zero ([SPEC.md](spec/lossy/SPEC.md), Section 5.3).

### 10.4 Validation

A decoder refuses a file in any of the following cases:

- the CRC of the extension chunk does not match;
- header fields are inconsistent;
- the length of the CD audio does not match the extension chunk;
- any coded value is out of range;
- the container version is not 5.

All header fields are validated before use. The reference decoder also provides the following protections:

- read bounds;
- checks against non-terminating loops caused by malicious files;
- rate-parameter checks, including rejection of negative output rates;
- 64-bit file sizes;
- protection against output larger than 4 GB;
- overflow protection.

### 10.5 CD-Extra encapsulation

Sections 10.5–10.9 summarise the encapsulation specification, which is to be published as a draft as [spec/cdx/SPEC.md](spec/cdx/SPEC.md). Where the two differ, the specification governs. The encapsulation lies entirely outside container v5: the v5 container and the decoder are unchanged, and the encapsulation specification has its own version number. The tool `lae_cdx` implements it (commands in Section 14.6).

**Scope.** CD-Extra applies to the 44.1 kHz family only, and every CD layer on a disc is 44,100 Hz. DVD and Blu-ray data discs do not use the CD layout. They carry ordinary files (Section 10.7), and these may belong to either family.

**Session 1 (audio).** Session 1 is written in one pass (disc-at-once or session-at-once). It contains only the following:

- the 150-sector pregap of track 1, which is digital silence (hidden tracks are not supported);
- the CD layers of the tracks, each starting at the track's INDEX 01, with track 1 INDEX 01 at LBA 0;
- after each CD layer, fewer than 588 samples (one sector) of digital silence that complete the last sector of the track.

Session 1 is subject to the following rules:

- no guard track and no further silence follows the last track;
- no data or marker is embedded in the PCM;
- every audio track is a whole number of sectors and at least 300 sectors (4 s) long;
- the pre-emphasis flag is not set;
- a disc holds at most 98 audio tracks, because the data track also takes a track number.

The two modes of the CD layer lead to different layouts:

- **Derived CD.** Track boundaries are those given by the publisher. In a continuous master, every track start must lie on the sector grid; otherwise the tool stops, lists the starts and writes nothing. No master sample is added or removed.
- **Independent CD** (option `layout=<publisher.cue>`). The publisher's INDEX entries (including INDEX 00 from track 2 on, and INDEX 02 and above), ISRC, FLAGS and CATALOG are kept at their positions. Session 1 must equal the publisher's programme sample for sample, and the tool checks this. The tool accepts only files that are already aligned (Sections 4.3 and 4.5); it performs no alignment itself.

**Session 2 (data).** Session 2 holds exactly one data track in CD-ROM XA Mode 2 Form 1, with 2048 bytes of user data per sector. No Mode 1 sector occurs anywhere on the disc. The file system and its contents are as follows:

- the file system is ISO 9660 Level 1 [5] with Joliet [15]; Rock Ridge, UDF and El Torito are not used;
- the data track starts at the next writable address reported by the drive. For an image written in one pass, the start is precomputed as A2 + 11,400 sectors, where A2 is the start of the lead-out of session 1 and the 11,400 sectors include the 150-sector pregap of the data track. This value is to be confirmed by a burn (Section 17.3);
- the disc contains `/README.TXT`, a plain-text description in Chinese and English without hashes, positions, offsets or length tables;
- the directory `/LAE/` contains only `TRACKnn.LAX`, one extension file for each audio track that carries an extension, where nn is the two-digit track number in the TOC;
- the data session contains no CUE sheet, no TOC copy, no report, no program and no `autorun.inf`. Other files, such as a booklet, are the publisher's choice and are ignored by readers;
- the data track ends with zero sectors (150 by default, to be confirmed by a burn) and is at least 300 sectors long.

A track may be published without an extension, for example when capacity is short; such a track plays as CD only. ISRC, MCN and CD-Text may be written but may carry only standard metadata.

**Deliverables.** The tool delivers:

- an image for writing in one pass: `IMAGE.CUE`, `AUDIO.BIN` (session 1 from LBA 0 to A2, 16-bit little-endian stereo, whole sectors) and the data track as `DATA.BIN` (2352 bytes per sector) or `DATA.ISO` (2048 bytes per sector);
- a kit for writing in two steps: a CUE sheet for session 1 only, and a command that regenerates session 2 for the start address actually reported by the drive. The address reported by READ TRACK INFORMATION (MMC [17]) is checked against the TOC value B0 and against A2 + 11,400; if the three disagree, the tool stops;
- burning and acceptance instructions (`BURN.TXT`) with a layout table: the LBA of each INDEX 01, A2 and the start of the data track.

The CUE sheet contains no CRC, MD5 or other hash.

### 10.6 Extension file and trailer

An extension file consists of exactly two parts:

1. the body of the track's `LAE ` chunk, $n$ bytes including its final CRC-32, byte-identical to the chunk in the track's v5 WAV file;
2. an 80-byte trailer.

There is no file header, and $n$ is the file length minus 80. The extension is `.LAX` on a CD and `.lax` elsewhere; readers ignore case.

**Trailer.** All integers are little-endian.

| Offset | Bytes | Field | Content |
|---|---|---|---|
| 0 | 4 | Identifier | ASCII `LTVT` |
| 4 | 2 | Version | 1 |
| 6 | 2 | Length | 80 |
| 8 | 2 | Extension block type | 1 = LAEY container v5; 2 = reserved for LAES; other values reserved |
| 10 | 2 | Track number | On a CD, the audio track number in the TOC (1–98) |
| 12 | 4 | Sampling rate | Rate of the CD layer: 44100 on a CD; 44100 or 48000 elsewhere |
| 16 | 4 | ncd | Number of samples of the CD layer, $1 \le \mathrm{ncd} < 2^{28}$ |
| 20 | 16 | MD5 | Whole-track MD5 of the CD layer |
| 36 | 8 | S1 | Sliding checksum (below) |
| 44 | 8 | S2 | Sliding checksum (below) |
| 52 | 4 | lead0 | Number of consecutive digital-silence samples at the start of the CD layer |
| 56 | 4 | tail0 | Number of consecutive digital-silence samples at the end of the CD layer |
| 60 | 16 | Reserved | Zero when written; ignored when read |
| 76 | 4 | CRC-32 | CRC-32 of bytes 0–75 |

The fields are defined as follows:

- **Whole-track MD5.** The MD5 [16] of the interleaved little-endian 16-bit samples of the CD layer, the same definition as the audio MD5 in the FLAC STREAMINFO block for 16-bit stereo audio [1]. It is the sole criterion by which a reader accepts a track.
- **S1 and S2.** Each sample is read as a 32-bit frame word $w_i$. Then $S_1 = \sum w_i \bmod M$ and $S_2 = \sum (i+1)\,w_i \bmod M$, with $M = 2^{61} - 1$. Both sums can be updated in constant time as a window slides by one sample, so a reader can test every position of a search range cheaply.
- **lead0 and tail0.** These are counts of the content of the CD layer, not positions, and they are independent of burning, ripping and offsets. They serve only the coarse location of a track (Section 10.8). The silent samples they count are part of the track: they are always played and are included in the whole-track MD5. No program may remove or skip them.

**Contents excluded.** The trailer and the extension file contain no value computed from the extension bytes, and no value computed over both the CD layer and the extension; the extension is protected only by its own CRC-32. They also contain no position, LBA, offset, delay or gain, no per-sector or per-frame data, and no seek or cue table.

**Checks on reading.** A reader checks an extension file in a fixed order: the presence of the trailer, its CRC, its version and length, the extension block type, and the ranges of its fields. A file that fails any check is not used, and its track is output as CD audio with the reason.

### 10.7 Distribution outside the CD

**Single v5 WAV.** The encoder and the repackaging tool write the v5 WAV file in one canonical layout: `RIFF`/`WAVE`, a 16-byte `fmt ` chunk, the `data` chunk, and then the `LAE ` chunk, padded to an even length.

**Base layer plus extension file.** A track may be distributed as two files:

- a **base-layer file**, FLAC [1] or WAV, holding exactly the track's CD layer;
- an **extension file** with the same name and the extension `.lax`. When the track is also published on CD, it is byte-identical to `TRACKnn.LAX` on the disc.

Ordinary players play the base-layer file. The extension and the trailer are never placed inside a FLAC file. A reader pairs the two files when the decoded base layer has ncd samples and its whole-track MD5 equals the trailer's MD5. A paired track is repackaged into the canonical v5 WAV, byte-identical to the encoder's output.

**LAE stream.** An LAE stream delivers, track by track, the CD layer sample for sample (FLAC, WAV or raw PCM) and the extension file byte for byte. A stream carries no header, index, position or offset. A stream whose CD layer was re-encoded lossily (for example AAC or MP3) is not an LAE stream: the extension is never applied to lossily decoded audio, and only the base layer is played.

The 48 kHz family may be used for all forms in this section.

### 10.8 Reading rips and files

The reader of `lae_cdx` (core: `src/lae_cdx_read.h`) treats every input in the same way: whole-disc rips (BIN or WAV, with or without a CUE sheet), per-track rips (with or without a CUE sheet), a single file and LAE streams. Rips may come from any ripping program, made with any offset setting. 16-bit stereo PCM WAV input is required.

**Disc stream and TOC.** The reader assembles its input into a disc stream. The TOC is taken, in order of priority, from:

1. a TOC read from the drive (specified; not yet implemented by `lae_cdx`, Section 17.3);
2. the CUE sheet;
3. the order and lengths of a set of per-track files; this source is marked prominently in the report.

Each file is placed at the position that the CUE sheet or TOC gives it. Positions covered by no file are unknown and read as zero; the reader does not retry other values. Without a TOC, each file is searched by content, and the report states "no TOC". An unrecognised CUE construct stops the reader.

**Per-track location.** Every track is located independently; the reader neither assumes nor needs a common offset for the disc. For each extension file, the reader proceeds as follows:

1. **Coarse location.** A coarse position is taken from the TOC, and from the information range: the positions of the first and last non-zero samples in the stream, corrected by lead0 and tail0.
2. **Fine location.** Every window start within a radius $W \ge 5880$ samples of the coarse positions is tested, or the whole file when there is no TOC. Only windows whose sliding sums equal S1 and S2 are candidates.
3. **Acceptance.** A candidate is accepted only if its whole-track MD5 equals the trailer's MD5.

A slip within a track, such as a lost or a duplicated sample, changes only that track's MD5. That track is played as CD, and the other tracks are unaffected.

**Output and failure.** The reader writes its output as follows:

- **Accepted tracks.** Each accepted track is rebuilt as `TRACKnn.lae.wav`, byte-identical to the file the encoder wrote.
- **Other tracks.** Every other track is written as `TRACKnn.cd.wav`, the ripped CD audio unchanged, with the reason. The reason categories are: unknown samples (a missing or truncated file), file too short, no match, no trailer, damaged trailer, unrecognised version or extension block type, invalid trailer field, track number not in the TOC, damaged extension, and lossy base layer.
- **No track accepted.** If no track is accepted, the reader reports that the input may be another disc or another edition.
- **Track without an extension.** A track of the TOC without an extension file is output as CD; this is not a failure.

**Prohibitions.** The reader observes the following:

- it never modifies, repairs or replaces a CD sample;
- it never uses the edge samples in the extension chunk to locate or join tracks;
- it uses no external database and no network;
- it emits no high-resolution data for a track before that track has been accepted, and it never switches between CD and extension within a track;
- measured offsets are displayed only and are written to no file. Rips made with offsets 0 and +667 gave byte-identical outputs (Section 10.9).

### 10.9 Self-check, capacity and tests

**Closed-loop self-check.** Before `lae_cdx` delivers an image, it reads the image back with the reader's own code, taking the extension files from the image's data session. The read-back covers the following rip forms:

- the whole-disc image with its CUE sheet, at offsets within ±5880 samples;
- a whole-disc rip without a CUE sheet;
- per-track rips, with a CUE sheet and with file order only, in the three gap modes (gaps appended to the previous track, prepended to the next track, or omitted), with and without the pregap of track 1.

Every track must pass and must rebuild a v5 WAV byte-identical to the encoder's output. In independent-CD discs with INDEX 00 gaps from track 2 on, a rip that omits the gaps cannot supply their samples; the self-check then expects the track before such a gap to fall back to CD unless the gap is digital silence. The self-check also includes deliberate damage: one sample changed, the extension file of another album, and a removed trailer. Each of these tracks must fall back to CD. If any check fails, the image is void and the process stops.

**Capacity.** The tool checks the capacity against the type of the blank disc before writing, and once more after the self-check. If the image does not fit, it stops and lists the options: publishing some tracks without an extension (option `noext=`), using another disc, or limiting the octaves, which requires re-encoding. It never removes a track or an octave automatically.

**Tests (development, 2026-10-21).** The tests used test discs built from 10–15 s excerpts of two tracks of one recording.

| Test | Result |
|---|---|
| Known-answer tests: checksum examples of the specification, the 7 test strings of RFC 1321, CRC-32, 1000 random positions of the sliding sums against direct computation, trailer ranges | All passed |
| ISO 9660 / Joliet writing and reading | 162 of 162 checks passed |
| Conformance cases of the encapsulation specification, in memory (including the reader's CUE parsing, the self-check, deliberate damage, missing files, GBK and UTF-8 CUE sheets) | 84 of 84 |
| End-to-end command-line tests with real files (including a data session mounted on Windows; outputs for offsets 0 and +667 byte-identical) | 17 of 17 |

The test suites were reviewed independently, and the issues confirmed by the review were corrected before these results were obtained. A burn of a real disc has not yet been made (Section 17.3).

---

## 11 Modes and Bit Rates

### 11.1 Schemes and modes

Two schemes and six modes give twelve combinations. The schemes are:

- scheme 1 (`s1`, high quality, default);
- scheme 2 (`s2`, high compression).

| Mode (`lae_enc` argument) | D retained | C retained | Depth layer | Proportion metric |
|---|---|---|---|---|
| `max` (maximum compression) | 10 % | 30 % | 18 bit (rule 1) | energy |
| `1` | 30 % | 50 % | 18 bit (rule 1) | cell count |
| `3` (default) | 50 % | 70 % | 18 bit (rule 1) | cell count |
| `5` | 70 % | 90 % | 18 bit (rule 1) | cell count |
| `keep` | 90 % | 100 % | 18 bit (rule 1) | cell count + energy |
| `keep20` | 90 % | 100 % | 20 bit (rule 3) | cell count + energy |

On recordings dominated by noise, the modes are necessarily close in size: the modes adjust only the C and D content, and such recordings contain little of it. Section 11.4 gives the measured size steps between adjacent modes.

### 11.2 Measurement basis

The figures in Tables 11.3a–11.5 and 12.2 were measured during development, on 2026-10-16, in the test round of the v5 release candidate (the "release-candidate round"). The round covered:

- 23 real recordings and 60 synthetic signals;
- 10-second stereo excerpts;
- both schemes and all six modes;
- 996 encodes in total.

**Published data.** The results are published in [data/loop10_v5_release_candidate.csv](data/loop10_v5_release_candidate.csv) and described in [data/README.md](data/README.md).

- The recordings in the published data are anonymised and cover 88.2–192 kHz.
- At 352.8 and 384 kHz, the published data contain synthetic signals only.
- Figures for recordings at 352.8 kHz are therefore not part of the published data. They are marked **(U)** in the tables below.
- The published data do not mark DSD-converted recordings or the "Synth" signal as separate classes.

**Build.** The release candidate differs from the published v5 codec in the reconstruction of zero coefficients. On the same excerpts, the published v5 encoder gives frequency layers 0.01–0.3 % smaller and an SNR 0–0.6 dB better. The frequency-layer sizes below should therefore be read as close upper bounds.

**Statistics.** Sizes are medians over the excerpts of each category. Values per minute are six times the 10-second medians, rounded. Medians of components need not add up to the median of the total, so totals are approximate.

### 11.3 File size per minute and per layer

The CD layer is uncompressed 16-bit stereo PCM: 10.58 MB/min at 44.1 kHz and 11.52 MB/min at 48 kHz (1 MB = 10^6 bytes). Carrying the base layer in FLAC (Section 10.7) would reduce this.

**Table 11.3a — Real recordings, mode 3, median MB per minute (release-candidate round).** (U): not part of the published data.

| Master rate | Scheme | Depth layer | Frequency layer | Extension total | Extension / CD |
|---|---|---|---|---|---|
| 88.2 kHz | 1 | 1.3 | 1.6 | 2.9 | 27 % |
| 88.2 kHz | 2 | 1.3 | 1.2 | 2.5 | 24 % |
| 96 kHz | 1 | 1.4 | 5.7 | 7.1 | 61 % |
| 96 kHz | 2 | 1.4 | 4.3 | 5.7 | 50 % |
| 176.4 kHz | 1 | 1.3 | 5.6 | 6.9 | 65 % |
| 176.4 kHz | 2 | 1.3 | 2.8 | 4.1 | 39 % |
| 192 kHz (one excerpt) | 1 | 1.4 | 12.8 | 14.2 | 123 % |
| 192 kHz (one excerpt) | 2 | 1.4 | 9.3 | 10.8 | 93 % |
| 352.8 kHz (U) | 1 | 1.3 | 10.7 | 12.0 | 113 % |
| 352.8 kHz (U) | 2 | 1.3 | 5.4 | 6.7 | 63 % |

With keep20:

- the depth layer grows to about 2.7 MB/min in the 44.1 kHz family and about 3.0 MB/min in the 48 kHz family;
- the frequency layer is the same as in `keep`;
- over the real recordings, keep20 adds 8.8 % to 55 % to the extension relative to `keep` (Table 11.4);
- keep20 improves the CD band by about 13 dB (Table 12.2).

**Table 11.3b — Other material classes, mode 3, median MB per minute (release-candidate round).** (U): not part of the published data.

| Material | Rate | Scheme | Depth | Frequency | Extension / CD |
|---|---|---|---|---|---|
| DSD-converted recording | 88.2 kHz | 1 / 2 | 1.3 | 4.9 / 3.7 | 58 % / 47 % |
| DSD-converted recording | 176.4 kHz | 1 / 2 | 1.3 | 11.5 / 7.9 | 121 % / 87 % |
| DSD-converted recording (U) | 352.8 kHz | 1 / 2 | 1.3 | 28.8 / 19.8 | 284 % / 199 % |
| Synthetic stress signal (Synth) | 352.8 kHz | 1 / 2 | 6.3 | 33.7 / 29.7 | 377 % / 340 % |
| Synthetic suite, largest excerpt | 384 kHz | 1 / 2 | — | 79.2 / 69.1 | 707 % / 619 % |

DSD-converted masters carry very high ultrasonic noise from the noise shaping of the DSD source. A large extension is expected for them, because LAE codes that noise as content (Section 7.3).

The synthetic suite spans extremes: in mode 3, its extension/CD ratio ranges from about 22 % to more than 700 %.

The following rows each rest on a single excerpt:

- the 192 kHz real-recording rows;
- each DSD-converted row;
- the Synth row.

### 11.4 Size steps between modes

**Table 11.4 — Median increase of frequency + depth layer size between adjacent modes (range in parentheses); release-candidate round.** The last column counts the steps from `max` to `keep`.

| Material | Scheme | max→1 | 1→3 | 3→5 | 5→keep | keep→keep20 | Share of steps max→keep of 5 % or more |
|---|---|---|---|---|---|---|---|
| Real recordings | 1 | 1.4 % (0.0–3.7) | 0.7 % (0.1–2.9) | 0.6 % (0.0–3.1) | 0.3 % (0.1–4.2) | 21.7 % (8.8–48.0) | 0 % |
| Real recordings | 2 | 3.6 % (1.0–11.1) | 1.6 % (0.1–3.2) | 1.5 % (0.3–5.7) | 1.4 % (0.2–6.3) | 27.4 % (14.1–55.3) | 9 % |
| DSD-converted | 1 | 1.0 % | 1.0 % | 1.2 % | 0.9 % | 10.8 % | 0 % |
| DSD-converted | 2 | 3.2 % | 2.4 % | 2.4 % | 1.6 % | 14.6 % | 8 % |
| Synthetic suite | 1 | 7.3 % (−0.1–206.3) | 2.3 % | 2.1 % | 2.0 % | 10.9 % | 35 % |
| Synthetic suite | 2 | 13.1 % (0.1–214.2) | 3.5 % | 2.9 % | 1.9 % | 10.9 % | 40 % |

On real recordings, the median steps between adjacent modes from `max` to `keep` are 0.3–3.6 %. Larger steps occur on structured synthetic material. On real recordings, the principal choices are therefore the scheme and whether to use keep20.

### 11.5 CD-Extra playing time

CD-Extra applies to the 44.1 kHz family only. On a CD-Extra disc, the CD layer is ordinary Red Book audio in the first session, and the extension occupies CD-ROM XA Mode 2 Form 1 data sectors, with 2048 bytes of user data each, in the second session (Section 10.5). The constants used are listed below.

| Constant | Value |
|---|---|
| 80-minute disc | 360,000 sectors at 75 sectors per second |
| Data sector (user data) | 2048 bytes, so one disc minute holds 9.22 MB of data |
| Multisession overhead (first-session lead-out ≈ 6,750 sectors, second-session lead-in ≈ 4,500 sectors) | ≈ 11,250 sectors ≈ 2.5 min. These are commonly used Blue Book values; confirmation with an actual burn is future work (Section 17.3). |
| ISO 9660 / Joliet file system [5][15] | about 1–2 MB (≈ 0.2 min), estimated; each file is aligned to 2048 bytes |
| Safety margin | 1.5 % or 4 % of capacity |

The layout of `lae_cdx` places the data track at A2 + 11,400 sectors, that is, the overhead above plus the 150-sector pregap of the data track, about 2.53 disc minutes. The playing times below were computed with 2.5 minutes; the difference is about 0.03 disc minutes.

Let $r$ be the extension size in MB per minute of music, divided by 9.22. The album length $L$ must then satisfy

$$L(1 + r) + 2.5 + 0.2 \le C$$

in disc minutes, where $C$ is the capacity after the safety margin has been deducted.

**Upper bound.** If the ultrasonic content is entirely class A, the frequency layer is almost empty. The playing time is then about:

| Case | 16-bit route | 24-bit (18-bit depth layer) | keep20 |
|---|---|---|---|
| Ultrasonics entirely class A | about 73–76 min | about 64–66 min | about 57–58 min |

**Table 11.5 — Measured playing time, 80-minute disc, 44.1 kHz family; release-candidate round.**

- Each cell gives the median for the 1.5 % margin / the 4 % margin, followed by a range in parentheses.
- The range runs from the shortest excerpt with the 4 % margin to the longest excerpt with the 1.5 % margin.
- (U): not part of the published data.

| Material | Rate | Combination | 24-bit route (min) | 16-bit route (min) |
|---|---|---|---|---|
| Real recordings | 88.2 kHz | scheme 2 max | 58.4 / 56.8 (55–60.8) | 65.4 / 63.7 (61.5–69.1) |
| Real recordings | 88.2 kHz | scheme 2 keep | 56.5 / 55 (53.1–59.5) | 63.1 / 61.4 (59.1–67.4) |
| Real recordings | 88.2 kHz | scheme 1 max | 51.7 / 50.4 (49.6–59.6) | 57.2 / 55.7 (54.8–67.5) |
| Real recordings | 88.2 kHz | scheme 1 keep | 51 / 49.7 (48.7–57.7) | 56.3 / 54.8 (53.8–66.2) |
| Real recordings | 88.2 kHz | scheme 1 keep20 | 46.2 / 45 (44.2–51.9) | 56.3 / 54.8 (53.8–66.2) |
| Real recordings | 176.4 kHz | scheme 2 max | 52.3 / 51 (46.4–53.4) | 57.9 / 56.4 (50.9–59.4) |
| Real recordings | 176.4 kHz | scheme 2 keep | 51 / 49.7 (44.4–51.7) | 57.1 / 55.6 (48.5–57.3) |
| Real recordings | 176.4 kHz | scheme 1 max | 43.5 / 42.4 (36.9–48.1) | 47.4 / 46.2 (39.7–52.9) |
| Real recordings | 176.4 kHz | scheme 1 keep | 43 / 41.9 (36.3–46.9) | 47 / 45.7 (39–52.1) |
| Real recordings | 176.4 kHz | scheme 1 keep20 | 39.6 / 38.5 (33.7–43) | 47 / 45.7 (39–52.1) |
| Real recordings (U) | 352.8 kHz | scheme 2 max | 44.4 / 43.2 (39.2–45.8) | 48.4 / 47.2 (42.4–50.2) |
| Real recordings (U) | 352.8 kHz | scheme 2 keep | 43.6 / 42.5 (36.4–44.4) | 47.7 / 46.4 (39.1–49) |
| Real recordings (U) | 352.8 kHz | scheme 1 max | 33.2 / 32.3 (28.1–37.4) | 35.4 / 34.4 (29.7–40.3) |
| Real recordings (U) | 352.8 kHz | scheme 1 keep | 32.7 / 31.9 (27.1–36.5) | 35 / 34 (28.5–39.6) |
| Real recordings (U) | 352.8 kHz | scheme 1 keep20 | 30.7 / 29.9 (25.6–34.1) | 35 / 34 (28.5–39.6) |
| DSD-converted | 88.2 kHz | scheme 2 max | 51 / 49.6 | 56.3 / 54.8 |
| DSD-converted | 176.4 kHz | scheme 2 max | 39 / 38 | 42 / 40.9 |
| DSD-converted (U) | 352.8 kHz | scheme 2 max | 23.4 / 22.8 | 24.5 / 23.8 |
| DSD-converted (U) | 352.8 kHz | scheme 1 max | 17.8 / 17.3 | 18.4 / 17.9 |

**Assessment against the design target.** The design target is at least 40 minutes on one CD-Extra disc for a typical 352.8 kHz master in scheme 2, mode max, 24-bit route. The figures below rest on recordings that are not part of the published data.

- **Median.** The target is met: 44.4 / 43.2 minutes.
- **Worst excerpt.** The target is narrowly missed: 39.2 minutes with the 4 % margin, 0.8 minutes short.

The option `zs` reduces playing time. On DXD recordings with weak ultrasonics in scheme 2, the reduction can be about a fifth, from about 45 to about 36 minutes (Section 8.4).

---

## 12 Quality

### 12.1 Measures

All errors are:

- expressed in dBFS, where more negative is better;
- computed between the decoded output and the master;
- averaged over both channels.

The measures are computed by `src/cmp_wav.cpp`. The following quantities are reported.

| Quantity | Definition |
|---|---|
| CD band | 0–20 kHz error, reported separately for the 24-bit route (`full`) and the 16-bit route |
| Ultrasonic 1/2/3 | Error in the first, second and third octave band above the CD band. For 352.8 kHz masters, these are 22.05–44.1, 44.1–88.2 and 88.2–176.4 kHz. |
| Peak error | Largest sample error |
| Worst 10 ms | Error level of the worst 10 ms window |

A written definition of the estimators (band-limiting filters, averaging) is future work (Section 17.3).

### 12.2 Results on real recordings

**Table 12.2 — Real recordings, median errors in dBFS; release-candidate round.** (U): not part of the published data.

| Rate | Scheme | Mode | CD band, 24-bit | CD band, 16-bit | Ultrasonic 1 | Peak | Worst 10 ms |
|---|---|---|---|---|---|---|---|
| 88.2 kHz | 1 | 3 | −111 | −102 | −114 | −95.5 | −107.9 |
| 88.2 kHz | 2 | 3 | −111 | −102 | −109.5 | −86.0 | −104.3 |
| 88.2 kHz | 1 | keep20 | −124 | −102 | −114 | −97.1 | −110.3 |
| 96 kHz | 1 | 3 | −112 | −102 | −116 | −95.7 | −108.8 |
| 96 kHz | 2 | 3 | −112 | −102 | −103 | −86.5 | −100.1 |
| 96 kHz | 1 | keep20 | −125 | −102 | −116 | −99.7 | −113.1 |
| 176.4 kHz | 1 | 3 | −111 | −102 | −117 | −93.6 | −107.3 |
| 176.4 kHz | 2 | 3 | −111 | −102 | −107.5 | −81.9 | −100.4 |
| 176.4 kHz | 1 | keep20 | −124.5 | −102 | −117 | −95.5 | −109.6 |
| 192 kHz (one excerpt) | 1 | 3 | −112 | −102 | −119 | −94.7 | −105.9 |
| 192 kHz (one excerpt) | 2 | 3 | −112 | −102 | −107 | −83.8 | −99.2 |
| 352.8 kHz (U) | 1 | 3 | −111 | −102 | −119 | −93.8 | −107.4 |
| 352.8 kHz (U) | 2 | 3 | −111 | −102 | −109.5 | −85.6 | −98.9 |
| 352.8 kHz (U) | 1 | keep20 | −124.5 | −102 | −119 | −94.9 | −109.8 |

The real-recording results are summarised as follows.

- **CD band, 24-bit route.**
  - The error is about −111 dBFS by default. At 6.02 dB per bit, this corresponds to roughly 18.5 bits, and it is 9 dB better than the 16-bit route.
  - Keep20 reaches about −124 to −125 dBFS.
  - The CD band is identical in both schemes, as designed.
  - Measurements during development on 40-second excerpts (2026-10-17) gave the same values.
- **Ultrasonic band.** The median error in the first ultrasonic octave is −114 to −119 dBFS in scheme 1 and −103 to −109.5 dBFS in scheme 2, depending on the sampling rate.
- **Scheme difference.**
  - The schemes differ by 4.5 to 13 dB in first-octave ultrasonic error, depending on the sampling rate.
  - In Table 11.3a, their frequency layers differ in size by a factor of about 1.3 to 2, and their extensions by a factor of about 1.2 to 1.8.

On DSD-converted material in mode 3 (release-candidate round), the first ultrasonic octave measured, for scheme 1 / scheme 2:

- 88.2 kHz: −115 / −103 dBFS;
- 176.4 kHz: −118 / −106 dBFS;
- 352.8 kHz (U): −121 / −109 dBFS.

On this material, the CD-band error was −111 dBFS in the 24-bit route and −101 to −102 dBFS in the 16-bit route.

For an independent CD aligned to a fraction of a master sample, the CD-band error of the 24-bit route was at the codec's normal level (Table 4.3).

### 12.3 Synthetic material and inherent limits

**Synthetic suite.** In the release-candidate round, the generated synthetic suite (60 signals) gave a median CD-band error in the 24-bit route of:

- −111 to −115 dBFS across sampling rates and the modes `max` to `keep`;
- −121 to −125 dBFS with keep20.

**Synth, 16-bit route.** The synthetic stress signal "Synth" contains square waves whose band-limited form exceeds full scale. The CD layer must clip these, and the 16-bit route has no depth layer with which to restore them. Its 16-bit-route CD-band error is therefore an inherent limit of that route for such signals:

- −42 dBFS on a 40-second excerpt, measured during development (2026-10-17);
- −35 to −36 dBFS on the 10-second excerpts of the release-candidate round.

**Synth, 24-bit route.**

- In a 40-second development comparison, leakage compensation improved the 24-bit-route CD-band error of Synth from −79 to −112 dBFS.
- In the release-candidate round, the 24-bit-route CD-band error of Synth in the modes `max` to `keep` was −101, −113, −96 and −82 dBFS at 88.2, 96, 176.4 and 352.8 kHz respectively.
- With keep20, the 96 kHz value was −125 dBFS; the other three were unchanged.
- The values at 176.4 and 352.8 kHz have not yet been analysed (Section 17.4).

### 12.4 Output limiting

The final output of the decoder is hard-limited to the 24-bit range.

- **Overshoot is bounded.** The 16-bit backstop (Section 8.1) bounds any overshoot above full scale by the coding error. In a development measurement on a master hard-clipped at +12 dB, the peak error was at most −76.6 dBFS. The hard limiter returns such samples to full scale, which is the master's own value at those samples.
- **Limiter activity.** In the release-candidate round, there was no encode in which the decoder's hard limiter would act (column `limiter_samples` of the published data).
- **Encoder report.** For information, the encoder performs a diagnostic decode and reports the number of samples to which the hard limiter will apply.

### 12.5 Development measurements for the v5 constants

A variant study during development compared:

- leakage compensation;
- the reconstruction offset;
- the depth-layer precision;
- a guard band;
- smaller dead zones.

The study used stereo excerpts of 12 sources. The excerpts were 40 seconds long, except the synthetic `electronic` and `transients` excerpts, which were 20 seconds long. The sources were real recordings at 96–352.8 kHz and synthetic stress signals.

The study used a pre-v5 development build whose reconstruction of zero coefficients differs from v5. Its absolute values are therefore superseded, and only the relative effects are reproduced here.

**Table 12.5 — Average change relative to a baseline without leakage compensation (scheme 1, mode 3; 12 sources; pre-v5 build).**

| Variant | Extension size | CD band (dB) | Ultrasonic 1 (dB) | Peak error (dB) |
|---|---|---|---|---|
| Leakage compensation | +0.2 % | −5.1 | 0 | −3.5 |
| Compensation + reconstruction offset 0.20 | +0.2 % | −5.8 | −1.8 | −5.5 |
| Compensation + 19-bit depth layer | +6.6 % | −11.0 | 0 | −3.9 |
| Compensation + 20-bit depth layer | +12.6 % | −16.8 | 0 | −4.1 |
| Compensation + 20-bit depth layer + offset 0.20 | +12.6 % | −18.6 | −1.8 | −6.9 |
| Guard band (fixed class D for 2 kHz above the start) | +2.6 % | 0 | +0.1 | −0.2 |
| Compensation + dead zone 0.15 | +3.2 % | −7.1 | −2.0 | −5.9 |
| Compensation + dead zone 0 | +5.6 % | −8.1 | −2.9 | −6.5 |

The synthetic sources dominate the CD-band average. On real recordings, leakage compensation alone left the CD band at −111 dBFS, except that one DSD-converted source improved from −109 to −111 dBFS.

Of these variants, version 5 uses:

- leakage compensation and the reconstruction offset 0.20 in all modes;
- the 20-bit depth layer in mode `keep20`.

### 12.6 Listening tests

The audible band of LAE Lossy is the CD layer plus a waveform-coded refinement. No listening-test results have been published.

Listening tests on difference-sensitive material are future work (Section 17.3). Candidate procedures are ABX and webMUSHRA [11].

---

## 13 Determinism and Conformance

### 13.1 Arithmetic

Interpolation and decimation use integer filter tables.

The coefficient reconstruction and the inverse MDCT are computed in IEEE-754 binary64 arithmetic [7], in exactly the order of the reference source. All MDCT constants come from exact hexadecimal literals in `src/mdct_tables.h`. Under these conditions, every platform produces bit-identical output.

The following build options are prohibited:

- `-ffast-math`;
- `/fp:fast`;
- any option that permits fused multiply-add contraction.

On 32-bit x86 with GCC or Clang, `-msse2 -mfpmath=sse` is mandatory. x87 builds are not supported, and the decoder's start-up self-test refuses them.

### 13.2 Start-up self-test

At every start, the decoder passes a fixed data set through the inverse MDCT and computes a 64-bit fingerprint of the result.

- The fingerprint is an FNV-1a-style hash [8] with a non-standard initial value, given in [SPEC.md](spec/lossy/SPEC.md), Section 13.
- It must equal `0x729b547fbdbfe203`, the value `SELFTEST_REF` in `src/mdct_tb.h`.
- If it differs, the build has altered floating-point evaluation. The decoder then refuses to run (exit code 4), rather than produce output that differs from other platforms.

The tool `tools/selftest_ref.cpp` prints the fingerprint of the current build.

The encoder runs an inverse-MDCT self-test and an MD5 self-test at start. The decoder library runs the self-test on the calling thread (Section 13.6).

### 13.3 Verified configurations

The verified builds are listed in [docs/BUILD.md](docs/BUILD.md), Section 3.

- **Windows, MSVC 2022.** The x64 `/O2`, x64 `/Od` and x86 SSE2 builds of the decoder produce identical results. The MSVC encoder produced the reference vector files.
- **Linux x86, decoder.** The decoder was built with GCC 13 x86-64 (`-O2` and `-O0`), Clang 18 x86-64 (`-O2`) and GCC 13 x86 32-bit (SSE2). 256 of 256 decoder results were identical to the reference.
- **Linux x86, encoder.** The encoder built with GCC 13 x86-64 (`-O2` and `-O0`) reproduces all 16 conformance vector files byte for byte.
- **Decoder library.** `lae_dec`, which now calls the decoder library, and `liblae` in x64 and x86 builds each reproduced 64 of 64 conformance outputs (16 vectors × 4 routes).
- **Linux ARM64.** On aarch64, the decoder built with GCC 13 (`-O2` and `-O0`) and Clang 18 (`-O2`) passed the start-up self-test, and 192 of 192 decoder results were identical to the reference. The encoder built with GCC 13 (`-O2`) reproduces all 16 conformance vector files byte for byte.
- **Not covered.** Other ARM systems, such as 32-bit ARM and macOS on ARM, have not been tested. x87 builds are not supported (Section 13.1).

The following results were also measured during development:

- with the `zs` option, optimised and unoptimised decodes were byte-identical;
- in the release-candidate round, 83 of 83 comparisons between optimised and unoptimised decoders were byte-identical (column `o2_equals_od` of the published data, which omits the recordings at 352.8 kHz).

### 13.4 Other invariants

- **Encoder threads.** The number of encoder threads does not affect the bitstream.
- **Encoder self-check.** The encoder re-decodes every layer and segment, and refuses to write a file if any coefficient plane differs. It then decodes the final file completely before releasing it.
- **Alignment.** An offset printed by the encoder and given back with `cdalign=` yields the same file. Inputs that are already aligned, of equal length and need no placement give the same output as before the alignment was introduced.
- **Development runs.** Measured during development (2026-10-17), 331 encode–decode runs completed without failure.
- **Release-candidate round.** The v5 release candidate completed 996 encodes without failure, and each encode passed the encoder's self-check.

### 13.5 Conformance vectors

The conformance vectors and their hashes are listed in [tests/vectors/MANIFEST.md](tests/vectors/MANIFEST.md).

**Material.** The vectors are taken from the self-generated synthetic suite: 2 seconds of each signal, starting at 5 s. No third-party recordings are used.

**Signals.** Four signals are used: `sweep` (88.2 kHz), `transients` (176.4 kHz), `imd` (96 kHz) and `electronic` (352.8 kHz). Each is coded in four configurations:

- mode 3, scheme 1;
- mode max, scheme 2;
- keep20, scheme 1;
- mode 3, scheme 1 with `zs`.

**Hashes.** Each vector is the encoder output file. The manifest gives the SHA-256 [10] of the encoded file, and of the decoded WAV file for each of four routes:

- `full`;
- `16`;
- `cd24`;
- `full` at half the master rate.

For the 88.2 and 96 kHz vectors, half the master rate equals the base rate, so the half-rate `full` route is not supported for them (Section 14.3).

The manifest currently lists the first 16 hexadecimal digits of each hash. Publication of the full values is an open item of [SPEC.md](spec/lossy/SPEC.md), Appendix C.

A conforming decoder must reproduce the listed hashes for every vector and route. The encoder of this revision writes all 16 vectors byte for byte.

### 13.6 Decoder library

The stages of the reference decoder have been moved unchanged into the library `src/lae_decoder.h`, which provides a `Decoder` class. The command-line decoder `lae_dec` calls the library. The library is streaming: its memory use does not grow with track length, apart from the CD layer and the extension chunk, which are held in memory.

**C interface.** `liblae` (`src/lae_api.h`, API version 3) is the C interface for players and plugins. It is built for x64 and x86.

- **Opening.** A track can be opened from a v5 WAV file, from the bytes of a v5 WAV file in memory, from a file read through a host callback, or from a base layer (the CD layer as PCM) with its extension file. A base layer is accepted only when its whole-track MD5 matches the extension file, or when the track is found in it by content and then matches.
- **Routes and output.** The caller chooses one of the four routes of Section 3.4 and an output rate. The output is always two interleaved channels as `int32` values: in the 24-bit range for the routes `cd24`, `16` and `full`, and in the 16-bit range for the CD route.
- **Statistics.** The library reports the number of samples held by the hard limiter, whether a stream error occurred, and the number of entropy-coded bytes consumed. The last of these allows a live bit rate to be displayed.
- **Threads.** One track must not be used by two threads at once. Different tracks may be decoded in different threads at the same time; in testing, two tracks decoded in two threads gave output bit-identical to decoding them separately.
- **Floating-point state.** The decoder needs strict IEEE-754 binary64 arithmetic in a fixed order (Section 13.1). Hosts whose threads may run with another floating-point state should set MXCSR to `0x1F80` around every call and restore it afterwards. `lae_selftest()` checks the calling thread; if it fails, the host should play the CD layer only. `lae_start()` runs the self-test again.

**Bit-exact fast seek.** While decoding, the library records entropy-coder checkpoints about every 4 s. A seek restores a checkpoint and performs a warm restart of every signal stage shortly before the target, so that the output after a seek is bit-identical to sequential decoding.

- A seek backwards, or into a part that has already been decoded, is fast.
- A seek forward past the decoded part must entropy-decode up to the target, because v5 defines no random-access points (Section 9). On the test machine, this cost about 1.6 s per minute skipped at 352.8 kHz.
- In development tests on 10–15 s excerpts, 540 random seeks, including positions never decoded before, over all routes and output rates, were all bit-identical; the slowest seek with the following read took 0.2 s.

The usage is given in Section 14.7.

### 13.7 Player component

The foobar2000 component `foo_input_lae` 0.3 is built on `liblae`, which is compiled into the component DLL. It plays LAE v5 files with all layers up to the master rate, for example 352.8 kHz / 24 bit.

- **Hosts.** 32-bit foobar2000 1.4, 1.5 and 2.x. A 64-bit version for foobar2000 2.x is planned.
- **Files.** The component accepts `.wav` files that carry an `LAE ` chunk, and the same files renamed to `.lae`. Such a renamed file is a v5 WAV file, not the `.lae` layout, which remains unspecified. A WAV file without an `LAE ` chunk is rejected at once and left to the built-in WAV decoder.
- **Layer selection.** All layers at the master rate (default); all layers at the highest octave up to 192 kHz or up to 96 kHz; CD layer + depth layer (24 bit at the base rate); or the CD layer alone. A change during playback takes effect at the current position.
- **Verification.** The component checks the whole file before playback: the CRC of the extension chunk and the structure of the header and the streams. A file that fails is played entirely as the CD layer, and the reason is written to the console and to the file properties.
- **No switching within a track.** The component never switches between the CD layer and the extension within a track. A stream error during playback is reported as an error and stops playback. Only a layer change selected by the user takes effect mid-track.
- **Determinism.** The floating-point state is fixed during decoding and restored afterwards, for both x87 and SSE.
- **Memory.** About the file size per track. Because a 32-bit host is limited to 4 GB, tracks longer than about one hour at 352.8 kHz require the planned 64-bit version.
- **Decoder priority.** Because the built-in WAV decoder also accepts `.wav`, the component places itself ahead of it in the decoder list when necessary and asks for one restart of foobar2000. This adjustment can be disabled in the advanced settings.

The component does not yet open CD-Extra rips directly; a rip is first restored with `lae_cdx read` (Section 10.8). The usage is given in Section 14.8; installation and the displayed information are documented in [plugins/foo_input_lae/README.md](plugins/foo_input_lae/README.md).

---

## 14 Usage

### 14.1 Building

The code is standard C++17 and uses no third-party libraries. The encoder is built from `src/lae_enc.cpp` and the decoder from `src/lae_dec.cpp`.

- **Decoder sources.** The decoder uses `src/lae_dec.cpp`, `src/lae_decoder.h`, `src/part_stream.h`, `src/partition_format.h`, `src/mdct_tb.h`, `src/mdct_tables.h`, `src/halfband.h` and `src/wavio.h` ([SPEC.md](spec/lossy/SPEC.md)).
- **Encoder sources.** The encoder compiles the decoder into the same executable. It additionally uses `src/partition.h`, `src/fft.h`, `src/pad_mirror.cpp`, `src/base_layer.cpp`, `src/routes.cpp`, `src/lossy_v2.cpp` (which includes `src/tone_scan.cpp`), `src/part_coder.cpp` and `src/cd_align.cpp`.
- **Encapsulation tool sources.** `lae_cdx` is built from `src/lae_cdx.cpp` with `src/lae_cdx.h` (trailer and checksums), `src/lae_cdx_read.h` (reader core) and `src/iso9660.h` (ISO 9660 and Joliet).
- **Decoder library.** `liblae` consists of `src/lae_api.h` and `src/lae_api.cpp`, built on `src/lae_decoder.h`. Define `LAE_API_BUILD` when building it as a DLL, and `LAE_STATIC` in both the host and the library when linking it statically.

`lae_enc` and `lae_dec` build on Windows with MSVC and on Linux with GCC and Clang; `lae_cdx` has been built and tested on Windows with MSVC. Example commands:

```
cl /O2 /EHsc /std:c++17 /D_USE_MATH_DEFINES src\lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_dec src/lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_enc src/lae_enc.cpp
```

Build commands (Windows, MSVC; the same flags as `lae_enc`): `cl /nologo /O2 /EHsc /std:c++17 /utf-8 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /bigobj src/lae_cdx.cpp` for `lae_cdx`. `liblae` is compiled from `src/lae_api.cpp` with `/fp:precise`; the verified flags are in `plugins/foo_input_lae/build_fb2k.cmd`.

Further build notes:

- For the MSVC encoder, add `/bigobj`.
- On 32-bit x86 with GCC or Clang, add `-msse2 -mfpmath=sse`.
- Do not use `-ffast-math`, `/fp:fast` or any option that permits FMA contraction (Section 13.1).

All options, the verified builds and the check scripts are given in [docs/BUILD.md](docs/BUILD.md).

### 14.2 Encoding

```
lae_enc <master.wav> <output.wav> [mode] [s1 | s2] [zs] [cd=<cd.wav>] [energy] [equal] [cdcheck] [cdalign=<offset>] [cdaccept] [bits=24]
```

| Argument | Meaning |
|---|---|
| `master.wav` | Stereo integer PCM WAV, 16/24/32 bit (32-bit input: Section 3.1). Rates 88.2/176.4/352.8 kHz (44.1 kHz family) or 96/192/384 kHz (48 kHz family). |
| `mode` | `max`, `1`, `3` (default), `5`, `keep`, `keep20` (Section 11.1). The mode is a positional argument: it must be given whenever any later argument is used. |
| `s1` / `s2` | Scheme 1, high quality (default) / scheme 2, high compression. The CD band is the same in both; they differ only in the frequency layer. |
| `zs` | Optional zero level (Section 8.4); off by default |
| `cd=<file>` | Independent CD: a 16-bit stereo WAV at the base rate of the master's family (44.1/48 kHz); its samples are written unchanged as the CD layer. The CD is aligned as described in Section 4.3. Without `cdalign=`, the encoder reports both searches and writes nothing. |
| `cdalign=<offset>` | The confirmed offset in master samples; positive means that the CD starts later. A whole number, or up to 9 decimals with `.` as the decimal point (`cdalign=1,5` is rejected). The offset printed by the encoder yields the same file. |
| `cdcheck` | Performs the alignment checks without encoding (exit code 0). Without `cdalign=`, it reports both searches; with `cdalign=`, its last line states "would encode", "would ask for confirmation" or "would not encode". |
| `cdaccept` | Continue without asking when the remaining time difference lies beyond −40 dB (the confirmation bands of Section 4.3) |
| `bits=24` | For a 32-bit master whose lowest 8 bits are not all zero: use the upper 24 bits without asking (Section 3.1) |
| `energy` | Measure retention proportions by energy (modes `max`, `keep` and `keep20` use their own metric) |
| `equal` | Use equal (halving) cuts only |

Notes on the encoder:

- The audio of the output WAV is the CD layer; the extension is in the `LAE ` chunk.
- An unrecognised argument is reported as an error, and the encoder stops.
- With `cd=`, the encoder prints the alignment results. Without `cdalign=`, these are the two search results and the line `ALIGN_SUGGEST …`, and the exit code is 1. With `cdalign=`, it prints the lines `PLACE …`, `ALIGN_RECHECK …` and `PHASE t=<whole> phi=0 move=<move> offset=<offset>`. After a successful encode, it writes the movement report `<output>.align.txt` (Section 4.3).
- The encoder uses as many threads as there are logical cores. The environment variable `PARTTHREADS` overrides this. The thread count does not affect the result.
- The encoder processes a whole track in memory. The Windows build currently accepts masters up to about 12.7 minutes at 352.8 kHz, about 23 minutes at 192 kHz and about 46 minutes at 96 kHz (a limit on the number of samples, independent of the bit depth), and stops with an explanation for longer masters (Section 17.2).
- **Failure handling.** Every file operation is checked. On a full disc, an internal error, lack of memory or an interruption, the encoder stops with the reason, writes no output file and deletes its temporary files. The output is first written as `<output>.part` and renamed only after it has been read back and verified; an existing file of the same name is left unchanged if the run fails. The free disk space is checked before the run starts.

Encoder performance, measured during development:

- **Single job (2026-10-16; [docs/BUILD.md](docs/BUILD.md), Section 5).** On Windows x64, a 40 s stereo master at 352.8 kHz, encoded in mode 3 with 16 threads, took 113 s (about 0.35 × real time). Peak memory was 1.35 GB.
- **Three parallel jobs at eight threads each (2026-10-17).** Encoding 40 s took:
  - 160–260 s at 352.8 kHz;
  - 90–140 s at 176.4/192 kHz;
  - 45–65 s at 96 kHz.

  A single job ran about twice as fast.

### 14.3 Decoding

```
lae_dec <input.wav> <output.wav> [full | 16 | cd24] [output rate]
```

| Route | Layers | Output |
|---|---|---|
| `full` (default) | CD + depth + frequency | Master rate or the selected lower octave, 24 bit |
| `16` | CD + frequency | As above; CD band at 16-bit precision |
| `cd24` | CD + depth | 44.1/48 kHz, 24 bit |

In the `full` and `16` routes, the output rate may be the master rate or any lower octave down to twice the base rate; other rates are rejected. Frequency streams above the selected octave are neither read nor decoded.

The decoder is single-threaded and streaming:

- it decodes one row of root blocks (8 frames) at a time and overlap-adds frame by frame;
- the resampling stages pull samples through short sliding windows;
- its memory use does not grow with track length, apart from the input data. The CD layer and the extension chunk are currently read into memory whole.

`lae_dec` uses the decoder library of Section 13.6.

Decoder performance, measured during development:

- **Single job (2026-10-16; [docs/BUILD.md](docs/BUILD.md), Section 5).** On Windows x64, a 40 s file at 352.8 kHz decoded in the 24-bit route in 2.8 s (about 14 × real time). Peak memory was 38 MB.
- **Three parallel jobs (2026-10-17).** Decoding 40 s in the 24-bit route took:
  - 3–5 s at 352.8 kHz;
  - 2–3 s at 176.4/192 kHz;
  - about 2 s at 96 kHz.

### 14.4 Examples

```
lae_enc master_352k.wav track_352k.wav                     # mode 3, scheme 1
lae_enc master_352k.wav track_352k_max.wav max s2          # smallest extension
lae_enc master_192k.wav track_192k.wav keep20 s1           # 20-bit depth layer
lae_enc master_96k.wav track_96k.wav 3 s1 cd=cd_48k.wav    # independent 48 kHz CD: reports both searches, writes nothing
lae_enc master_96k.wav track_96k.wav 3 s1 cd=cd_48k.wav cdalign=<offset>   # encode at the confirmed offset
lae_enc master_96k.wav track_96k.wav 3 s1 cd=cd_48k.wav cdalign=<offset> cdcheck   # check only
lae_dec track_352k.wav out_full.wav                        # 24 bit, 352.8 kHz
lae_dec track_352k.wav out_176k.wav full 176400            # 24 bit, one octave down
lae_dec track_352k.wav out_cd24.wav cd24                   # 24 bit, 44.1 kHz
```

### 14.5 Whole-disc planner

```
lae_enc plan <master.wav> [<master2.wav> …] <publisher.cue> <out dir> [cdalign=<p1>,<p2>,…] [cdaccept]
```

- `<publisher.cue>` describes the publisher's disc (CUE with BIN or WAV).
- Without `cdalign=`, the planner performs both searches through the whole plan, prints the positions and a line `PLAN_SUGGEST whitened=<status>:<positions> plain=<status>:<positions>`, writes nothing and exits with code 1.
- `cdalign=<p1>,<p2>,…` gives one value per piece: the master sample at which the piece's CD frame 0 lies, with a fraction allowed, or `-` for a piece that is digital silence on the CD.
- `cdaccept` confirms pieces that fall into the confirmation band.
- Output in `<out dir>`: `TRACKnn.master.wav` and `TRACKnn.cd.wav` for each piece, and the report `plan.align.txt`.

The chain from a publisher's disc to a CD-Extra image (Section 4.5):

```
lae_enc plan album.wav publisher.cue planned                                  # report both searches; nothing written
lae_enc plan album.wav publisher.cue planned cdalign=<p1>,<p2>,…              # after confirmation: move and cut
lae_enc planned/TRACK01.master.wav planned/TRACK01.v5.wav 3 cd=planned/TRACK01.cd.wav cdalign=0   # each piece
lae_cdx image <image dir> planned/TRACK*.v5.wav layout=publisher.cue                  # CD-Extra image
```

The mode (here `3`) must be given as the third argument. Pieces that were confirmed in the planner with `cdaccept` must also be encoded with `cdaccept`.

### 14.6 CD-Extra packaging and reading

`lae_cdx` provides the commands `pack`, `split`, `repack`, `image`, `data`, `read`, `sim`, `selftest` and `test`.

**Image.**

```
lae_cdx image <image dir> <track1.v5.wav> <track2.v5.wav> … [label=<album>] [noext=<n>,<m>,…] [layout=<publisher.cue>]
```

- `<image dir>` must be new or empty.
- `label=` sets the volume label of the data session.
- `noext=` publishes the listed tracks as CD only, for example when capacity is short.
- `layout=` selects the independent-CD layout of a publisher's disc (Section 10.5).

Each input v5 file first passes all checks of the decoder. The image is delivered only after the closed-loop self-check (Section 10.9). The burning instructions `BURN.TXT` describe two routes: route A writes `IMAGE.CUE` in one pass; route B writes in two steps, and the second step uses `lae_cdx data` to generate session 2 for the start address actually reported by the drive.

**Reading.**

```
lae_cdx read <output dir> <disc>\LAE <rip>\RIP.CUE
```

The second argument is the folder with the extension files, for example the `LAE` folder of the disc's data session or a copy of it. The last argument is the rip: a whole-disc or per-track rip, with or without a CUE sheet, made with any offset. The output is described in Section 10.8.

**Other commands.** The commands `pack`, `split` and `repack` convert between v5 WAV files, base-layer files and extension files (Section 10.7). `sim`, `selftest` and `test` serve testing. Their arguments:

```
lae_cdx pack <v5.wav> <out.lax> [track=N]              # the extension file of one track
lae_cdx split <v5.wav> <base.wav> <ext.lax> [track=N]  # base-layer WAV + extension file
lae_cdx repack <base.wav> <ext.lax> <out.v5.wav>       # base-layer file + extension file -> v5 WAV
lae_cdx sim <imagedir> <outdir> <form> [offset=N] [pregap=1] [overread=N] [change=K] [slip=K]   # simulated rips (tests)
lae_cdx selftest                                       # known answers
lae_cdx test <fixturedir>                              # the test cases on the test discs
```

### 14.7 Decoder library (liblae)

A minimal decoding sequence:

```c
char err[256];
lae_track* t = lae_open_file("track.wav", err, sizeof err);   /* UTF-8 path */
if (t && lae_selftest() && lae_start(t, LAE_ROUTE_FULL, 0, err, sizeof err) == 0) {
    int rate, bits; uint64_t frames;
    lae_output_format(t, &rate, &bits, &frames);
    int32_t buf[2 * 4096];
    int64_t n;
    while ((n = lae_read(t, buf, 4096)) > 0) { /* n interleaved stereo frames */ }
}
lae_close(t);
```

- `out_rate` 0 selects the master rate for the routes `16` and `full`, and the base rate for the CD and `cd24` routes.
- `lae_seek(t, frame)` seeks to an output frame bit-exactly (Section 13.6).
- `lae_stats` returns the limiter count and the stream-error flag; `lae_stream_bytes` returns the entropy-coded bytes consumed.
- `lae_open_memory`, `lae_open_reader`, `lae_open_pair` and `lae_open_pair_files` open a track from memory, through a host read callback, or from a base layer with its extension file.
- A host requires `lae_api_version()` to be at least the version it was built against.

The full interface is documented in `src/lae_api.h`.

### 14.8 foobar2000 component

Copy `foo_input_lae.dll` into the `components` folder of foobar2000 and restart foobar2000. The layers are selected in the menu Playback → LAE, or in Preferences → Advanced → Decoding → LAE decoder. The status bar shows the layers being played and the output rate. The file properties show the layers present in the file (`lae_layers`), the master rate (`lae_master`) and the format of the CD layer (`lae_cd_layer`).

Building the component requires the foobar2000 1.5 SDK (`SDK-2019-12-27.7z`, from foobar2000.org), which is not included in the repository, and the x86 compiler of the Visual Studio 2022 build tools:

```
plugins\foo_input_lae\build_fb2k.cmd <unpacked SDK folder> <output folder>
```

The SDK code is compiled with the options required by the SDK; the LAE decoder is compiled with its own verified options (`/fp:precise`). Details are given in [plugins/foo_input_lae/README.md](plugins/foo_input_lae/README.md).

---

## 15 Patents and Prior Art

### 15.1 Pledge

The author has not filed, and will not file, any patent application on LAE. The author makes the following **patent non-assertion pledge** for implementations of the LAE specification:

> "The author has not filed and will not file patent applications on the LAE format, and will not assert any patent claim against any implementation of the LAE specification."

The pledge and the definitions of its terms are given in [PATENTS.md](PATENTS.md).

### 15.2 Prior art

LAE Lossy is built from the following published techniques:

- **MDCT.** The MDCT in textbook form, with the DCT-IV computed through a complex FFT with pre- and post-twiddles $\exp(-i 2\pi (n + 1/8)/(2N))$. This twiddle form was published in FFmpeg `libavcodec/mdct.c` on 2002-10-28 (commit `bb6f569`) [6].
- **Quantisation.** Dead-zone scalar quantisation.
- **Entropy coding.** Adaptive binary range coding.
- **Resampling.** Half-band FIR decimation and interpolation.
- **Dither.** TPDF dither.
- **Stereo.** Integer mid/side lifting.

The alignment of an independent CD and the encapsulation use the following published techniques:

- **Time-delay estimation.** Cross-correlation and the generalized cross-correlation with phase transform (GCC-PHAT) [12], applied coarse to fine.
- **Interpolation.** Band-limited windowed-sinc interpolation [13] with a Kaiser window.
- **Fractional delay.** Fractional-delay filtering [14].
- **File system.** ISO 9660 [5] with the Joliet extensions [15].
- **Disc layout.** The CD-Extra (Enhanced CD) multisession layout of an audio session followed by a data session [4].
- **Integrity.** The MD5 message digest [16], computed over the audio samples as in the FLAC STREAMINFO block [1]; and the CRC-32 [9].
- **Checksums.** Modular sums that are updated incrementally as a window slides over a sequence.

---

## 16 Licensing and Contributing

### 16.1 Licences

| Component | Licence |
|---|---|
| Specification and design texts (`spec/`, including `spec/cdx/`) | Creative Commons Attribution 4.0 International (CC-BY-4.0) |
| Documentation (`README.md`, `docs/`) | CC-BY-4.0 |
| Reference decoder and the other decoder-side sources in `src/`, including the decoder library `src/lae_decoder.h` and `liblae` (`src/lae_api.*`); tools in `tools/` | Apache License 2.0 |
| Encapsulation tool `lae_cdx` (`src/lae_cdx.*`, `src/lae_cdx_read.h`, `src/iso9660.h`) | Apache License 2.0 |
| foobar2000 component sources (`plugins/foo_input_lae/`) | Apache License 2.0. The built DLL also contains code of the foobar2000 SDK, under the SDK's BSD-style licence, and of pfc, under the zlib licence; the SDK is not included in the repository. |
| Reference encoder (encoder-side sources in `src/`, including the alignment `src/cd_align.cpp` and the planner `src/cd_plan.cpp`) | GNU General Public License v3.0 or later (GPL-3.0-or-later), or a commercial licence from the author ([COMMERCIAL.md](COMMERCIAL.md)) |
| Test scripts, build files, CI configuration | Apache License 2.0 |
| Conformance vectors, test data and the vector manifest; measurement data in `data/` | Creative Commons Zero v1.0 Universal (CC0-1.0). The vectors are generated signals only; no third-party recordings are included. |
| Project files (`LICENSE`, `NOTICE`, `AUTHORS`, `PATENTS.md`, `COMMERCIAL.md`, `CONTRIBUTING.md`, `REUSE.toml`) | CC0-1.0 |

The file-by-file assignment is given in [LICENSE](LICENSE) and [CONTRIBUTING.md](CONTRIBUTING.md).

**Patents.** The author's patent pledge is given in Section 15 and in [PATENTS.md](PATENTS.md).

### 16.2 Contributions

- **Inbound licence.** Contributions are accepted under the Apache License 2.0.
- **Sign-off.** Every commit must carry a Developer Certificate of Origin sign-off (`Signed-off-by:`).
- **Copyright and credit.** Contributors keep their copyright and are credited in the git history and in `AUTHORS`.
- **Use in the encoder.** The author may include contributions in the GPL encoder and in its commercially licensed version.

The rules are given in [CONTRIBUTING.md](CONTRIBUTING.md).

### 16.3 Format stability

The bitstream defined in SPEC.md is frozen for container version 5.

- A proposed change to the format, to the decoding process or to the reference encoder's coding tools must first be raised in an issue, with its motivation.
- Changes that alter decoder output for any conformance vector require a new container version.
- The alignment of an independent CD and the encapsulation required no change to the container or to the decoder.

---

## 17 Limitations and Future Work

### 17.1 Format and tiers

- **LAE Lossless (LAES).** With all layers present, the lossless tier is to reconstruct the master bit for bit; the normative specification is not yet written. Directions decided since the design document of 2026-10-18 ([spec/lossless/DESIGN.md](spec/lossless/DESIGN.md), under review): LAES uses the same alignment, encapsulation and reading chain as LAEY, with its own extension block type (2, reserved); a CD-Extra holds only its subset "CD + 24 bit at 44.1 kHz", and the full lossless tier is distributed on BD or as files; a FLAC-compatible form is under design (not decided).
- **Containers.** The WAV container and CD-Extra packaging are implemented. The encapsulation specification is a draft; it is to be revised after the measurements of a real burn (Section 17.3). The `.lae` file layout remains unspecified.
- **Channels.** Only stereo is supported.

### 17.2 Encoder

- **Streaming encoder.** The encoder processes a whole track, because the mode proportions are track-wide statistics. Its memory grows linearly with length: about 1.35 GB for 40 s at 352.8 kHz. The Windows build therefore accepts masters only up to about 12.7 minutes at 352.8 kHz (about 23 minutes at 192 kHz, about 46 minutes at 96 kHz). The whole-disc planner holds the whole master in memory. A segmented streaming encoder is to be designed, and encoder speed and memory remain to be addressed.
- **Limits of the alignment search.** The search can fail on short or narrowband material, and the encoder then stops. Examples are a 2 s excerpt of one sustained tone, a 2–3 s passage dominated by one low tone, and content entirely above about 1.2 kHz, which lies outside the band of level 0. The user then gives the position by hand with `cdalign=`.
  - Strictly periodic synthetic signals (tones, sawtooth waves, loops) surrounded by digital silence can be matched one whole period away. This is why every position is confirmed by the user (Section 4.3).
  - When the sound is longer than about 3.6 minutes, level 0 of the search uses a lower sampling rate; an improvement is planned.
  - Beyond about 1 hour 55 minutes of sound, no search is performed, and the position must be given with `cdalign=`.
- **Differing content.** A master and a CD whose non-silent content differs are not combined: sound that only one side has, of one CD sample or more, stops the encoder and the planner. Combining such material is future work.
- **Noise-removal export interface.** This interface is not yet implemented. It is to:
  - present waveform and spectrum views;
  - provide audition;
  - let the user remove selected noise regions (class A).

  Until it exists, all noise in the master is retained.
- **Closed-loop re-validation.** The structure-preservation tests of the label closed loop, using injected tones and clicks, are to be repeated on v5.
- **Detector documentation.** The statistics of the transient detector are to be documented (Section 7.4).

### 17.3 Tools and validation

- **Real independent CD pairs.** The alignment has been tested only with CDs derived from the master with equalisation, limiting, gain changes, delay, noise reduction, gating, edits and drift applied. Tests with real independent CD and master pairs from the market are pending.
- **CD-Extra.** The packaging tool has not yet been tested on a real burn. The following are pending:
  - a burning test of the image, by both routes of Section 14.6;
  - confirmation of the multisession overhead of Section 11.5 and of the data-track start A2 + 11,400 sectors;
  - the number of trailing zero sectors of the data track;
  - the effect of the CD-ROM XA marker in the data session, which the tool does not currently write;
  - compatibility tests with real CD players and with reading the data session on Windows, macOS and Linux.
- **lae_cdx.** The memory peak for an image of a whole 74-minute disc is about 3.5–4 GB. The specification also defines FLAC base-layer files and a TOC read from the drive; `lae_cdx` does not implement them yet: the reader refuses FLAC input, and it takes the TOC from a CUE sheet or from the order and lengths of the files.
- **Player component.** A 64-bit build for foobar2000 2.x is pending. The component cannot yet open CD-Extra rips directly.
- **Measurements and tests pending.**
  - Characterisation of the half-band filters for the specification.
  - Builds and self-test fingerprints on other ARM systems (32-bit ARM, macOS on ARM).
  - The effect of the leakage-compensation term on the `cd24` route (Section 6.5).
  - Analysis of the Synth CD-band values at 176.4 and 352.8 kHz (Section 12.3).
  - Player compatibility tests.
  - Listening tests.
- **Quality estimators.** The exact definitions of the quality measures of Section 12.1 are to be given in the test documentation.
- **Normative text.** SPEC.md specifies the M/S lifting, the dither generator and its seeds, the entropy-coder binarisation, the stream header field K and the filter tables. Its open items ([SPEC.md](spec/lossy/SPEC.md), Appendix C) include:
  - the full SHA-256 values of the test vectors;
  - `src/mdct_tables.h` as a normative annex;
  - whether a decoder must reject a `fmt ` sample rate that differs from the base rate;
  - the values of the stream-header fields `rate` and `L`, and whether bytes may follow the last stream;
  - a guaranteed bound against 64-bit overflow in the half-band accumulators;
  - whether corrupt-stream conditions must be detected in the whole stream or only in the parts a route needs;
  - the `.lae` file layout and the carriage in FLAC (the encapsulation specification answers the FLAC question for its scope: the base layer in FLAC, the extension in a separate file; Section 10.7);
  - the encoder's edge padding and its definition of the leaf exponent.

### 17.4 Known operating limits

- **Mode spacing.** On real recordings, the median size steps between adjacent modes from `max` to `keep` are only about 0.3–3.6 % (Section 11.4).
- **Worst-case playing time.** In the release-candidate round, the worst case for 352.8 kHz, scheme 2, max, 24-bit route falls 0.8 minutes short of the 40-minute target with the 4 % margin. This rests on recordings that are not part of the published data (Section 11.5).
- **Clipped CD content.** The 16-bit route cannot restore content that the CD layer had to clip (Section 12.3).
- **Leakage outside the `full` route.** Frequency-layer leakage into the CD band is compensated only in the `full` route (Section 6.5).
- **Synth at high rates.** The 24-bit-route CD-band error of the Synth signal at 176.4 and 352.8 kHz (−96 and −82 dBFS) is markedly worse than at 88.2 and 96 kHz. Its cause has not yet been analysed (Section 12.3).
- **Samples carried by a move.** Where the move of the master carries samples to a place at which no piece takes master samples (the very start or end of the master, or a gap that only the master has), these samples are not kept. The planner reports where this happens and how many samples are concerned; the user may add at least 1 ms (44 CD frames) of digital silence at that place to both the master and the CD (Section 4.5).
- **Hard-cut masters.** A master whose sound reaches its first or last sample keeps a few frames of digital silence at the ends of the CD layer after a fractional move (Section 4.3).
- **Forward seeking.** A seek forward past the decoded part requires entropy decoding up to the target, because v5 defines no random-access points (Section 13.6).
- **Sampling-rate families.** The encoder performs no rational-ratio resampling.
  - A 48 kHz family master is combined with a 48 kHz base. Combining it with a 44.1 kHz CD remains a mastering task.
  - The 48 kHz base layer is intended for DVD, Blu-ray and file distribution; CD-Extra applies to the 44.1 kHz family only.

---

## References

1. FLAC, Free Lossless Audio Codec. Format specification, IETF RFC 9639.
2. MPEG-4 Audio: Bit-Sliced Arithmetic Coding (BSAC) and Scalable Lossless Coding (SLS).
3. Compact Disc Digital Audio ("Red Book").
4. CD-Extra / Enhanced CD ("Blue Book"), 1995.
5. ISO 9660, file system for CD-ROM.
6. FFmpeg, `libavcodec/mdct.c`, commit `bb6f569`, 2002-10-28.
7. IEEE 754, Standard for Floating-Point Arithmetic (binary64).
8. FNV-1a, Fowler–Noll–Vo hash function.
9. CRC-32, cyclic redundancy check.
10. SHA-256, Secure Hash Algorithm (FIPS 180).
11. ABX and webMUSHRA listening-test procedures.
12. C. H. Knapp and G. C. Carter, "The generalized correlation method for estimation of time delay", *IEEE Transactions on Acoustics, Speech, and Signal Processing*, vol. 24, no. 4, 1976.
13. J. O. Smith and P. Gossett, "A flexible sampling-rate conversion method", *Proc. IEEE International Conference on Acoustics, Speech, and Signal Processing (ICASSP)*, 1984.
14. T. I. Laakso, V. Välimäki, M. Karjalainen and U. K. Laine, "Splitting the unit delay: tools for fractional delay filter design", *IEEE Signal Processing Magazine*, vol. 13, no. 1, 1996.
15. Joliet extensions to ISO 9660 (Microsoft).
16. R. Rivest, "The MD5 Message-Digest Algorithm", IETF RFC 1321, 1992.
17. SCSI Multimedia Commands (MMC).

