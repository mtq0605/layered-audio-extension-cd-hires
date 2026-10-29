<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: 2026 mtq0605 -->

# LAE Lossless (LAES): File Format and Decoding Specification, Version 1

**English** | [中文](SPEC.zh.md)

**Status:** draft for public review, 2026-10-29.
**Applies to:** LAE Lossless (short name LAES), the lossless tier of the LAE (Layered Audio Extension) format family, payload version 1.
**Licence of this document:** CC BY 4.0. (c) 2026 mtq0605.

This document specifies the LAES payload, its three carriers (WAV file, FLAC file, CD-Extra disc) and the decoding process. It is written so that an independent implementer can write a conforming decoder without other material. It replaces the informative design document `spec/lossless/DESIGN.md` (2026-10-18) as the normative description; where the two differ, this document applies.

Sections 11 and 12, the appendices marked informative, and all text marked *informative* are not normative. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

The reference implementation is normative wherever this text is silent or ambiguous. Its files are:

| File | Content |
|---|---|
| `src/laes_math.h` | normative arithmetic: 128-bit accumulation, symmetric extension, rate-level lifting, reference taps and reference |
| `src/halfband.h` | the half-band table `kHalf0` used by the lifting (shared with LAE Lossy) |
| `src/laes_codec.h` | layer coder (Section 5) |
| `src/laes_file.h` | LAES payload, encoder and decoder (Sections 6 and 7) |
| `src/laes_flac.h` | FLAC reader and writer for the FLAC form (Section 8) |
| `src/lae_cdx.h` | CD-Extra extension file and trailer record (Section 9) |
| `src/laes_tool.cpp` | command-line tool `laes`: encoder choices, decoder outputs, derived 16-bit version (Sections 10 and 11) |

`src/laes_lift.h` contains faster forms of the lifting and the reference. They give identical results (verified against `src/laes_math.h` on test recordings during development) and are informative.

---

## 1. Scope and conformance

### 1.1 What LAES is

LAES stores, in one file or on one disc:

* a **core**: two-channel integer PCM at a **base rate** of 44 100 Hz or 48 000 Hz, 16, 24 or 32 bits. An ordinary player plays the core unchanged;
* one to sixteen **masters**: two-channel integer PCM of 16, 24 or 32 bits, each at the base rate × 1, 2, 4 or 8. Masters are independent of each other: each may come from a different production (for example a 16-bit master at 88.2 kHz and a 24-bit master at 352.8 kHz of the same piece).

Every master is restored **bit for bit**. From each master the decoder also obtains its **rate levels**, the master at its rate / 2, / 4 and / 8 down to the base rate, by the lifting of Section 3. The rate levels are exactly defined integer signals; they are not stored separately.

LAES is lossless throughout. Nothing in the payload is approximated. A master is stored as the difference to another signal already in the file (the core or an earlier master), or in full, whichever the encoder finds smaller (Section 6.4).

### 1.2 Carriers

| Carrier | Core | LAES payload | Section |
|---|---|---|---|
| **WAV form** | the `data` chunk of a RIFF/WAVE file | a chunk `LAE ` (with a trailing space) in the same file | 7 |
| **FLAC form** | the audio frames of a FLAC file | one or more APPLICATION metadata blocks with ID `LAE2` | 8 |
| **CD-Extra** | the audio tracks of session 1 (16-bit, 44 100 Hz) | one extension file per track in the data session | 9 |

All three carriers hold the same payload (Section 6). Only the place of the core differs.

### 1.3 Conformance

A conforming decoder:

1. MUST check the payload as specified in Section 6.7 and the carrier as specified in Sections 7 to 9;
2. MUST reproduce every master exactly as specified in Section 6.6;
3. MUST compare each reconstructed master with its SHA-256 (Section 6.5) and MUST NOT present a master whose hash does not match as that master;
4. MUST, when the payload is missing, damaged or fails a check, still play the core unchanged ("core only");
5. MAY produce any of the outputs of Section 10.

A conforming encoder writes payloads that a conforming decoder accepts and that reproduce the encoder's input masters bit for bit. The choices left to the encoder are listed in Section 11.

### 1.4 Terms

