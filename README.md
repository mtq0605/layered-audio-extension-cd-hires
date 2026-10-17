# LAE: A Layered High-Resolution Extension of the Compact Disc

**Status:** this is the working paper and repository front page for container version 5. The format family is named **LAE** (**Layered Audio Extension**). This document describes the lossy tier, **LAE Lossy** (short name **LAEY**). For the lossless tier, **LAE Lossless** (short name **LAES**), a design document is published ([spec/lossless/DESIGN.md](spec/lossless/DESIGN.md)); its normative specification is not yet written.

**Normative text:** the bitstream and the decoding process are specified in [spec/lossy/SPEC.md](spec/lossy/SPEC.md), which is currently a draft for public review. Where SPEC.md is silent or ambiguous, the reference decoder is normative. This document is informative.

**Further documents:**

- Building and verified builds: [docs/BUILD.md](docs/BUILD.md)
- Conformance vectors: [tests/vectors/MANIFEST.md](tests/vectors/MANIFEST.md)
- Measurement data: [data/](data/), described in [data/README.md](data/README.md)
- Licences: [LICENSE](LICENSE). Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Patent pledge: [PATENTS.md](PATENTS.md). Commercial encoder licence: [COMMERCIAL.md](COMMERCIAL.md)

**Identifiers and files:**

- The executables are `lae_enc` and `lae_dec`.
- The RIFF chunk ID is `LAE ` (with a trailing space). The chunk begins with the magic `LAE1`.
- LAE files are distributed with the extension `.wav`, so that every player plays the CD layer. Distribution as `.flac` is planned.
- `.lae` is the format's own file extension. The layout of a `.lae` file is not specified for container version 5.

---

## Abstract

LAE lets a single file carry a high-resolution master, up to 24 bit and up to 352.8/384 kHz, together with a 16-bit Compact Disc layer. Every existing WAV player reproduces the CD layer bit for bit.

- **44.1 kHz family.** A planned CD-Extra package will carry the CD layer unchanged in the audio session of the disc and the extension in the data session, so that every CD player reproduces the CD layer.
- **48 kHz family (96/192/384 kHz masters).** These masters have a 48 kHz base layer intended for DVD, Blu-ray and file distribution.
- **FLAC.** Carriage in FLAC is also planned.

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
- Disc-capacity planning by the encoder. The encoder compresses only. The publisher chooses scheme, mode and output range, and is responsible for whether the result fits a given medium.

### 1.3 Principal features

- **Pyramid decomposition.** The signal is decomposed into CD, depth and frequency targets. These recombine by plain time-domain addition. The CD layer may be derived from the master or supplied independently.
- **Per-recording frequency-layer start.** The frequency layer begins at the detected cut-off of the actual CD, rather than at a fixed fraction of the CD Nyquist frequency.
- **Encoder-only leakage compensation.** Quantisation noise of the frequency layer can leak into the CD band through the MDCT window. The depth layer cancels this leakage, and the decoder needs no change for it.
- **Precision rules with a 16-bit backstop.** Explicit precision tiers (B/C/D) are defined relative to a per-leaf exponent and governed by three precision rules. Under all three rules, no quantisation step is coarser than one 16-bit LSB.
- **Global rate–precision search.** A track-wide search chooses a shape-adaptive binary time-frequency partition. The search meets retention proportions for the highest-precision content at minimum total bytes.
- **Determinism and conformance.** The decoding process is deterministic under strict IEEE-754 arithmetic. A start-up self-test fingerprint and 16 conformance vectors check this.
- **Open licensing and a patent non-assertion pledge.** The specification, the reference decoder and the reference encoder are published under the licences of Section 16.

---

## 2 Related Work

**FLAC** [1] is a lossless audio format.

- The CD layer of LAE is an ordinary PCM signal, so it can be carried in an ordinary FLAC stream. FLAC encapsulation of LAE is future work (Section 17.1).
- Two aspects of the LAE encoder were inspired by FLAC:
  - the operating modes are numbered like compression levels;
  - the partition search chooses partitions by actual coded size, modelled on FLAC's power-of-two partitioning.

**MPEG-4 Audio BSAC and Scalable Lossless Coding (SLS)** [2] are earlier examples of layered audio coding.

