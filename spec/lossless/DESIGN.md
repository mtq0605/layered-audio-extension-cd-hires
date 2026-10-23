<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: 2026 mtq0605 -->

# LAE Lossless (LAES): Design Document

**English** | [中文](DESIGN.zh.md)

**Status:** design document published ([spec/lossless/DESIGN.md](DESIGN.md), this document), under review; normative specification not yet written.
**Licence of this document:** CC BY 4.0. Copyright (c) 2026 mtq0605.

**Review.** This document is under review. The following directions were decided after 2026-10-18 and take precedence over the text below. All other statements of this document are under review.

- LAES uses the same alignment, encapsulation and reading chain as LAE Lossy (LAEY) ([README](../../README.md), Sections 4.3, 4.5 and 10.5–10.8; [spec/cdx/SPEC.md](../cdx/SPEC.md)).
- LAES has its own extension block type, 2, which is reserved in [spec/cdx/SPEC.md](../cdx/SPEC.md).
- A CD-Extra disc holds only the subset "CD + 24 bit at 44.1 kHz" of LAES. The full lossless tier is distributed on BD or as files.
- A FLAC-compatible form (Section 9.2) is under design (not decided).

This document is informative. It describes the current design of LAE Lossless, as of 2026-10-18. The outline is stable, but details may still change. Decisions that have not yet been taken are marked **open**. Section 11 collects them.

---

## 1 Scope and status

### 1.1 Purpose

This document describes **LAE Lossless** (short name **LAES**). LAES is the lossless tier of the format family **LAE** (Layered Audio Extension).

It states:

- the layer structure;
- the bit positions;
- the rates;
- the candidate coding methods and their measured results;
- the variants under consideration.

### 1.2 Position within the family

LAE has two tiers. Both share one layer model and one set of container principles.

| Tier | Short name | Property |
|---|---|---|
| LAE Lossy | LAEY | Extension layers are coded with quantisation. The error is bounded by quantisation steps that the decoder knows. |
| LAE Lossless | LAES | Every layer is coded exactly. A decoder that holds all layers reproduces the master bit for bit. |

LAE Lossless keeps the following elements of LAE Lossy unchanged:

- the CD layer;
- the split into a depth layer at the base rate and octave layers at the higher rates;
- the container principles;
- the encoder's analysis tools.

The coding of the extension layers changes: it is exact instead of quantised.

Parts shared with LAE Lossy are not repeated here. The text refers instead to the LAE Lossy specification, [SPEC.md](../lossy/SPEC.md). This applies in particular to:

- the half-band filter tables and their arithmetic ([SPEC.md](../lossy/SPEC.md) Section 10);
- the dither generator and its seeds ([SPEC.md](../lossy/SPEC.md) Section 11.2);
- the integer M/S lifting ([SPEC.md](../lossy/SPEC.md) Section 11.4).

### 1.3 Scope

**In scope:**

- stereo integer PCM masters with at most 24 significant bits;
- master rates $R = R_0 \cdot 2^n$, with $R_0 \in \lbrace 44.1, 48 \rbrace$ kHz and $n \in \lbrace 1, 2, 3 \rbrace$. These are 88.2, 176.4 and 352.8 kHz, or 96, 192 and 384 kHz.

**Out of scope:**

- masters with more than 24 significant bits;
- rational-ratio rate conversion between the families.

As in LAE Lossy, a 48 kHz-family master has a 48 kHz base layer.

### 1.4 Notation

The notation follows [SPEC.md](../lossy/SPEC.md) and the LAE Lossy working paper ([README.md](../../README.md)).

| Symbol | Meaning |
|---|---|
| $R_0$ | Base rate, 44.1 kHz or 48 kHz. $B = R_0/2$ is the base Nyquist frequency. |
| $R = R_0 2^n$ | Master rate. |
| $x$ | Master signal at rate $R$, as 24-bit integers. |
| $\mathcal{D}$, $\mathcal{U}$ | Integer half-band decimation and interpolation by $2^n$ ([SPEC.md](../lossy/SPEC.md) Section 10). |
| $b = \mathcal{D}(x)$ | Unclipped 24-bit base word at rate $R_0$. |
| $c$ | 16-bit CD samples (CD16, Level 0). |
| $d$ | Regenerated TPDF dither. It is non-zero only in flagged blocks. |
| $v$ | CD path in 24-bit units, as defined in [SPEC.md](../lossy/SPEC.md). Every decoder can compute it from $c$ and the dither flags alone. |
| $w$ | Number of unused low bits of the master, that is, the trailing zero bits common to all samples. |
| $\lfloor\cdot\rfloor$ | Floor. For integers it is realised as an arithmetic right shift. |

Bit positions refer to the 24-bit word. Bit 23 is the most significant bit and bit 0 the least significant bit. The CD LSB is bit 8, which has a weight of 256.

---

## 2 Goals

### 2.1 Bit-exact layers

Every layer of LAES is exact. The layers are:

- the CD layer (CD16);
- the depth layer, which takes the base rate from 16 to 24 bit;
- the octave layers, which take the rate to ×2, ×4 and ×8 of the base rate.

A decoder that holds all layers reproduces the master in every sample.

A layer is never partially decoded. It is either present and complete, or absent. The lossless tier therefore needs no truncatable bit planes inside a layer.

### 2.2 Legacy playback

The base layer is a 16-bit PCM CD layer in the 44.1 kHz or 48 kHz family (Level 0), and every player can play it. The CD layer is unmodified PCM.

All extension layers are carried outside the PCM. Legacy players play the CD layer only.

### 2.3 One file instead of several versions

A publisher who offers several quality levels must otherwise store each one separately, for example:

- CD 16 bit;
- 24 bit at the base rate;
- 24 bit at 88.2 or 96 kHz;
- the full master.

A layered lossless file holds all of these bit-exactly in one file. This is the principal value of the tier.

### 2.4 Realistic size expectations

**LAES cannot be much smaller than FLAC.** Layering does not compress the master much better than FLAC does:

- a spectral bound on the achievable lossless rate lies only about 4–9 % below FLAC at compression level 8 on the measured masters (Section 10.2);
- FLAC already strips unused low bits.

**Expected file size.** The following figures are the author's estimates (Section 10.2).

