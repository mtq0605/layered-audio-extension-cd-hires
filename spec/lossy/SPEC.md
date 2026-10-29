<!-- SPDX-License-Identifier: CC-BY-4.0 -->
<!-- SPDX-FileCopyrightText: 2026 mtq0605 -->

# LAE Lossy: Bitstream and Decoding Specification, Container Version 5

**English** | [中文](SPEC.zh.md)

**Status:** Draft for public review.
**Applies to:** LAE Lossy (short name LAEY), the lossy tier of the LAE (Layered Audio Extension) format family, container version 5.
**Licence of this document:** CC BY 4.0 (see Section 15). Copyright (c) 2026 mtq0605.

This document specifies the container syntax and the decoding process of LAE Lossy, container version 5. It is intended to let an independent implementer write a conforming decoder without other material. There are two exceptions:

* the MDCT constant tables of `src/mdct_tables.h` (Section 9.2), which are not yet reproduced here;
* the conformance vectors and their hash values, which are listed in [tests/vectors/MANIFEST.md](../../tests/vectors/MANIFEST.md) (Appendix B).

Sections 14 and 15 and all text marked *informative* are not normative.

The reference decoder is normative wherever this text is silent or ambiguous. Its files are:

* `src/lae_dec.cpp`
* `src/lae_decoder.h`
* `src/part_stream.h`
* `src/partition_format.h`
* `src/mdct_tb.h`
* `src/mdct_tables.h`
* `src/halfband.h`
* `src/wavio.h`

The encoder's partition search is in `src/partition.h`. That file is not needed by a decoder. Items that are not yet specified are listed in Appendix C.

The format family is called LAE. It has two tiers:

* the lossy tier, LAE Lossy (LAEY), specified here;
* the lossless tier, LAE Lossless (LAES): design document published ([spec/lossless/DESIGN.md](../lossless/DESIGN.md), under review); normative specification not yet written.

The encapsulation of the extension on CD-Extra discs and in separate extension files is specified in [spec/cdx/SPEC.md](../cdx/SPEC.md) (draft).

The RIFF chunk identifier is `LAE `, with a trailing space. The chunk magic is `LAE1` (Section 4). The magic names the format: `LAE1` = LAEY (this specification); `LAE2` = LAES, the lossless tier, which has its own specification. A decoder SHOULD tell the user which format it found; an LAEY decoder that finds `LAE2` reports an LAES file and does not decode it (the CD layer stays playable).

---

## 1. Scope and conformance

### 1.1 Scope

An LAE Lossy file is a RIFF/WAVE file with two parts:

* **CD layer.** The `data` chunk holds ordinary 16-bit two-channel PCM at a base rate of 44 100 Hz or 48 000 Hz. Any WAV player can play it unchanged.
* **Extension.** A chunk with identifier `LAE ` follows the audio. It carries two layers:
  * a **depth layer**, which extends the CD layer towards 24-bit resolution at the base rate, with one plane per channel L and R;
  * a **frequency layer**, which extends the bandwidth up to a master rate of base × 2, × 4 or × 8. It is stored as mid/side (M, S) planes, split into one stream per frequency segment.

**Distribution and file extensions.**

* Files are distributed with the extension `.wav`, so that every player plays the CD layer. Distribution as `.flac` may optionally be added later; carriage in FLAC is not specified for container version 5.
* `.lae` is the format's own file extension. The layout of `.lae` files is not specified for container version 5; it will be specified in a later revision.

A decoder can produce the following outputs. All of them are written as 24-bit two-channel PCM.

| Route | Content | Output rate |
|---|---|---|
| 24-bit route | CD + depth layer + frequency layer | master rate / 2^d, with 0 ≤ d < stages (d = 0 gives the master rate) |
| 16-bit route | CD + frequency layer | master rate / 2^d, with 0 ≤ d < stages |
| CD + depth route ("cd24") | CD + depth layer | base rate |

When an output rate below the master rate is selected, the frequency-layer streams that start at or above the output Nyquist frequency are skipped without being decoded (Section 5.4). The result is then brought down to the selected rate with the half-band decimators of Section 10.

LAE Lossless (LAES), the lossless tier, is not specified here. Its status: design document published ([spec/lossless/DESIGN.md](../lossless/DESIGN.md), under review); normative specification not yet written.

### 1.2 Decoding model (informative)

All reconstruction takes place in the time domain. The order of operations is:

1. Range-decode each coded plane, one row of 8 frames at a time, and dequantise its coefficients (Sections 6 to 8).
2. Apply the textbook inverse MDCT in strict IEEE-754 double arithmetic, with overlap-add (Section 9).
3. Form the base signal: 4096 × CD − regenerated TPDF dither + 16 × depth layer. The result is in units of 1/16 LSB24 (Section 11.2).
4. Interpolate by 2 per stage with integer half-band filters up to the master rate. The filters use Q31 coefficients, 64-bit accumulation and one rounding per output sample.
5. Add 16 × the frequency layer, after the integer M/S inverse.
6. Optionally decimate with the same half-band tables down to the selected rate.
7. Apply one final rounding to 24 bits, then a hard limiter.

In the cd24 route, steps 4 to 6 are omitted.

### 1.3 Conformance

A conforming decoder MUST meet every requirement of Sections 3 to 13 that applies to a decoder, including the validation and refusal rules of Sections 4.3 and 12 and the requirement to remain memory-safe for every input. In addition, for every test vector and every route listed in [tests/vectors/MANIFEST.md](../../tests/vectors/MANIFEST.md) (Appendix B), it MUST produce an output WAV file (header and samples, Section 11.8) whose SHA-256 matches the listed value.

The main table of the manifest lists the first 16 hexadecimal digits of each SHA-256 value; the full 64-digit values are listed in its section "Full SHA-256 values".

The test vectors exercise the process defined in Sections 3 to 13. An implementation that follows these sections exactly produces the listed outputs.

A decoder MAY be structured differently internally, provided that every output byte is identical. For example, it may decode whole tracks instead of streaming, or decode streams in parallel.

### 1.4 Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY" and "OPTIONAL" are to be interpreted as described in BCP 14 (RFC 2119 and RFC 8174) when, and only when, they appear in all capitals, as shown here.

### 1.5 Floating-point and pseudo-random elements (informative)

* The only floating-point stages are the coefficient reconstruction (Section 8.3) and the inverse MDCT (Section 9). Both use strict IEEE-754 double precision.
* A coefficient quantised to zero is reconstructed as zero, unless the optional zero level (Section 8.4) is in use.
* The only pseudo-random sequence in the decoder is the dither regeneration (Section 11.2). It reproduces the dither that the encoder added to the CD layer.

---

## 2. Notation and arithmetic

### 2.1 Integer arithmetic

* `int16`, `int32` and `int64` denote two's-complement signed integers of 16, 32 and 64 bits. `uint8`, `uint16`, `uint32` and `uint64` denote unsigned integers.
* All multi-byte quantities in the file are **little-endian**.
* `a >> k` on a signed value is an arithmetic shift, that is, floor division by 2^k. On an unsigned value it is a logical shift.
* `a / b` on integers denotes truncating division. Wherever this specification uses it, the operands are non-negative or the division is exact.
* `ceil(a / b)` for positive integers equals `(a + b - 1) / b`.
* Arithmetic on `uint32` values is modulo 2^32. Arithmetic on `uint64` values is modulo 2^64.
* Where this specification states `int64`, the operation MUST be carried out in at least 64-bit signed arithmetic.
* `min`, `max` and `clamp(x, lo, hi) = min(max(x, lo), hi)` have their usual meaning.

### 2.2 Floating-point arithmetic

The coefficient reconstruction (Section 8) and the inverse MDCT (Section 9) use IEEE-754 binary64 ("double"). The following rules apply:

* Every addition, subtraction and multiplication MUST be rounded individually to binary64, using round-to-nearest, ties-to-even.
* Fused multiply-add contraction MUST NOT be used.
* Extended-precision intermediates (for example x87 80-bit registers) MUST NOT be used.
* Re-association, re-ordering or any "fast-math" transformation MUST NOT be applied.
* Operations MUST be evaluated in the order written in this specification: left to right, respecting parentheses.
* Conversion of an `int32` to double is exact.
* `2^s` denotes the exact double value of two to the power s.

*Informative.* For C and C++, the rules above are met with the following compiler settings:

