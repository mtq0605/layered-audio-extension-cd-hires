<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: 2026 mtq0605 -->

# LAE Lossless (LAES): File Format and Decoding Specification, Version 1

**English** | [中文](SPEC.zh.md)

**Status:** Stable, payload version 2, 2026-11-12 (payload version 1: 2026-11-07).
**Applies to:** LAE Lossless (short name LAES), the lossless tier of the LAE (Layered Audio Extension) format family, payload versions 1 and 2.
**Licence of this document:** CC BY 4.0. (c) 2026 mtq0605.

This document specifies the LAES payload, its three carriers (WAV file, FLAC file and CD-Extra disc) and the decoding process. It aims to let an independent implementer write a conforming decoder without other material. It replaces the design document `spec/lossless/DESIGN.md` (2026-10-18), which is informative: this document is the normative description, and where the two differ, this document applies.

Sections 11 and 12, the appendices marked informative, and all text marked *informative* are not normative. The key words MUST, MUST NOT, SHOULD and MAY are to be interpreted as described in RFC 2119.

The reference implementation is normative wherever this text is silent or ambiguous. It consists of the following files:

| File | Content |
|---|---|
| `src/laes_math.h` | normative arithmetic: 128-bit accumulation, symmetric extension, rate-level lifting, reference taps and the reference |
| `src/halfband.h` | the half-band table `kHalf0` used by the lifting (shared with LAE Lossy) |
| `src/laes_codec.h` | the layer coder (Section 5) |
| `src/laes_file.h` | the LAES payload and its decoder (Sections 6 and 7) |
| `src/laes_flac.h` | the FLAC reader for the FLAC form (Section 8) |
| `src/lae_cdx.h` | the CD-Extra extension file and trailer record (Section 9) |
| `src/laes_tool.cpp` | the command-line tool `laes`: decoder outputs and derived 16-bit master (Section 10); its encoder choices (Section 11) are informative |
| `src/laes_choices.h` | the list of entries a file offers, their order and the text shown (Section 10.3) |

`src/laes_lift.h` contains faster forms of the lifting and the reference. They give identical results (verified against `src/laes_math.h` on test recordings during development) and are informative. The encoder files (`src/laes_*_enc.h`, for example `src/laes_stream_enc.h`, cited in Section 11.1) are informative, like Section 11.

---

## 1. Scope and conformance

### 1.1 What LAES is

LAES stores, in one file or on one disc:

* a **core master**: two-channel integer PCM at a **base rate** of 44 100 Hz or 48 000 Hz, with 16, 24 or 32 bits, which an ordinary player plays unchanged;
* one to 255 **masters** (at most sixteen in payload version 1; Section 6.1): two-channel integer PCM with 16, 24 or 32 bits, each at the base rate × 1, 2, 4 or 8. The masters are independent of one another: each may come from a different production (for example, a 16-bit master at 88.2 kHz and a 24-bit master at 352.8 kHz of the same piece). Every rate and every word length can hold a real, independent master, and several masters may have the same rate and word length. Each master may carry a **name** given by the publisher (Section 6.2).

Every master is restored **bit for bit**. From each master, the decoder also obtains its **rate levels** by the lifting of Section 3: the master at its rate / 2, / 4 and / 8, down to the base rate. The rate levels are exactly defined integer signals and are not stored separately.

LAES storage is lossless throughout: nothing in the payload is approximated. (The derived 16-bit master of Section 10.1 is an output, not stored content.) A master is stored either as the difference from another signal already in the file (the core master or an earlier master) or in full, whichever the encoder finds smaller (Section 6.4).

### 1.2 Carriers

| Carrier | Core master | LAES payload | Section |
|---|---|---|---|
| **WAV form** | the `data` chunk of a RIFF/WAVE file | a chunk `LAE ` (with a trailing space) in the same file | 7 |
| **FLAC form** | the audio frames of a FLAC file | one or more APPLICATION metadata blocks with ID `LAE2` | 8 |
| **CD-Extra** | the audio tracks of session 1 (16-bit, 44 100 Hz) | one extension file per track in the data session | 9 |

All three carriers hold the same payload (Section 6); they differ only in where the core master is stored.

### 1.3 Conformance

A conforming decoder:

1. MUST check the payload as specified in Section 6.7 and the carrier as specified in Sections 7 to 9;
2. MUST reproduce every master exactly as specified in Section 6.6;
3. MUST compare each reconstructed master with its SHA-256 (Section 6.5) and MUST NOT present a master whose hash does not match as that master. A player MUST NOT play a master (or its rate levels or derived versions) before a complete decode of that master and of every master in its chain of tiers has matched their SHA-256 values. It MAY remember this result for the same file content (for example, keyed by the SHA-256 of the whole file), so that the file plays at once the next time;
4. MUST, when the payload is missing, damaged or fails a check, still play the core master unchanged ("core master only"), except in the FLAC form when the STREAMINFO MD5 of the core master is missing or wrong (Section 8): then the payload MUST NOT be used, and the reference tools refuse the file; a player MAY play the core master with a warning to the user, or refuse the file;
5. MAY produce any of the outputs of Section 10.

A conforming encoder writes payloads that a conforming decoder accepts and that reproduce the encoder's input masters bit for bit. The choices left to the encoder are listed in Section 11.

### 1.4 Terms

| Term | Meaning |
|---|---|
| base rate | 44 100 Hz (CD family) or 48 000 Hz (DVD family) |
| core master | the signal that an ordinary player plays (Section 1.1). As a 16-bit signal (for example on a CD), it corresponds to what LAE Lossy calls the CD layer; in the WAV and FLAC forms, it may also have 24 or 32 bits |
| stages | for a master at rate base × 2^S, the number S (0 to 3) |
| level k | for a master with S stages, the signal at rate base × 2^k, 0 ≤ k ≤ S. Level S is the master itself (after removal of its unused low bits, Section 6.3); level 0 is its **base level** |
| octave layer k | the detail signal produced when level k is split into level k − 1 (Section 3) |
| tier | the signal against which a master is stored: the core master or an earlier master in the payload (not to be confused with the lossy and lossless tiers of the format family) |
| T | the **time position** of a master relative to its tier, in base-rate samples, given as an integer part and a fraction in units of 2^−32 base-rate sample |
| w | the number of low bits that are zero in every sample of a master ("unused low bits"); w = 0 if every sample is 0 |