| Storage approach | Size as % of the master's FLAC file |
|---|---|
| One layered file holding every quality level | about 96–100 % |
| The same quality levels stored as separate versions | about 175 % |

The prototypes measured so far do not yet reach this estimate (Section 10.6).

**Storage media.** A full lossless high-resolution file does not fit on a CD: it is about 3.7 times the raw size of the CD audio. The intended media are Blu-ray data discs and files.

### 2.5 Labelling

Lossy files are always flagged as lossy and are never called lossless. Only a file whose layers are all exact may be called LAE Lossless.

---

## 3 Layer model

### 3.1 Two axes

The extension forms a two-dimensional grid. Along the rate axis, the sample rate doubles at every step. Along the depth axis, the word length grows from 16 to 18, 20 and 24 bits.

**Depth axis (base rate).** The axis starts at the 16-bit CD layer, which is the 16-bit level. Plane groups at fixed bit positions below the CD LSB refine it:

- P1 = planes 1–2 (18-bit level);
- P2 = planes 3–4 (20-bit level);
- P3 = planes 5–8 (24-bit level).

**Rate axis.** Above the base band $0 \ldots B$ there are the octave layers:

- O1 = $B \ldots 2B$ (×2 rate);
- O2 = $2B \ldots 4B$ (×4 rate);
- O3 = $4B \ldots 8B$ (×8 rate).

The octaves are defined by rate ratios, so the same algorithm serves both families.

### 3.2 The grid

```
                              base band     octave O1     octave O2     octave O3
                              0 .. B        B .. 2B       2B .. 4B      4B .. 8B
                              (rate R0)     (rate 2*R0)   (rate 4*R0)   (rate 8*R0)

  P3 (planes 5-8, -> 24 bit)  [0,P3]
  P2 (planes 3-4, -> 20 bit)  [0,P2]
  P1 (planes 1-2, -> 18 bit)  [0,P1]
  CD (16 bit, Level 0)        [0,CD]
  octave layers (full depth)                [O1]          [O2]          [O3]
```

The base-band column $[0,\cdot]$ consists of the CD layer and the depth layer. It is defined in Sections 4 and 5.

In this outline, each octave column is a single cell at full depth. The octave columns are defined in Section 6. Whether they are subdivided into depth rows is part of the open octave-layer framing (Section 11, item O3).

### 3.3 Nesting

The layers are nested along each axis.

- **Depth.** A plane group requires all coarser plane groups and the CD layer. P1 refines the CD level, P2 refines the P1 level and P3 refines the P2 level.
- **Rate.** Octave layer $O_k$ requires $O_1, \ldots, O_{k-1}$ and the CD layer.

### 3.4 Subsets

A user or publisher may keep any subset of the grid that respects the nesting. Examples:

- CD + 24 bit at the base rate (CD, P1, P2, P3);
- CD + 88.2 kHz (44.1 kHz family) or 96 kHz (48 kHz family) (CD, O1);
- the full grid.

Removing layers from a file only removes data; nothing is re-encoded.

The example "CD + 88.2/96 kHz" presupposes that the two axes can be kept independently. That is, an octave layer must be decodable without the depth plane groups. Section 6.2 states this property; the octave-layer framing must preserve it (item O3).

### 3.5 What exactness guarantees

The following guarantees hold:

1. **Full grid.** With all layers present, the output equals $x$ in every sample.
2. **Base-rate prefixes.** With the CD layer and the plane groups up to $P_k$ present, the output equals the base word $b$ at the corresponding level, $b_{18}$, $b_{20}$ or $b$ (Section 5.2).
3. **Other subsets.** Every layer present is decoded exactly. The reference signal of an intermediate rate point, such as 88.2 kHz / 24 bit, is open (item O2). The natural candidate is the master reduced by the integer half-band chain of [SPEC.md](../lossy/SPEC.md).

The encoder has no precision, noise or mode choice that affects the decoded result. Its choices affect only:

- the file size;
- which layers are emitted.

---

## 4 CD layer

### 4.1 Derived CD (shared with LAE Lossy)

By default, the CD layer is derived exactly as in LAE Lossy, in four steps:

1. $n$ cascaded integer half-band decimation stages produce the unclipped base word $b$. The filters are Q31 tables with 64-bit accumulation, mirrored ends and one rounding per output sample.
2. Seeded TPDF dither is added, but only in blocks in which the recording's own noise is below 0.5 LSB16. Digital silence is never dithered.
3. The result is rounded to 16 bit.
4. The result is clipped to the 16-bit range.

**Normative references.** The filter tables, the arithmetic, the dither generator, the seeds and the per-block dither flags are those of [SPEC.md](../lossy/SPEC.md) (Sections 10 and 11.2).

**Dither regeneration.** The decoder regenerates the dither from the fixed seeds and the flags. No dither values are transmitted. Every decoder therefore knows the CD path $v$ exactly.

### 4.2 Independent CD

As an alternative, an existing CD master can be supplied. It becomes the CD layer bit for bit. The following conditions apply, as in LAE Lossy:

- **Format.** The CD must be at the base rate of the master's family.
- **Alignment.** It must be sample-aligned with the master and of equal length.
- **Dither.** No dither is added, so all dither flags are 0.

The lossless extension then codes the master minus the CD path exactly. Losslessness is unaffected by the choice of CD. The size of the depth layer is not: the further the supplied CD departs from $b$, the larger the depth layer becomes. Departures include separate limiting, equalisation and level changes.

The octave layers carry only their own octaves (Section 6). They are therefore not affected by the choice of CD.

### 4.3 Unclipped base word

The base word $b$ is unclipped, and the CD path is clipped. The depth layer therefore carries band-limited overshoot above full scale exactly.

Because $b$ may exceed the 24-bit range, the output of the base-rate 24-bit level must be limited when it is written as 24-bit PCM. LAE Lossy applies a final hard limit for this purpose. Whether LAES does the same at that level is open (item D3).

The internal reconstruction of the full master always uses the unclipped $b$.

---

## 5 Depth layer

### 5.1 Plane groups at exact bit positions

The depth layer refines the CD layer at the base rate. It consists of three nested plane groups at fixed bit positions of the 24-bit base word.