* GCC and Clang: `-ffp-contract=off`, without `-ffast-math`.
* GCC and Clang on 32-bit x86: additionally `-msse2 -mfpmath=sse`.
* MSVC: `/fp:precise`, which is the MSVC default; `/fp:fast` must not be used. On 32-bit x86, `/arch:SSE2`, which is the MSVC default.

Builds that generate x87 floating-point code are not supported (Section 13). The build commands of the reference implementation are given in [docs/BUILD.md](../../docs/BUILD.md).

### 2.3 Rounding functions

* `llround(x)` rounds a double to the nearest integer, with **ties away from zero**. This is the C `llround` function.
* `rq31(a) = (a + 2^30) >> 31` on `int64` values. It rounds a / 2^31 to the nearest integer, with ties towards +∞.
* The final output rounding is `(x + 8) >> 4` on `int64` (Section 11.7).

### 2.4 Units

* `LSB24` is one step of a 24-bit signal. Full scale is ±2^23 LSB24.
* `LSB16` equals 256 LSB24.
* The **fine unit** (FU) equals 1/16 LSB24. One LSB16 equals 4096 FU.
* All time-domain stages of Section 11 operate in FU.
* The inverse-MDCT outputs of the depth and frequency layers are in LSB24.

---

## 3. File structure

### 3.1 RIFF/WAVE

An LAE Lossy file is a RIFF/WAVE file. The first 4 bytes MUST be `RIFF` and bytes 8 to 11 MUST be `WAVE`. Chunks follow from offset 12. Each chunk consists of:

* a 4-byte identifier;
* a `uint32` body size n;
* n body bytes;
* one pad byte if n is odd.

**Decoder parsing rules** (behaviour of the reference reader):

* Chunks MUST be parsed sequentially from offset 12 to the end of the file. The RIFF size field at offset 4 is not used.
* If a chunk body extends beyond the end of the file, it is truncated to the bytes available.
* Unknown chunks MUST be skipped.
* If a chunk identifier occurs more than once, the last occurrence is used.
* The `fmt ` chunk MUST have a body of at least 16 bytes. Its fields are:
  * format tag, `uint16` at body offset 0;
  * channels, `uint16` at offset 2;
  * sample rate, `uint32` at offset 4;
  * bits per sample, `uint16` at offset 14.
* If the format tag is 0xFFFE (WAVE_FORMAT_EXTENSIBLE) and the body is at least 26 bytes long, the format tag is replaced by the `uint16` at body offset 24. These are the first two bytes of the sub-format GUID.
* The effective format tag MUST be 1 (integer PCM). Bits per sample MUST be 16 and channels MUST be 2. A `data` chunk MUST be present.

**Encoder requirements.** An encoder MUST write, in this order:

1. a `fmt ` chunk (PCM, 2 channels, the base rate, 16 bits);
2. the `data` chunk containing the CD layer;
3. the `LAE ` chunk.

The RIFF size MUST cover all chunks. Players that do not know the `LAE ` chunk skip it and play the CD layer.

The reference decoder does not compare the `fmt ` sample rate with the `base` field of Section 4. Encoders MUST write `base` there. Whether a decoder is required to reject a mismatch will be specified in a later revision.

### 3.2 The CD layer (`data` chunk)

The `data` chunk holds interleaved little-endian `int16` samples in the order L, R, L, R, and so on. The number of stereo frames is

`ncd = floor(floor(dlen / 2) / 2)`

where `dlen` is the `data` body size. Trailing bytes that do not form a complete stereo frame are ignored. `ncd` MUST equal `nb - 2*pb` (Section 4).

The CD layer is ordinary PCM and is never modified by the extension.

The only binding check between the CD layer and the extension is this length check. A CD layer that is altered without changing its length is not detected.

### 3.3 The `LAE ` chunk

The chunk identifier is the four bytes `0x4C 0x41 0x45 0x20` (`LAE `). Its body of `n` bytes is laid out as follows:

| Bytes | Content |
|---|---|
| `0 ... n-5` | Main header, edge samples, dither flags and coded streams (Sections 4 and 5) |
| `n-4 ... n-1` | `uint32` CRC-32 of bytes `0 ... n-5`, little-endian |

The chunk body MUST be at least 8 bytes long. A chunk body of 0 bytes is treated by the reference decoder as if no `LAE ` chunk were present (Section 12).

The CRC-32 is the IEEE 802.3 CRC with the following parameters:

* reflected polynomial `0xEDB88320` (normal form `0x04C11DB7`);
* initial value `0xFFFFFFFF`;
* reflected input and output;
* final XOR `0xFFFFFFFF`.

The CRC of the ASCII string `123456789` is `0xCBF43926`. The reference algorithm is:

```
crc = 0xFFFFFFFF
for each byte b: crc = T[(crc XOR b) AND 0xFF] XOR (crc >> 8)
crc = crc XOR 0xFFFFFFFF
where T[i] = i iterated 8 times through: c = (c AND 1) ? 0xEDB88320 XOR (c >> 1) : (c >> 1)
```

A decoder MUST verify the CRC **before** interpreting any other byte of the chunk body. The CRC covers the whole extension once.

In the rest of this document, the **payload** means bytes `0 ... n-5`, that is, the chunk body without the CRC. Offsets are relative to its start.

A field that would extend beyond the end of the payload is an error. The reference reader returns 0 for such a field, and the subsequent validation then always fails.

---

## 4. Main header

The payload begins with thirteen little-endian `int32` fields.

| Offset | Name | Meaning | Valid values |
|---|---|---|---|
| 0 | `magic` | Chunk magic, bytes `L A E 1` | `0x3145414C` |
| 4 | `version` | Container version | `5` |
| 8 | `rate` | Master sample rate (Hz) | `base * 2^stages` |
| 12 | `base` | Base (CD layer) sample rate (Hz) | `44100` or `48000` |
| 16 | `stages` | Number of octave stages between base and master | `1 ... 3` |
| 20 | `Lm` | Master length, samples per channel | `1 ... 2000000000 - 2*P` |
| 24 | `P` | Padding at the master rate, samples per side | `1 ... 65536`, multiple of `2^stages` |
| 28 | `fc` | Detected CD cut-off frequency (Hz) | informational |
| 32 | `mi` | Partition mode index (Section 14) | `0 ... 5` |
| 36 | `eq` | Cut type used by the encoder (0 = unequal, ≠ 0 = equal) | informational |
| 40 | `wm` | Share measure used by the encoder (0 = cells, 1 = energy, 2 = cells + energy) | informational |
| 44 | `nb` | Padded length at the base rate, samples per channel | `(Lm + 2*P) >> stages` |
| 48 | `pb` | Padding at the base rate, samples per side | `P >> stages` |

The header is followed by:

| Offset | Size (bytes) | Content |
|---|---|---|
| 52 | `8*pb` | Edge samples: `4*pb` little-endian `int16` values (Section 4.1) |
| `52 + 8*pb` | 4 | `ndf`, `int32`: number of dither-flag bytes |
| `56 + 8*pb` | `ndf` | Dither flags (Section 4.2) |
| `56 + 8*pb + ndf` | variable | Stream records (Section 5) |

### 4.1 Edge samples

The base-rate signal is defined on a padded timeline of `nb` samples per channel. Indices `[pb, pb + ncd)` hold the CD samples.

The `pb` samples on either side are stored in the edge area as interleaved stereo `int16` pairs, `edge[2*j + c]`:

* `j` in `[0, pb)` gives the leading padding;
* `j` in `[pb, 2*pb)` gives the trailing padding.

The padding extends the signal at both ends. Its content is chosen by the encoder. The decoder uses the stored values and computes nothing.

*Informative.* The reference encoder mirror-pads the master by `P = 4096` samples at each end, reflecting about the end samples (whole-sample symmetric, `src/pad_mirror.cpp`). It derives the 16-bit base-rate signal from the padded master (Section 14) and stores the first and the last `pb` stereo samples of that signal as the edge samples (`src/lae_enc.cpp`).

### 4.2 Dither flags

Let `nbk = ndf / 2`. The flags are the bytes `dfl[c*nbk + b]` for channel `c` in `{0, 1}` and block `b` in `[0, nbk)`. All channel-0 flags come first.

Block `b` covers the padded base-rate indices `[4096*b, 4096*b + 4096)`.

* A zero byte means that no dither was added in that block.
* Any non-zero byte means that TPDF dither was added (Section 11.2).