**CD-Extra** [4] (the Blue Book, 1995) defines a multisession disc: an audio session (Red Book [3]) followed by a data session.

- LAE uses CD-Extra as its planned physical-disc container for the 44.1 kHz family.
- The CD layer occupies the audio session unchanged, and the extension files reside in the data session.
- Use of the "CD EXTRA" logo requires a licence.

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

The input master must be a stereo integer PCM WAV file with 16, 24 or 32 bits per sample. For 32-bit input, the upper 24 bits are used.

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
- **Coding.** The extension codes the master minus the CD path. The pyramid of Section 3.2 is unchanged; only $c$ is no longer derived by the encoder.
- **Dither.** The encoder adds no dither to a supplied CD, so all dither flags are 0 in this mode.
- **Alignment.** The supplied CD must be sample-aligned with the master and have the same length. The encoder neither equalises the CD nor compensates its gain. Detection of misalignment is future work (Section 17.2).
- **Size.** The closer the CD is to the master, the smaller the extension. A CD that was separately limited, equalised or level-adjusted produces a markedly larger extension.
- **Cut-off detection.** In this mode, the cut-off is detected from the coherence between master and CD, which makes the detection insensitive to level differences (Section 6.3).

The publisher decides which CD and which master are combined.

A 48 kHz family master can be combined with a 44.1 kHz CD only after resampling. This is a mastering task: the encoder performs no rational-ratio resampling.

### 4.4 Bit-exactness

The CD layer is the audio content of the output WAV file. It is therefore byte-identical to the derived or supplied CD.

Before writing any file, the encoder decodes every layer and every segment again and verifies that all coefficient planes match.

The decoder refuses a file whose CD audio length does not match the extension chunk (Section 10.4). An alteration of the CD audio that leaves its length unchanged is not detected ([SPEC.md](spec/lossy/SPEC.md), Section 3.2).

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
- **Independent CD.** The cut-off is detected from the coherence between master and CD, independently of level.

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

**Model state.** Each stream starts from the default model state. Version 5 defines no random-access points ([SPEC.md](spec/lossy/SPEC.md), Section 6.8).

---

## 10 Container (v5)

This section is a summary. The normative description is given in [SPEC.md](spec/lossy/SPEC.md), Sections 3–5.

### 10.1 Carriage

In container v5, the LAE file is a RIFF/WAV file:

- its `data` chunk is the CD layer;
- the extension resides in a separate chunk with chunk ID `LAE ` (with a trailing space).

Players ignore the unknown chunk and play the CD layer. Files are distributed with the extension `.wav`. `.lae` is the format's own extension; the layout of a `.lae` file is not specified for container v5.

The following belong to the encapsulation stage and are not yet implemented:

- carriage in FLAC (distribution as `.flac`);
- packaging onto CD-Extra (44.1 kHz family), in which the audio session holds the CD layer and the data session holds the extension.

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

The CD layer is uncompressed 16-bit stereo PCM: 10.58 MB/min at 44.1 kHz and 11.52 MB/min at 48 kHz (1 MB = 10^6 bytes). Encapsulation in FLAC (future work) would reduce this.

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

CD-Extra applies to the 44.1 kHz family only. On a CD-Extra disc, the CD layer is ordinary Red Book audio in the first session, and the extension occupies Mode 1 data sectors in the second session. The constants used are listed below.

| Constant | Value |
|---|---|
| 80-minute disc | 360,000 sectors at 75 sectors per second |
| Mode 1 data sector | 2048 bytes, so one disc minute holds 9.22 MB of data |
| Multisession overhead (first-session lead-out ≈ 6,750 sectors, second-session lead-in ≈ 4,500 sectors) | ≈ 11,250 sectors ≈ 2.5 min. These are commonly used Blue Book values; confirmation with authoring software is future work (Section 17.3). |
| ISO 9660 file system [5] | about 1–2 MB (≈ 0.2 min), estimated; each file is aligned to 2048 bytes |
| Safety margin | 1.5 % or 4 % of capacity |

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

### 13.3 Verified configurations

The verified builds are listed in [docs/BUILD.md](docs/BUILD.md), Section 3.