| Plane group | Planes below the CD LSB | Bit positions in the 24-bit word | Resulting level |
|---|---|---|---|
| CD (Level 0) | — | (rounded, dithered 16-bit value; see Section 5.3) | 16 bit |
| P1 | 1–2 | bits 7, 6 | 18 bit |
| P2 | 3–4 | bits 5, 4 | 20 bit |
| P3 | 5–8 | bits 3, 2, 1, 0 | 24 bit |

### 5.2 Levels

In this outline, the intermediate levels are defined by clearing the planes below the boundary:

$$
b_{18} = 2^{6}\left\lfloor b / 2^{6} \right\rfloor, \qquad b_{20} = 2^{4}\left\lfloor b / 2^{4} \right\rfloor, \qquad b_{24} = b .
$$

Whether the normative specification uses this truncation or a rounding convention at the 18-bit and 20-bit levels is open (item D1). Any such convention keeps the same bit positions and the same exactness.

### 5.3 Exact reconstruction

The CD level is not a truncation of $b$, because the CD value is rounded, dithered and clipped. The first plane group therefore links the CD path to the 18-bit level. The two lower groups extend the word bit-position by bit-position.

**Content of each group.**

- **P1** carries the exact integer increment $e_1 = b_{18}/64 - p(c, d)$. Here $p$ is an integer prediction of $\lfloor b/64 \rfloor$ that is computed from the CD samples and, in flagged blocks, from the regenerated dither.
  - $e_1$ is small, except under clipping, under overshoot (Section 4.3) or with an independent CD.
  - The exact form of $p$ (with or without dither compensation, and its rounding) is open (item D2).
  - The baseline is $p = 4c$, which corresponds to the residual $b - 256c$ used in the prototype of Section 10.6.
- **P2** carries $e_2 = (b_{20} - b_{18})/16 \in \lbrace 0, 1, 2, 3 \rbrace$.
- **P3** carries $e_3 = b - b_{20} \in \lbrace 0, \ldots, 15 \rbrace$.

**Decoding.** The decoder reconstructs the levels as follows:

$$
b_{18} = 64 (p + e_1), \qquad b_{20} = b_{18} + 16 e_2, \qquad b = b_{20} + e_3 .
$$

Every step is an exact integer operation. The content of each plane group is therefore fully determined by the master and the CD path; only its entropy coding is a design choice.

Plane groups P2 and P3 are close to random in recorded material. Their coding has no structure to exploit beyond the following:

- the general statistics of the low bits;
- unused low bits, where they exist.

### 5.4 Differences from the LAE Lossy depth layer

| Aspect | LAE Lossy | LAE Lossless |
|---|---|---|
| Target | $T = \mathrm{LP}_{f_c}(b - v)$, minus a leakage compensation term | $b$ exactly, without a low-pass |
| Precision | Precision rules (B18/C20/D24, keep20 B20/C22/D24) | Fixed plane groups P1/P2/P3, each exact |
| Partial precision | Quiet blocks are coded relative to the block peak | None; a plane group is either complete or absent |
| Leakage compensation | Required, because quantisation noise leaks into the CD band | Not applicable; there is no quantisation noise |

### 5.5 Measured cost

On one track of the measured set (Section 10), the complete 16-to-24-bit depth layer cost about 0.68 Mb/s. The CD16 layer of the same track cost about 0.52 Mb/s.

---

## 6 Octave layers

### 6.1 Bands and ratios

| Layer | Band | Rate after adding the layer | 44.1 kHz family | 48 kHz family | Coded values per second and channel |
|---|---|---|---|---|---|
| Base (CD + depth) | $0 \ldots B$ | $R_0$ | 0–22.05 kHz, 44.1 kHz | 0–24 kHz, 48 kHz | $R_0$ |
| O1 | $B \ldots 2B$ | $2R_0$ | 22.05–44.1 kHz, 88.2 kHz | 24–48 kHz, 96 kHz | $R_0$ |
| O2 | $2B \ldots 4B$ | $4R_0$ | 44.1–88.2 kHz, 176.4 kHz | 48–96 kHz, 192 kHz | $2R_0$ |
| O3 | $4B \ldots 8B$ | $8R_0$ | 88.2–176.4 kHz, 352.8 kHz | 96–192 kHz, 384 kHz | $4R_0$ |

A master at $R_0 2^n$ has the octave layers $O_1, \ldots, O_n$. The boundaries are fixed by rate ratios. The algorithm is identical for both families.

### 6.2 Critical sampling

The octave layers are critically sampled. Each octave layer carries only its own octave: octave $k$ carries $R_0 2^{k-1}$ values per second per channel.

At every rate point, the base and the octave layers together carry exactly as many values per second as the corresponding rate. For a 352.8 kHz master this is

$$
44.1 + 44.1 + 88.2 + 176.4 = 352.8 \text{ kHz.}
$$

The octave layers are *not* residuals of the form "full rate minus the interpolated base". Such a residual would carry the full rate $R$ again on top of the $R_0$ base values. For a ×8 master, this is one eighth more values than the master itself has. It would also carry an interpolation residual in the base band.

Section 6.6 describes this alternative and its measured results. It was used in the bit-exact prototype.

**Independence from the depth refinement.** An octave layer is computed from the master itself and carries only its octave. It is therefore not tied to the depth refinement of the base band. This independence is what allows subsets such as "CD + 88.2/96 kHz" (Section 3.4).

### 6.3 Content of the octave layers

The measured ultrasonic octaves of the 352.8 kHz masters contain the following:

- **Never empty.** The bands above 88.2 kHz are never empty. They consist of real recording noise and carry about 35 % of the bits of a full lossless file.
- **Measured cost.** The ultrasonic layers together cost about 4.0 Mb/s on one track.

Because the coding is exact, this noise is coded like any other content. There is no noise option in the lossless tier.

### 6.4 Unused low bits

Masters often carry fewer significant bits than their word length. All seven measured masters have 22 significant bits, so $w = 2$. FLAC strips such bits. A lossless layer must do the same, or it pays for zeros that carry no information.

The prototype (Section 10.6) uses the following mechanism:

1. **Detection.** $w$ is detected as the number of trailing zero bits common to all samples of the track.
2. **Grid rounding.** The prediction derived from the lower layers is rounded onto the master's grid of multiples of $2^w$, rounding half up:

   $$
   u' = 2^w \left\lfloor \left(u + 2^{w-1}\right) / 2^w \right\rfloor .
   $$