Encoders SHOULD write 0 or 1.

### 4.3 Validation the decoder MUST perform

The decoder MUST check the following conditions in this order before using any field to address data. If any check fails, the decoder MUST refuse to decode the extension (Section 12).

| # | Condition |
|---|---|
| V1 | `LAE ` chunk body ≥ 8 bytes, and CRC-32 matches (Section 3.3) |
| V2 | `magic == 0x3145414C` and `version == 5` |
| V3 | `base` in `{44100, 48000}` |
| V4 | `1 <= stages <= 3` |
| V5 | `rate == base << stages` |
| V6 | `0 < P <= 65536`, and `P mod 2^stages == 0` |
| V7 | `pb == P >> stages` |
| V8 | `0 < Lm <= 2000000000 - 2*P` |
| V9 | `nb == (Lm + 2*P) >> stages` |
| V10 | `0 <= mi < 6` |
| V11 | At least `8*pb` bytes remain after offset 52 |
| V12 | `ndf == 2 * ceil(nb / 4096)`, and `ndf` bytes remain after the `ndf` field |
| V13 | `ncd == nb - 2*pb` (Section 3.2) |

*Informative.* V6, V7 and V9 imply `nb - 2*pb = floor(Lm / 2^stages)`. With V13, therefore, `ncd = floor(Lm / 2^stages)`.

Derived quantities:

* `Lp = Lm + 2*P`, the padded master length;
* `N = 512`;
* `ncd` (Section 3.2);
* `nbk` (Section 4.2).

---

## 5. Stream records and stream order

### 5.1 Stream records

After the dither flags, the payload contains stream records in a fixed order. Each **stream record** consists of:

* an `int32` value `nbytes`, which MUST be at least 40;
* `nbytes` bytes of stream data, which MUST lie entirely within the payload.

The stream data consist of a 40-byte stream header followed by `nbytes - 40` bytes of range-coded payload.

### 5.2 Stream order

| # | Content |
|---|---|
| 1 | Depth stream, channel L (0) |
| 2 | Depth stream, channel R (1) |
| 3 | `nseg`, `int32`: number of frequency segments per channel. MUST satisfy `1 <= nseg <= 8` |
| 4 | `nseg` frequency stream records of plane M, segments `0 ... nseg-1` |
| 5 | `nseg` frequency stream records of plane S, segments `0 ... nseg-1` |

A decoder MUST parse every record in order, including records whose content it does not decode, because each record is located from the end of the previous one.

The reference decoder ignores any bytes between the end of the last S record and the CRC. The reference encoder writes no such bytes; the CRC follows the last S record directly (`src/lae_enc.cpp`). Encoders SHOULD NOT write any.

### 5.3 Stream header

The stream header consists of ten little-endian `int32` fields.

| Offset | Name | Meaning | Valid values |
|---|---|---|---|
| 0 | `K` | Number of frames in the plane | `> 0` (checked) |
| 4 | `N` | Coefficients per frame | `== 512` (checked) |
| 8 | `k0` | First coded bin | `>= 0` (checked) |
| 12 | `k1` | One past the last coded bin | `k0 < k1 <= 512` (checked) |
| 16 | `rate` | Sample rate of the plane's signal | not used by the decoder |
| 20 | `L` | Signal length of the plane | not used by the decoder |
| 24 | `cw` | `CW + 256 * rule` | Encoders MUST write `CW = 8` in the low byte (not checked). `rule = (cw >> 8) & 255` (Section 8.2) |
| 28 | `lv` | Partition mode index | not used by the decoder |
| 32 | `eq` | Cut type: 0 = unequal cuts, ≠ 0 = equal cuts (Section 7) | any |
| 36 | `plen` | Range-coded payload length | `== nbytes - 40` (checked) |

Notes on the stream header fields:

* The reference decoder does not check the low byte of `cw`. It always uses a cell width of 8.
* `K` SHOULD equal `frames(Ls)` (Section 9.1). `Ls` is `nb` for depth streams and `Lp` for frequency streams.
* If `K` is smaller than `frames(Ls)`, the missing frames are decoded as zero. If `K` is larger, the extra frames are never requested.
* The exact values that the reference encoder writes into `rate` and `L` will be documented in a later revision.

The checks marked "(checked)" MUST be performed for every stream that is decoded. Streams that are skipped (Section 5.4) are checked only for `nbytes >= 40` and for lying within the payload.

### 5.4 Stream selection per route

**Depth streams** are decoded in the 24-bit and cd24 routes. They are skipped in the 16-bit route.

**Frequency streams** are skipped in the cd24 route. In the 24-bit and 16-bit routes, with output rate `rate >> d`, a frequency stream is decoded if and only if its `k0` is less than `kcut`, where

`kcut = 512 >> d`.

The reference decoder computes the same value as `lround(512 * (orate/2) / (rate/2))`.

In the reference decoder, `k0` is read from the stream header before the record is consumed. If fewer than 16 bytes remain at the record position, `k0` is taken as 0. Such a record is then necessarily rejected by the record checks of Section 5.1.

The selected M streams form the M signal, and the selected S streams form the S signal (Section 9.5). If **no M stream** is selected, the frequency layer is omitted entirely, including any selected S streams (Section 11.5).

*Informative.* Frequency bins are expressed at the master rate: bin k of 512 is centred at `(k + 1/2) * (rate/2) / 512`. The reference encoder stores one stream per octave (comment in `src/lae_dec.cpp`). The decoder relies only on the `k0 < kcut` test.

### 5.5 Coefficient-plane geometry

A stream describes a plane `X[m][k]` with `m` in `[0, K)` and `k` in `[0, 512)`. Bins outside `[k0, k1)` are zero. The geometry is defined in `src/partition_format.h`.

* Bins `[k0, k1)` are grouped into `nc = ceil((k1 - k0) / 8)` **cells** of `CW = 8` bins.
* Cell `cc` covers bins `kb = k0 + 8*cc` up to `min(k1, kb + 8) - 1`. The last cell may therefore be narrower than 8 bins.
* Cells are grouped into **root blocks** of `ROOTF = 16` cells × `ROOTT = 8` frames. Roots at the high-frequency edge and at the end of the track may be smaller.

---

## 6. Range decoder

Each decoded stream has its own range decoder (`struct Dec` in `src/part_stream.h`). It is initialised at the start of the stream's coded payload, which is at offset 40 of the stream data and has length `plen`.

### 6.1 State and byte input

* `range` is a `uint32`, initialised to `0xFFFFFFFF`.
* `code` is a `uint32`, initialised to 0.
* `next()` returns the next payload byte. Once all `plen` bytes have been consumed, it returns 0.

### 6.2 Initialisation

```
repeat 5 times: code = (code << 8) | next()        // uint32: the first byte is shifted out
```

*Informative.* The reference encoder writes a leading `0x00` byte, which is the byte that is shifted out.

### 6.3 Adaptive binary decision

A probability `p` is a `uint16` holding the probability of a 0-bit in units of 1/2048. It is initialised to 1024.

```
bit(p):
  bound = (range >> 11) * p                         // uint32
  if code < bound:  range = bound;  p = p + ((2048 - p) >> 5);  b = 0
  else:             code = code - bound;  range = range - bound;  p = p - (p >> 5);  b = 1
  while range < 2^24:  range = range << 8;  code = (code << 8) | next()
  return b
```

The adaptation shift is fixed at 5.

### 6.4 Bypass (direct) bits

```
direct(n):                                          // n >= 1, most significant bit first
  v = 0
  repeat n times:
    range = range >> 1
    b = (code >= range) ? 1 : 0
    if b: code = code - range
    v = (v << 1) | b
    while range < 2^24:  range = range << 8;  code = (code << 8) | next()
  return v
```

### 6.5 Binary trees

`tree(pr, nbits)` decodes an `nbits`-bit value, most significant bit first, through the probability array `pr`. It uses nodes `1 ... 2^nbits - 1`:

```
node = 1
repeat nbits times:  node = 2*node + bit(pr[node])
return node - 2^nbits
```

### 6.6 Integer model `U`

A `U` model holds two arrays, `tree[64]` and `mant[64]`, with all probabilities initialised to 1024. `U(model)` decodes a non-negative integer `v < 2^31` from its bit length:

```
len = tree(model.tree, 5)                           // 0 ... 31
if len <= 1: return len
top  = bit(model.mant[len])
nr   = len - 2
rest = (nr > 0) ? direct(nr) : 0
return 2^(len-1) + top*2^nr + rest                  // uint32
```

### 6.7 Termination

The reference encoder flushes four bytes at the end of a stream. A decoder MUST NOT require any termination pattern and MUST NOT check how many bytes it has consumed. Reads beyond `plen` return 0 (Section 6.1).

### 6.8 Model persistence

All adaptive models of a stream are created once, when the stream is opened. They persist through the whole stream, across root and row boundaries.

Version 5 defines no random-access points. Each stream is decoded from its beginning.

---

## 7. Partition syntax

The symbol walk is implemented by `Walker` in `src/part_stream.h`. The geometry helpers are in `src/partition_format.h`.

### 7.1 Models and state per stream

| Name | Count | Initial value | Use |
|---|---|---|---|
| `TierP` | 4 contexts × 4 nodes | 1024 | Tier, 2-bit tree. Context = previous leaf tier |
| `ExpZero[t]` | 4 (t = 1 ... 3 used) | 1024 | Exponent delta is zero |
| `ExpSign[t]` | 4 (t = 1 ... 3 used) | 1024 | Exponent delta sign |
| `ExpU[t]` | 4 `U` models (t = 1 ... 3 used) | 1024 | Exponent delta magnitude − 1 |
| `ZlP` | 4 contexts × 8 nodes | 1024 | Zero-level index, 3-bit tree. Context = tier |
| `CoefU` | 3 × 13 `U` models | 1024 | Coefficient magnitudes |
| `prevTier` | | 0 | Tier of the previous leaf, including tier-A leaves |
| `prevE` | | 12 | Exponent of the previous non-A leaf |

### 7.2 Helper functions

```
cuts(w, equal):                    // admissible cut positions of an extent w
  if w < 2:   return []
  if equal:   return [floor(w / 2)]
  return [1, 2, ..., w - 1]

posBits(n): smallest b >= 0 with 2^b >= n
```

### 7.3 Stream walk

Roots are visited row by row in increasing time order, 8 frames per row. Within a row, roots are visited in increasing frequency order.

```
decode_plane(hdr):
  nc = ceil((k1 - k0) / 8)
  for M0 = 0; M0 < K; M0 += 8:
    rt = min(8, K - M0)
    set X[M0 ... M0+rt-1][0 ... 511] = 0
    for C0 = 0; C0 < nc; C0 += 16:
      rf = min(16, nc - C0)
      node(0, rf, 0, rt)           // cell range [f0, f1), frame range [t0, t1), relative to the root
```

### 7.4 Nodes

Each node either splits into two parts along one direction or is a leaf. Split, direction and position bits are bypass bits.

```
node(f0, f1, t0, t1):
  cf = cuts(f1 - f0, eq != 0);  ct = cuts(t1 - t0, eq != 0)
  if cf != [] or ct != []:
    split = direct(1)
    if split = 1:
      if ct = []:      dir = 0                   // frequency cut
      else if cf = []: dir = 1                   // time cut
      else:            dir = direct(1)
      cc = (dir = 0) ? cf : ct
      nb = posBits(|cc|)
      pi = (nb > 0) ? direct(nb) : 0
      if pi >= |cc|: CORRUPT; pi = |cc| - 1
      p = cc[pi]
      if dir = 0: node(f0, f0+p, t0, t1); node(f0+p, f1, t0, t1)
      else:       node(f0, f1, t0, t0+p); node(f0, f1, t0+p, t1)
      return
  leaf(f0, f1, t0, t1)
```

### 7.5 Leaves

Each leaf carries its own tier, exponent and zero-level index. These are signalled only at leaves. Interior nodes carry nothing.

```
leaf(f0, f1, t0, t1):
  tier = tree(TierP[4*prevTier ...], 2)          // 0 = A, 1 = B, 2 = C, 3 = D
  prevTier = tier
  if tier = 0: return                            // all coefficients of the leaf stay 0

  if bit(ExpZero[tier]) = 1:  d = 0
  else:
    neg = bit(ExpSign[tier])
    a   = U(ExpU[tier])
    d   = neg ? -(a + 1) : (a + 1)
  if prevE + d < 0 or prevE + d > 34: CORRUPT; d = clamp(prevE + d, 0, 34) - prevE
  E = prevE + d;  prevE = E

  z = tree(ZlP[8*tier ...], 3)                   // zero-level index 0 ... 7
  s    = shift(E, tier, rule)                    // Section 8.2
  step = 2^s
  cm   = CoefU[(tier - 1)*13 + min(12, floor(E / 2))]

  for m = M0+t0 ... M0+t1-1:                     // frames
    for cell = C0+f0 ... C0+f1-1:                // cells, increasing frequency
      kb = k0 + 8*cell;  nbins = min(k1, kb + 8) - kb
      for j = 0 ... nbins-1:
        a = U(cm)
        if a > 0: neg = direct(1); q = neg ? -a : a
        else:     q = 0
        zs = (q = 0 and z > 0) ? direct(1) : 0
        X[m][kb + j] = dequant(q, z, zs, step)   // Section 8.3
```

Context selection:

* The coefficient model is selected by the leaf's tier and exponent.
* The tier context is the tier of the previous leaf in decoding order.
* The exponent is coded as a difference to the exponent of the previous non-A leaf.

### 7.6 Corrupt-stream handling

`CORRUPT` marks the stream as corrupt. The reference decoder clamps the offending value as shown and continues without overrunning memory. At the end of decoding, it discards the output (Section 12). Because the output is never delivered in that case, the clamped values have no normative effect. A decoder MUST nevertheless remain memory-safe for every input.

The reference decoder decodes only the rows of a stream, and computes only the inverse-MDCT output samples, that the requested route and output rate need. A `CORRUPT` condition in parts that are never needed can therefore go undetected. Whether a decoder is required to detect `CORRUPT` in the whole stream, or only in the parts it needs, will be specified in a later revision.

---

## 8. Coefficient decoding and dequantisation

The tables and functions of this section are defined in `src/partition_format.h`.

### 8.1 Tiers

| Tier | Code | `TB[tier]` (bits relative to the leaf exponent) |
|---|---|---|
| A | 0 | 0 (no coefficients coded; all zero) |
| B | 1 | 2 |
| C | 2 | 4 |
| D | 3 | 8 |

### 8.2 Precision rules

The step exponent `s` is computed from the leaf exponent `E`, the tier and the stream's `rule = (cw >> 8) & 255`. The constants are `TB = {0, 2, 4, 8}`, `TB16 = {0, 0, 4, 8}` and `STEPMAX = 8`.

| `rule` | `s = shift(E, tier, rule)` | Intended use (informative) |
|---|---|---|
| 1 | `max(0, min(E, 8) - TB[tier])` | Scheme 1, both layers; scheme 2, depth layer. Loud blocks: B 18 / C 20 / D 24 bit |
| 2 | `max(0, min(E - TB[tier], 8 - TB16[tier]))` | Scheme 2, frequency layer. Loud blocks: B 16 / C 20 / D 24 bit |
| 3 | `max(0, min(E, 6) - TB[tier])` | Depth layer of mode keep20. Loud blocks: B 20 / C 22 / D 24 bit |
| 0, or any other value | `min(8, max(0, E - TB[tier]))` | Default rule (the formula of container version 4) |

Version 5 encoders write rule 1, 2 or 3. Other values are reserved. A decoder MUST nevertheless apply the last row of the table to them, as the reference decoder does.

The `rule` value is a per-stream field. The last row gives every value of that field a defined decoding; its formula is the one used in container version 4. Files of container version 4 are themselves rejected by check V2 (Section 4.3). Whether reserved `rule` values must instead be rejected will be specified in a later revision.

Every rule satisfies `0 <= s <= 8`. No quantisation step is therefore coarser than 1 LSB16, the step of the CD layer.

*Informative.* The encoder derives `E` from the peak of the leaf. In the reference encoder, `E` is the maximum of the per-cell exponents of the cells in the leaf (`src/partition.h`). The computation of the per-cell exponents will be documented in a later revision.

### 8.3 Reconstruction

`dequant(q, z, zs, step)` computes `v` and stores `llround(v)` into the plane as an `int32`:

```
if q > 0:       v = ((double)q + 0.20) * step
else if q < 0:  v = ((double)q - 0.20) * step
else if z > 0:  v = (zs = 1 ? -1.0 : 1.0) * ZC[z] * step      // evaluated left to right
else:           v = 0.0
if v > 1073741823.0 or v < -1073741823.0: CORRUPT; v = clamp(v, -1073741823.0, 1073741823.0)
X = llround(v)
```

* `0.20` is the binary64 value nearest to 0.20 (`0x1.999999999999ap-3`). The reconstruction offset `RECOFF = 0.20` is a format constant.
* Every addition, subtraction and multiplication above is rounded individually to binary64 (Section 2.2).
* `llround` rounds ties away from zero. This matters for zero-level values such as 0.5.

The quantiser dead zone is **encoder-only**. It is not signalled and does not affect decoding. The reference encoder uses a dead zone of 0.30 (Section 14).

### 8.4 Zero level

The zero-level index `z` of a leaf selects `ZC[z]` from a table of dyadic constants, all of which are exact in binary:

| `z` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| `ZC[z]` | 0 | 1/32 | 1/16 | 3/32 | 1/8 | 3/16 | 1/4 | 3/8 |

* If `z = 0`, coefficients quantised to zero are reconstructed as 0.
* If `z > 0`, every zero coefficient of the leaf carries one bypass bit, `zs`. This bit is the coefficient's true sign (1 = negative). The coefficient is reconstructed as `-ZC[z] * step` if `zs = 1`, and as `ZC[z] * step` otherwise.

The reference encoder writes `z > 0` only when the optional `zs` encoder option is enabled. That option is off by default. Decoders MUST support all values of `z`.

---

## 9. Inverse MDCT

The transform is implemented in `src/mdct_tb.h`.

### 9.1 Frame layout

The transform is an MDCT with sine window, `N = 512` coefficients per frame and 50 % overlap.

For a signal of length `Ls`:

* the padded sequence is `[N zeros | Ls signal samples | zeros]`;
* the number of frames is `frames(Ls) = ceil(Ls / N) + 1`;
* frame `m` covers padded positions `[m*N, m*N + 2N)`;
* signal sample `i` is at padded position `i + N`.

| Signal | `Ls` | Rate |
|---|---|---|
| Depth, channel c | `nb` | base |
| M, S | `Lp` | master |

### 9.2 Constant tables

Let `M = N/2 = 256`. The tables are defined by these generating formulas (`tools/gen_mdct_tables.cpp`):

| Table | Size | Formula |
|---|---|---|
| `WIN[n]` | 1024 | `sin(pi * (n + 1/2) / (2*N))` |
| `PRE_C[n]`, `PRE_S[n]` | 256 | `cos`, `sin` of `-2*pi * (n + 1/8) / (2*N)` |
| `POST_C[k]`, `POST_S[k]` | 256 | `cos`, `sin` of `-2*pi * (k + 1/8) / (2*N)` |
| `FFT_C[k]`, `FFT_S[k]` | 128 | `cos`, `sin` of `-2*pi * k / M` |
| `BITREV[i]` | 256 | `i` with its 8 bits in reverse order (integers) |
| `SCALE` | 1 | `1 / sqrt(M) = 2^-4 = 0x1p-4` (exact) |

The pre- and post-twiddles are both `exp(-i * 2*pi * (n + 1/8) / (2*N))`. The PRE and POST tables are generated by the same expression and therefore hold identical values.

*Informative.* This twiddle form is that of FFmpeg `libavcodec/mdct.c` (Section 15.3).

The exact values are the hexadecimal double literals of `src/mdct_tables.h`, generated by `tools/gen_mdct_tables.cpp`. Values recomputed with a host math library may differ in the last bit. A decoder MUST use the values of `src/mdct_tables.h`. The content of `src/mdct_tables.h` will be reproduced as a normative annex of this document in a later revision.

### 9.3 DCT-IV through an M-point complex FFT

`DCT4` is the orthonormal DCT-IV, which is self-inverse:

$$
X[k] = \sqrt{\frac{2}{N}} \sum_{n=0}^{N-1} u[n] \cos\left(\frac{\pi}{N}\left(n + \frac{1}{2}\right)\left(k + \frac{1}{2}\right)\right)
$$

It MUST be computed exactly as follows. Each product `x*y` is rounded, then each sum or difference is rounded, in the order written.

```
DCT4(c[0 ... N-1]) -> u[0 ... N-1]:
  for n = 0 ... M-1:
    a = c[2n];  b = c[N-1-2n]
    re[n] = a*PRE_C[n] - b*PRE_S[n]
    im[n] = a*PRE_S[n] + b*PRE_C[n]
  FFT(re, im)
  for k = 0 ... M-1:
    yr = (re[k]*POST_C[k] - im[k]*POST_S[k]) * SCALE
    yi = (re[k]*POST_S[k] + im[k]*POST_C[k]) * SCALE
    u[2k]     = yr
    u[N-1-2k] = -yi

FFT(re, im):                                     // radix-2, decimation in time, M = 256
  for i = 0 ... M-1:                             // bit-reversal permutation
    j = BITREV[i]
    if i < j: swap(re[i], re[j]); swap(im[i], im[j])
  for len = 2, 4, ..., M:
    step = M / len;  h = len / 2
    for i = 0; i < M; i += len:
      for k = 0 ... h-1:
        wr = FFT_C[k*step];  wi = FFT_S[k*step]
        xr = re[i+k+h];  xi = im[i+k+h]
        tr = xr*wr - xi*wi
        ti = xr*wi + xi*wr
        re[i+k+h] = re[i+k] - tr;  im[i+k+h] = im[i+k] - ti
        re[i+k]   = re[i+k] + tr;  im[i+k]   = im[i+k] + ti
```

### 9.4 Inverse transform, overlap-add and output rounding

The accumulator `y` is a double array over the padded positions, initialised to `+0.0`. Frames are processed in increasing `m`. Each position accumulates its contributions in frame order:

```
for m = 0 ... frames(Ls)-1:
  c[k] = sum over planes of (double) X_plane[m][k]   // exact (Section 9.5); 0 if no plane has frame m
  u = DCT4(c);  s = m*N
  for n = 0 ... N/2-1:                               // unfold [u2, -u2r, -u1r, -u1], window, add
    y[s + n]        += u[N/2 + n]       * WIN[n]
    y[s + N/2 + n]  += (-u[N-1-n])      * WIN[N/2 + n]
    y[s + N + n]    += (-u[N/2-1-n])    * WIN[N + n]
    y[s + 3N/2 + n] += (-u[n])          * WIN[3N/2 + n]
```

The samples at positions `[s, s + N)` are final once frame `m` has been added. Output sample `i` in `[0, Ls)` is

```
v = y[i + N]
if v > 1073741823.0 or v < -1073741823.0: CORRUPT; v = clamp(v, -1073741823.0, 1073741823.0)
o[i] = (int32) llround(v)
```

The transform MUST be computed with exactly the structure given in Sections 9.3 and 9.4. Any other factorisation generally changes the output bits.

*Informative.* The forward transform used by the encoder works as follows:

1. It windows the 2N samples.
2. It folds them into `[-c_r - d, a - b_r]`, that is, `u[n] = -z[3N/2-1-n] - z[3N/2+n]` and `u[N/2+n] = z[n] - z[N-1-n]`.
3. It applies `DCT4` and `llround`.

### 9.5 Summing planes

Each IMDCT signal sums the selected planes before the transform:

* depth channel c: its single depth plane;
* M: the selected M segments;
* S: the selected S segments.

The segments are intended to be disjoint in frequency. Each plane value is an `int32` with magnitude at most 1073741823, and there are at most 8 planes. Their double sum is therefore exact, and its order is irrelevant.

A plane whose stream has fewer than `m + 1` frames contributes zero to frame `m`. The transform of an all-zero frame contributes only zeros, so it MAY be skipped.

---

## 10. Half-band interpolation and decimation

### 10.1 Tables

The half-band filters are symmetric, odd-length FIR filters. Only the odd taps `h[1], h[3], ..., h[half]` are stored, as `int32` values in Q31. The centre tap is `2^30`. The stored taps are listed here as `ht[t] = h[2t+1]` (`src/halfband.h`).