| Term | Meaning |
|---|---|
| base rate | 44 100 Hz (CD family) or 48 000 Hz (DVD family) |
| core | the signal an ordinary player plays (Section 1.1). On a CD or DVD it is what LAE Lossy calls the CD layer; in the FLAC form it may also be 24 or 32 bits |
| stages | for a master at rate base × 2^S, the number S (0 to 3) |
| level k | for a master with S stages, the signal at rate base × 2^k, 0 ≤ k ≤ S. Level S is the master itself (after removing unused low bits, Section 6.3); level 0 is its **base level** |
| octave layer k | the detail signal produced when level k is split into level k − 1 (Section 3) |
| tier | a signal a master is stored against: the core, or an earlier master in the payload |
| T | the **time position** of a master relative to its tier, in base-rate samples, as an integer part and a fraction in units of 2^−32 base-rate sample |
| w | the number of low bits that are zero in every sample of a master ("unused low bits"); w = 0 if every sample is 0 |

### 1.5 Design properties (informative)

* No index, seek table, cue table or table of layer offsets exists anywhere in the payload or its carriers. Time positions are known only by decoding.
* The payload is stored as one whole block. It is not interleaved with the core.
* Each master has exactly one storage choice and one tier for its whole length. No weights or gains are stored.
* The two channels are always coded separately: the left channel of a master is reconstructed only from left-channel signals, the right only from right-channel signals.

---

## 2. Notation and arithmetic

### 2.1 Integers