3. **Shifting.** The residual $x - u'$, which is then a multiple of $2^w$, is shifted right by $w$ before coding.
4. **Reconstruction.** The decoder adds back $u' + 2^w \cdot (\text{decoded residual})$.

How $w$ is signalled, and whether it is per track or per block, is part of the open octave-layer framing (item O3).

### 6.5 Stereo

The integer M/S lifting of [SPEC.md](../lossy/SPEC.md) Section 11.4 is available to every layer:

$$
S = L - R, \qquad M = R + \lfloor S/2 \rfloor ;
$$

$$
R = M - \lfloor S/2 \rfloor, \qquad L = R + S .
$$

The mapping is exactly reversible. The prototype applied it to the full-rate difference layer. Whether stereo decorrelation is fixed per layer (M/S or L/R) or chosen adaptively per block is open (item C4).

### 6.6 Measured alternative: the difference model

The bit-exact prototype used a full-rate difference layer in place of critically sampled octave layers. Its steps were:

1. **Base word.** The base word is reconstructed exactly from CD16 and an exact depth residual $r = b - 256c$.
2. **Up-conversion.** The base word is up-converted to the master rate with the integer half-band interpolator. The interpolator uses the Q31 tables of [SPEC.md](../lossy/SPEC.md) and mirrors the signal at its edges.
   - In each stage, the even output samples copy the input: $y[2i] = x[i]$.
   - The odd output samples are

     $$
     y[2i+1] = \left\lfloor \left( \sum_t h_t \left(x[i-t] + x[i+1+t]\right) + 2^{29} \right) / 2^{30} \right\rfloor ,
     $$

     where $h_t$ are the stored odd taps. The divisor $2^{30}$ realises the interpolation gain of 2.
   - Indices outside the signal are mirrored: $-i$ for $i < 0$, and $2(L-1) - i$ for $i \ge L$.
3. **Difference.** The difference to the master is formed on the master's grid, with unused bits removed (Section 6.4).
4. **Stereo.** The difference is mapped to integer M/S.
5. **Coding.** The difference is coded exactly.

The decoded output equalled the master in every sample. Section 10.6 gives the sizes.

The prototype established the exactness of this whole chain of integer operations. That chain is the deterministic interpolation, the grid rounding, the shifts and the integer M/S, all identical in encoder and decoder. The critically sampled octave layers of Section 6.2 replace this model in the design.

### 6.7 Realisation and framing (open)

The following questions about the octave layers are open:

- **O1** — how the octave split is realised in integers, such that each octave layer is critically sampled and every partial reconstruction is exact;
- **O2** — the reference signal of intermediate rate points (Section 3.5);
- **O3** — the octave-layer framing: the segmentation in time, the signalling of $w$ (Section 6.4), the subdivision of the octave columns by depth (Section 3.2), and the preservation of the independence of Section 6.2.

The candidate realisations for O1 are those of Section 7:

- spectral-line segments of an integer-reversible transform, analogous to the "octave = spectral-line segment" organisation of LAE Lossy;
- time-domain coding;
- an integer wavelet octave decomposition, which has been rejected on measured coding gain.

---

## 7 Coding options

### 7.1 Requirements common to all options

Every coding option must satisfy five requirements:

1. **Reversibility.** It is exactly reversible for every input in the value range of the layer, including full-scale extreme signals.
2. **Integer arithmetic.** It uses integer arithmetic only. All candidates studied are integer: 32-bit values with 64-bit accumulators. LAE Lossless decoding therefore needs no floating point. This differs from the binary64 inverse MDCT of LAE Lossy.
3. **Determinism.** It is deterministic: encoder and decoder execute the same operations in the same order.
4. **Lossless entropy coding.** It is combined with a lossless entropy coder.
5. **No dependence on classification.** It does not depend on any classification for correctness (Section 8).

### 7.2 Option (a): integer MDCT

**Structure.**

| Parameter | Value |
|---|---|
| Coefficients per block (hop) | $N = 512$ |
| Window | Sine window, length $2N$ |
| Building blocks | Exactly reversible integer lifting steps only |

**Forward transform.** The forward transform has two stages.

1. **Time-domain-aliasing fold.** At every overlap centre $C$, each sample pair $(y[C-1-j], y[C+j])$, for $j = 0, \ldots, N/2 - 1$, is rotated by $\alpha_j + \pi/2$, with

   $$
   \alpha_j = \frac{\pi}{4} + \frac{\pi (j + 1/2)}{2N} .
   $$

   The rotation is realised as an exact quarter rotation followed by one lifting rotation by $\alpha_j$. The corresponding real-valued map is

   $$
   (l, r) \mapsto \left(-l \sin\alpha_j - r \cos\alpha_j, \quad l \cos\alpha_j - r \sin\alpha_j\right).
   $$

2. **DCT-IV per block.** The $N$ folded samples of a block are arranged as $u[n] = y[C + N - 1 - n]$ and $u[N/2 + n] = y[C + N/2 - 1 - n]$. They are transformed by an integer DCT-IV built from a unitary FFT factorisation (Figure 7.1).

**Figure 7.1 — Integer DCT-IV program (unitary FFT factorisation).**

1. **Pack.** Form the complex pairs $z_n = (u[2n], u[N-1-2n])$, $n = 0, \ldots, N/2 - 1$.
2. **Pre-rotation.** Rotate each $z_n$ by $-\pi(4n+1)/(4N)$.
3. **Permutation.** Apply the bit-reversal permutation (pair swaps).
4. **Butterflies.** Apply radix-2 butterflies. At each stage of length `len`:
   - rotate the second operand by $-2\pi k/\mathrm{len}$;
   - then form $\left((a+b)/\sqrt{2}, (a-b)/\sqrt{2}\right)$ on the real and imaginary parts, realised as a negation of $b$ followed by a rotation by $\pi/4$.
5. **Post-rotation.** Rotate each output pair by $-\pi k/N$.
6. **Negation.** Negate the imaginary parts.
7. **Unpack.** Write $X[2k] = \mathrm{Re}$ and $X[N-1-2k] = \mathrm{Im}$.