```
Table 0 (base*1 <-> base*2), half = 79, 40 values:
 682938002, -225979149, 133607419, -93347908, 70491357, -55579122, 44978765, -36999032,
 30747123, -25707634, 21562088, -18102637, 15187220, -12714693, 10610315, -8816912,
 7289329, -5990851, 4890848, -3963205, 3185274, -2537151, 2001191, -1561677,
 1204578, -917388, 688994, -509581, 370536, -264370, 184638, -125854,
 83405, -53473, 32940, -19306, 10605, -5322, 2321, -771
Table 1 (base*2 <-> base*4), half = 15, 8 values:
 668035431, -184994700, 75935913, -29954182, 10005932, -2547898, 417025, -26609
Table 2 (base*4 <-> base*8), half = 11, 6 values:
 656183834, -156920197, 47052629, -10858972, 1477097, -63479
Table 3 (base*8 <-> base*16), half = 9, 5 values (defined in halfband.h; not used in container v5):
 644456397, -132671512, 28454516, -3484033, 115544
```

In every table the stored taps sum to exactly 2^29. The DC gain of each interpolator and decimator is therefore exactly 1.

The same table serves the interpolator and the decimator for a given pair of rates. Table `k` connects `base*2^k` and `base*2^(k+1)`.

### 10.2 Edge extension

Input indices outside `[0, n)`, where `n` is the input length, are reflected about the end samples (whole-sample symmetric) and then clamped:

```
mirror(i, n):
  if i < 0:   i = -i
  if i >= n:  i = 2*(n - 1) - i
  return clamp(i, 0, n - 1)
```

### 10.3 Interpolation by 2

The input `x` has length `n`. The output `y` has length `2n`. Table `k` has `T` stored taps.

```
y[2m]     = x[m]
y[2m + 1] = rq31(2 * A),   A = sum_{t=0}^{T-1} ht[t] * ( x[mirror(m - t, n)] + x[mirror(m + 1 + t, n)] )      // int64
```

### 10.4 Decimation by 2

The input `x` has length `n`. The output `y` has length `floor(n / 2)`.

```
y[i] = rq31( x[2i]*2^30 + sum_{t=0}^{T-1} ht[t] * ( x[mirror(2i - 1 - 2t, n)] + x[mirror(2i + 1 + 2t, n)] ) )      // int64
```

Only one rounding occurs per output sample. Because integer accumulation is exact, the order of summation is irrelevant.

The value bounds of Sections 8.3 and 9.4 do not by themselves exclude `int64` overflow in these accumulators:

* With `|D_c[i]| <= 1073741823`, the magnitude of `B_c[i]` (Section 11.2) can exceed 2^34 FU.
* The absolute values of the taps of table 0 sum to more than 2^30.
* The worst-case accumulator of Section 10.3 therefore exceeds 2^63.

The reference decoder does not detect `int64` overflow, and the result for streams that would cause it is not specified. Whether such streams must be rejected, and a guaranteed bound for valid streams, will be specified in a later revision.

---

## 11. Reconstruction pipeline

All signals in this section are per channel `c` in `{0 = L, 1 = R}`, in FU, and computed in `int64` unless stated otherwise.

### 11.1 Route and output rate

| Route | `d` | Output rate | `Po` (output offset) | `Lo` (output length) |
|---|---|---|---|---|
| 24-bit, 16-bit | `0 <= d < stages` | `rate >> d` | `P >> d` | `Lm >> d` |
| cd24 | — | `base` | `pb` | `nb - 2*pb` (= `ncd`) |

For the 24-bit and 16-bit routes, a requested output rate that is not of the form `rate >> d` with `0 <= d < stages` MUST be rejected.

### 11.2 Base signal and dither regeneration

Two xorshift32 generators are used, one per channel. Their states are `uint32`:

* `g_0` is seeded with `0x12345678`;
* `g_1` is seeded with `0x12345678 + 0x9E3779B9 = 0xB06BD031`.

The seeds are format constants and are not transmitted.

```
xs(g): g = g XOR (g << 13); g = g XOR (g >> 17); g = g XOR (g << 5); return g
```

For each channel `c`, for every padded base index `i = 0, 1, ..., nb - 1` in order:

```
g_c = xs(g_c);  u1 = (g_c >> 20) AND 0xFFF
g_c = xs(g_c);  u2 = (g_c >> 20) AND 0xFFF
delta_c[i] = (dfl[c*nbk + floor(i / 4096)] != 0) ? (u1 + u2 - 4096) : 0
```

The generator advances by two steps for **every** index, including the padding and blocks whose flag is 0. The result is triangular-PDF dither in `[-4096, 4094]` FU, about ±1 LSB16.

The CD sample `x_c[i]` is:

* `edge[2*i + c]` for `i < pb`;
* `cd[2*(i - pb) + c]` for `pb <= i < pb + ncd`;
* `edge[2*pb + 2*(i - pb - ncd) + c]` otherwise.

Here `cd` is the interleaved `int16` data of Section 3.2. The base signal is

```
B_c[i] = 4096 * x_c[i] - delta_c[i] + 16 * D_c[i]         for i in [0, nb)
```

* `D_c` is the inverse-MDCT output of the depth plane of channel `c` (Section 9, `Ls = nb`), in LSB24.
* In the 16-bit route the term `16 * D_c[i]` is omitted. The dither is still subtracted.

### 11.3 Interpolation to the master rate

In the 24-bit and 16-bit routes, `B_c` is interpolated `stages` times (Section 10.3). Stage `k = 0, 1, ..., stages-1` uses table `k`. The result is `U_c`, of length `nU = nb * 2^stages`.

### 11.4 Frequency layer (M/S inverse)

`m[i]` and `s[i]` are the inverse-MDCT outputs of the summed selected M and S planes (`Ls = Lp`), in LSB24. For `i` in `[0, Lp)`:

```
r      = m[i] - (s[i] >> 1)
F_0[i] = 16 * (r + s[i])                              // L
F_1[i] = 16 * r                                       // R
```

If no S plane is selected, `s[i] = 0`.

### 11.5 Mix

For `i` in `[0, Lp)`:

```
Z_c[i] = (i < nU ? U_c[i] : 0) + F_c[i]
```

If no M plane is selected, `F_c[i] = 0`. The frequency layer is added here, in the time domain.

### 11.6 Decimation to the output rate

In the 24-bit and 16-bit routes, `Z_c` (length `Lp`) is decimated `d` times (Section 10.4). Decimation step `k = 0, 1, ..., d-1` uses table `stages - 1 - k`. The result is `Y_c`.

In the cd24 route, `Y_c = B_c`.

### 11.7 Final rounding and hard limiter

For `i` in `[0, Lo)`:

```
v = (Y_c[Po + i] + 8) >> 4                             // FU -> LSB24, round half up
o_c[i] = clamp(v, -8388608, 8388607)                   // hard limiter
```

### 11.8 Output file

The output is a 44-byte canonical WAV header followed by `Lo` stereo frames. Each frame contains `o_0[i]` then `o_1[i]`. Each value is written as 3 little-endian bytes, the low 24 bits of its two's-complement representation. With `D = 6*Lo`:

| Offset | Content |
|---|---|
| 0 | `RIFF` |
| 4 | `uint32` `36 + D` |
| 8 | `WAVE` |
| 12 | `fmt ` |
| 16 | `uint32` 16 |
| 20 | `uint16` 1 (PCM) |
| 22 | `uint16` 2 (channels) |
| 24 | `uint32` output rate |
| 28 | `uint32` 6 × output rate |
| 32 | `uint16` 6 (block align) |
| 34 | `uint16` 24 (bits) |
| 36 | `data` |
| 40 | `uint32` `D` |
| 44 | sample data |

`D` is always even, so no pad byte follows.

If `37 + D > 0xFFFFFFFF`, the output cannot be represented as WAV and MUST be refused. This is the size check of the reference writer (`wavio::fits32`), which allows for one pad byte.

The 16-bit route also produces a 24-bit file.

---

## 12. Error handling

The CD layer is ordinary PCM and is never affected by an error in the extension. When a decoder refuses to decode the extension, a player SHOULD fall back to playing the `data` chunk as 16-bit audio.