* `int64` and `uint64` denote 64-bit two's-complement signed and unsigned integers; similarly for 8, 16 and 32 bits.
* All multi-byte quantities in the payload, in WAV chunks and in the CD-Extra trailer are **little-endian**. FLAC structures follow RFC 9639 (big-endian bit order).
* `a >> s` on a signed value is an arithmetic shift (floor division by 2^s).
* **Exact sums.** Where this specification writes a sum of products, the sum MUST be formed exactly. A 128-bit two's-complement accumulator is sufficient for every sum in Sections 3 and 4.
* **Wrap-around.** Every value that Sections 3, 4.2, 5.1 and 6.6 produce (a prediction, a reconstructed sample, a lifting output, a reference value, `x << w`) is taken modulo 2^64 as int64 (two's complement). A correct payload never needs a wider value; a payload that would can only fail its SHA-256.
* **Rounding.** For an exact integer A and s ≥ 1, `round_s(A) = (A + 2^(s−1)) >> s`, computed on the exact value. This rounds A / 2^s to the nearest integer, ties towards +∞.
* **Symmetric extension.** For a signal x of length n ≥ 1 and any integer index i:
  `m(i, n) = 0` if n = 1; otherwise, with p = 2(n − 1), `j = i mod p` (0 ≤ j < p), `m(i, n) = j` if j < n, else `p − j`.
  `x[m(i, n)]` is the value of x at index i extended by whole-sample mirroring. Indices are 64-bit.
* `levLen(n, k)`: n halved k times, rounding up each time (`n = (n + 1) / 2`, k times).

### 2.2 Floating point (Section 4.1 only)

The reference taps are computed in IEEE-754 binary64. Every `+ − × ÷` and `sqrt` MUST be rounded individually (round to nearest, ties to even), in the order written. Fused multiply-add, extended-precision intermediates and re-ordering MUST NOT be used. Library functions other than `sqrt` and `floor` MUST NOT be used. *Informative:* MSVC `/fp:strict`, GCC/Clang `-ffp-contract=off` without `-ffast-math`.

---

## 3. Rate levels: the lifting L

### 3.1 One stage

One stage splits a signal x of length n into a low part s (length ⌈n / 2⌉, the next lower level) and a detail d (length ⌊n / 2⌋, the octave layer):

```
xe[i] = x[2i]          0 ≤ i < ⌈n/2⌉
xo[i] = x[2i + 1]      0 ≤ i < ⌊n/2⌋
d[i]  = xo[i] − P(xe, i)
s[i]  = xe[i] + U(d, i)

P(xe, i) = round_30( Σ_{t=0..39} H0[t] · ( xe[m(i − t, ne)] + xe[m(i + 1 + t, ne)] ) )          ne = ⌈n/2⌉
U(d, i)  = round_31( Σ_{t=0..59} U0[t] · ( d[m(i − 1 − t, no)] + d[m(i + t, no)] ) )            no = ⌊n/2⌋; U = 0 if no = 0
```

H0 is the table `kHalf0` and U0 the table `kU0` (Appendix A). Each product is formed separately; the sum is exact (Section 2.1).

The inverse stage, given s, d and n, is:

```
xe[i]     = s[i] − U(d, i)
x[2i]     = xe[i]
x[2i + 1] = d[i] + P(xe, i)
```

It reproduces x exactly.

### 3.2 All levels of a master

For a master with S stages, after removing the unused low bits (Section 6.3), level S is the master. For k = S down to 1, one stage applied to level k gives level k − 1 and octave layer k. So:

* level k has `levLen(F, S − k)` samples per channel, where F is the master's frame count;
* octave layer k has `⌊levLen(F, S − k) / 2⌋` samples per channel.

---

## 4. The reference: time position and word length

A master (or one of its levels) is stored as the difference to a **reference**: a tier signal moved to the master's time position and scaled to the master's word length.

### 4.1 Reference taps

For a fraction f ∈ [0, 1) of a sample (given as an unsigned 32-bit value F, f = F / 2^32), the 256 reference taps h[q], q = −127 … 128, are computed as follows (binary64, Section 2.2):

```
PI    = 3.141592653589793115997963468544185161590576171875       (the binary64 value nearest to π)
sin(x) for |x| ≤ π/2:  y = x·x; s = 1; for k = 15 down to 1: s = 1 − s·y / ((2k)·(2k + 1)); result x·s
I0(x):                 y = x·x / 4; term = 1; s = 1; for k = 1 to 80: term = term·y / (k·k); s = s + term; result s

g   = f if f ≤ 0.5, else 1 − f
sf  = sin(PI·g)
i0b = I0(14)
for q = −127 … 128:
    u = f − q
    sinc = 1 if u = 0, else (q even ? +1 : −1) · sf / (PI·u)
    r = u / 128
    w = I0(14 · sqrt(1 − r·r)) / i0b   if −1 < r < 1, else 0
    h[q] = floor(sinc · w · 2^31 + 0.5)                              (as int64)
c = the smallest q with the smallest |u| (q scanned upwards, strict comparison; matters for f = 0.5)
h[c] = h[c] + 2^31 − Σ_q h[q]
```

The taps are integers; their sum is exactly 2^31. For f = 0, h[0] = 2^31 and every other tap is 0.

### 4.2 Reference values

Let `src` be the tier signal at the relevant level (length n), `W` the integer part of the time position and F its fraction, both in samples of that level, and `up` the grid difference (Section 6.6). The reference has the length of the master's signal at that level (`nOut` values):

```
A_i     = Σ_{q=−127..128} h[q] · src[m(i + W + q, n)]          exact
ref[i]  = round_{31−up}(A_i)            if 31 − up > 0
ref[i]  = A_i · 2^(up−31)               if 31 − up ≤ 0 (taken modulo 2^64 as int64)
```

*Informative.* `up` is the number of extra significant bits the master has over the tier. It is negative when the master has fewer bits; the reference then rounds the tier down to the master's word length.

### 4.3 Time position at level j

A time position T = W + F / 2^32 base-rate samples is, at level j (rate base × 2^j), T · 2^j samples of that level:

```
W_j = W · 2^j + (F >> (32 − j))        for j ≥ 1;   W_0 = W
F_j = (F · 2^j) mod 2^32
```

---

## 5. Layer coder

Every stored signal (base level, level difference or octave layer, one channel at a time) is coded by the layer coder into a byte string. Bits are written most significant first. A **layer** of n samples is a sequence of **blocks** of length B (the last block takes the remainder), written back to back with no byte alignment between blocks; the layer ends with zero bits up to the next byte boundary.

### 5.1 Block

| Field | Bits | Meaning |
|---|---|---|
| type | 2 | 0 = constant, 1 = verbatim, 2 = fixed predictor, 3 = LPC |
| W | 6 | width of the stored constant, verbatim and warm-up values (W-bit two's complement); 1 ≤ W ≤ 62. An encoder chooses W so that every sample of the block fits; a decoder does not check decoded samples against W |

Then, by type, for a block of n samples x[0 … n−1]:

* **constant:** one W-bit value v; x[i] = v.
* **verbatim:** n W-bit values.
* **fixed:** order o (3 bits, 0 ≤ o ≤ 4, o < n); o warm-up samples (W bits each); residual of n − o values. Allowed only if W ≤ 58. Prediction (as in FLAC): o = 0: 0; 1: x[i−1]; 2: 2x[i−1] − x[i−2]; 3: 3x[i−1] − 3x[i−2] + x[i−3]; 4: 4x[i−1] − 6x[i−2] + 4x[i−3] − x[i−4].
* **LPC:** order − 1 (5 bits; order o = 1 … 32, o < n); precision − 1 (4 bits; p = 1 … 16); shift (5 bits; 0 … 31); o coefficients c[0 … o−1] (p-bit two's complement each); o warm-up samples (W bits each); residual of n − o values. Allowed only if W ≤ 44. Prediction: `pred_i = floor( Σ_{j=0..o−1} c[j] · x[i−1−j] / 2^shift )` with the exact sum (for shift = 0 the sum itself).

Reconstruction: x[i] = residual[i] + pred_i for i ≥ o.

### 5.2 Residual

| Field | Bits | Meaning |
|---|---|---|
| partition order po | 4 | 0 ≤ po ≤ 8, 2^po ≤ m (m = number of residual values) |

The m values are split into 2^po partitions: partitions 0 … 2^po − 2 have ⌊m / 2^po⌋ values each; the last has m − (2^po − 1) · ⌊m / 2^po⌋. Each partition has:

* a 6-bit parameter k. For k = 0 … 62 the values are Rice-coded: u = 2v for v ≥ 0, u = −2v − 1 for v < 0; the quotient q = u >> k is written as q zero bits followed by a one bit, then the k low bits of u. The value u MUST fit in 64 bits.
* k = 63 is an escape: a 6-bit width r (1 … 63) follows, then every value as r-bit two's complement.

All side information is fixed-length.

### 5.3 Block lengths

| Signal | Block length B |
|---|---|
| base level (storage 1) and base-level difference (storage 0) | 4096 |
| level-j difference (storage 2) | 4096 · 2^j |
| octave layer k | 4096 · 2^(k−1) |

### 5.4 Checks

A decoder MUST reject a layer (Section 6.7) when a field is outside the ranges above, when a predictor type is used with a W above its limit, when the order is not smaller than the block length, or when the data ends early. Bytes of the stored layer length after the last block, and the padding bits, are not checked; a decoder MUST ignore them.

---

## 6. The LAES payload

The payload is a byte string followed by its CRC-32 (4 bytes). The CRC-32 is the IEEE 802.3 CRC (reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF) over every payload byte before it.

### 6.1 Header (64 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `LAE2` |
| 4 | 2 | version | 1 |
| 6 | 2 | header size | 64 |
| 8 | 4 | base rate | 44 100 or 48 000 |
| 12 | 1 | core bits | 16, 24 or 32 |
| 13 | 1 | channels | 2 |
| 14 | 1 | number of masters M | 1 … 16 |
| 15 | 1 | core mode | 0 = core in a WAV `data` chunk or on CD audio tracks; 1 = core in FLAC audio frames |
| 16 | 8 | core frames | number of core samples per channel |
| 24 | 32 | core SHA-256 | Section 6.5 |
| 56 | 8 | reserved | 0 |

### 6.2 Master record (72 bytes), followed by its layers

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | bits | 16, 24 or 32 |
| 1 | 1 | stages S | 0 … 3 (rate = base × 2^S) |
| 2 | 1 | w | unused low bits, w < bits |
| 3 | 1 | storage | 0, 1 or 2 (Section 6.4) |
| 4 | 1 | reference tier | 0 = core; k = the k-th master of this payload (1-based) |
| 5 | 3 | reserved | 0 |
| 8 | 4 | rate | base × 2^S |
| 12 | 4 | reserved | 0 |
| 16 | 8 | frames F | samples per channel, ≥ 1 |
| 24 | 8 | T whole | int64, integer part of T (base-rate samples, relative to the tier) |
| 32 | 4 | T fraction | uint32, units of 2^−32 base-rate sample |
| 36 | 4 | reserved | 0 |
| 40 | 32 | master SHA-256 | Section 6.5 |

The masters follow each other in payload order: record of master 1, its layers, record of master 2, its layers, and so on. After the last master's layers comes the CRC-32. A master's tier MUST be the core or a master that comes earlier in the payload.

### 6.3 Layers of a master

Let j = 0 for storage 0 and 1, and j = min(S, S_t) for storage 2, where S_t is the stages of the tier master. For each channel, left first, then right:

1. a uint32 byte length and the layer coding (Section 5) of:
   * storage 0 or 2: the **difference** at level j, `levLen(F, S − j)` values: the master's level j minus the reference (Section 6.6);
   * storage 1: the master's base level itself, `levLen(F, S)` values;
2. for each k = j + 1 … S: a uint32 byte length and the layer coding of octave layer k.

The samples of a master are taken **after removing its unused low bits**: `x = sample >> w`. The levels and octave layers of Section 3 are computed from x.

### 6.4 Storage values

| Storage | Reference tier | Stored at level | Meaning |
|---|---|---|---|
| 0 | core or any earlier master | j = 0 | difference to the tier at the base level |
| 1 | 0 (none) | j = 0 | base level stored in full |
| 2 | an earlier master | j = min(S, S_t) | difference to the tier master at the highest level both have; only the octave layers above j are stored |

### 6.5 Hashes

* **Master SHA-256:** over the master's samples in file order L, R, L, R, …, each sample as its int32 two's-complement value in 4 bytes little-endian.
* **Core SHA-256:** for a 16-bit core, over the samples as int16 little-endian, L, R, L, R, … (the byte content of a 16-bit `data` chunk). For a 24- or 32-bit core, as for a master (int32 little-endian).

### 6.6 Reconstruction

For each master m in payload order, and each channel separately:

```
j  = 0 (storage 0, 1) or min(S, S_t) (storage 2)
x_j = decoded first layer                               (levLen(F, S − j) values)
if storage ≠ 1:
    tier signal src = the core channel (tier 0), or level j of tier master t (tier t ≥ 1)
    up  = (bits − w) − (core bits)                       for tier 0
          (bits − w) − (bits_t − w_t)                    for a tier master t
    (W_j, F_j) = T at level j (Section 4.3)
    x_j = x_j + reference(src, W_j, F_j, up)             (Section 4.2, nOut = levLen(F, S − j))
for k = j + 1 … S:
    x_k = inverse stage(x_{k−1}, octave layer k, levLen(F, S − k))
master sample = x_S << w
```

Level j of a tier master t is obtained from its reconstructed samples: x_t = sample >> w_t is its level S_t; one stage per step (Section 3.1) gives the lower levels.

### 6.7 Checks

A decoder MUST reject the payload (and play the core only) if any of the following holds:

* the CRC-32 does not match;
* magic, version (1) or header size (64) differ; the base rate is not 44 100 or 48 000, or differs from the carrier's core rate; the core bits differ from the carrier's core; channels ≠ 2; core mode > 1; M is not 1 … 16;
* core frames differ from the carrier's core length, or the core SHA-256 differs;
* for a master: bits not 16, 24 or 32; S > 3; w ≥ bits; storage > 2; rate ≠ base × 2^S; F < 1; `levLen(F, S)` > core frames + 2^24; |T whole| ≥ 2^40. Core frames and F are uint64 and MUST be compared without truncation; a decoder that cannot hold a master of F samples MUST reject the payload rather than truncate;
* storage 0 or 2 with a tier that is not earlier in the payload; storage 2 with tier 0; storage 1 with a tier other than 0;
* a layer fails Section 5.4, or the payload ends early;
* bytes remain between the last master's layers and the CRC-32.

A master whose reconstruction does not match its SHA-256 MUST NOT be presented as that master, nor its rate levels. The core and the other masters are unaffected, except masters whose chain of tiers includes it: they then also fail their SHA-256.

Reserved fields: writers MUST write 0. Readers MUST ignore them. The core mode is not compared with the carrier.

---

## 7. WAV form

An LAES WAV file is a RIFF/WAVE file:

* `fmt `: integer PCM (format tag 1, or 0xFFFE with the PCM sub-format), 2 channels, the base rate, 16, 24 or 32 bits;
* `data`: the core;
* `LAE `: the LAES payload followed by its CRC-32 (Section 6), core mode 0.

A file MUST contain exactly one `data` chunk and at most one `LAE ` chunk. *Informative:* the reference reader takes the last of each and clamps a chunk size that runs past the end of the file.

Any WAV player plays the core. The first 4 bytes of the chunk name the format: `LAE1` = LAEY (LAE Lossy), `LAE2` = LAES (this specification). A reader SHOULD tell the user which format it found; an LAEY decoder that meets `LAE2` reports an LAES file and does not decode it, and a player plays the core. A WAV file whose single `data` chunk would exceed 4 GiB is outside version 1.

---

## 8. FLAC form

An LAES FLAC file is a FLAC stream (RFC 9639):

```
"fLaC" | STREAMINFO | APPLICATION blocks with ID "LAE2" (k ≥ 1) | other metadata blocks (optional) | audio frames = the core
```

* STREAMINFO: 2 channels, the base rate, 16, 24 or 32 bits, the core's sample count (MUST NOT be 0) and the MD5 of the core (MUST be given and correct). A decoder that finds the MD5 missing or wrong MUST NOT use the payload: the core is not the one the payload was made for. Whether it still plays the core is up to the player.
* The LAES payload (with its CRC-32, core mode 1) is split into pieces of at most 2^24 − 5 bytes. Each piece is the data of one APPLICATION block whose 4-byte ID is `LAE2` (0x4C 0x41 0x45 0x32). Writers place these blocks directly after STREAMINFO. A reader concatenates every APPLICATION block with ID `LAE2`, in file order, wherever it stands among the metadata blocks.
* The audio frames hold the core. Any valid FLAC coding of the core is allowed.
* No SEEKTABLE and no CUESHEET block is written.

Every FLAC decoder plays the core and skips the APPLICATION blocks.

*Informative.* The reference writer (`src/laes_flac.h`) uses a fixed block size of 4096, two independently coded channels, LPC orders up to 12, precision up to 15 and Rice partition orders up to 8. At 44.1 / 48 kHz this is FLAC's streamable subset. Many older players do not play 32-bit FLAC; the reference encoder therefore uses a 24-bit core unless asked otherwise (Section 11.1). The reference reader decodes only cores with independently coded channels. The application ID `LAE2` is provisional until it is registered with IANA (RFC 9639, Section 12.2).

---

## 9. CD-Extra

On a CD-Extra disc made with the LAE CD-Extra encapsulation (`spec/cdx/SPEC.md`), each track's extension file (`.LAX`) is:

```
LAES payload with its CRC-32 (core mode 0) | 80-byte trailer record
```

The trailer record is that of `spec/cdx/SPEC.md`, Section 3.2, with **extension block type 2**. A reader MUST check that the first 4 bytes of the extension block are `LAE2` for type 2 (and `LAE1` for type 1) and MUST otherwise reject the extension and play the CD track only. All other rules of the CD-Extra specification (whole-track MD5, sliding checksums, rip-offset search, the order of checks) apply unchanged. The core is the CD track (16-bit, 44 100 Hz).

---

## 10. Decoder outputs

A decoder offers, for each file:

| Output | Lossless | Definition |
|---|---|---|
| the core | yes | as stored |
| each master | yes | Section 6.6 |
| each rate level of a master | yes | level k (Section 3.2), samples `x_k << w`, written at the master's word length; a value outside that word length is limited to it on output |
| a derived 16-bit version | **no** | Section 10.1 |

### 10.1 Derived 16-bit version

For a rate at which the file offers no 16-bit output, a decoder MAY offer a 16-bit version derived from the output with the most bits at that rate (b bits, b > 16, s = b − 16). The outputs counted are the core, every master whose SHA-256 matched, and their rate levels (after limiting). On a tie the first in this order is used: the core; then, for each master in payload order, the master and its rate levels from high rate to low.

```
per channel, a xorshift64 generator with state st = 20261029 + 0x9E3779B97F4A7C15 · (channel + 1)  (mod 2^64; channel 0 = left)
next():  st ^= st << 13;  st ^= st >> 7;  st ^= st << 17;  return st >> (64 − s)
for each sample v in order:  r1 = next(); r2 = next();  y = clamp((v + r1 − r2 + 2^(s−1)) >> s, −32768, 32767)
```

The result is a new 16-bit master (TPDF dither of ±1 LSB), **lossy** against its source. A decoder MUST label it as derived and lossy wherever it offers it. It is the same on every run and every platform.

---

## 11. Encoder (informative)

### 11.1 Core

The core is either given (for example a released CD or DVD) or derived from a master's base level at a chosen word length. The reference encoder uses 16 bits for the WAV form and, for the FLAC form, the highest master word length up to 24 bits. The source master is one whose word length equals the core's (its base level is then the core exactly), otherwise the longest one, TPDF-dithered down.

### 11.2 Storage, tier and order

The reference encoder tries, for each master: storage 1; storage 0 against the core and against every earlier master; storage 2 against every earlier master when both have a level above the base. It keeps the smallest, once per master. It writes the masters in the smaller of two orders: longer word lengths first, then higher rates first; or, when the core was derived from a master that is not already first, that master first and the others as before. A master can only refer to earlier masters, and a shorter word length refers to a longer one cheaply.

### 11.3 Time position

A master derived from the same production as the core has T = 0. For an independently produced core (a released CD), the reference tool `laes align` uses the automatic search of LAE Lossy's encoder and stores the measured position as T, fraction included; the master itself is never moved. An alignment error within −60 dB passes. Otherwise the error is shown and the user decides to continue or stop. A whole album is cut at the CD's track boundaries by the LAE Lossy planner (`lae_enc plan … laes`), which writes each track's master unmoved with its own T.

### 11.4 Measured sizes

Measured on 10-second excerpts: two 2L test recordings at 352.8 kHz (02 and 05), and one recording at 192 kHz for the 48 kHz family. Each comparison is with `flac -8`.

| Content | LAES FLAC form, compared with |
|---|---|
| one 24-bit master with all its rate levels | 93.5–98.4 % of a FLAC of that master alone |
| 16-, 24- and 32-bit masters of one mix at 352.8 kHz, each with all four rate levels (12 versions) | 99.6–99.8 % of a FLAC of the 32-bit master alone |
| the same 12 versions | about 30 % of 12 separate FLAC files |

---

## 12. Conformance testing (informative)

`laes selftest` runs, with synthetic signals only:
* round trips over both carriers, all word lengths, both rate families, fractional and negative T, unused low bits, short and odd lengths, and all three storage values;
* damage tests: byte flips, flips behind a recomputed CRC, and truncation of the payload and of the FLAC file;
* random input to the FLAC parsers.

The fast forms (`src/laes_lift.h`, the fast paths in `src/laes_codec.h`) were compared with the normative forms on test recordings during development; every result was identical.

---

## Appendix A. Tables

`H0` = `kHalf0` (`src/halfband.h`), 40 values, Q31:

```
682938002, -225979149, 133607419, -93347908, 70491357, -55579122, 44978765, -36999032, 30747123, -25707634,
21562088, -18102637, 15187220, -12714693, 10610315, -8816912, 7289329, -5990851, 4890848, -3963205,
3185274, -2537151, 2001191, -1561677, 1204578, -917388, 688994, -509581, 370536, -264370,
184638, -125854, 83405, -53473, 32940, -19306, 10605, -5322, 2321, -771
```

`U0` = `kU0` (`src/laes_math.h`), 60 values, Q31:

```
683270116, -226970762, 135242270, -95599562, 73323768, -58947533, 48830814, -41276037, 35385490, -30640348,
26720250, -23416981, 20589580, -18139377, 15995343, -14105100, 12429177, -10937236, 9605506, -8415006,
7350282, -6398504, 5548802, -4791782, 4119170, -3523546, 2998153, -2536758, 2133556, -1783104,
1480274, -1220234, 998431, -810587, 652702, -521052, 412202, -322998, 250575, -192354,
146076, -109516, 81159, -59401, 42896, -30529, 21386, -14723, 9943, -6573,
4241, -2663, 1619, -948, 531, -281, 138, -61, 23, -6
```

## Appendix B. Changes against the design document of 2026-10-18 (informative)

* The rate levels use the lifting L of Section 3 (fixed tables, exact inverse).
* Masters are stored as differences to a tier (core or earlier master) or in full. Storage 2 (difference at the highest shared level) is new.
* The time position T replaces any moving of masters: masters are stored unmoved.
* The core may be 16, 24 or 32 bits. The FLAC form and the CD-Extra extension type 2 are specified.
* Decoder outputs include a derived 16-bit version, labelled lossy.