**Rotations as lifting steps.** Every rotation by $\theta$ with $|\theta| \le \pi/2$ is three lifting steps with Q30 coefficients $c_1 = \tan(\theta/2)$ and $c_2 = \sin\theta$:

$$
x \leftarrow x - r(c_1 y), \qquad y \leftarrow y + r(c_2 x), \qquad x \leftarrow x - r(c_1 y),
$$

with the rounding

$$
r(v) = \left\lfloor (v + 2^{29}) / 2^{30} \right\rfloor .
$$

Rotations beyond $\pm\pi/2$ are first reduced by an exact quarter rotation: $(A, B) \mapsto (-B, A)$ or $(B, -A)$.

The complete DCT-IV program has 6193 elementary operations per 512-coefficient block. These are lifting triplets, quarter rotations, negations and swaps.

**Inverse transform.** The inverse transform executes the same program in reverse order, with every step inverted. This makes it bit-exact for every input.

**Measured results** (Section 10.4):

- **Reversibility.** Bit-exact on six extreme full-scale 24-bit test signals and on real audio. There were 0 mismatching samples.
- **Accuracy.** The deviation from the floating-point orthonormal MDCT is a rounding noise from the lifting steps:

  | Master | RMS deviation | Maximum deviation |
  |---|---|---|
  | 352.8 kHz | 1.32 LSB | 6.1 LSB |
  | 176.4 kHz | 1.31 LSB | 6.5 LSB |

- **Rate–distortion.** Equal to the floating-point MDCT.
- **Speed.** Forward 0.7 s and inverse 0.6 s for 40 s of 352.8 kHz stereo, about 60 times real time.
- **Lossless cost estimate.** The estimate used adaptive Rice coding of 16-coefficient groups without context modelling (Section 10.5).

  | Master | Integer MDCT (bit/sample) | FLAC level 8 (bit/sample) |
  |---|---|---|
  | 352.8 kHz | 9.67 | 9.24 (integer MDCT +4.7 %) |
  | 176.4 kHz | 10.24 | 9.91 |

- **Bit-exact prototype.** A full-rate difference layer coded with exact integer-MDCT coefficients reached 110.3 % of the FLAC reference file of Section 10.6.

**Assessment.** The integer MDCT is exact and fast, and it retains the coding gain of the MDCT for spectrally coloured content. Its rounding noise, about 1.3 LSB per coefficient, enters the coded coefficients. On the measured masters, this rounding noise is of the same order as the masters' very low ultrasonic noise floor. In lossless coding, this costs a noticeable number of bits.

### 7.3 Option (b): time-domain lossless residual coding

**Structure.** Each layer is coded in the manner of FLAC:

- a linear predictor with quantised integer coefficients predicts each sample from previous samples;
- the integer residual (sample minus prediction) is entropy-coded, for example with partitioned Rice codes;
- the decoder adds the residual to the identical integer prediction.

The prediction loop involves no rounding other than the deterministic integer evaluation of the predictor. No transform rounding noise enters the residual.

**Measured result** (estimate; Section 10.6). An order-32 time-domain linear predictor was applied to the full-rate difference layer of the bit-exact prototype. The residual was costed by a Laplacian entropy per partition, without side information. The resulting estimate was about 103.7 % of the FLAC reference file of Section 10.6, compared with 110.3 % for option (a) on the same layer.

**Assessment.** This is the simplest option and it is fully FLAC-like. It can be realised with the same subframe and residual-coding structure as FLAC.

### 7.4 Option (c): integer lifting wavelets

**Structure.** The 5/3 and 13/7 integer lifting wavelets were evaluated in an octave decomposition. Both are exactly reversible.

**Measured results** (Section 10.4):

- **Reversibility.** Perfect reconstruction, with 0 mismatching samples, on six extreme full-scale 24-bit signals and on real audio at 352.8 and 176.4 kHz.
- **Rate–distortion.** The comparison was made on the high-frequency layer above 22.05 kHz, under the quantiser of the lossy tier. It measures coding gain (Tables 10.4a and 10.4b). At every quantiser setting, both wavelets had a higher MSE than the 512-line floating-point MDCT:
  - 5/3: about 10 to 12 times the MSE of the MDCT, at a higher rate;
  - 13/7: about 1.6 to 1.9 times the MSE on the 352.8 kHz recording, at a similar or lower rate, and about 2.4 to 3.4 times on the 176.4 kHz recording, at a higher rate.

**Assessment.** Rejected.

### 7.5 Summary

| Option | Exact | Integer | Measured lossless cost | Coding gain | Status |
|---|---|---|---|---|---|
| (a) Integer MDCT, N = 512 | yes | yes | 110.3 % of the FLAC reference of Section 10.6 (prototype); Rice estimate +4.7 % vs FLAC level 8 | equal to the floating-point MDCT | candidate |
| (b) Time-domain LPC residual | yes | yes | about 103.7 % of the FLAC reference of Section 10.6 (estimate, side information not counted) | — | candidate; simplest |
| (c) Integer lifting wavelets 5/3, 13/7 | yes | yes | — | higher MSE than the MDCT at every tested setting (Section 10.4) | rejected |

### 7.6 Selection criteria and status

The final choice of coding option is **open** (item C1). The criteria are:

1. **Size.** The size relative to FLAC of the master, measured with side information included, against one stated FLAC reference (encoder, version and compression level).
2. **Simplicity.** Simplicity and closeness to FLAC. Option (b) is the simplest.
3. **Octave layers.** Compatibility with the critically sampled octave layers and their framing (items O1 and O3).
4. **Decoding speed.**

Stronger prediction is a separate research item (item C3). Its target is a size 3–6 % below FLAC.

The lossless tier may use a coding path different from that of the lossy tier inside the same container. One candidate combination uses different options for different layers, for example:

- time-domain coding for the depth plane groups;
- a transform for the octave layers.

---

## 8 Block classes and their constraints

### 8.1 Class code

Each band-block of a layer may carry a 2-bit class. A band-block is the block of one band over one time segment.

| Class | Meaning | Coded data |
|---|---|---|
| A | 0 bits: digital silence; every value in the block is exactly zero | none |
| B | Sample-depth tier, lowest | values, coded with the model set of B |
| C | Sample-depth tier, intermediate | values, coded with the model set of C |
| D | Sample-depth tier, highest | values, coded with the model set of D |