| Condition | Required behaviour | Reference exit status (informative) |
|---|---|---|
| Self-test failure (Section 13; the reference decoder runs it before reading the input) | MUST NOT decode | 4 |
| Not a RIFF/WAVE file; no `data` chunk; not 16-bit two-channel integer PCM | Not an LAE Lossy file. MUST NOT decode | 1 |
| No `LAE ` chunk, or an `LAE ` chunk with an empty body | Plain CD audio. No extension to decode | 1 |
| `LAE ` chunk body < 8 bytes, or CRC mismatch | MUST refuse before parsing any field | 3 |
| Magic or version not `LAE1` / 5 | MUST NOT decode (unknown version) | 1 |
| Any of V3–V11 fails | MUST refuse | 3 |
| V12 fails (dither flags) | MUST refuse | 3 |
| V13 fails (CD length differs: edited or misaligned CD layer) | MUST refuse | 3 |
| Output rate invalid for the route (Section 11.1); checked after V13, before the stream records | MUST refuse | 1 |
| A stream record outside the payload; a decoded stream header invalid (Section 5.3); `nseg` outside `[1, 8]` | MUST refuse | 3 |
| Output exceeds the WAV size limit (Section 11.8) | MUST refuse | 1 |
| `CORRUPT` raised during decoding (Sections 7.4, 7.5, 8.3, 9.4) | MUST report the stream as corrupt and MUST NOT deliver the output. The reference decoder deletes the file it has written | 3 |

A decoder MUST remain memory-safe for every input. The reference decoder validates every header field before using it to address data, and clamps out-of-range decoded values.

The reference decoder contains two encoder-only hooks, `LEAKOUT` and `CLIPMAP`. They are compiled in only when the encoder builds the decoder (`LAE_ENCODER_HOOKS`) and are not part of the format.

---

## 13. Self-test hash

The self-test detects builds that do not follow the arithmetic of Section 2.2. Such builds would produce output that differs between platforms. The test is:

1. Let `L = 6*N = 3072` and `K = frames(L) = 7`.
2. Form the coefficients `X[i]` for `i` in `[0, 3584)` in `uint32` arithmetic:

   `X[i] = (int32)((((i * 2654435761) mod 2^32) >> 7) mod 2000001) - 1000000`

   Frame `m` consists of `X[512*m ... 512*m + 511]`.
3. Apply the inverse MDCT of Section 9.4 with `Ls = L`, giving 3072 `int32` values `o[i]`. The `CORRUPT` check is omitted, because it cannot trigger here.
4. Compute a 64-bit FNV-1a-style hash over the bytes of `o` in little-endian order:

   ```
   h = 1469598103934665603                   // 0x14650FB0739D0383
   for each o[i]: for b = 0 ... 3: h = (h XOR (((uint32)o[i] >> 8*b) AND 0xFF)) * 1099511628211   (mod 2^64)
   ```

   The initial value is the one used by the reference implementation (`src/mdct_tb.h`). It is **not** the standard FNV-1a offset basis (14695981039346656037 = `0xcbf29ce484222325`). Implementations MUST use the value given here.
5. The result MUST equal `0x729b547fbdbfe203`.

A decoder SHOULD run this test at start-up. A decoder that runs it and obtains a different value MUST NOT decode.

Passing the self-test does not replace the conformance test of Section 1.3.

*Informative.* Status of the reference implementation ([docs/BUILD.md](../../docs/BUILD.md)):

* The decoder and the encoder build and run on Windows (MSVC) and on Linux (GCC 13 at `-O2` and `-O0`, Clang 18). The decoder has also been built with GCC as a 32-bit SSE2 build.
* On Linux x86, the decoder outputs were identical in 256 of 256 comparisons, and the encoder reproduces all 16 vector files byte for byte.
* On Linux ARM64 (aarch64; GCC 13 at `-O2` and `-O0`, Clang 18 at `-O2`), the decoder outputs were identical in 192 of 192 comparisons, and the encoder (GCC 13, `-O2`) reproduces all 16 vector files byte for byte. Other ARM platforms have not been tested.
* x87 builds are not supported; the reference decoder's self-test refuses them.

---

## 14. Encoder-side elements (informative)

The following elements belong to the encoder. They are listed so that implementers can understand the meaning of the bitstream. They do not affect decoding.

Unless another source is cited, the statements in this section are based on the reference encoder sources `src/lae_enc.cpp`, `src/partition.h`, `src/pad_mirror.cpp`, `src/base_layer.cpp` and `src/routes.cpp`.

* **Layer targets.** The decoder adds the three parts (CD path, depth layer, frequency layer) in the time domain (Section 11).
  * The encoder derives the CD layer from the master and adds TPDF dither in the blocks flagged in Section 4.2.
  * **CD layer derivation** (`src/base_layer.cpp`). The padded master (Section 4.1) is converted to FU and decimated `stages` times to the base rate with the integer half-band decimators of Section 10.4, highest-rate stage first; intermediate values are limited only to the `int32` range. With `x` the decimated value in FU and `delta` the dither of Section 11.2 (0 in blocks whose flag is 0), each CD sample is `clamp((x + delta + 2048) >> 12, -32768, 32767)`.
  * **Dither decision** (`src/base_layer.cpp`). For each channel and each block of 4096 base-rate samples, the encoder fits a 16th-order linear predictor to the decimated signal and computes the RMS of the prediction residual in LSB24. The block is dithered if this RMS is below 128 LSB24 (0.5 LSB16) and the block is not digital silence. All-zero blocks are never dithered, so the CD layer keeps true zeros.
  * **Depth target** (`src/routes.cpp`). Let `v = 256 * CD - delta/16` be the CD path in LSB24, and `B24 = (x + 8) >> 4` the decimated master in LSB24, which is not limited to full scale. The depth target is `T = llround(lowpass(B24 - v))`, limited to `[-8388608, 8388607]`. The lowpass is a zero-phase windowed-sinc FIR filter at the base rate (511 taps, Kaiser window β = 9) with cut-off `fc`. It is omitted when `fc >= base/2 - 1` Hz.
  * The depth layer covers the base-rate band.
  * **Frequency target** (`src/routes.cpp`). The frequency target, in LSB24 at the master rate, is `F = master - up(v + T)`, limited to `[-8388608, 8388607]`. `up` interpolates by 2 `stages` times with the half-band tables of Section 10.1; the encoder computes it in double precision.
  * The frequency layer starts below the detected CD cut-off: the encoder passes a start frequency of `fc - 1000` Hz to the layer coder (`src/lae_enc.cpp`). `fc` records the cut-off.
  * **Cut-off detection** (`src/routes.cpp`). From averaged cross-spectra of the master and the interpolated CD path, the encoder scans downwards from the base-rate Nyquist frequency to 15 kHz. It sets `fc` just above the highest frequency at which the transfer of the CD path is at least −0.1 dB. Bins in which the master has almost no energy are skipped. If no such frequency is found, `fc = 15000` Hz.
* **Independent CD input.** The encoder option `cd=<file>` accepts a separately supplied 16-bit two-channel CD at the base rate.
  * The supplied CD becomes the CD layer bit for bit. The encoder places the master on the time axis of the CD and moves the master by whole master samples and by a fraction of a master sample; the CD is never moved (README, Section 4.3). Nothing about the alignment is stored in the file, and decoding is unchanged.
  * All dither flags are then 0.
  * The CD cut-off is found by a coherence test (threshold 0.98), which does not depend on the gain of the CD, instead of the transfer test.
  * The extension codes the master minus the CD path.
  * The edge samples (Section 4.1) are still taken from the CD layer that the encoder derives from the master.
* **Partition search** (`src/partition.h`).
  * The search runs per root block, in parallel. The number of threads does not affect the encoded stream ([docs/BUILD.md](../../docs/BUILD.md)).
  * The search aims at the D and C share targets of the selected mode (`src/partition_format.h`). The share targets exist only in the encoder.
  * The decoder needs no mode table. `mi`, `lv`, `wm` and the main-header `eq` are informational.
  * For given penalties λD and λC, a dynamic program over all sub-rectangles of each root chooses the cut tree and the tier of each leaf. It minimises the estimated number of bits plus λD per demoted D unit (a D cell in a leaf below tier D) and λC per demoted C unit (a C cell in a leaf below tier C). A unit is one cell (share by cells), the cell's energy relative to the mean energy of all D (or C) cells of the plane (share by energy), or the mean of both (cells + energy). A leaf is tier A only if all its cells are A.
  * The penalties are found by bisection on a logarithmic scale between 10^−3 and 10^4, in 7 steps: first λC with λD = 0, until the C share target is met, then λD. For modes with a C share of 100 % (keep, keep20), λC is fixed at 10^9. With the share measure "cells + energy", the smaller of the two shares must meet the target.