- **Windows, MSVC 2022.** The x64 `/O2`, x64 `/Od` and x86 SSE2 builds of the decoder produce identical results. The MSVC encoder produced the reference vector files.
- **Linux, decoder.** The decoder was built with GCC 13 x86-64 (`-O2` and `-O0`), Clang 18 x86-64 (`-O2`) and GCC 13 x86 32-bit (SSE2). 256 of 256 decoder results were identical to the reference.
- **Linux, encoder.** The encoder built with GCC 13 x86-64 (`-O2` and `-O0`) reproduces all 16 conformance vector files byte for byte.
- **Not covered.** ARM has not yet been tested. x87 builds are not supported (Section 13.1).

The following results were also measured during development:

- with the `zs` option, optimised and unoptimised decodes were byte-identical;
- in the release-candidate round, 83 of 83 comparisons between optimised and unoptimised decoders were byte-identical (column `o2_equals_od` of the published data, which omits the recordings at 352.8 kHz).

### 13.4 Other invariants

- **Encoder threads.** The number of encoder threads does not affect the bitstream.
- **Encoder self-check.** The encoder re-decodes every layer and segment, and refuses to write a file if any coefficient plane differs.
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

A conforming decoder must reproduce the listed hashes for every vector and route.

---

## 14 Usage

### 14.1 Building

The code is standard C++17 and uses no third-party libraries. The encoder is built from `src/lae_enc.cpp` and the decoder from `src/lae_dec.cpp`.

- **Decoder sources.** The decoder uses `src/lae_dec.cpp`, `src/part_stream.h`, `src/partition_format.h`, `src/mdct_tb.h`, `src/mdct_tables.h`, `src/halfband.h` and `src/wavio.h` ([SPEC.md](spec/lossy/SPEC.md)).
- **Encoder sources.** The encoder compiles the decoder into the same executable. It additionally uses `src/partition.h`, `src/fft.h`, `src/pad_mirror.cpp`, `src/base_layer.cpp`, `src/routes.cpp`, `src/lossy_v2.cpp` (which includes `src/tone_scan.cpp`) and `src/part_coder.cpp`.

Both programs build on Windows with MSVC and on Linux with GCC and Clang. Example commands:

```
cl /O2 /EHsc /std:c++17 /D_USE_MATH_DEFINES src\lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_dec src/lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_enc src/lae_enc.cpp
```

Further build notes:

- For the MSVC encoder, add `/bigobj`.
- On 32-bit x86 with GCC or Clang, add `-msse2 -mfpmath=sse`.
- Do not use `-ffast-math`, `/fp:fast` or any option that permits FMA contraction (Section 13.1).

All options, the verified builds and the check scripts are given in [docs/BUILD.md](docs/BUILD.md).

### 14.2 Encoding

```
lae_enc <master.wav> <output.wav> [mode] [s1 | s2] [zs] [cd=<cd.wav>] [energy] [equal]
```

| Argument | Meaning |
|---|---|
| `master.wav` | Stereo integer PCM WAV, 16/24/32 bit (32-bit input: upper 24 bits used). Rates 88.2/176.4/352.8 kHz (44.1 kHz family) or 96/192/384 kHz (48 kHz family). |
| `mode` | `max`, `1`, `3` (default), `5`, `keep`, `keep20` (Section 11.1). The mode is a positional argument: it must be given whenever any later argument is used. |
| `s1` / `s2` | Scheme 1, high quality (default) / scheme 2, high compression. The CD band is the same in both; they differ only in the frequency layer. |
| `zs` | Optional zero level (Section 8.4); off by default |
| `cd=<file>` | Independent CD: a 16-bit stereo WAV at the base rate of the master's family (44.1/48 kHz), sample-aligned with the master and of equal length; written bit for bit as the CD layer |
| `energy` | Measure retention proportions by energy (modes `max`, `keep` and `keep20` use their own metric) |
| `equal` | Use equal (halving) cuts only |

Notes on the encoder:

- The audio of the output WAV is the CD layer; the extension is in the `LAE ` chunk.
- The encoder uses as many threads as there are logical cores. The environment variable `PARTTHREADS` overrides this. The thread count does not affect the result.
- The encoder processes a whole track in memory.

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
lae_enc master_96k.wav track_96k.wav 3 s1 cd=cd_48k.wav    # independent 48 kHz CD
lae_dec track_352k.wav out_full.wav                        # 24 bit, 352.8 kHz
lae_dec track_352k.wav out_176k.wav full 176400            # 24 bit, one octave down
lae_dec track_352k.wav out_cd24.wav cd24                   # 24 bit, 44.1 kHz
```

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

---

## 16 Licensing and Contributing

### 16.1 Licences

| Component | Licence |
|---|---|
| Specification and design texts (`spec/`) | Creative Commons Attribution 4.0 International (CC-BY-4.0) |
| Documentation (`README.md`, `docs/`) | CC-BY-4.0 |
| Reference decoder and the other decoder-side sources in `src/`; tools in `tools/` | Apache License 2.0 |
| Reference encoder (encoder-side sources in `src/`) | GNU General Public License v3.0 or later (GPL-3.0-or-later), or a commercial licence from the author ([COMMERCIAL.md](COMMERCIAL.md)) |
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

---

## 17 Limitations and Future Work

### 17.1 Format and tiers

- **LAE Lossless (LAES).** A design document is published ([spec/lossless/DESIGN.md](spec/lossless/DESIGN.md)); the normative specification is not yet written. With all layers present, the lossless tier is to reconstruct the master bit for bit.
- **Containers.** Only the WAV container is implemented. FLAC encapsulation, CD-Extra packaging and the `.lae` file layout belong to the encapsulation stage.
- **Channels.** Only stereo is supported.

### 17.2 Encoder

- **Streaming encoder.** The encoder processes a whole track, because the mode proportions are track-wide statistics. Its memory grows linearly with length: about 1.35 GB for 40 s at 352.8 kHz. A segmented streaming encoder is to be designed.
- **CD alignment.** In independent-CD mode, the CD must be supplied sample-aligned with the master and of equal length. Automatic detection of misalignment is planned for the encapsulation stage. A misaligned CD will then be reported as an error, not compensated.
- **Noise-removal export interface.** This interface is not yet implemented. It is to:
  - present waveform and spectrum views;
  - provide audition;
  - let the user remove selected noise regions (class A).

  Until it exists, all noise in the master is retained.
- **Closed-loop re-validation.** The structure-preservation tests of the label closed loop, using injected tones and clicks, are to be repeated on v5.
- **Detector documentation.** The statistics of the transient detector are to be documented (Section 7.4).

### 17.3 Tools and validation

- **CD-Extra.** No CD-Extra authoring tool is available yet. The following are pending:
  - confirmation of the multisession overhead values of Section 11.5 with actual authoring software;
  - a CD-Extra authoring and burning test;
  - compatibility tests with real CD-Extra players.
- **Measurements and tests pending.**
  - Characterisation of the half-band filters for the specification.
  - Builds and self-test fingerprints on ARM.
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
  - the `.lae` file layout and the carriage in FLAC;
  - the encoder's edge padding and its definition of the leaf exponent.

### 17.4 Known operating limits

- **Mode spacing.** On real recordings, the median size steps between adjacent modes from `max` to `keep` are only about 0.3–3.6 % (Section 11.4).
- **Worst-case playing time.** In the release-candidate round, the worst case for 352.8 kHz, scheme 2, max, 24-bit route falls 0.8 minutes short of the 40-minute target with the 4 % margin. This rests on recordings that are not part of the published data (Section 11.5).
- **Clipped CD content.** The 16-bit route cannot restore content that the CD layer had to clip (Section 12.3).
- **Leakage outside the `full` route.** Frequency-layer leakage into the CD band is compensated only in the `full` route (Section 6.5).
- **Synth at high rates.** The 24-bit-route CD-band error of the Synth signal at 176.4 and 352.8 kHz (−96 and −82 dBFS) is markedly worse than at 88.2 and 96 kHz. Its cause has not yet been analysed (Section 12.3).
- **Sampling-rate families.** The encoder performs no rational-ratio resampling.
  - A 48 kHz family master is combined with a 48 kHz base. Combining it with a 44.1 kHz CD remains a mastering task.
  - The 48 kHz base layer is intended for DVD, Blu-ray and file distribution; CD-Extra applies to the 44.1 kHz family only.

---

## References

1. FLAC, Free Lossless Audio Codec. Format specification.
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