The mapping of B, C and D to concrete depth ranges is open (item K1).

### 8.2 Role

A class may steer:

- the segmentation of a layer into blocks;
- the grouping of values into entropy-coding groups, that is, the choice of model or parameter set.

The encoder may use the analysis tools of LAE Lossy to propose classes. These tools are the noise-floor estimation and the sustained-tone and transient detectors.

### 8.3 Constraints

1. **Classification never affects losslessness.** A class selects only how exact values are coded, never which values are reconstructed. Every class decodes to the exact values.
2. **Every class coder is total.** The coders of classes B, C and D accept every value that can occur in the layer, and every such value round-trips exactly. A value outside the typical range of its tier costs more bits but never precision. The mechanism, for example escape coding, is open (item K2).
3. **Class A is admissible only for exact zero.** The encoder may assign class A only to a block whose values are all exactly zero.
4. **Choice by trial encoding.** The encoder chooses the class of a block by trial encoding, selecting the cheapest class. A misjudged class can therefore cost only bytes.

### 8.4 Comparison with LAE Lossy

In LAE Lossy, the classes A, B, C and D are *precision* tiers. The class sets the quantisation step, and therefore the decoded result.

In LAE Lossless, the classes are *coding* tiers only. The decoded result is the same for every admissible class assignment.

---

## 9 Containers

### 9.1 Principles (shared with LAE Lossy)

The two tiers share the following container principles:

- The CD layer is the audio of the file. It is stored without any alteration.
- All extension data reside outside the PCM. They are placed in structures that legacy players ignore.
- One file carries all layers that are kept. Removing layers means removing their data.

### 9.2 FLAC

The audio frames of the FLAC stream carry the CD layer as an ordinary 16-bit FLAC stream. The extension is carried in FLAC APPLICATION metadata blocks. The relevant FLAC rules are:

- **Block layout.** An APPLICATION block consists of a 32-bit application ID followed by the data.
- **Block size.** The 24-bit length field of a metadata block limits each block to at most $2^{24} - 1$ bytes. A large extension is therefore split over several blocks.
- **Placement.** Metadata blocks precede the audio frames. All extension data therefore lie before the CD audio in the file.
- **Audio MD5.** The audio MD5 in STREAMINFO covers only the decoded CD audio and is unchanged by the extension.

The block layout and the application ID are open (item F1). This includes the splitting and ordering of layers over blocks, and any index that enables reading only the layers required. Section 10.7 gives the measured encapsulation result.

### 9.3 RIFF/WAV

The `data` chunk carries the CD layer as 16-bit PCM. The extension is carried in a separate RIFF chunk, and players ignore the unknown chunk.

LAE Lossy v5 uses the chunk ID `LAE ` with the magic `LAE1` ([SPEC.md](../lossy/SPEC.md) Sections 3–5). Whether LAES uses the same chunk, distinguished by tier and container version, or a separate chunk ID is open (item F2).

### 9.4 Legacy behaviour

Legacy players behave as follows:

- A FLAC decoder skips unknown APPLICATION blocks.
- A WAV player ignores unknown chunks.
- Both play the CD layer bit for bit.

An LAE Lossy v5 decoder refuses any container version other than 5 ([SPEC.md](../lossy/SPEC.md) Section 4.3). A future LAES container version is therefore not misread as lossy.

### 9.5 Integrity

Integrity is protected by **one hash over the whole track**. The hash algorithm is open (item F3). So is its coverage: the decoded master PCM, each kept rate and depth point, or the extension bytes.

One candidate is a hash of the decoded full-resolution master, analogous to the FLAC audio MD5. With it, a decoder can confirm bit-exactness end to end.

### 9.6 Tier identification

Every file states its tier.

- A lossy extension is always flagged as lossy.
- A file is identified as LAE Lossless only if every layer it contains is exact.

The field that carries the tier is open (item F2).

### 9.7 Media

The full lossless grid is about 3.7 times the raw size of the CD audio, so it does not fit on a CD. The intended carriers are Blu-ray data discs and files.

Whether CD-Extra carriage is offered for small subsets of the grid is open (item F4). An example of such a subset is CD + depth layer, with the CD layer in the audio session and the extension in the data session.

---

## 10 Measurements

All figures in this section were measured during development, unless they are marked as estimates. The recordings of Table 10.1 and the measurements of this section are not part of the published measurement data in [data/](../../data/).

**FLAC references.** Sizes are given relative to FLAC encodings of the masters. Two references occur:

- FLAC at compression level 8 (the spectral bound of Section 10.2 and the estimate of Section 10.5);
- in Section 10.6, a FLAC file of 21,996,137 bytes, recorded as compression level 12. The reference FLAC encoder offers compression levels 0 to 8 only; the encoder and version that produced this file are not documented here.

Percentages relative to these two references are not directly comparable. Where a percentage is given without a level (Sections 2.4 and 10.2), the level is not documented.

### 10.1 Material

**Table 10.1 — Material used in the measurements.**

| Material | Used for |
|---|---|
| Seven 352.8 kHz / 24-bit DXD recordings with 22 significant bits ($w = 2$), as 40 s stereo excerpts | Main set; Sections 10.2, 10.3 and 10.6 (one of the seven) |
| One further 352.8 kHz / 24-bit DXD recording with no unused low bits | Transform tests (Sections 10.4 and 10.5) |
| One 176.4 kHz / 24-bit recording | Transform tests (Sections 10.4 and 10.5) |

### 10.2 Sizes of the rate and depth points

The table gives the seven DXD recordings, with sizes as a percentage of the master's FLAC file.

| Version | Size |
|---|---|
| CD 44.1 kHz / 16 bit (dithered) | about 10 % |
| 44.1 kHz / 24 bit | about 23–28 % |
| 88.2 kHz / 24 bit | about 40–46 % |
| 96 kHz / 24 bit (for comparison only) | about 42–48 % |
| Full master, 352.8 kHz / 24 bit | 100 % |

The 96 kHz row is a comparison of a separately stored version. It is not a point of a 44.1 kHz-family LAES file, because LAE performs no rational-ratio conversion.

Storing these versions separately costs about 175 % of the master's FLAC file. A single layered file holding all of them is estimated at about 96–100 %.