### 1.5 Design properties (informative)

* The payload and its carriers contain no index, seek table, cue table or table of layer offsets. Time positions are known only by decoding.
* The payload is stored as a single block; it is not interleaved with the core master.
* Each master has exactly one storage choice and one tier for its whole length. No weights or gains are stored.
* The two channels are always coded separately: the left channel of a master is reconstructed only from left-channel signals, and the right channel only from right-channel signals.

---

## 2. Notation and arithmetic

### 2.1 Integers

* `int64` and `uint64` denote 64-bit signed (two's-complement) and unsigned integers; the same notation is used for 8, 16 and 32 bits.
* All multi-byte quantities in the payload, in WAV chunks and in the CD-Extra trailer are **little-endian**. FLAC structures follow RFC 9639 (big-endian bit order).
* `a >> s` on a signed value is an arithmetic shift (floor division by 2^s).
* **Exact sums.** Where this specification writes a sum of products, the sum MUST be formed exactly. A 128-bit two's-complement accumulator is sufficient for every sum in Sections 3 and 4.
* **Wrap-around.** Every value produced in Sections 3, 4.2, 5.1 and 6.6 (a prediction, a reconstructed sample, a lifting output, a reference value, `x << w`) is taken modulo 2^64 as an int64 (two's complement). A correct payload never needs a wider value; a payload that would need one can only fail its SHA-256.
* **Rounding.** For an exact integer A and s ≥ 1, `round_s(A) = (A + 2^(s−1)) >> s`, computed on the exact value. This rounds A / 2^s to the nearest integer, with ties rounded towards +∞.
* **Symmetric extension.** For a signal x of length n ≥ 1 and any integer index i:
  `m(i, n) = 0` if n = 1; otherwise, with p = 2(n − 1), `j = i mod p` (0 ≤ j < p), `m(i, n) = j` if j < n, else `p − j`.
  `x[m(i, n)]` is the value of x at index i, extended by whole-sample mirroring. Indices are 64-bit.
* `levLen(n, k)`: n halved k times, rounding up each time (`n = (n + 1) / 2`, applied k times).

### 2.2 Floating point (Section 4.1 only)

The reference taps are computed in IEEE-754 binary64. Each `+ − × ÷` and `sqrt` operation MUST be rounded individually (round to nearest, ties to even), in the order written. Fused multiply-add, extended-precision intermediates and reordering MUST NOT be used. Library functions other than `sqrt` and `floor` MUST NOT be used. *Informative:* MSVC `/fp:strict`; GCC/Clang `-ffp-contract=off` without `-ffast-math`.

---

## 3. Rate levels: the lifting L

### 3.1 One stage

One stage splits a signal x of length n into a low part s (length ⌈n / 2⌉; the next lower level) and a detail signal d (length ⌊n / 2⌋; the octave layer):

```
xe[i] = x[2i]          0 ≤ i < ⌈n/2⌉
xo[i] = x[2i + 1]      0 ≤ i < ⌊n/2⌋
d[i]  = xo[i] − P(xe, i)
s[i]  = xe[i] + U(d, i)

P(xe, i) = round_30( Σ_{t=0..39} H0[t] · ( xe[m(i − t, ne)] + xe[m(i + 1 + t, ne)] ) )          ne = ⌈n/2⌉
U(d, i)  = round_31( Σ_{t=0..59} U0[t] · ( d[m(i − 1 − t, no)] + d[m(i + t, no)] ) )            no = ⌊n/2⌋; U = 0 if no = 0
```

H0 is the table `kHalf0` and U0 is the table `kU0` (Appendix A). Each product is formed separately, and the sum is exact (Section 2.1).

The inverse stage, given s, d and n, is:

```
xe[i]     = s[i] − U(d, i)
x[2i]     = xe[i]
x[2i + 1] = d[i] + P(xe, i)
```

It reproduces x exactly.

### 3.2 All levels of a master

For a master with S stages, level S is the master after removal of its unused low bits (Section 6.3). For k = S down to 1, applying one stage to level k gives level k − 1 and octave layer k. Hence:

* level k has `levLen(F, S − k)` samples per channel, where F is the frame count of the master;
* octave layer k has `⌊levLen(F, S − k) / 2⌋` samples per channel.

---

## 4. The reference: time position and word length

A master (or one of its levels) is stored as the difference from a **reference**: a tier signal moved to the time position of the master and scaled to the word length of the master.

### 4.1 Reference taps

For a fraction f ∈ [0, 1) of a sample (given as an unsigned 32-bit value F, with f = F / 2^32), the 256 reference taps h[q], q = −127 … 128, are computed as follows (in binary64; Section 2.2):

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

The taps are integers, and their sum is exactly 2^31. For f = 0, h[0] = 2^31 and all other taps are 0.

### 4.2 Reference values

Let `src` be the tier signal at the relevant level (length n), `W` the integer part of the time position and F its fraction, both in samples of that level, and `up` the grid difference (Section 6.6). The reference has the length of the signal of the master at that level (`nOut` values):

```
A_i     = Σ_{q=−127..128} h[q] · src[m(i + W + q, n)]          exact
ref[i]  = round_{31−up}(A_i)            if 31 − up > 0
ref[i]  = A_i · 2^(up−31)               if 31 − up ≤ 0 (taken modulo 2^64 as int64)
```

*Informative:* `up` is the number of extra significant bits that the master has over the tier. It is negative when the master has fewer bits; the reference then rounds the tier down to the word length of the master.

### 4.3 Time position at level j

A time position T = W + F / 2^32 base-rate samples corresponds, at level j (rate base × 2^j), to T · 2^j samples of that level:

```
W_j = W · 2^j + (F >> (32 − j))        for j ≥ 1;   W_0 = W
F_j = (F · 2^j) mod 2^32
```

---

## 5. Layer coder

The layer coder codes every stored signal (base level, level difference or octave layer, one channel at a time) into a byte string. Bits are written most significant first. A **layer** of n samples is a sequence of **blocks** of length B (the last block holds the remainder), written back to back without byte alignment between blocks; the layer is padded with zero bits up to the next byte boundary.

### 5.1 Block

| Field | Bits | Meaning |
|---|---|---|
| type | 2 | 0 = constant, 1 = verbatim, 2 = fixed predictor, 3 = LPC |
| W | 6 | width of the stored constant, verbatim and warm-up values (W-bit two's complement); 1 ≤ W ≤ 62. An encoder chooses W so that every sample of the block fits; a decoder does not check decoded samples against W |

The block continues according to its type, for a block of n samples x[0 … n−1]:

* **constant:** one W-bit value v; x[i] = v.
* **verbatim:** n W-bit values.
* **fixed:** order o (3 bits; 0 ≤ o ≤ 4, o < n); o warm-up samples (W bits each); a residual of n − o values. Allowed only if W ≤ 58. Prediction (as in FLAC): o = 0: 0; o = 1: x[i−1]; o = 2: 2x[i−1] − x[i−2]; o = 3: 3x[i−1] − 3x[i−2] + x[i−3]; o = 4: 4x[i−1] − 6x[i−2] + 4x[i−3] − x[i−4].
* **LPC:** order − 1 (5 bits; order o = 1 … 32, o < n); precision − 1 (4 bits; p = 1 … 16); shift (5 bits; 0 … 31); o coefficients c[0 … o−1] (p-bit two's complement each); o warm-up samples (W bits each); a residual of n − o values. Allowed only if W ≤ 44. Prediction: `pred_i = floor( Σ_{j=0..o−1} c[j] · x[i−1−j] / 2^shift )`, with the sum formed exactly (for shift = 0, the sum itself).

Reconstruction: x[i] = residual[i] + pred_i for i ≥ o.

### 5.2 Residual

| Field | Bits | Meaning |
|---|---|---|
| partition order po | 4 | 0 ≤ po ≤ 8, 2^po ≤ m (m = number of residual values) |

The m values are split into 2^po partitions: partitions 0 … 2^po − 2 hold ⌊m / 2^po⌋ values each, and the last holds m − (2^po − 1) · ⌊m / 2^po⌋. Each partition consists of:

* a 6-bit parameter k. For k = 0 … 62, the values are Rice-coded: u = 2v for v ≥ 0 and u = −2v − 1 for v < 0; the quotient q = u >> k is written as q zero bits followed by a one bit, followed by the k low bits of u. The value u MUST fit in 64 bits.
* k = 63 is an escape: a 6-bit width r (1 … 63) follows, then every value as an r-bit two's-complement number.

All side information is fixed-length.

### 5.3 Block lengths

| Signal | Block length B |
|---|---|
| base level (storage 1) and base-level difference (storage 0) | 4096 |
| level-j difference (storage 2) | 4096 · 2^j |
| octave layer k | 4096 · 2^(k−1) |

### 5.4 Checks

A decoder MUST reject a layer (Section 6.7) when a field is outside the ranges above, when a predictor type is used with a W above its limit, when the order is not smaller than the block length, or when the data ends early. Bytes within the stored layer length that follow the last block, and the padding bits, are not checked; a decoder MUST ignore them.

---

## 6. The LAES payload

The payload is a byte string followed by its CRC-32 (4 bytes). The CRC-32 is the IEEE 802.3 CRC (reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF), computed over all payload bytes that precede it.

### 6.1 Header (64 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `LAE2` |
| 4 | 2 | version | 1 or 2 (see Section 6.2) |
| 6 | 2 | header size | 64 |
| 8 | 4 | base rate | 44 100 or 48 000 |
| 12 | 1 | core master bits | 16, 24 or 32 |
| 13 | 1 | channels | 2 |
| 14 | 1 | number of masters M | version 1: 1 … 16; version 2: 1 … 255 |
| 15 | 1 | core master mode | 0 = core master in a WAV `data` chunk or on CD audio tracks; 1 = core master in FLAC audio frames |
| 16 | 8 | core master frames | number of core master samples per channel |
| 24 | 32 | core master SHA-256 | Section 6.5 |
| 56 | 1 | default master | version 2: 0 = the core master, k = master k in payload order (1 … M): the entry that a player plays when the user has chosen nothing (Section 10.3); version 1: reserved, 0 |
| 57 | 1 | core master flags | version 2: bit 0 = 1: the core master offers no derived 16-bit version (Section 10.2); bits 1 … 7 are 0; version 1: reserved, 0 |
| 58 | 6 | reserved | 0 |

The encoder sets bytes 56 and 57 as the publisher chooses; if the publisher chooses nothing, both are 0.

### 6.2 Master record (72 bytes), followed by its layers

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | bits | 16, 24 or 32 |
| 1 | 1 | stages S | 0 … 3 (rate = base × 2^S) |
| 2 | 1 | w | unused low bits, w < bits |
| 3 | 1 | storage | 0, 1 or 2 (Section 6.4) |
| 4 | 1 | reference tier | 0 = core master; k = the k-th master of this payload (1-based) |
| 5 | 1 | name length N | version 2: 0 … 255 (0 = no name); version 1: reserved, 0 |
| 6 | 1 | flags | version 2: bit 0 = 1: this master offers no derived 16-bit versions (Section 10.2); bits 1 … 7 are 0; version 1: reserved, 0 |
| 7 | 1 | menu position | version 2: 0, or 1 … M (see below); version 1: reserved, 0 |
| 8 | 4 | rate | base × 2^S |
| 12 | 4 | reserved | 0 |
| 16 | 8 | frames F | samples per channel, ≥ 1 |
| 24 | 8 | T whole | int64, integer part of T (base-rate samples, relative to the tier) |
| 32 | 4 | T fraction | uint32, in units of 2^−32 base-rate sample |
| 36 | 4 | reserved | 0 |
| 40 | 32 | master SHA-256 | Section 6.5 |

For storage 1, the T fields hold the T of the master relative to the core master; decoders do not use them (Section 6.6). Files written before 2026-11-15 hold 0 in these fields.

In version 2, each master record is followed directly by the N bytes of its **name** and then by its layers; version 1 has no names.

The masters follow one another in payload order: the record of master 1 (in version 2 with its name), its layers, the record of master 2, its layers, and so on. The CRC-32 follows the layers of the last master. The tier of a master MUST be the core master or a master that comes earlier in the payload.

**Version.** When a payload can be written as version 1 (at most sixteen masters, no names, all flags and menu positions 0, header bytes 56 and 57 both 0), a writer MUST write version 1; the payload is then byte for byte the one that a writer of payload version 1 writes. Otherwise, it writes version 2. A decoder that knows only version 1 rejects a version 2 payload and plays the core master only (Section 1.3).

**Menu positions.** The order of the masters in the file is chosen by the encoder (for the smallest file; a master can refer only to earlier masters). The order in menus and in selection is the one that the publisher marks; the two orders are independent. The menu positions are either all 0 (the menu order is then the file order) or a permutation of 1 … M, each value occurring exactly once. The number of a master in menus is its menu position or, without marks, its place in the file. A menu position is an order only: it carries no kind, no priority and no automatic choice, and it does not make a master the default (the default is header byte 56).

**Names.** A name is free text from the publisher: well-formed UTF-8 (shortest forms, no surrogates, at most U+10FFFF), at most 255 bytes, without the control characters U+0000 … U+001F and U+007F. A reader shows it as it is, without parsing or changing it. A name takes no part in reconstruction, tiers, storage, hashes or any automatic choice (Section 10.3); changing a name changes no sample of any master. A master without a name is referred to by its number only.

### 6.3 Layers of a master

Let j = 0 for storage 0 and 1, and j = min(S, S_t) for storage 2, where S_t is the number of stages of the tier master. For each channel, left first, then right, the following are stored:

1. a uint32 byte length followed by the layer coding (Section 5) of:
   * storage 0 or 2: the **difference** at level j, `levLen(F, S − j)` values: level j of the master minus the reference (Section 6.6);
   * storage 1: the base level of the master itself, `levLen(F, S)` values;
2. for each k = j + 1 … S: a uint32 byte length followed by the layer coding of octave layer k.

The samples of a master are taken **after removal of its unused low bits**: `x = sample >> w`. The levels and octave layers of Section 3 are computed from x.

### 6.4 Storage values

| Storage | Reference tier | Stored at level | Meaning |
|---|---|---|---|
| 0 | core master or any earlier master | j = 0 | difference from the tier at the base level |
| 1 | 0 (none) | j = 0 | base level stored in full |
| 2 | an earlier master | j = min(S, S_t) | difference from the tier master at the highest level that both have; only the octave layers above j are stored |

### 6.5 Hashes

* **Master SHA-256:** computed over the samples of the master in file order L, R, L, R, …, each sample as its int32 two's-complement value in 4 bytes, little-endian.
* **Core master SHA-256:** for a 16-bit core master, computed over the samples as int16 little-endian, L, R, L, R, … (the byte content of a 16-bit `data` chunk); for a 24- or 32-bit core master, computed as for a master (int32 little-endian).

### 6.6 Reconstruction

For each master m in payload order, and for each channel separately:

```
j  = 0 (storage 0, 1) or min(S, S_t) (storage 2)
x_j = decoded first layer                               (levLen(F, S − j) values)
if storage ≠ 1:
    tier signal src = the core master channel (tier 0), or level j of tier master t (tier t ≥ 1)
    up  = (bits − w) − (core master bits)                       for tier 0
          (bits − w) − (bits_t − w_t)                    for a tier master t
    (W_j, F_j) = T at level j (Section 4.3)
    x_j = x_j + reference(src, W_j, F_j, up)             (Section 4.2, nOut = levLen(F, S − j))
for k = j + 1 … S:
    x_k = inverse stage(x_{k−1}, octave layer k, levLen(F, S − k))
master sample = x_S << w
```

Level j of a tier master t is obtained from its reconstructed samples: x_t = sample >> w_t is its level S_t, and each application of one stage (Section 3.1) gives the next lower level.

### 6.7 Checks

A decoder MUST reject the payload (and play the core master only) if any of the following holds:

* the CRC-32 does not match;
* the magic, version (1 or 2) or header size (64) differs; the base rate is not 44 100 or 48 000, or differs from the rate of the carrier's core master; the core master bits differ from those of the carrier's core master; channels ≠ 2; core master mode > 1; M is not 1 … 16 (version 1) or 1 … 255 (version 2);
* version 2: the menu positions are neither all 0 nor a permutation of 1 … M;
* version 2: flag bits 1 … 7 of a master are not 0; a name is not well-formed UTF-8, contains a control character, or runs past the end of the payload;
* the core master frames differ from the length of the carrier's core master, or the core master SHA-256 differs;
* for a master: bits is not 16, 24 or 32; S > 3; w ≥ bits; storage > 2; rate ≠ base × 2^S; F < 1; `levLen(F, S)` > core master frames + 2^24; |T whole| ≥ 2^40. Core master frames and F are uint64 and MUST be compared without truncation; a decoder that cannot hold a master of F samples MUST reject the payload rather than truncate the master;
* for a master: T (T whole + T fraction · 2^−32) lies outside −N … N, where N is the number of core master frames (an offset beyond the track is meaningless);
* storage 0 or 2 with a tier that is not earlier in the payload; storage 2 with tier 0; storage 1 with a tier other than 0;
* a layer fails a check of Section 5.4, or the payload ends early;
* bytes remain between the layers of the last master and the CRC-32.

A master whose reconstruction does not match its SHA-256 MUST NOT be presented as that master, nor its rate levels. The core master and the other masters are unaffected, except for masters whose chain of tiers includes the failed master: these are then reconstructed from wrong values and, barring a SHA-256 collision, also fail their SHA-256.

Reserved fields: writers MUST write 0, and readers MUST ignore them. The core master mode is not compared with the carrier.

---

## 7. WAV form

An LAES WAV file is a RIFF/WAVE file (or, beyond 4 GiB, an RF64 file; see below) with the following chunks:

* `fmt `: integer PCM (format tag 1, or 0xFFFE with the PCM sub-format), 2 channels, the base rate, 16, 24 or 32 bits;
* `data`: the core master;
* `LAE `: the LAES payload followed by its CRC-32 (Section 6), with core master mode 0;
* `LIST` of type `INFO` and `id3 ` (an ID3v2 tag), both optional: tags and pictures. Writers place them after `fmt ` and before `data`. One set describes the core master and every master; no hash defined in this specification covers them, and they take no part in choosing or checking an output (Section 10).

A file MUST contain exactly one `data` chunk and at most one `LAE ` chunk: writers MUST NOT write more than one of each. A reader that finds more than one `data` or `LAE ` chunk uses the last one of each type, as the reference decoder does. A reader skips every chunk that it does not use. *Informative:* the reference decoder clamps any chunk size that runs past the end of the file.

Any WAV player plays the core master (a 32-bit core master only in players that support 32-bit PCM). The first 4 bytes of the `LAE ` chunk identify the format: `LAE1` = LAE Lossy (LAEY), `LAE2` = LAE Lossless (LAES) (this specification). A reader SHOULD tell the user which format it found; an LAEY decoder that encounters `LAE2` reports an LAES file and does not decode it, and a player plays the core master.

**Files beyond 4 GiB (RF64).** A writer uses the RF64 form (EBU Tech 3306, ITU-R BS.2088) when the RIFF size of the file (the value of the RIFF size field: the file length minus 8) would exceed 0xFFFFFFFF; otherwise it uses the RIFF form. The RF64 form differs from the RIFF form only as follows:

* bytes 0–3 are `RF64`, and the 32-bit size field at offset 4 is 0xFFFFFFFF;
* the first chunk, at offset 12, is `ds64` with a body of 40 bytes: the RIFF size as a `uint64` (the file length minus 8), the size of the `data` body as a `uint64`, the number of core master frames as a `uint64`, a table length of 1 as a `uint32`, and one table entry: the chunk ID `LAE ` and the size of its body as a `uint64`;
* the 32-bit size fields of the `data` and `LAE ` chunks are 0xFFFFFFFF; their sizes are those in `ds64`.

A reader MUST accept the RF64 form, and also the same layout with `BW64` in bytes 0–3. In such a file, the first chunk MUST be `ds64` with a body of at least 28 bytes; a chunk whose 32-bit size field is 0xFFFFFFFF takes its size from `ds64`: the `data` size for `data`, and the table entry with the same chunk ID for any other chunk.

---

## 8. FLAC form

An LAES FLAC file is a FLAC stream (RFC 9639) with the following structure:

```
"fLaC" | STREAMINFO | VORBIS_COMMENT, PICTURE (optional) | APPLICATION blocks with ID "LAE2" (1 or more) | other metadata blocks (optional) | audio frames = the core master
```

* STREAMINFO: 2 channels, the base rate, 16, 24 or 32 bits, the sample count of the core master (MUST NOT be 0) and the MD5 of the core master (MUST be present and correct). A decoder that finds the MD5 missing or wrong MUST NOT use the payload: the core master is not the one for which the payload was made. In that case, the reference tools refuse the file; a player MAY play the core master with a warning to the user, or refuse the file (Section 1.3, item 4).
* The LAES payload (with its CRC-32; core master mode 1) is split into pieces of at most 2^24 − 5 bytes. Each piece is the data of one APPLICATION block whose 4-byte ID is `LAE2` (0x4C 0x41 0x45 0x32), registered by IANA in the FLAC Application Metadata Block IDs registry (RFC 9639, Section 12.2; 2026-10-31). Writers place these blocks after STREAMINFO and after the VORBIS_COMMENT and PICTURE blocks, if any, consecutively with no other block between them. A reader concatenates all APPLICATION blocks with ID `LAE2`, in file order, wherever they stand among the metadata blocks.
* Tags and pictures (optional): at most one VORBIS_COMMENT block and any number of PICTURE blocks (RFC 9639, Sections 8.6 and 8.8). Writers place them before the `LAE2` blocks, so that a player reads them without skipping the payload. One set describes the core master and every master; no hash defined in this specification covers them, and they take no part in choosing or checking an output (Section 10).
* The audio frames hold the core master, coded with independent left and right channels: channel assignment 0b0001 in every frame header, with no inter-channel decorrelation. Within this channel assignment, any valid FLAC coding of the core master is allowed. Support for inter-channel decorrelation may be added in a later revision.
* No SEEKTABLE or CUESHEET block is written.
* Tags written by other software: a reader accepts ID3v2 tags before `fLaC`, and APEv2 and ID3v1 tags after the last audio frame (that is, once the sample count given in STREAMINFO has been reached). It does not interpret them, and a tool that writes a new file from such a file keeps them unchanged.

Every FLAC decoder that implements RFC 9639 in full plays the core master and skips the APPLICATION blocks (checked 2026-11-05 with libFLAC and FFmpeg). Decoders that implement only part of it may not do so: the built-in FLAC decoder of Windows (Media Foundation, also used by Explorer) does not decode 32-bit FLAC, and it does not recognise a file whose first audio frame starts more than about 4 MiB into the file, as is the case in an LAES FLAC file whose payload is larger than that. Tested decoders and workarounds are given in [COMPATIBILITY.md](../COMPATIBILITY.md).

*Informative:* the FLAC writer of the reference encoder (`src/laes_flac_enc.h`) uses a fixed block size of 4096, two independently coded channels, LPC orders up to 12, a precision of up to 15 and Rice partition orders up to 8. At 44.1 / 48 kHz, this conforms to FLAC's streamable subset. Many older players do not play 32-bit FLAC; the reference encoder therefore does not use a 32-bit core master in the FLAC form unless instructed otherwise (it uses the highest master word length up to 24 bits; Section 11.1). The reference decoder decodes only core masters with independently coded channels.

---

## 9. CD-Extra

On a CD-Extra disc made with the LAE CD-Extra encapsulation (`spec/cdx/SPEC.md`), the extension file (`.LAX`) of each track is:

```
LAES payload with its CRC-32 (core master mode 0) | 80-byte trailer record
```

The trailer record is the one defined in `spec/cdx/SPEC.md`, Section 3.2, with **extension block type 2**. A reader MUST check that the first 4 bytes of the extension block are `LAE2` for type 2 (and `LAE1` for type 1); otherwise, it MUST reject the extension and play the CD track only. All other rules of the CD-Extra specification (whole-track MD5, sliding checksums, rip-offset search, the order of checks) apply unchanged. The core master is the CD track (16-bit, 44 100 Hz).

---

## 10. Decoder outputs

The outputs that a decoder may offer for each file (Section 1.3, item 5) are:

| Output | Lossless | Definition |
|---|---|---|
| the core master | yes | as stored |
| each master | yes | Section 6.6 |
| each rate level of a master | bit-exact by definition (level k, Section 3.2) within the word length of the master; a value beyond that word length is limited to it on output | level k (Section 3.2), with samples `x_k << w`, written at the word length of the master |
| a derived 16-bit master | relative to its definition: **yes** (every decoder reproduces it bit for bit); relative to the master from which it is made: **no** (requantised to 16 bits with TPDF dither) | Section 10.1 |

### 10.1 Derived 16-bit version

A decoder MAY offer one derived master, a 16-bit version, derived from each of the following **sources** (b bits, b > 16, s = b − 16):

* the core master, if it has more than 16 bits and bit 0 of header byte 57 is clear;
* every master with more than 16 bits whose SHA-256 matched and whose flag bit 0 is clear: the master itself and each of its rate levels (after limiting).

Each derived version belongs to its source alone. A 16-bit output at the same rate (a 16-bit core master, a 16-bit master or one of its levels) or a derived version of another master does not affect it, and a derived version never replaces or hides a master. (Before 2026-11-11, decoders offered at most one derived version per rate, from the first output with the most bits at that rate, and only where the rate had no 16-bit output. Under the present rule, the derived versions of master 1 are bit-identical to those.)

```
per channel, a xorshift64 generator with state st = 20261029 + 0x9E3779B97F4A7C15 · (channel + 1)  (mod 2^64; channel 0 = left)
next():  st ^= st << 13;  st ^= st >> 7;  st ^= st << 17;  return st >> (64 − s)
for each sample v in order:  r1 = next(); r2 = next();  y = clamp((v + r1 − r2 + 2^(s−1)) >> s, −32768, 32767)
```

*Informative (starting at frame n, for seeking):* the state depends only on the channel and on n, never on the audio. It is the state after 2n calls of next() (r1 and then r2, as above). next() is linear over GF(2), so a decoder can compute this state directly, for example by raising a 64 × 64 bit matrix to the power 2n. The output from frame n onwards is then bit for bit the same as when decoding from frame 0.

The result is a derived 16-bit master (TPDF dither of ±1 LSB):

* **Lossless relative to this definition:** every decoder reproduces it bit for bit, on every run and every platform. Decoding it again, or with another decoder, never changes a sample.
* **Not lossless relative to the master from which it is made:** its b − 16 low bits are dropped and TPDF dither is added. That master cannot be recovered from it.

It is not the original master. Wherever a decoder offers it, the decoder MUST state from which master it is derived (for example, `derived from master 3 "Name" · 88.2 kHz / 16-bit`); the description of the output states that it is lossless relative to its definition and not lossless relative to the master from which it is made.

### 10.2 Turning derived versions off when encoding

When flag bit 0 of a master record (version 2) is 1, a decoder offers no derived 16-bit version of that master; bit 0 of header byte 57 does the same for the core master. The flag decides only whether these derived versions are listed: it carries no parameters, does not choose between ways of deriving, and does not affect the master itself, its rate levels, reconstruction, checks or the fallback.

### 10.3 Selection and display (players and tools)

**Entries and their order:**

1. The core master; if it has more than 16 bits and its flag (header byte 57) allows it, its derived 16-bit version immediately after it.
2. One group per master, in menu order (Section 6.2; without marks, payload order): the master itself; its rate levels from high to low; its derived 16-bit versions (first at the rate of the master, then for each level from high to low).

**Menus:** the first level lists only the core master and the masters themselves. The rate levels and derived versions of a master are in the submenu of that master, never mixed with those of another master. An interface that can show only one level (a command line, for example) lists them in the order above and shows to which master each sub-entry belongs.

**Text shown:**

* The number is always shown. Names may repeat or be missing; the number keeps every entry distinct.
* The first level shows the whole description of each master and nothing derived: number, name (if it has one), rate, word length, length, "lossless", and a mark on the default; for example, `master 3 "XX Records 2019 remaster" · 176.4 kHz / 24-bit · 13:23 · lossless · default`. The same applies to the core master.
* Only the core master and the masters themselves are called lossless. Rate levels and derived 16-bit versions are both "derived from" their master and carry its number and name: `derived from master 3 "XX Records 2019 remaster" · 88.2 kHz / 24-bit` and `derived from master 3 "XX Records 2019 remaster" · 88.2 kHz / 16-bit`. Nothing further is written after the rate and word length.
* A long name may be shortened on screen, but the whole name MUST remain reachable (for example, in a tooltip or a properties page).
* A master whose SHA-256 did not match is marked "not available" and is not silently left out; the same applies to every master whose chain of tiers contains it.
* In Chinese interfaces, the core master is called "核心母带", never "核心" alone.

**Default and automatic choices:**

* The default (what a player plays when the user has chosen nothing) is the master in header byte 56, chosen by the publisher when the file is encoded; if byte 56 is 0, or in a version 1 file, it is the core master. The default is always a master itself or the core master, never a rate level or a derived version. If the player cannot play it (it does not decode, or its SHA-256 does not match), the player plays the core master. The default is never chosen by bit rate, size, device capability, network or load. This specification gives no number, menu position or name any meaning of kind.
* The number of a master (the "master k" shown, `master:<k>`, the file name `master_<k>_…`) is always its number in the menu.
* Limits set by the user (highest rate, longest word length) apply only within the selected master: its highest level within the limits; if its word length is above the limit, its own derived 16-bit version. A limit MUST NOT switch to another master.
* There is no automatic choice by device capability, name, file category or defaults in a configuration file, and no name is carried over from one file to the next. A player that remembers the choice of the user remembers, per file, the number that the user picked.

**Command line (`laes dec --pick` of the reference tool):** `<n>` = the n-th entry of the list; `<rate>:<bits>` and `name:<text>` match masters only (never a rate level or a derived version), and one match is written; if several match, the user ticks them: each candidate is shown with its number, rate, word length and name, and is ticked one by one or by numbers and ranges typed below the list (`6,12`, `6-12`); outside a console, nothing is written and the candidates are listed; `<a>-<b>` = entries a to b; `master:<k>` = master k.

---

## 11. Encoder (informative)

### 11.1 Core master

The core master is either given (for example, a released CD or DVD) or derived from the base level of a master at a chosen word length. The reference encoder uses 16 bits for the WAV form and, for the FLAC form, the highest master word length up to 24 bits. The source master is a master whose word length equals that of the core master (its base level is then exactly the core master) or, if there is none, the master with the longest word length, TPDF-dithered down.

A derived core master is stored in the file like any other core master: what the file holds is the core master, checked by its SHA-256 (Section 6.5). The rule below lets anyone check, bit for bit, that the core master was derived from a given master; it is the rule by which the reference encoder (`laes enc --derive-core`, `src/laes_stream_enc.h`) derives it.

```
source:  among the masters in the order given to the encoder, the first whose word length equals b (the core master's bits);
         if there is none, the first with the longest word length (the encoder prints which: "core master: b-bit base level of …")
x      = the source's level 0 (Section 3.2, after removing its unused low bits w), per channel
drop   = (bits − w) − b
if drop ≤ 0:  core sample = x << (−drop)                                     (no dither)
else:         one xorshift64 generator for both channels, state st = 20261028 · 2654435761 + 88172645463325252 (mod 2^64)
              next():  st ^= st << 13;  st ^= st >> 7;  st ^= st << 17;  return st
              u():     (next() >> 11) / 2^53 − 0.5                            (IEEE-754 double)
              for channel = left, then right; for each sample x in order:
                  t = x / 2^drop;  t = t + u();  t = t + u();  t = t + 0.5   (double, round to nearest, in this order;
                  core sample = clamp(⌊t⌋, −2^(b−1), 2^(b−1) − 1)             the first u() is added first)
```

The arithmetic is exact except for the two additions of u() and the addition of 0.5, which IEEE-754 double arithmetic rounds identically on every platform. The left channel uses the first `levLen(F, S)` × 2 values of the generator, and the right channel the values that follow.

### 11.2 Storage, tier and order

For each master, the reference encoder tries storage 1; storage 0 against the core master and against every earlier master; and storage 2 against every earlier master when both have a level above the base level. It keeps the smallest result, with one choice per master. It writes the masters in whichever of two orders is smaller: either longer word lengths first, then higher rates first; or, when the core master was derived from a master that is not already first, that master first and the others in the same order as before. A master can refer only to earlier masters, and a master with a shorter word length can be stored cheaply against one with a longer word length.

With more than sixteen masters (since 2026-11-11), each candidate is first coded on four short excerpts (2^15 base-rate samples each, × 2^j at level j), placed at the centres of the four quarters of the piece and aligned down to the block length; their positions depend on the length alone, never on the content. The candidates are ranked by this size scaled up to the whole length; only the three smallest are coded in full, and the smallest of these is kept. Only the first of the two file orders is used. All of this is a method of the encoder: nothing about the trial excerpts, the candidates or the ranking is stored in the file. Measured on 10-second excerpts: with 20 masters, the file is byte for byte the one that a full search gives; with 64 masters, every master is restored bit for bit.

### 11.3 Time position

A master derived from the same production as the core master has T = 0. For an independently produced core master (a released CD), the reference tool `laes align` uses the automatic search of the LAE Lossy encoder and stores the measured position as T, including the fraction; the master itself is never moved. An alignment error (the residual energy after alignment, relative to the energy of the signal, as measured by the LAE Lossy search) of −60 dB or less is accepted. Otherwise, the error is shown and the user decides whether to continue or stop. A whole album is cut at the track boundaries of the CD by the whole-disc planner of LAE Lossy (`lae_enc plan … laes`), which writes the master of each track unmoved, with its own T.

A T given as a decimal number is split into whole = floor(T) and q = round((T − whole) · 2^32). If q = 2^32, the whole part is increased by one and the fraction is 0; otherwise, the fraction is q. The reference encoder refuses a T outside −N … N (Section 6.7), and it does not use a tier relative to which the T of the master would lie outside this range.

### 11.4 Measured sizes

Measured on 10-second excerpts of two 2L test recordings at 352.8 kHz (02 and 05) and of one recording at 192 kHz for the 48 kHz family. Each comparison is with `flac -8`.

| Content | LAES FLAC form, compared with |
|---|---|
| one 24-bit master with all its rate levels | 93.5–98.4 % of a FLAC of that master alone |
| 16-, 24- and 32-bit masters of one mix at 352.8 kHz, each at all four rates (the master and its three rate levels; 12 versions) | 99.6–99.8 % of a FLAC of the 32-bit master alone |
| the same 12 versions | about 30 % of 12 separate FLAC files |

### 11.5 Resources and resuming

The following items describe the reference encoder (`laes enc`). None of them changes the file written: the same masters and options give the same bytes.

* **Masters.** A master is a 16-, 24- or 32-bit stereo WAV file (RIFF, or RF64 beyond 4 GiB), read in place, or a FLAC file. A FLAC master is decoded once into the work folder and checked against the MD5 in its STREAMINFO.
* **Work folder.** Intermediate data is written to `<output>.laes.tmp` next to the output. The folder is removed when the run completes; a run that stops keeps it, except with `--disk-stop-remove` (see below).
* **Memory.** Intermediate signals are processed one piece at a time. The memory budget (`--mem=<GB>`, default 2 GB, less on machines with little memory) determines only the piece length. The core master is held in memory at 4 bytes per sample per channel.
* **Disk.** Before creating the work folder, the encoder estimates the disk space it needs. If this exceeds the free space, it does not start; `--force-disk` overrides this.
  * While it runs, free space below 2 GB makes it **wait**: nothing is removed, and the run continues by itself once there is room again.
  * With `--disk-stop`, it stops instead and keeps the work folder.
  * With `--disk-stop-remove`, it stops and removes the work folder; the work done so far is lost.
* **Resuming.** A run that stopped (disk full, program closed, crash) keeps its work folder. Running the same command again continues where the run stopped.
  * The work folder records what the file depends on: the arguments, the size and modification time of each master, and the encoder build. If any of these has changed, the run starts again.
  * A resumed run writes the same bytes as an uninterrupted one.
* **`--small-disk`.** For small disks. The largest rate level of each master is not stored; it is recomputed from the master whenever it is read. A FLAC master is not copied into the work folder; its frames are decoded again when read (their positions are kept in memory only). Encoding is slower; memory use does not increase.
* **`--timing`.** Prints the time and peak memory of each stage, and the largest size of the work folder.

Measured on a 32-thread machine with the default 2 GB budget, FLAC form. The masters are given as WAV unless noted otherwise.

| Content | Mode | Time | Peak memory | Work folder, largest |
|---|---|---|---|---|
| 6:41, 2 masters (352.8 kHz 32-bit, 176.4 kHz 24-bit), given as FLAC | normal | 72 s | 0.72 GB | 3.1 GB |
| | `--small-disk` | 82 s | 0.57 GB | 1.1 GB |
| 10:05, 4 masters (352.8 kHz 32-, 24- and 16-bit, 176.4 kHz 24-bit) | normal | 312 s | 1.03 GB | 5.3 GB |
| | `--small-disk` | 386 s | 0.69 GB | 2.7 GB |
| 30:13, 12 masters (16, 24 and 32 bits × 44.1, 88.2, 176.4 and 352.8 kHz) | normal | 2181 s | 1.41 GB | 27.8 GB |
| | `--small-disk` | 3047 s | 1.53 GB (*) | 15.0 GB |

(*) Measured with an earlier build that derived the core master in one piece; with that build, the normal run also needed 1.53 GB. The current build lowered the normal run to 1.41 GB; `--small-disk` has not been measured again with it.

Peak memory grows with the length (the core master is held whole); disk use grows with the length and with the number and rates of the masters.

---

## 12. Conformance testing (informative)

`laes selftest` runs the following, with synthetic signals only:

* round trips over the WAV and FLAC carriers, all word lengths, both rate families, fractional and negative T, unused low bits, short and odd lengths, and all three storage values;
* damage tests: byte flips, flips behind a recomputed CRC, and truncation of the payload and of the FLAC file;
* random input to the FLAC parsers.

The fast forms (`src/laes_lift.h` and the fast paths in `src/laes_codec.h`) were compared with the normative forms on test recordings during development; every result was identical.

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
* Masters are stored as differences from a tier (the core master or an earlier master) or in full. Storage 2 (difference at the highest shared level) is new.
* The time position T replaces any moving of masters: masters are stored unmoved.
* The core master may have 16, 24 or 32 bits. The FLAC form and the CD-Extra extension type 2 are specified.
* Decoder outputs include a derived 16-bit master, labelled as derived (lossless relative to Section 10.1, not lossless relative to the master from which it is made).
* 2026-11-11 and 2026-11-12: payload version 2: up to 255 masters, and per master a name, a flag that turns its derived versions off, and a menu position; in the header, a default master and a flag for the core master. A payload that fits version 1 is still written as version 1, byte for byte unchanged. Derived 16-bit versions are now one per source (the core master, each master and each of its levels); Section 10.3 sets out selection and display. The file order remains the smallest one found by the encoder; the menu order is the one that the publisher marks. A player plays a master only after the master and its chain of tiers have matched their SHA-256 values (Section 1.3).