* **Modes.** There are six modes per scheme (`src/partition_format.h`):

  | Index | Name | D share | C share | Note |
  |---|---|---|---|---|
  | 0 | max | 10 % | 30 % | |
  | 1 | 1 | 30 % | 50 % | |
  | 2 | 3 | 50 % | 70 % | |
  | 3 | 5 | 70 % | 90 % | |
  | 4 | keep | 90 % | 100 % | |
  | 5 | keep20 | 90 % | 100 % | Depth layer with rule 3 (opt-in, larger file) |

  Scheme 1 uses rule 1 for both layers. Scheme 2 uses rule 2 for the frequency layer and rule 1 for the depth layer.
* **Quantiser.** The encoder uses scalar quantisation with a dead zone. A coefficient `x` of a leaf with step `2^s` is quantised to `q = sign(x) * floor(|x| / step + 0.5 - 0.30)`. The dead zone 0.30 is used by the encoder only and is not signalled. The decoder reconstructs `(|q| + 0.20) * step` with the sign of `q` (Section 8.3); the reconstruction offset 0.20 is a format constant.
* **Leakage compensation.** The encoder obtains the decoded frequency layer, brought down to the base rate with the decimators of Section 10 (`LEAKOUT` hook of `src/lae_dec.cpp`), and uses it to compensate the depth target.
  * After a first complete encode, the encoder decodes the written file and obtains the frequency layer alone at the base rate.
  * It filters this leak with a zero-phase windowed-sinc lowpass (2047 taps, Kaiser window β = 9) with cut-off `fc - 1150` Hz, 150 Hz below the start of the frequency layer, and subtracts the result from `B24 - v` before the depth target is formed. The cut-off `fc` of the first pass is kept.
  * Only the depth layer is then re-encoded, and the file is written again. The frequency layer of the first pass is kept.
  * Leakage compensation is always on in version 5.
* **Clipping.** The only output protection is the decoder's final hard limiter (Section 11.7). The reference encoder runs one diagnostic decode with the `CLIPMAP` hook and reports how many samples the limiter touches. This report is not stored in the file.
* **Signal analysis.** The encoder's analysis uses the FFT module `src/fft.h`. The inverse MDCT of Section 9.3 is implemented separately, in `src/mdct_tb.h`.

---

## 15. Intellectual property and licensing (informative)

### 15.1 Licences and contributions

| Item | Licence |
|---|---|
| This specification | CC BY 4.0 |
| Reference decoder | Apache-2.0 |
| Reference encoder | GPL-3.0-or-later, or a commercial licence from the author |
| Test vectors | CC0-1.0. Generated synthetic signals only; no third-party recordings |

Contributions are accepted under the Apache-2.0 licence (inbound licence). Every commit must carry a Developer Certificate of Origin sign-off (`Signed-off-by`). No Contributor Licence Agreement is required.

Contributors keep their copyright and are credited in the git history and in the `AUTHORS` file. The author may include contributions in the GPL-licensed reference encoder and in its commercially licensed version.

Commercial licensing and private matters are handled through GitHub: issues, and private vulnerability reporting.

### 15.2 Patents

The author makes the following pledge:

> The author has not filed and will not file patent applications on the LAE format, and will not assert any patent claim against any implementation of the LAE specification.

### 15.3 Published techniques used

LAE Lossy is built from the following published techniques:

* the textbook MDCT with sine window, computed through a DCT-IV via an M-point complex FFT. The pre- and post-twiddle form `exp(-i * 2*pi * (n + 1/8) / (2*N))` was published in FFmpeg `libavcodec/mdct.c` on 2002-10-28;
* dead-zone scalar quantisation (encoder side);
* binary adaptive range coding;
* half-band FIR interpolation and decimation;
* TPDF dither;
* integer M/S lifting (Section 11.4).

---

## Appendix A. Constants and limits

| Constant | Value | Section |
|---|---|---|
| Chunk identifier | `LAE ` (`0x4C 0x41 0x45 0x20`) | 3.3 |
| Magic | `0x3145414C` (`LAE1`) | 4 |
| Container version | 5 | 4 |
| Base rates | 44 100, 48 000 Hz | 4 |
| `stages` | 1 … 3 | 4 |
| `P` | 1 … 65536, multiple of `2^stages` | 4 |
| `Lm` | 1 … 2 000 000 000 − 2P | 4 |
| Dither block | 4096 base-rate samples | 4.2 |
| Dither seeds | `0x12345678`, `0xB06BD031` | 11.2 |
| Dither range | `u1 + u2 - 4096`, in [−4096, 4094] FU | 11.2 |
| `nseg` | 1 … 8 | 5 |
| Stream header | 40 bytes | 5.3 |
| `N`, `M` | 512, 256 | 9 |
| Cell width `CW` | 8 bins | 5.5 |
| Root block | 16 cells × 8 frames | 5.5 |
| Probability | 11-bit, initial 1024, shift 5 | 6.3 |
| Normalisation threshold | `2^24` | 6 |
| Initial decoder bytes | 5 | 6.2 |
| Encoder flush bytes | 4 | 6.7 |
| Length tree | 5 bits (lengths 0 … 31) | 6.6 |
| Initial `prevTier`, `prevE` | 0, 12 | 7.1 |
| Exponent range | 0 … 34 | 7.5 |
| Coefficient contexts | 3 tiers × 13 (`min(12, floor(E/2))`) | 7.5 |
| `TB` | {0, 2, 4, 8} | 8.1 |
| `TB16` | {0, 0, 4, 8} | 8.2 |
| `STEPMAX` | 8 | 8.2 |
| Reconstruction offset | 0.20 | 8.3 |
| `ZC` | {0, 1/32, 1/16, 3/32, 1/8, 3/16, 1/4, 3/8} | 8.4 |
| Value bound (coefficients, IMDCT output) | ±1 073 741 823 | 8.3, 9.4 |
| `SCALE` | `2^-4` | 9.2 |
| `BITREV` | 256 entries, 8-bit reversal | 9.2 |
| Half-band lengths (half / taps / stored) | 79/159/40, 15/31/8, 11/23/6, 9/19/5 | 10.1 |
| Half-band centre tap | `2^30` (Q31) | 10.1 |
| Output range | −8 388 608 … 8 388 607 | 11.7 |
| Self-test reference | `0x729b547fbdbfe203` | 13 |
| Self-test hash initial value / prime | 1469598103934665603 (`0x14650FB0739D0383`) / 1099511628211 | 13 |
| CRC-32 | reflected `0xEDB88320`, init and final XOR `0xFFFFFFFF` | 3.3 |
| Number of modes | 6 | 4, 14 |

## Appendix B. Test vectors

The conformance vectors and their hash values are listed in [tests/vectors/MANIFEST.md](../../tests/vectors/MANIFEST.md). That file is the single list of the values referred to in Section 1.3.

* The vectors are 2-second excerpts, starting at 5 s, of synthetic signals generated by `src/gen_suite.cpp`.
* Each vector name gives the material, the master rate, the mode, the scheme (`s1` or `s2`) and, where present, the `zs` option.
* For each vector, the manifest lists the first 16 hexadecimal digits of the SHA-256 of the encoded file and of the decoded output for each route: 24-bit at the master rate, 16-bit at the master rate (written as a 24-bit file, Section 11.8), cd24 at the base rate, and 24-bit at half the master rate.
* One octave down is permitted only when `stages >= 2`. The half-rate route is therefore not available for the 88.2 kHz and 96 kHz vectors.

## Appendix C. Open items

1. Publish the full SHA-256 values of all test vectors (Section 1.3).
2. Reproduce `src/mdct_tables.h` as a normative annex (Section 9.2).
3. Confirm whether a decoder must reject a `fmt ` sample rate that differs from `base` (Section 3.1).
4. Document the values that the reference encoder writes into the stream-header fields `rate` and `L` (Section 5.3).
5. State a guaranteed bound below which no `int64` overflow occurs, and specify the treatment of streams that exceed it (Section 10.4).
6. Confirm whether `CORRUPT` must be detected in the whole stream or only in the parts a route needs (Section 7.6).
7. Specify the layout of `.lae` files and the planned carriage in FLAC (Section 1.1). Neither is specified for container version 5.
8. Document how the reference encoder computes the per-cell exponents from which the leaf exponent `E` is derived (Section 8.2).
9. Confirm whether reserved `rule` values must be decoded with the default rule or rejected (Section 8.2).