A spectral bound on the achievable lossless rate lies only about 4–9 % below FLAC at compression level 8. FLAC already removes the two unused low bits of these masters.

### 10.3 Layer costs

The layer costs for one track of the seven are:

| Layer | Cost |
|---|---|
| CD16 | about 0.52 Mb/s |
| 16 → 24-bit depth layer | about 0.68 Mb/s |
| Ultrasonic octave layers | about 4.0 Mb/s |

The sum, about 5.2 Mb/s, is consistent with the full lossless file being about 3.7 times the raw CD audio rate of 1.41 Mb/s.

The bands above 88.2 kHz carry about 35 % of the bits. They consist of real noise and are never empty.

### 10.4 Transform tests

**Reversibility.**

| Transform | Extreme 24-bit full-scale signals (6) | Real audio, 352.8 kHz | Real audio, 176.4 kHz |
|---|---|---|---|
| Integer lifting wavelet 5/3 | 0 mismatches | 0 mismatches | 0 mismatches |
| Integer lifting wavelet 13/7 | 0 mismatches | 0 mismatches | 0 mismatches |
| Integer MDCT, N = 512 | 0 mismatches | 0 mismatches | 0 mismatches |

**Accuracy and speed of the integer MDCT.**

| Master | RMS / max. deviation from the floating-point orthonormal MDCT | Forward / inverse time, 40 s stereo |
|---|---|---|
| 352.8 kHz | 1.32 / 6.1 LSB | 0.7 / 0.6 s |
| 176.4 kHz | 1.31 / 6.5 LSB | 0.4 / 0.3 s |

**Coding gain under lossy quantisation.** The comparison uses the high-frequency layer above 22.05 kHz of 40 s stereo excerpts. The quantiser has the following properties:

- dead-zone scalar quantisation with θ = 0.30;
- groups of 32 lines;
- step $\Delta = c \cdot F$, where $F$ is a per-group noise floor. $F$ is the 10th percentile of the group RMS within segments of about 1 s, with a minimum of 0.3.

Rate is given in kb/s. MSE is the mean squared error per high-frequency sample, in squared 24-bit LSB.

*Table 10.4a — 352.8 kHz DXD recording.*

| c | 5/3 kb/s | 5/3 MSE | 13/7 kb/s | 13/7 MSE | MDCT-512 (floating point) kb/s | MSE | Integer MDCT kb/s | MSE |
|---|---|---|---|---|---|---|---|---|
| 2 | 892.8 | 706,232 | 694.6 | 98,964 | 676.7 | 63,123 | 676.9 | 63,105 |
| 3 | 501.9 | 1,322,147 | 317.8 | 201,807 | 320.4 | 115,772 | 318.8 | 115,739 |
| 4 | 275.5 | 1,805,318 | 129.8 | 295,802 | 147.5 | 160,459 | 145.5 | 160,442 |
| 6 | 97.2 | 2,281,269 | 36.5 | 406,308 | 42.9 | 218,842 | 41.9 | 218,790 |

*Table 10.4b — 176.4 kHz recording.*

| c | 5/3 kb/s | 5/3 MSE | 13/7 kb/s | 13/7 MSE | MDCT-512 (floating point) kb/s | MSE | Integer MDCT kb/s | MSE |
|---|---|---|---|---|---|---|---|---|
| 2 | 446.7 | 1,324,446 | 392.1 | 291,423 | 294.8 | 119,129 | 287.4 | 119,026 |
| 3 | 279.9 | 2,569,692 | 227.2 | 617,408 | 143.3 | 212,152 | 137.3 | 211,961 |
| 4 | 183.3 | 3,639,300 | 135.3 | 955,593 | 70.0 | 293,218 | 65.1 | 292,950 |
| 6 | 95.8 | 4,788,453 | 62.0 | 1,419,257 | 22.2 | 412,337 | 20.0 | 412,158 |

The integer MDCT matches the floating-point MDCT in rate–distortion. Both wavelets have a higher MSE than the MDCT at every setting (Section 7.4); this result motivated the rejection of option (c).

### 10.5 Lossless cost estimate of the integer MDCT

**Method.** Every coefficient of the integer MDCT is coded exactly. Each group of 16 coefficients is zig-zag mapped. The Rice parameter $k \in \lbrace 0, \ldots, 23 \rbrace$ that minimises the group's code length is chosen, and 5 bits per group are counted for $k$. No context modelling is used.

| Master | L/R (bit/sample) | M/S (bit/sample) | FLAC level 8, actual (bit/sample) |
|---|---|---|---|
| 352.8 kHz DXD | 9.673 | 9.679 | 9.237 |
| 176.4 kHz | 10.235 | 10.254 | 9.905 |

On the DXD recording, the estimate exceeds FLAC at level 8 by 4.7 %. M/S brings no gain on the full signal in this estimate.

### 10.6 Bit-exact prototype (difference model)

The prototype was run on one of the seven DXD recordings, 40 s stereo. It had three parts:

- **CD16.** The CD layer, coded as FLAC.
- **Depth residual.** The residual $r = b - 256c$, coded with the integer MDCT, with every coefficient exact.
- **Difference layer.** The master minus the CD path. The CD path was up-converted with the integer Q31 half-band tables, with mirrored edges (Section 6.6).

The decoded output equalled the master in every sample.

**Coding of the exact coefficients.** The exact coefficients were coded with an adaptive range coder developed for the lossy tier. It was configured with quantisation step 1, so that every coefficient was exact, in groups of 32 lines.

**Variants.**

1. L/R, with the unused low bits kept.
2. As 1, with the unused low bits removed and integer M/S applied to the difference. Removing the unused bits means reconstructing on the master's grid of multiples of 4 and shifting the difference right by 2 (Section 6.4).
3. As 2, with the difference layer instead costed by an order-32 time-domain linear predictor and a Laplacian entropy per partition. This variant is an estimate, and its side information is not counted.

The reference is a FLAC encoding of the master of 21,996,137 bytes, recorded as compression level 12 (see the note on FLAC references at the start of Section 10).

**Table 10.6 — Sizes of the prototype variants, in bytes.**

| Variant | CD16 (FLAC) | Depth residual | Difference layer | Total | % of the FLAC reference |
|---|---|---|---|---|---|
| 1 | 1,887,683 | 3,695,740 | 24,641,414 | 30,224,837 | 137.4 % |
| 2 | 1,887,683 | 3,695,740 | 18,674,558 | 24,257,981 | 110.3 % |
| 3 (estimate) | 1,887,683 | 3,695,740 | about 17,236,531 | about 22,820,000 | about 103.7 % |

Variant 2 combines two changes, the removal of unused bits and M/S. Their individual contributions were not separated.

**Interpretation.** The gap between variants 2 and 3 comes from the rounding noise of the integer MDCT, about 1.3 LSB per coefficient. This noise is of the same order as the very low ultrasonic noise floor of these masters. Time-domain prediction has no such rounding.

The prototype uses a full-rate difference layer, not the critically sampled octave layers of the design (Section 6.2).

### 10.7 Container encapsulation

In a test during development, an extension payload was wrapped into a FLAC file and into a WAV chunk. The FLAC file grew by 5,000,008 bytes. The results were:

- the audio MD5 of the CD layer was identical before and after wrapping, in both containers;
- the payload round-tripped intact.

---

## 11 Open items

**Coding**

- **C1** — Final coding option: (a) integer MDCT or (b) time-domain residual, or a combination per layer (Section 7.6).
- **C2** — Entropy coder and context design for the chosen option, measured with side information included.
- **C3** — Stronger prediction, targeting 3–6 % below FLAC. This is a separate research item.
- **C4** — Stereo decorrelation: fixed per layer, or adaptive per block.

**Depth layer**

- **D1** — Truncation or rounding at the 18-bit and 20-bit levels (Section 5.2).
- **D2** — The exact form of the CD-path prediction $p$ in plane group P1, including whether dither compensation is used (Section 5.3).
- **D3** — Output limiting of the base-rate 24-bit level when $b$ exceeds the 24-bit range (Section 4.3).

**Octave layers**

- **O1** — The integer realisation of critically sampled octave layers that keeps every partial reconstruction exact (Section 6.7).
- **O2** — The reference signals of intermediate rate points, for example 88.2 kHz / 24 bit (Sections 3.5 and 6.7).
- **O3** — Octave-layer framing: time segmentation, signalling of unused low bits, subdivision of octave columns by depth, and preservation of the independence of octave layers from the depth plane groups (Sections 3.2, 3.4, 6.4 and 6.7).

**Classes**

- **K1** — The mapping of classes B, C and D to depth ranges, and the size of the band-blocks.
- **K2** — The totality mechanism of the class coders, for example escape coding.

**Containers**

- **F1** — FLAC APPLICATION block layout, application ID, splitting over blocks and any index.
- **F2** — RIFF chunk ID and the tier/version field: a shared or a separate chunk.
- **F3** — Whole-track hash algorithm and coverage.
- **F4** — CD-Extra carriage of small subsets of the grid.
- **F5** — The `.lae` file layout, shared with LAE Lossy.

**Scope**

- **S1** — Channel configurations other than stereo.
- **S2** — Admission of masters at the base rate, with depth axis only.

**Specification**

- **N1** — The normative specification, conformance vectors and reference decoder.

---

## 12 Relationship to LAE Lossy

| Aspect | LAE Lossy (LAEY) | LAE Lossless (LAES) |
|---|---|---|
| CD layer | Derived by integer half-band decimation and adaptive seeded TPDF dither, or independent CD | identical ([SPEC.md](../lossy/SPEC.md) Sections 10 and 11.2) |
| Layer split | Depth layer at the base rate; frequency layer at the master rate | Depth layer at the base rate; octave layers by rate ratio |
| Depth layer | Coded approximation of $\mathrm{LP}_{f_c}(b - v)$ with leakage compensation; precision rules 1 and 3 | $b$ exactly, in plane groups P1/P2/P3 at fixed bit positions |
| High-rate layers | One MDCT at the master rate, spectral-line segments per octave, starting 1 kHz below the detected CD cut-off | Critically sampled octave layers $B \cdot 2^{k-1} \ldots B \cdot 2^k$, exact; realisation and framing open (items O1–O3) |
| Coding | Dead-zone quantiser, precision tiers, reconstruction offset, optional zero level, range coder | Exact coding; option (a) or (b) open |
| Classes A/B/C/D | Precision tiers that determine the decoded result | Coding tiers only; the decoded result is independent of the class |
| Schemes and modes | 2 schemes × 6 modes | None that affect the result |
| Partial precision within a layer | Precision varies with the block peak | None; layers are complete or absent |
| Decoder arithmetic | Integer filters; inverse MDCT in IEEE-754 binary64 in fixed order, with a start-up self-test | Integer only, for all candidate options |
| Stereo | M/S (frequency layer), L/R (depth layer) | Integer M/S available; choice open |
| Analysis tools | Noise floor, tone and transient detectors, closed loop | Same tools; they may steer classes and segmentation only |
| Container | RIFF chunk `LAE ` (v5); FLAC planned | FLAC APPLICATION blocks or a RIFF chunk; layouts open |
| Integrity | CRC-32 over the extension chunk | One whole-track hash |
| Labelling | Always flagged as lossy; never called lossless | Lossless; every layer exact |

The tools of LAE Lossy that exist only to manage quantisation error have no role in the lossless tier. These tools are:

- the precision rules and the 16-bit backstop;
- the dead zone and the reconstruction offset;
- the optional zero level;
- leakage compensation;
- mode-driven demotion.

Two decoder paths result:

- **LAE Lossy.** A deterministic floating-point inverse with one rounding at the output.
- **LAE Lossless.** Integer-reversible inverses whose roundings cancel exactly.

Both tiers can be carried in the same container.

---

## References

- **[SPEC]** LAE Lossy specification, [spec/lossy/SPEC.md](../lossy/SPEC.md), draft for public review, container version 5.
- **[LAEY]** LAE: A Layered High-Resolution Extension of the Compact Disc. LAE Lossy working paper, container version 5, [README.md](../../README.md).
- **[FLAC]** FLAC, Free Lossless Audio Codec. Format specification (metadata blocks, APPLICATION block, STREAMINFO MD5).
- **[RIFF]** Resource Interchange File Format / WAVE.
- **[CDX]** CD-Extra / Enhanced CD ("Blue Book"), 1995.
