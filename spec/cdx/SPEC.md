# LAE CD-Extra Encapsulation Specification (CDX)

| | |
|---|---|
| Status | draft, version 0 |
| Date | 2026-10-24 |
| Licence | CC BY 4.0 (text) |
| Author | mtq0605 |

**Scope.** This document specifies how the extension layer of LAE Lossy (LAEY), container version 5, is encapsulated in, and read from, three kinds of carrier: a CD-Extra disc, ordinary files (including files on DVD and BD data discs), and LAE streams. It defines the following:

- the CD-Extra layout;
- the byte format of the extension file (.LAX) and its trailer record;
- the pairing of base-layer files with extension files;
- the re-encapsulation of a verified CD layer and its extension block as a v5 WAV file;
- the requirements on encapsulation tools and on readers;
- the conformance test cases.

The v5 bitstream and its decoding are specified in [SPEC] and are not changed by this document. The encapsulation tool `lae_cdx` (pack, read, repack, image, self-check) and its reader core implement this draft. The draft will be revised after the measurements on real burns and rips listed in Appendix A have been completed.

**References** (cited in the text in square brackets)

- [SPEC] spec/lossy/SPEC.md (LAE Lossy specification, container v5)
- [AH] src/align_hash.h; [ENC] src/lae_enc.cpp; [WV] src/wavio.h; [DEC] src/lae_dec.cpp

**Marking of numbers**

- Numbers without a marker are defined by this specification or taken from the cited references and standards.
- "To be measured": the value can only be determined by an actual burn or rip; see Appendix A.
- "Derived": the value is computed from known constants.
- "Estimate": an initial value chosen without measurement. It is to be measured; see Appendix A.
- "Computed for this draft": the value was computed while drafting this specification. It is checked against `cdMd5` in [AH] and against the S1/S2 implementation of the reference tool: `lae_cdx selftest` verifies every example against these values (known-answer tests; all pass, 2026-10-24).
- "Primary source" / "Secondary source": the value is taken from published documentation. A primary source is the defining document or the documentation of the implementation concerned; a secondary source is a third-party report. Unless stated otherwise, neither has yet been confirmed by measurement for this specification.

---

## 1. Scope and Terminology

### 1.1 Scope

This specification defines:

- the layout of the CD-Extra (Section 2);
- the byte format of the extension file .LAX (Section 3);
- the forms other than CD, and how they are re-encapsulated as v5 WAV (Section 4). These forms are ordinary files (including files on DVD and BD data discs) and LAE streams;
- the results that encapsulation tools and readers MUST achieve (Sections 5 and 6);
- the conformance test cases (Section 7).

The 44.1 kHz restriction applies only to CD (CD-Extra, Red Book). DVD, BD and other data discs do not use the layout of Section 2. They store ordinary files as specified in Section 4, and may use the 44.1 kHz family or the 48 kHz family.

This specification does not define:

- the v5 bitstream and decoding (see [SPEC]);
- the extension block of LAES;
- "keep CD / keep master" and per-layer provenance (see Section 8);
- concrete implementations of placement, search and other algorithms;
- the transport of LAE streams (protocol, segmentation, buffering);
- the user interface.

### 1.2 Relation to Container v5

- This specification requires no change to container v5 or to the decoder ([DEC]).
- The extension block in an extension file is the body of the `LAE ` chunk of a v5 WAV file, byte for byte ([SPEC] 3.3).
- Everything specified here lies outside v5: the CD layout, the trailer record, file pairing and the delivery of LAE streams. This specification has its own version number (Section 8).
- `.lae` remains reserved for the complete file layout of [SPEC] 1.1. `.LAX` is a different kind of file.
- [SPEC] Appendix C item 7 asks how FLAC carries LAE, and 4.2 of this specification answers it. FLAC carries only the base layer, as a base-layer WAV file does; the extension layer is placed in an accompanying .lax file, which has the same format as the extension file on CD.

### 1.3 Key Words and Clause Identifiers

- The key words "MUST", "MUST NOT", "SHOULD", "SHOULD NOT" and "MAY" in this document are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals.
- Every normative clause carries an identifier in square brackets, for example [L1]. The test cases of Section 7 cover the clauses one by one.
- Paragraphs marked "Informative" are explanations only and impose no requirements.

### 1.4 Terms

| Term | Definition |
|---|---|
| sample | One stereo frame: one int16 for each of L and R |
| sector | 588 samples (2352 bytes), 1/75 s |
| digital silence | Samples in which L and R are both exactly 0. Passages containing only dither noise are not silence; they are content |
| CD layer | The 16-bit PCM of a track, i.e. the content of the data chunk of the track's v5 file. Its length is ncd samples |
| extension layer | The v5 `LAE ` chunk: depth layer plus frequency layer, coding "master − CD" |
| extension block | The entire body of the `LAE ` chunk, n bytes, including the CRC-32 at the end of the body. Note: the payload of [SPEC] 3.3 is only bytes 0 … n−5, without the CRC, and is therefore not the same as the extension block |
| extension file | Extension block plus trailer record. The file name extension is .LAX on CD and .lax in file distribution and in LAE streams (Section 3) |
| trailer record | The fixed record occupying the last 80 bytes of the extension file (3.2) |
| whole-track MD5, S1, S2 | Defined in 3.3 and 3.4 |
| lead0, tail0 | The number of consecutive digital-silence samples at the start and at the end of the CD layer, stored in the trailer record (3.2) |
| information range | The part of the CD layer from the first to the last non-zero sample, i.e. samples lead0 … ncd − tail0 − 1 of the CD layer |
| session 1 / session 2 | The audio session / data session of the CD-Extra. A CD player reads only session 1 |
| LBA | Logical block address of the CD (MMC), common to both sessions. LBA 0 = absolute time 00:02:00. [L4] requires INDEX 01 of track 1 to be at LBA 0 |
| absolute position | LBA × 588 + index of the sample within the sector, in samples |
| TOC | Table of contents of the CD |
| p_k | Absolute position of INDEX 01 of track k |
| A2 | LBA (sector) of the start of the lead-out of session 1. The corresponding absolute position is A2 × 588 |
| nominal position q_k | Absolute position of the first sample of the CD layer of track k; q_k = p_k ([L6]) |
| padding z_k | Digital silence after the CD layer of track k that pads the track to a whole sector, 0 ≤ z_k < 588 ([L7]) |
| start guard | Digital silence before the CD layer of track 1: at least 5880 samples = 10 sectors = 0.133 s. It is provided by the pregap of track 1 ([L5]) |
| offset o | The first sample of the CD layer of track k on the disc appears at position q_k + o of the disc stream. o > 0 means later in the disc stream. o combines the read offset and the write/pressing offset and may differ between tracks. The reader does not assume a uniform disc-wide offset |
| disc stream | The sequence of samples that the reader assembles from its input (6.2). With a TOC, it is addressed by absolute position. Without a TOC, it consists of the samples of a single file, with positions counted from the first sample of the file |
| coarse location / fine location | Coarse location: an approximate start of a track derived from the TOC or from the information range. Fine location: every position near the coarse location is screened with S1 and S2, and the result is confirmed by the whole-track MD5 ([R7]) |
| W | Search radius of fine location, W ≥ 5880 samples (10 sectors) |
| slip | Loss or insertion of samples within a track, for example by a read error |
| derived mode | The CD layer is derived from the master by the encoder. This is the current default |
| independent-CD mode | The publisher's existing CD is used as the CD layer (encoder option `cd=`, [ENC]) |
| CD route (fallback) | When a track fails verification, only the ripped CD audio is output; the extension layer is not used |
| LAE stream | Per-track delivery of the CD layer, unchanged sample by sample, together with the extension file (4.5) |
| encapsulation tool | A program that produces CD images and distribution files |
| reader | A program that reads rips, files or LAE streams and outputs high resolution, including player plug-ins |

### 1.5 General Conventions

- All multi-byte integers are little-endian, as in [SPEC] 2.1.
- M = 2^61 − 1.
- CRC-32 is the same as in [SPEC] 3.3:
  - reflected polynomial 0xEDB88320;
  - initial value 0xFFFFFFFF and final XOR 0xFFFFFFFF;
  - check value: CRC-32("123456789") = 0xCBF43926;
  - reference implementation: `crc32` in src/part_stream.h.
- Positions and lengths are given in samples (stereo frames).

---

## 2. CD Layout

### 2.1 General

**[L1]** The CD MUST be a two-session CD-Extra. Session 1 is audio (2.2); session 2 is one data track (2.3).

- The CD is used only for the 44.1 kHz family. The sample rate of every CD layer MUST be 44 100 Hz.
- This restriction applies to CD only. DVD and BD data discs store ordinary files as specified in Section 4, and both families are permitted there.

**[L2]** If n is the number of audio tracks, n + 1 ≤ 99 MUST hold. The data track also occupies a track number, so there are at most 98 audio tracks. A CD provides at most 99 track numbers; the figure 98 is derived.

### 2.2 Session 1 (Audio)

```
absolute position  −150×588                 0 = p_1 = q_1               p_2 = q_2            p_n = q_n                 A2×588
                   | pregap 150 sectors, silence | CD layer 1 | padding z_1 | CD layer 2 … | CD layer n | padding z_n | lead-out
                   |<---- track 1 INDEX 00 ----->|<------ TOC track 1 ------->|<- track 2 … |<------ TOC track n ------>|
```

With only one track (n = 1), TOC track 1 = CD layer + padding z_1.

**[L3]** Session 1 MUST be written in one pass (DAO/SAO). Apart from the content specified in this section, session 1 MUST NOT contain any other samples or gaps.

**[L4]** Pregap and INDEX 01 of track 1:

- INDEX 01 of track 1 MUST be at LBA 0.
- The pregap (INDEX 00) of track 1 is 150 sectors = 88,200 samples (derived), and its content MUST be digital silence.
- Hidden tracks are therefore not supported.
- In independent-CD mode, if INDEX 00 of the publisher's track 1 contains non-silent content, the encapsulation tool stops ([P10]).

**[L5]** Start guard: at least 5880 samples of digital silence MUST precede the CD layer of track 1. The pregap of track 1 ([L4]) satisfies this requirement; therefore zeros MUST NOT be added within track 1.

**[L6]** The CD layer of every track begins at INDEX 01: q_k = p_k (k = 1 … n), and q_1 = 0.

Exception (independent-CD mode): the master's sound may begin less than one CD sample earlier than the first sample of the publisher's track 1. In that case one frame of digital silence is placed in front of the CD layer of track 1, and q_1 = −1. The digital silence at the start of every encapsulation absorbs this difference:

- the frame lies within the digital silence of the pregap of track 1, and [L4] and [L5] continue to hold;
- session 1 remains sample-identical to the publisher's programme ([P11]);
- a reader that searches by whole-track MD5 within W of the TOC position finds the CD layer at −1. If the pregap was not ripped, these positions are unknown and are read as 0.

**[L7]** Track-end padding: the CD layer of every track MUST lie entirely within that track in the TOC.

- Let z_k = p_{k+1} − q_k − ncd_k (k = 1 … n), where p_{n+1} = A2 × 588. The condition 0 ≤ z_k < 588 MUST hold.
- These z_k padding samples MUST be digital silence. There are at most 587 of them, i.e. 13.3 ms (derived).

**[L8]** End: between the CD layer of the last track and the lead-out there is only the padding z_n of the last track ([L7]). A guard track MUST NOT be added, and no other silence MUST be added either. The end of the CD layer of the last track is given by its length ncd_n ([R6]).

**[L9]** Neither the pregap of track 1 nor the padding belongs to the CD layer of any track. They do not enter the v5 file or the extension file, and they do not change the decoded length.

**[L10]** Every TOC audio track MUST consist of a whole number of sectors and MUST NOT be shorter than 300 sectors (4 s). The reader does not require ncd to be a multiple of 588.

**[L11]** The pre-emphasis flag (FLAGS PRE) MUST NOT be set. If the publisher's CD uses pre-emphasis, the encapsulation tool stops ([P10]).

**[L12]** The PCM of session 1 contains only CD layers, the digital silence of the pregap of track 1, and padding. Data or markers of any kind MUST NOT be embedded in the PCM.

**[L13]** Derived mode:

- From track 2 onward no INDEX 00 is set; the pregap is 0.
- For a continuous master ([P5]), z_k = 0 for every track except the last, and the CD layers follow one another without gaps.
- For the last track, and for every track of a per-track master ([P6]), z_k is determined by [L7].

**[L14]** Independent-CD mode:

- The INDEX marks, ISRC and FLAGS of the publisher's CD MUST be preserved at the same positions as on the publisher's CD. The INDEX marks include INDEX 00 from track 2 onward and INDEX 02 and above of every track. The pregap of track 1 is always handled according to [L4].
- The CD layer of track k consists of all samples from INDEX 01 of the publisher's track k to INDEX 01 of track k+1, including INDEX 00 of track k+1. Hence z_k = 0.
- The CD layer of the last track extends from INDEX 01 of the last track to the end of the publisher's programme. Every audio track of the publisher consists of a whole number of sectors. Hence z_n = 0.

*Informative.*

- With a TOC, reading uses only the positions of INDEX 01. INDEX 00, INDEX 02 and above, and ISRC do not affect reading.
- The guard does not alter the audio of any track:
  - the start guard is the pregap of track 1 that every CD has, and no silence is added at the end;
  - each track of session 1 is its CD layer plus padding of less than one sector;
  - files ripped per track contain no added zeros.
- Session 1 exceeds the total length of the CD layers only by the padding, which is less than one sector per track.

### 2.3 Session 2 (Data)

**[D1]** Session 2 MUST contain exactly one data track, in CD-ROM XA Mode 2 Form 1 format with 2048 bytes of user data per sector. Mode 1 sectors MUST NOT occur anywhere on the CD. The Joliet specification (1995-05-22) permits no Mode 1 sectors on a multisession disc (primary source).

**[D2]** File system:

- The file system MUST be ISO 9660 Level 1 + Joliet.
- Volume descriptors are counted from the LBA of INDEX 01 of the data track:
  - +16: primary volume descriptor;
  - +17: Joliet supplementary volume descriptor;
  - +18: terminator.
- All addresses are absolute LBAs.
- Rock Ridge, UDF and El Torito are not used.

**[D3]** The LBA of INDEX 01 of the data track of session 2 (sector 0 of the ISO 9660 volume) MUST equal the next writable address reported by the drive at write time.

- For an image written in one pass, this LBA is precomputed as A2 + 11,400 sectors. The figure 11,400 = 6,750 + 4,500 + 150: the lead-out of session 1, the lead-in of session 2, and the 150-sector pregap of the data track (primary sources; to be measured, Appendix A.1 item 1).
- For two-step writing, it is generated from the address actually reported ([P15]).

**[D4]** Directories:

- `/README.TXT` MUST be present.
- The directory `/LAE/` contains only files `TRACKnn.LAX`: one for each audio track that has an extension. nn is the two-digit track number of that track in the TOC (01–98).
- The data session MUST NOT contain CUE files, copies of the TOC, reports, programs or autorun.inf.
- Other files (for example a booklet or a cover) are at the publisher's discretion. The reader ignores them.

**[D5]** README.TXT:

- SHOULD be an explanation in Chinese and English, encoded in UTF-8. It states what the disc is, that session 1 plays on any CD player, and how high resolution is obtained.
- MUST NOT contain hashes, positions, offsets or tables of lengths.

**[D6]** End of the data track:

- The end of the data track SHOULD be padded with zero sectors, initially 150 (estimate, to be measured; Appendix A.1 item 2). The zero sectors belong to no file.
- The data track, including the trailing zero sectors, MUST NOT be shorter than 300 sectors. If it would be shorter, the trailing zero padding is increased. A track appended in track-at-once mode must be at least 300 sectors long (secondary source).

**[D7]** An audio track MAY have no extension file. The publisher may choose this when capacity is insufficient (see [P18]). Such tracks are played as CD only ([R19]).

*Informative.*

- The volume label does not affect reading. It is recommended to derive the label from the album name, and to use LAE_EXT if this conversion fails.
- Windows Explorer shows the last session, so the extension files can be read as ordinary files (secondary source, to be measured; Appendix A.1 item 10).

### 2.4 Subcode and Metadata

**[M1]** ISRC, MCN (UPC/EAN) and CD-Text MAY be written. They contain only standard metadata; they MUST NOT contain hashes or position data.

### 2.5 Capacity (Informative)

**In sectors:** N1 + 11,400 + N2 ≤ C, where:

- N1 = A2: the number of sectors of session 1 from LBA 0, padding included.
- N2 = file system sectors + Σ ceil(file size in bytes / 2048) + trailing zero sectors. With few files, the file system occupies only a few tens of sectors (estimate).
- C = ATIP capacity of the blank disc minus a safety margin. For an 80-minute disc this is about 359,849 sectors (secondary source; to be measured, Appendix A.1 item 9).

**In disc minutes** (1 disc minute = 4500 sectors; every term in disc minutes): L·(1 + r) + padding + 2.533 + file system + trailing zero sectors ≤ C / 4500, where:

- L is the length of the music in minutes.
- The padding is less than one sector per track, i.e. less than 0.00023 disc minutes. The constant 2.533 = 11,400 / 4500 (both derived).
- r = (size of the extension layer in MB per minute of music) ÷ 9.216. The constant 9.216 = 2048 × 4500 / 10^6 (derived; 9.22 when rounded).

---

## 3. Extension File (.LAX)

### 3.1 Byte Layout

**[X1]** An extension file MUST consist of exactly the following two parts:

| Bytes | Content |
|---|---|
| 0 … n−1 | Extension block: the entire n-byte body of the v5 `LAE ` chunk, including the CRC-32 at the end of the body, byte-identical to that in the track's v5 WAV. It excludes the chunk identifier, the chunk length and any RIFF pad byte |
| n … n+79 | Trailer record (3.2) |

- There is no file header.
- The extension block length is n = file length − 80. It is not recorded separately, and n ≥ 8 ([SPEC] 3.3).
- The file name extension is .LAX on CD (upper case, as required by ISO 9660) and .lax outside CD. Readers treat the extension case-insensitively.

### 3.2 Trailer Record

**[X2]** The trailer record is 80 bytes long and contains the following fields:

| Offset | Bytes | Type | Field | Specification |
|---|---|---|---|---|
| 0 | 4 | bytes | identifier | ASCII `LTVT` (4C 54 56 54) |
| 4 | 2 | uint16 | version | 1 |
| 6 | 2 | uint16 | length | 80, the size of the whole trailer record in bytes |
| 8 | 2 | uint16 | extension block type | 1 = body of the `LAE ` chunk of LAEY container v5; 2 = reserved for LAES; other values reserved |
| 10 | 2 | uint16 | track number | On CD: the TOC audio track number, 1–98. Outside CD: if the track is also released on CD, its track number in the CD TOC; otherwise its track number within the album (≥ 1); 1 for a single |
| 12 | 4 | uint32 | sample rate | Sample rate of the CD layer. On CD MUST be 44100; outside CD, 44100 or 48000. For extension block type 1, MUST equal base in the extension block |
| 16 | 4 | uint32 | ncd | Number of samples of the CD layer, 1 ≤ ncd < 2^28. For extension block type 1, MUST equal nb − 2·pb of the extension block |
| 20 | 16 | bytes | MD5 | Whole-track MD5 (3.3), stored in the byte order of the MD5 output |
| 36 | 8 | uint64 | S1 | See 3.4; 0 ≤ S1 < M |
| 44 | 8 | uint64 | S2 | See 3.4; 0 ≤ S2 < M |
| 52 | 4 | uint32 | lead0 | Number of consecutive digital-silence samples at the start of the CD layer, 0 ≤ lead0 ≤ ncd |
| 56 | 4 | uint32 | tail0 | Number of consecutive digital-silence samples at the end of the CD layer, 0 ≤ tail0 ≤ ncd. If the CD layer is entirely digital silence, lead0 = tail0 = ncd; otherwise lead0 + tail0 < ncd |
| 60 | 16 | bytes | reserved | MUST be all 0 when written; ignored when read |
| 76 | 4 | uint32 | CRC-32 | CRC-32 (1.5) of bytes 0–75 |

*Informative.*

- The limit ncd < 2^28 corresponds to about 101 minutes at 44.1 kHz and about 93 minutes at 48 kHz (derived). It ensures that all computations of 3.4 fit into 64-bit unsigned integers.
- lead0 and tail0 describe the content of the CD layer itself: how much digital silence it contains at its start and at its end.
  - They are independent of burning, ripping and offsets, and they are not positions ([X6]).
  - The reader uses them only for coarse location ([R7]).
  - Wrong values can only make coarse location miss. They cannot cause wrong audio to pass, because acceptance depends solely on the whole-track MD5.
- These silent samples are part of the track: they are always played and included in the whole-track MD5. No program (decoder, player, packaging or reading tool) may remove or skip them, or use or pass lead0 and tail0 as an amount that may be removed or as "invalid" data.

### 3.3 Whole-Track MD5

The whole-track MD5 is a simple means of checking whether the CD audio has been damaged or altered. After a track has been aligned, the reader uses it to confirm that the audio of the track is identical to the audio at production time ([R7]).

**[X3]** The whole-track MD5 is the MD5 (RFC 1321) computed over ncd × 4 bytes. For i = 0 … ncd−1, the bytes of sample i are taken in this order:

1. low byte of L;
2. high byte of L;
3. low byte of R;
4. high byte of R.

This is interleaved little-endian int16. The definition is the same as that of the audio MD5 in FLAC STREAMINFO for 16-bit stereo audio (RFC 9639). The reference implementation is `cdMd5` in [AH].

### 3.4 S1 and S2

**[X4]** Frame words are defined first, then S1 and S2:

- **Frame word:** w_i = b0 + 2^8·b1 + 2^16·b2 + 2^24·b3, where b0 … b3 are the four bytes of sample i in the byte stream of 3.3.
  - Equivalently, w_i = (L_i mod 2^16) + 2^16·(R_i mod 2^16), and 0 ≤ w_i < 2^32.
  - Frame words MUST be assembled byte by byte, independently of the platform's byte order.
- **S1** = (Σ_{i=0}^{ncd−1} w_i) mod M
- **S2** = (Σ_{i=0}^{ncd−1} (i+1)·w_i) mod M

**Sliding update.** Let x_j be the frame words of the disc stream, t the start of the window, and n = ncd the window length. Then:

- A(t) = Σ_{i=0}^{n−1} x_{t+i} mod M
- B(t) = Σ_{i=0}^{n−1} (i+1)·x_{t+i} mod M

When the window moves right by one sample:

- B(t+1) = (B(t) − A(t) + n·x_{t+n}) mod M
- A(t+1) = (A(t) − x_t + x_{t+n}) mod M

B is updated first, using the old value A(t); A is updated afterwards.

**[X5]** The values A and B obtained by the sliding update MUST equal the values computed directly from the definition.

*Informative: absence of overflow.*

- A, B < 2^61, and n·x < 2^28 · 2^32 = 2^60.
- Therefore B + (M − A) + n·x < 2^63, and 64-bit unsigned integers suffice.
- The reduction modulo M can be computed as (v AND M) + (v >> 61), followed by at most one subtraction of M (derived).

**Known answers (computed for this draft)**

Example 1:

- Input: 3 samples (1, −1), (−32768, 32767), (0, 0).
- Byte stream: 01 00 FF FF 00 80 FF 7F 00 00 00 00.
- Frame words: w = 4294901761, 2147450880, 0.
- S1 = 6442352641; S2 = 8589803521; MD5 = 08b7bb4b0c7432f1c9e94e3c493f236e.
- lead0 = 0; tail0 = 1 (the last sample is (0, 0)).
- Sliding check:
  - The 3 samples are placed in the disc stream x = 0, w0, w1, w2, 0, with n = 3.
  - Initial window: A(0) = 6442352641, B(0) = 15032156162.
  - After one step to the right: A(1) = 6442352641 = S1, B(1) = 8589803521 = S2.

Example 2 (the frame words moving out of and into the window are both non-zero, so every term of the sliding formula is exercised):

- Input: 3 samples (1, −1), (−32768, 32767), (32767, −32768).
- Byte stream: 01 00 FF FF 00 80 FF 7F FF 7F 00 80.
- Frame words: w = 4294901761, 2147450880, 2147516415.
- S1 = 8589869056; S2 = 15032352766; MD5 = e5e4df0d69a7afa36b20e70c5c09fc82.
- lead0 = tail0 = 0.
- Sliding check:
  - Disc stream x = 131071 (i.e. sample (−1, 1)), w0, w1, w2, with n = 3.
  - Initial window: A(0) = 6442483712, B(0) = 15032287233.
  - After one step to the right: A(1) = 8589869056 = S1, B(1) = 15032352766 = S2.

Example 3 (all zero): 3 samples (0, 0) give lead0 = tail0 = 3 and S1 = S2 = 0 (derived).

### 3.5 Content Excluded from the Extension File

**[X6]** The trailer record and the extension file MUST NOT contain the following:

- Values derived from the extension layer:
  - any value computed from the bytes of the extension layer (CRC, hash, size);
  - any value covering both the CD layer and the extension layer;
  - any value computed from the two layers together.

  The extension layer is protected only by its own CRC-32 ([SPEC] 3.3).
- Any position, LBA, offset, delay or gain. lead0 and tail0 are counts of the content of the CD layer and are not covered by this item.
- Any per-sector or per-frame data; entry tables, seek tables or cue-point tables.
- Any header. Within the extension file, the whole-track MD5 is stored only in the trailer record.

### 3.6 Order of Checks when Reading

**[X7]** The reader MUST check the extension file in the order below. If any check fails, the file is not used, and the reason given in parentheses is reported:

1. The file length is ≥ 88, and the last 80 bytes begin with `LTVT` ("no trailer record"; see [R14]).
2. The CRC-32 matches ("record corrupted").
3. The version is 1, and the length is 80 ("unknown record version").
4. The extension block type is 1 ("unknown extension block type"; if the type is 2, "LAES extension block, not supported by this reader").
5. Track number, sample rate, ncd, S1, S2, lead0 and tail0 are within the ranges specified in 3.2 ("invalid record field").

The extension block itself is checked by the decoder: CRC and V1–V13 ([SPEC] 4.3). The reader MAY check the CRC of the extension block in advance and, if it does not match, report "extension layer corrupted".

*Informative.* The production side guarantees ([P2]) that:

- ncd equals nb − 2·pb of the extension block;
- lead0 and tail0 match the CD layer.

After reading, the decoder's check V13 covers the decoding, so the reader need not check these again.

---

## 4. Files and Streams Outside CD

This section applies to every form other than CD: ordinary file downloads, files on DVD or BD data discs, and LAE streams. These forms do not use the layout of Section 2. The sample rate of the base layer may be 44 100 Hz or 48 000 Hz ([P1]).

### 4.1 Single-File v5 WAV (Current Form)

**[F1]** In a single v5 WAV file, the CD layer is in the data chunk, and the `LAE ` chunk follows the audio ([SPEC] 3.1).

- A v5 WAV written by the encoder or by a re-encapsulation tool MUST have the canonical layout below.
- This layout is the same as the output of wavio::writeCD ([WV]).
- Let D = ncd × 4.

| Offset | Content |
|---|---|
| 0 | `RIFF` |
| 4 | uint32: 36 + D + 8 + n + (n mod 2) |
| 8 | `WAVE` |
| 12 | `fmt `; uint32 16; uint16 1; uint16 2; uint32 sample rate; uint32 4 × sample rate; uint16 4; uint16 16 |
| 36 | `data`; uint32 D; followed by D bytes of PCM |
| 44 + D | `LAE `; uint32 n; followed by the n-byte extension block; if n is odd, one further 0 byte |

Version 0 does not define a trailer record appended inside the WAV file.

### 4.2 Base-Layer File Plus Extension File

**[F2]** A track MAY also be released as two files:

- **Base-layer file:** FLAC (RFC 9639) or WAV.
  - Its content is exactly the CD layer of the track: ncd samples, 16-bit, 2 channels, at the sample rate given in the trailer record.
  - A base-layer WAV contains no `LAE ` chunk.
  - Ordinary players play only this file.
- **Extension file:** it has the same name as the base-layer file with the file name extension .lax, and its format is that of Section 3. If the same track is also released on CD, this file MUST be byte-identical to TRACKnn.LAX on the CD.
- The extension layer and the trailer record MUST NOT be placed inside the FLAC file. No FLAC metadata block carries them.
- The audio MD5 SHOULD be written into the FLAC STREAMINFO.

### 4.3 Pairing and Ripped Files

**[F3]** The reader first looks for a .lax file of the same name (case-insensitive), and MAY then try other .lax files in the same folder.

**Condition for pairing:** the number of samples decoded from the base-layer file equals ncd, and the whole-track MD5 of the decoded PCM equals the MD5 in the trailer record. Detailed rules:

- If the STREAMINFO MD5 is not 0, it MAY be used first to select candidate files.
- Acceptance, however, MUST be based on the decoded PCM. Either the reader computes the MD5 itself, or the FLAC decoder verifies the STREAMINFO MD5 and that MD5 equals the trailer-record MD5.
- If the STREAMINFO MD5 is 0, the reader MUST compute the MD5 itself.
- If several .lax files pair successfully but their extension blocks differ, the reader MUST reject the track and report this.
- If pairing fails, [F4] applies.

When pairing succeeds, no offset needs to be found, and S1 and S2 are not used.

**[F4]** The pairing of [F3] serves base-layer files distributed by the publisher and LAE streams (4.5). Ripped files (WAV, FLAC and so on, including per-track rips) usually fail to pair under [F3]. The rip carries an offset, the files contain padding, and the file of track 1 may contain the pregap. Therefore:

- the reader first attempts [F3];
- if pairing fails, the reader MUST locate the tracks one by one as specified in Section 6 ([R2], [R7]):
  - if there is a CUE/TOC or an ordered set of per-track files, it uses the TOC;
  - if there is only a single file, it searches the whole file by content;
- if Section 6 also fails, only the base layer is played.

### 4.4 Re-encapsulation

**[F5]** Re-encapsulation (for extension block type 1):

- Input: a verified CD layer, plus the extension block from the extension file. The CD layer comes from one of:
  - a rip or an LAE stream ([R8]);
  - a successfully paired base-layer file ([F3]).
- Output: a v5 WAV in the canonical layout of 4.1. The result MUST be byte-identical to the file originally written by the encoder.

### 4.5 LAE Stream

**[F6]** An LAE stream (LAEY or LAES) is delivered track by track. Each track consists of two items:

- the CD layer of the track, unchanged sample by sample, as FLAC, WAV or raw PCM. Raw PCM is little-endian 16-bit stereo, at the sample rate given in the trailer record;
- the extension file of the track (Section 3), unchanged byte by byte.

The stream carries no additional header, index, position or offset.

The player treats streams and rips in the same way:

1. each track is first paired according to [F3];
2. if pairing fails, the track is located according to Section 6. There is no TOC, so the search runs within the audio delivered for this track;
3. the result is verified with the whole-track MD5.

**[F7]** A stream in which the CD layer has been re-encoded lossily (AAC, MP3 and so on) is not an LAE stream. The extension layer cannot be used, and only the base layer is played.

- If the base layer is in a lossy format, the reader does not attempt location and reports "base layer is lossy".
- The reader MUST NOT apply the extension layer to lossily decoded audio. Audio that has been lossily decoded and then delivered in a lossless form does not match the whole-track MD5, and only the base layer is played in that case too ([R11]).

---

## 5. Production Requirements (Encapsulation Tool)

### 5.1 General

**[P1]** Sample-rate families:

- When a CD is produced, the master MUST belong to the 44.1 kHz family (88.2 / 176.4 / 352.8 kHz), and the CD layer is 44 100 Hz.
- When only forms other than CD are produced, the master MAY also belong to the 48 kHz family (96 / 192 / 384 kHz), and the CD layer is then 48 000 Hz. These forms are files, files on DVD or BD data discs, and LAE streams.
- If these conditions are not met, the tool stops and states the reason.

**[P2]** The v5 file and trailer record of each track:

- The v5 file of every track MUST pass all checks of the decoder ([SPEC] 4.3, V1–V13).
- The trailer record MUST be computed from the CD layer of that file:
  - ncd = nb − 2·pb;
  - sample rate = base;
  - MD5, S1, S2, lead0 and tail0 as specified in Section 3.

**[P3]** Neither encapsulation nor alignment changes the CD layer. For every track, the MD5 of the range [q_k, q_k + ncd_k) of session 1 MUST equal the trailer-record MD5 and the MD5 of the data chunk of the track's v5 file.

**[P4]** Measured offsets, position differences, shift amounts and trim or padding amounts MAY be displayed. They MUST NOT be written into any file, including images, CUE files, extension files, reports and logs. Reports and CUE files are given only to the producer and are not placed on the disc.

### 5.2 Derived Mode

**[P5]** A continuous master is one file, or a set of per-track files that the publisher declares to be gapless and concatenated in order.

- Track boundaries are placed at the positions given by the publisher and are not moved by a single sample. In particular, the tool does not move a start to the nearest point of the sector grid.
- The start of every track MUST lie on the sector grid.
  - The grid unit is G = 588 × 2^s master samples, i.e. 588 CD samples, where s is stages.
  - Starts given in CUE "minutes:seconds:frames" notation always lie on the grid, because one frame is one sector.
- If any start does not lie on the grid, the tool MUST stop and write no file. It lists these starts and asks the publisher to give them on the grid.
- Master samples MUST NOT be deleted or added. Consequently:
  - the ncd of every track except the last is a multiple of 588;
  - z_k = 0;
  - the CD layers of the tracks follow one another without gaps in session 1.

*Informative.* The encoder may use the actual neighbouring master samples as the edges of a track (ctx), so that the high-resolution output is seamless at track boundaries. This is a matter for the encoder and affects neither encapsulation nor reading. ctx is a planned encoder feature and is not yet implemented; the current encoder uses mirror padding at both ends of each track ([ENC]).

**[P6]** For a non-continuous per-track master, every track is encoded independently, and ncd need not be a multiple of 588. In the image, each track is padded with z_k digital zeros to a whole sector, as specified in [L7].

**[P7]** The division into tracks is entirely the publisher's decision:

- The tool MUST NOT add, delete or move any track boundary.
- If a complete long work forms one track, its ncd need not be a multiple of 588. The part of a sector that remains is absorbed by the padding ([L7]).
- Every track MUST satisfy ncd < 2^28.

### 5.3 Independent-CD Mode

**[P8]** Alignment is performed when the v5 file of each piece is produced ([ENC]):

- for a single piece, with the encoder option `cd=`;
- for a whole disc, the planner `lae_enc plan` first cuts the master at the CD's track markers.

The encapsulation tool accepts only files that are already aligned and performs no alignment itself. The following rules apply:

- There MUST NOT be an option that skips the alignment check (`cdalign=none` has been withdrawn).
- Every piece MUST pass the alignment check when it is encoded. The check consists of:
  - a coarse-to-fine search on the waveform only;
  - a three-block check;
  - a drift check;
  - classification of the error into bands.
- The position of every piece MUST be confirmed by the user:
  - Without `cdalign=`, the encoder runs the search twice, once with a whitened and once with a plain cross-correlation. It reports both results and writes nothing.
  - A piece is encoded only with the offset given by `cdalign=`. For a whole disc, the planner prints this offset for each piece.
- A piece that falls into a confirmation band is encoded only after confirmation, either at the console or with `cdaccept`. The encoder states this in its result.
- If the result is "warning" (for example inverted polarity or swapped channels), a warning MUST be displayed, and processing continues.
- If both the CD piece and the corresponding master segment are entirely digital zero, a sample-by-sample comparison MUST be used instead of the statistical decision.

**[P9]** The planner places the master automatically only if every difference between master and CD consists entirely of digital silence. This covers the differences at both ends and those between pieces.

- The master is laid onto the CD's time line in its original order, and the silences are treated as blank passages.
- The master may consist of several files, which are concatenated sample by sample in the given order. Their own split points are not used. If these split points differ from the CD's, the planner reports that the master has been re-split at the CD's positions.

The result of the placement MUST satisfy all of the following:

- Every non-zero sample of the master enters the v5 file of exactly one track, and the master segments of the tracks are in track-number order.
- The master samples that are left out are all digital zero.
- Alignment is length matching:
  - the side with the longer beginning plays first, and the side with the longer end finishes later;
  - the shorter side is padded with digital silence, and no non-silent sample of either side is cut.
- Sub-sample differences:
  - If the master's sound begins less than one CD sample earlier than the CD's (for example with a CD processed by minimum-phase EQ), one frame of digital silence is added in front of the CD layer. This is the silence that is present at the start of every encapsulation in any case. The master then plays from its own first sample.
  - If the master ends less than one CD sample later than the CD, the format itself accommodates the difference; the master length need not be a multiple of 2^stages.
  - Sound of one CD sample or more that only the master has leads to a stop under [P10].
- Samples added to the master for alignment are all digital zero, and they are added only at positions where the CD is also digital zero.

The concrete placement algorithm is a matter of implementation. Automatic placement is not performed if the master or the CD has non-silent content that the other lacks; content consisting only of dither noise also counts as content. This case belongs to the future "keep CD / keep master" task (Section 8). Version 0 stops under [P10] and states this.

**[P10]** In any of the following cases, the tool MUST stop, write no file, state the reason and list the time ranges concerned:

- The alignment check of a piece results in "stop" ([P8]), or a piece falls into a confirmation band without being confirmed. "Stop" covers: no position found, no match, drift or an edit, or the wrong CD.
- Placement according to [P9] is not possible. This includes non-silent content that the master or the CD lacks in the other, for example a passage repeated or missing when master files were joined, and a different order of pieces.
- INDEX 00 of track 1 of the publisher's CD contains non-silent content ([L4]).
- The publisher's CD uses pre-emphasis ([L11]).

**[P11]** The CD layer is the publisher's CD, unchanged by a single sample. All samples of session 1 from LBA 0 to A2 MUST be sample-identical to the programme of the publisher's CD. The programme extends from INDEX 01 of track 1 to the end of the programme.

### 5.4 Image and Deliverables

**[P12]** AUDIO.BIN MUST contain all samples of session 1 at absolute positions [0, A2 × 588), i.e. from LBA 0 to A2.

- The format is little-endian 16-bit stereo, and the file consists of a whole number of sectors.
- The 150-sector pregap of track 1 is not in the file. It is written as digital silence by the burner according to the CUE (Appendix A.1 item 7).

**[P13]** IMAGE.CUE follows the CUE conventions used by ImgBurn (and by CDRWIN and EAC), under which sector 0 of the file is LBA 0 and the 150-sector pregap of track 1 is not contained in the file:

- Write `FILE "AUDIO.BIN" BINARY`. Sector 0 of the file is LBA 0. Track 1 has `INDEX 01 00:00:00`, and no PREGAP is written.
- Derived mode: from track 2 onward, only INDEX 01 is written. Independent-CD mode: write according to [L14].
- The two sessions are separated by `REM SESSION 01` / `REM SESSION 02`. After `REM SESSION 02`, write, in this order:
  - `FILE "DATA.BIN" BINARY`, or `FILE "DATA.ISO" BINARY` when DATA.ISO is used;
  - `TRACK nn MODE2/2352`, or with DATA.ISO the `MODE2/2048` variant found to work in practice;
  - `INDEX 01 00:00:00`.
- `REM LEAD-OUT` gives the length of AUDIO.BIN.
- The CUE MUST NOT contain any CRC, MD5 or other hash.

**[P14]** The deliverables MUST include:

- an image for writing in one pass: IMAGE.CUE, AUDIO.BIN, and either DATA.BIN (2352 bytes per sector) or DATA.ISO (2048 bytes per sector, absolute addresses);
- a two-step writing kit: a CUE containing session 1 only, and the commands and instructions for regenerating session 2 for a given start address;
- burning and acceptance instructions in Chinese and English. They contain a layout table listing the LBA of INDEX 01 of every track, A2, and the LBA of INDEX 01 of the data track of session 2 (for the check in Appendix A.2).

The v5 WAV of each track (4.1) MAY be supplied in addition, for file distribution.

**[P15]** For two-step writing, the next writable address MUST be taken from the drive's READ TRACK INFORMATION report (MMC 52h). It is checked against both the B0 entry of the TOC and A2 + 11,400. If the three values disagree, the tool MUST stop. Published descriptions of the B0 entry disagree, and B0 alone may be off by 150 sectors (secondary source).

### 5.5 Closed-Loop Self-Check

**[P16]** Before an image is released, the tool MUST read it once with the same code as the reader, taking the extension files from the data session of the image. The following combinations are read:

- whole-disc image with CUE: offsets taken from values within ±5880;
- at offset 0:
  - whole disc as one file without CUE;
  - per-track files, both with CUE and with file order only, in the three gap modes (appended to the previous track, prepended to the next track, omitted), with and without the pregap in the file of track 1.

Every track MUST pass and MUST be reconstructed into a v5 WAV that is byte-identical to the encoder output.

Exception: independent-CD mode with INDEX 00 gaps from track 2 onward. In a rip in which the gaps are omitted, the reader does not receive the gap samples.

- With a CUE (PREGAP), these positions are unknown and are read as 0.
  - If the gap is digital silence, the previous track still passes.
  - If it is not, the previous track falls back with "unknown samples" (consistent with C30).
- With file order only, the positions cannot be known, and the previous track generally falls back.
  - It may pass if the gap is digital silence and the next track happens to begin with digital silence at least as long, so that the window is unchanged.
  - In that case it MUST be reconstructed into a byte-identical v5.
- The self-check judges each track against this expectation. All other combinations still require every track to pass.

If these conditions are not met, the image is void. The specific offsets and rip combinations are given in C02, C06, C23 and C25 of 7.2.

**[P17]** The self-check MUST also include deliberately damaged cases:

- 1 sample changed;
- an extension file from a different album substituted;
- the trailer record removed.

Each of these tracks MUST fall back. If any one does not fall back, the image is void and the process stops.

### 5.6 Capacity

**[P18]** Capacity check:

- Capacity MUST first be checked against the ATIP of the blank disc, or against the type of blank disc (2.5).
- If the content does not fit, the tool stops and lists the available options:
  - omit the extension of some tracks;
  - use a different disc;
  - limit the octaves, which requires re-encoding.
- Tracks or octaves MUST NOT be removed automatically.
- Capacity MUST be checked once more after the self-check.

---

## 6. Reader Requirements (Readers, Plug-ins and Stream Players)

*Informative: overview.*

- The reader treats all inputs in the same way: whole-disc rips (with or without CUE/TOC), per-track rips, single files and LAE streams (4.5).
- Each track is located independently by content (6.3):
  1. coarse location, from the TOC position and/or the information range given by lead0 and tail0 in the trailer record;
  2. fine location, in which every position is screened with S1 and S2;
  3. confirmation by the whole-track MD5.
- A uniform disc-wide offset is neither assumed nor needed.
- A slip within a track (lost or extra samples) makes only that track's MD5 fail. That track is played as CD, and the other tracks are unaffected ([R10]).

### 6.1 Input

**[R1]** The reader has three kinds of input:

- **Rips:** a whole-disc image (BIN or WAV, with or without CUE), per-track files (with or without CUE), or a single per-track file. They may come from any ripping software and any offset setting.
- **LAE streams:** the CD layer delivered per track, plus the extension file (4.5).
- **Extension files:** from the `/LAE/` directory of session 2 of a CD, from a copied folder, or delivered with an LAE stream.

Requirements on audio formats:

- The reader MUST accept 16-bit, 2-channel PCM WAV: at 44 100 Hz for rips, and at the sample rate given in the trailer record for files outside CD and for LAE streams.
- The reader MAY accept other lossless forms: FLAC, AIFF, raw PCM, and 16-bit rips stored in a 24-bit container whose low bytes are all 0.
- Lossy formats (MP3, AAC and so on) MUST NOT be used with the extension layer ([F7]). The reader does not attempt location; it only plays or outputs the base layer and reports "base layer is lossy".

**[R2]** The sources of the TOC are, in order of priority:

1. the TOC read from the drive;
2. the CUE;
3. the file order plus the file lengths of a set of per-track files.

If only the last source is available, the report MUST highlight this prominently.

A single file (a single-track file, or a whole-disc rip without CUE) has no TOC, and neither has any track of an LAE stream. The reader searches such a file by content according to 6.3, and the report MUST state "no TOC".

**[R3]** On encountering an unrecognised CUE construct, the reader MUST stop. It does not guess.

### 6.2 Disc Stream

**[R4]** Composition of the disc stream:

- With a TOC, every file MUST be placed into the disc stream at the position given by the CUE/TOC: the first sample of the file is placed at its corresponding absolute position.
  - Files MUST NOT be positioned by accumulating file lengths.
  - The only exception is a TOC that has itself been derived from the file order according to [R2].
- Without a TOC, each file forms a separate disc stream, with positions counted from 0 at the first sample of that file.

**[R5]** Unknown positions and conflicts:

- Positions in the disc stream that are covered by no file are recorded as "unknown". This includes, without a TOC, the positions outside the file.
  - Unknown positions are always treated as 0.
  - The reader MUST NOT retry with other values to make the MD5 match.
- If two files give different values at the same position, the reader reports a conflict, and the position is treated as unknown.

**[R6]** The length of the CD layer of every track is always the ncd of the trailer record. For the last track, only the start is taken from the TOC. The start of the data track, or A2, MUST NOT be used as the end of the CD layer of the last track; A2 is used only for bookkeeping.

### 6.3 Per-Track Location and Verification

**[R7]** Every track is located independently. Each extension file that has passed the checks of [X7] has a track number k and a CD layer length ncd.

With a TOC, k MUST be an audio track of the TOC. Otherwise the file is not used, and "track number not in TOC" is reported.

Each extension file is processed in the following steps:

1. **Coarse location:** determine a set of coarse positions c.
   - With a TOC: c = q_k ([L6]).
   - From the information range, if lead0 < ncd and the range R contains non-zero samples:
     - find the positions f and l of the first and last non-zero samples in R;
     - this gives c = f − lead0 and c = l + 1 + tail0 − ncd;
     - with a TOC, R = [q_k − W, q_k + ncd + W), and the computation is repeated with R = [q_k, q_k + ncd + W). The second range is needed when the file of this track begins with a silent gap: in the first range, the first sound is then the end of the previous track, and coarse location would land on the boundary;
     - without a TOC, R is the whole file;
     - unknown positions are counted as 0.
2. **Fine location:** examine each window [t, t + ncd).
   - With a TOC, t covers every position within W of each coarse position: c − W ≤ t ≤ c + W.
   - If the TOC is derived from the file order only and none of the above windows matches, t additionally covers q_k − W up to the start of the next file + W. The file of this track may begin with its INDEX 00 gap, whatever that gap contains.
   - Without a TOC, t covers the whole file: −W ≤ t ≤ file length − ncd + W. The coarse positions only determine where the search starts.
3. Screen with the sliding values of S1 and S2 ([X4]). Only positions with A = S1 and B = S2 are candidate windows.
4. The track passes only if the whole-track MD5 of a candidate window equals the MD5 in the trailer record.

W ≥ 5880. W MAY be larger, for example 44 100. For windows that are entirely zero, the MD5 MAY be computed only once.

*Informative.*

- Coarse and fine location only determine where to look for the track. Once the track is found, the whole-track MD5 serves as a simple check of whether the audio has been damaged or altered: it confirms, track by track, that the track is identical to the audio at production time. Acceptance depends solely on it.
- The information range is most useful for tracks with silence at the start or at the end. Even if the offset exceeds W, such a track can still be found from the boundary between silence and sound. Tracks that run into one another without intervening silence rely on the TOC alone.
- A single per-track file without CUE and without neighbouring files is affected by the offset:
  - |o| samples at one end of the track are pushed out of the file, and |o| samples of the adjacent track are pulled in.
  - If the samples pushed out are only padding or digital silence at the start or end of the CD layer, the track is still found and passes. Otherwise the track falls back.
  - With a CUE or a complete set of per-track files, the neighbouring files supply the samples that were pushed out ([R4]).

**[R8]** A track that passes is processed as follows:

- Its CD layer consists of the ncd samples within the matching window, without pregap or padding.
- For extension block type 1, a v5 WAV is reconstructed according to 4.4 and passed to the unmodified decoder.
- If the decoder rejects the file, the track takes the CD route with the reason "extension layer corrupted".

**[R9]** If several candidates pass, their window contents are necessarily identical (for example in an entirely silent track). Any one of them may be used, and the result is the same.

**[R10]** Every track is searched and judged independently.

- A slip within a track makes only the whole-track MD5 of that track fail. That track takes the CD route and is played as CD.
- The other tracks are not affected. The tracks after the slip are found at their shifted positions, because the shift is much smaller than W.

*Informative.* An implementation may first try the offsets already confirmed for other tracks, to speed up the search. Acceptance depends solely on the MD5 of the track itself ([R7]). A future dedicated direct reader may re-read tracks with a slip (Section 8).

### 6.4 Failure and Reporting

**[R11]** A track that does not pass takes the CD route. The ripped CD audio of the track is output unchanged (without a TOC, according to [R19]), and the extension layer is not used.

The report MUST give the reason category. Reasons are decided by counts only, without using extension data:

- **Unknown samples (missing file or truncated file)** (with a TOC): unknown samples occur within W of the nominal window, i.e. in [q_k − W, q_k + ncd + W). Their number is reported.
- **File too short** (without a TOC): the file is shorter than ncd.
- **No match:** none of the above applies, and no matching window is found. Possible causes include:
  - a slip or read error within the track;
  - a different pressing or master;
  - a rip that has been processed (gain and so on);
  - an extension file that does not belong to this track;
  - a single file that lacks, at one end, the samples pushed out by the offset.
- **No trailer record / record corrupted / unknown record version or extension block type / invalid record field / track number not in TOC / extension layer corrupted / base layer is lossy.**

**[R12]** If no track passes, the reader MUST report "possibly a different disc or a different version". Without a TOC, "no track passes" means that no extension file finds a match.

**[R13]** Handling of several extension files:

- The reader reads all .LAX files in the folder and groups them by the track number in the trailer record, not by file name.
- If there are several extension files for the same track number, each is tried in turn, and the MD5 decides.
- If several pass but their extension blocks differ, the reader MUST reject the track and report this.
- Without a TOC:
  - every extension file is searched for in the input files, and the file of the same name is tried first;
  - an extension file that is found in no file is listed as "unused" in the report and does not count as a failed track.

**[R14]** A file without a trailer record, or whose trailer record fails [X7], MUST NOT be used. Tracks without a usable extension file are output as CD.

### 6.5 Prohibitions

**[R15]** CD samples MUST NOT be modified, repaired or replaced. CD samples MUST NOT be filled in from any of the following:

- extension data;
- the edge samples in the extension block;
- check values;
- data from another read.

**[R16]** Measured offsets MAY be displayed. They MUST NOT be written into any output file, including v5 WAV files, high-resolution files, CUE files, reports and logs.

**[R17]** No high-resolution data of a track MUST be output before the track has been accepted or rejected. Within a track, the reader MUST NOT switch between the CD and the extension layer.

**[R18]** Location and verification use only the TOC and, from the trailer record, lead0, tail0, S1, S2 and the whole-track MD5. In particular:

- no window shorter than the whole track is used for verification;
- no external database is consulted;
- no network access is required;
- the edge samples in the extension block are used only by the decoder, according to [SPEC] 4.1. The reader MUST NOT use them to find offsets, to verify, or to join adjacent tracks.

### 6.6 Output and Plug-ins

**[R19]** Output:

- A track that passes: the reader MAY output the reconstructed v5 WAV (4.4) or the decoded result.
- A track on the CD route: 16-bit CD audio is output, and the file name MUST indicate CD.
- An audio track of the TOC without a corresponding extension file is output as CD. This is not a failure and is not reported as an error.
- Without a TOC:
  - passed tracks are extracted from the file at the positions where they were found;
  - parts of the file that belong to no passed track and are not entirely digital silence are output unchanged as CD, and the file name MUST indicate CD;
  - if no track in a single file passes, the whole file is output as CD.
- An album CUE MAY be supplied in addition, for gapless playback.

**[R20]** Player plug-ins and LAE stream players MUST follow all rules of this section:

- before high-resolution playback of a track begins, the whole track MUST have been verified;
- the next track MAY be verified while the current track is playing;
- a track that does not pass, or whose verification is not completed in time, is played as CD in its entirety.

---

## 7. Conformance

### 7.1 Method

- A simulation tool (`lae_cdx sim`) generates images, whole-disc and per-track rips, CUE files and simulated LAE streams. The expected result of every case is written down before the cases are compared one by one.
- Test material:
  - excerpts of 10–15 s of two tracks of one recording, Track A and Track B;
  - test discs derived from these excerpts;
  - CC0 synthetic vectors.

  Intermediate files are deleted after use.
- Defaults for each case, unless stated otherwise:
  - W = 5880;
  - offset 0;
  - whole-disc image with CUE, with the samples before and after the programme read.
- Prohibitive clauses are covered by code review (CR1). The behaviour of real CDs is covered in Appendix A (BT1).
- Every "MUST" and every "MUST NOT" is covered by at least one test case.

**Test discs** (excerpts at 352.8 kHz)

- **Disc X** (per-track master, 2 tracks):
  - Track 1 = a 10 s excerpt of Track A shortened by 1000 master samples. ncd = 440,875, which is not a multiple of 588 (mod 588 = 463, z_1 = 125).
  - Track 2 = a 10 s excerpt of Track B. ncd = 441,000 = 750 sectors, z_2 = 0.
  - p_2 = 441,000 and A2 = 1500 (all derived).
- **Disc Y** ("a different album"): made in the same way as disc X, from different 10 s excerpts of Track A and Track B.
- **Disc S** (continuous master, 3 tracks):
  - A continuous 15 s excerpt of Track A, divided into tracks at 5 s and at 10 s. The music is continuous across the divisions.
  - 5 s = 1,764,000 master samples = 220,500 CD samples = 375 sectors, which lies on the grid.
  - All three tracks have ncd = 220,500; z_k = 0; A2 = 1125 (all derived).
- **Disc T** (per-track master, 2 tracks, with silence at start and end):
  - Track 1 = 1 s of digital zero + a 10 s excerpt of Track A. Track 2 = a 10 s excerpt of Track B + 1 s of digital zero.
  - Both tracks have ncd = 485,100 = 825 sectors; z_k = 0; A2 = 1650 (derived).
  - After production, the following MUST be checked: lead0 ≥ 30,000 for track 1; tail0 ≥ 30,000 for track 2; tail0 = 0 for track 1 and lead0 = 0 for track 2, because the junction of the two tracks is music. If these conditions are not met, the digital zero is lengthened or another excerpt is chosen.
- **Independent-CD mode:** cases I1–I10 below. The publisher's CD is derived by the current encoder from the master of disc S and then modified as each case requires.

| Case | Modification of the publisher's CD | Expected result |
|---|---|---|
| I1 | Tracks unchanged | All offsets are 0; passes |
| I2 | 2 s of digital zero inserted between two tracks; the publisher marks it as INDEX 00 of the following track | Passes; the high-resolution output is digital silence in the gap |
| I3 | 300 CD samples of digital zero added before track 2 | Passes |
| I4 | The first 1 s of track 3 removed | Stops; the statement indicates that the case belongs to the future "keep CD / keep master" task |
| I5 | Noise at −70 dBFS in a gap between tracks | Stops; the statement indicates that the case belongs to the future "keep CD / keep master" task |
| I6 | Polarity of one track inverted | A warning is displayed; passes |
| I7 | Order of two tracks swapped | Stops |
| I8 | An all-zero track of 4 s (300 sectors) | Sample-by-sample comparison instead of the statistical decision ([P8]); passes |
| I9 | Pre-emphasis flag (FLAGS PRE) set | Stops ([L11]) |
| I10 | Non-silent content in INDEX 00 of track 1 | Stops ([L4]) |

In every passing case, session 1 is sample-identical to the publisher's programme. In every stopping case, no file is written and the reason is stated ([P10]).

### 7.2 Test Cases

| ID | Case | Expected result | Clauses covered |
|---|---|---|---|
| C01 | Disc X, offset 0, whole-disc image (WAV + CUE) | All tracks pass. The SHA-256 of each reconstructed v5 WAV equals the encoder output. The CD layer of track 1 starts at absolute position 0. In session 1, the MD5 of [q_k, q_k + ncd_k) of each track = trailer-record MD5 = MD5 of the data chunk of the v5 file | L1 L6 L9 X1 X3 X7 F5 P3 R1 R4 R7 R8 |
| C02 | Discs X and T at offsets 0, 1, 6, 48, 102, 588, 667, 1164, 1292, 1776, 2939, 4103, 4715 and 5880, each with both signs. In addition, a coarse sweep in steps of 97 samples within ±5880. For disc X the samples before and after the programme are read; for disc T they are read or zero-filled | All tracks pass | L5 L8 R6 R7 P16 |
| C03 | Offset ±20,000 (more than twice W): (a) disc T, W = 5880; (b) disc S, W = 5880; (c) disc S, W = 44 100 | (a) All tracks pass (coarse location from the information range). (b) At +20,000, track 1 passes and tracks 2 and 3 fall back; at −20,000, track 3 passes and tracks 1 and 2 fall back; the reason is always "no match". (c) All tracks pass | R7 R11 |
| C04 | Slip: disc S, offset +667, in the middle of track 2: (a) 1 sample lost; (b) 1 sample inserted; (c) 1 sample lost and, 1 s later, 1 sample inserted | Only track 2 falls back, with reason "no match". This track produces no high-resolution output and is played as CD. Tracks 1 and 3 pass; the offset displayed for track 3 is +666, +668 and +667 respectively | R10 R11 R15 R17 |
| C05 | Disc X, 1 sample of track 1 changed | Only this track falls back, with reason "no match". It produces no high-resolution output; only 16-bit audio is output, with CD indicated in the file name | R10 R11 R15 R17 R19 |
| C06 | Per-track rips in the three gap modes (appended to the previous track, prepended to the next track, omitted) × track 1 with / without pregap: (a) disc X; (b) the image of I2 (the publisher has marked an inserted 2 s of digital zero as INDEX 00 of the next track); (c) disc X with the per-track file of track 1 artificially lengthened by 588 samples whose content differs from the start of the file of track 2 | (a)(b) All tracks pass. Within [0, A2 × 588), the MD5 of the assembled disc stream equals that of the whole-disc image. (c) A conflict is reported, and the conflicting positions are treated as unknown. Track 2 falls back with reason "unknown samples"; track 1 passes | R4 R5 R11 P16 |
| C07 | Disc S, per-track rip (track 1 with pregap), offset −667, file of track 2 missing | Tracks 2 and 3 fall back with reason "unknown samples"; track 1 passes | R5 R11 |
| C08 | (a) Image of disc X with the extension files of disc Y. (b) The extension file of track 2 of disc X (Track B), with its trailer-record track number changed to 1 and the CRC recomputed, replaces TRACK01.LAX | (a) All tracks fall back with reason "no match", and "possibly a different disc or a different version" is reported. (b) Track 1 falls back with reason "no match"; track 2 passes | R11 R12 |
| C09 | (a) Whole-disc rip of disc X with modified gain. (b) Per-track rip of disc S in which 6000 samples (more than W) have been removed from the start of the file of track 2, simulating software that strips leading silence | (a) All tracks fall back with reason "no match", and the disc-level notice is reported. (b) Track 2 falls back with reason "unknown samples"; tracks 1 and 3 pass | R11 R12 |
| C10 | (a) Whole-disc rip of disc X with 588 extra samples after the last track. (b) Whole-disc rip file of disc T with the last 5000 samples removed | (a) Passes; the last track ends at ncd. (b) Passes: the removed samples are digital silence at the end of the last track and are treated as unknown, i.e. as 0 | L8 R5 R6 |
| C11 | Disc X with the trailer record removed from TRACK01.LAX | Rejected; track 1 takes the CD route | X7 R14 |
| C12 | Disc X: 1 byte flipped in the trailer record; 1 byte flipped in the extension block; extension file truncated; trailer-record track number changed to 5 with the CRC recomputed (the TOC has only 2 tracks) | Reported in turn: "record corrupted", "extension layer corrupted", "no trailer record", "track number not in TOC". All take the CD route, and the output file names indicate CD | X7 R7 R8 R11 R19 |
| C13 | Extension block type changed to 2; record version changed to 2; lead0 changed to ncd + 1 (CRC recomputed in each case) | The first two cases are skipped with a notice. The third reports "invalid record field". All take the CD route | X2 X7 |
| C14 | (a) TRACK01.LAX of disc X and of disc Y in the same folder, the latter renamed TRACK01B.LAX. (b) Track 1 re-encoded in independent-CD mode (`cd=` the CD layer of track 1 of disc X) with a different encoder setting, giving an extension file with the same CD layer but a different extension block; renamed in the same way and added | (a) Both files are grouped as track 1 by their trailer-record track number, and the MD5 decides: the file of disc X passes. (b) Both pass but their extension blocks differ; the track is rejected, and this is reported | R13 |
| C15 | Disc S with a different offset for each of the three tracks (0, +6, +48) | Each track passes with its own offset; no uniform disc-wide offset is needed | R7 R10 |
| C16 | Disc X with a 4 s (300-sector) all-zero track inserted between its two tracks, offset +667 | All tracks pass | R7 R9 |
| C17 | Disc X, track 1: ncd = 440,875, not a multiple of 588 | z_1 = 125 digital zeros in the image; reading passes | L7 L10 P6 |
| C18 | A single track: the excerpt of track 1 of disc X, as one complete work | z_1 = 125, A2 = 750 (derived). In session 1, only these 125 zeros follow the CD layer. Passes | L7 L8 P7 R6 |
| C19 | Known answers: the three examples of 3.4 (including lead0 and tail0); all 7 test strings of RFC 1321; CRC-32("123456789"); sliding results compared with direct computation at 1000 random offsets | All identical | X2 X3 X4 X5 |
| C20 | Disc X: pack, then unpack. The extension block of the .LAX is compared with the body of the `LAE ` chunk in the v5 WAV. The distribution .lax of the same track is compared with TRACKnn.LAX on the CD | The SHA-256 of the v5 WAV is identical. The extension blocks are byte-identical, and the two extension files are byte-identical. Every trailer-record field conforms to 3.2: lead0 and tail0 equal the values counted from the CD layer, the reserved bytes are 0, and there are no extra fields | X1 X2 X6 F1 F2 F5 P2 |
| C21 | FLAC base layer + .lax: STREAMINFO MD5 present / 0; .lax renamed; two .lax files that both pair but have different extension blocks; one case in the 48 kHz family (the sample-rate field of a Track A excerpt changed to 384 000; files only) | Pairing succeeds, and re-encapsulation is byte-identical. The FLAC file contains no extension layer and no trailer record. After renaming, the file still pairs by MD5 (tested only if "try other .lax files" is implemented). The case with two .lax files is rejected and reported. In the 48 kHz case, the trailer-record sample rate is 48000 and pairing succeeds | F2 F3 F5 P1 |
| C22 | WAV base layer + .lax; a second case in which the base layer lacks its last sample | The first pairs, and re-encapsulation is byte-identical. The second does not pair and is not found in the whole file under Section 6 either (reason "file too short"); only the base layer is output | F2 F3 F4 F5 R11 |
| C23 | Per-track rip of disc T (offset +667) stored as FLAC, in the same folder as the two .LAX files: (a) with CUE; (b) without CUE | No pairing under [F3]. (a) Processed under Section 6; all tracks pass. (b) The TOC is derived from the file order and highlighted prominently in the report; all tracks pass | F4 R1 R2 R4 |
| C24 | A single per-track file without CUE, with all .LAX files of the disc in the same folder: (a) the file of track 2 of disc T at offset +667 (it begins with the last 667 samples of track 1 and lacks the last 667 digital-silence samples of track 2); (b) the file of track 1 of disc T at offset −667 (it lacks 667 digital-silence samples at the start and ends with the first 667 samples of track 2); (c) the file of track 2 of disc S at offset +667, then with the file of track 3 and the CUE added | (a)(b) The track is found by content and passes, and the report states "no TOC". The other .LAX files are listed as "unused" and do not count as failures. (c) On its own, track 2 falls back with reason "no match", because the 667 samples pushed out are music. The whole file is output as CD with the notice of [R12]. After the additions, track 2 passes | F4 R2 R4 R7 R11 R13 R19 |
| C25 | Whole-disc rip stored as one WAV without CUE: (a) disc S, offset 0; (b) disc T, offset +667 | All tracks are found by content and pass; the report states "no TOC". Each track is extracted at the position where it was found. (b) The 667 digital-silence samples at the start of the file belong to no track and are not output | R2 R4 R7 R19 |
| C26 | LAE stream (simulated): the CD layer of each track of disc S stored as FLAC and delivered to the reader track by track together with the .lax files: (a) delivered unchanged; (b) 1 sample changed in the base-layer file of track 2 | (a) Every track passes, and re-encapsulation is byte-identical. (b) The whole-track MD5 of track 2 does not match. The track does not pair, is not found under Section 6 either (reason "no match"), and only the base layer is played. Tracks 1 and 3 pass | F6 R1 |
| C27 | Production refusals: (a) a CD produced from a 48 kHz-family master (sample-rate field of a Track A excerpt changed to 384 000); (b) disc S divided at 5 s and 8 s instead (track 2 is 3 s, shorter than 4 s); (c) a track list with 99 audio tracks; (d) a v5 file of one track with a corrupted CRC in its `LAE ` chunk; (e) insufficient capacity (C set artificially small) | All stop and leave no output. (e) The available options are listed, and no track or octave is removed automatically | L1 L2 L10 P1 P2 P18 |
| C28 | Derived mode, continuous master: (a) disc S; (b) disc S with the start of track 2 moved to 5 s plus 1000 master samples (off the grid) | (a) The track boundaries equal the positions given by the publisher, and z_k = 0. [0, A2 × 588) of session 1 = the CD layers of the tracks without gaps. The MD5 of each track's range = trailer-record MD5. (b) The tool stops, lists this start, asks the publisher to give it on the grid, and writes no file | L13 P3 P5 P7 |
| C29 | Independent-CD mode, I1–I10 as listed in 7.1 (the short track of I8 is a 4 s (300-sector) all-zero track; in I10 the publisher's CD has non-silent content in INDEX 00 of track 1) | I1, I2, I3 and I8 pass, and session 1 is sample-identical to the publisher's programme. I6 passes after displaying a warning. I4, I5, I7, I9 and I10 stop, write no file and state the reason. For I4 and I5, the statement indicates that the case belongs to the future "keep CD / keep master" task | L4 L11 L14 P3 P8 P9 P10 P11 |
| C30 | Independent-CD mode: the publisher's CD marks a 2 s INDEX 00 before track 2 that contains music (the master contains it as well, and encapsulation succeeds); per-track rip with the gaps omitted | Track 1 falls back with reason "unknown samples"; the other tracks pass | L14 R4 R11 |
| C31 | Disc X ripped and read at offset 0 and at offset +667 | All output files of the two runs are byte-identical, which shows that the offset has entered no file | R16 |
| C32 | During the self-check: (a) a positive case is made to fail artificially; (b) the reader is made to accept a modified track artificially | (a) The image is void. (b) The image is void, and the process stops | P16 P17 |
| C33 | Image inspection (disc X, disc S, I2). AUDIO.BIN: contains, sample by sample, only the CD layers and padding; the CD layer of track 1 starts at sample 0; only z_n zeros follow the CD layer of the last track; the length is a whole number of sectors. IMAGE.CUE: track 1 has INDEX 01 00:00:00 and no PREGAP; in derived mode only INDEX 01 from track 2 onward; REM SESSION lines; the FILE line and MODE2 of the data track; no FLAGS PRE; no hashes. ISRC, CATALOG and CDTEXT lines contain only standard metadata. Data track: the mode byte in every sector header of DATA.BIN is 2; the track, including trailing zero sectors, is at least 300 sectors; its start = A2 + 11,400. The deliverables are complete | All conform | L3 L4 L5 L6 L7 L8 L9 L11 L12 L13 D1 D3 D6 M1 P12 P13 P14 |
| C34 | Data session: both directory trees read with the built-in parser; a copy with start address 0 mounted on Windows | Files byte-identical; README.TXT present; only TRACKnn.LAX under /LAE/; no CUE, TOC copy, report, program or autorun.inf; README.TXT contains no hashes, positions, offsets or tables of lengths | L1 D1 D2 D4 D5 |
| C35 | Unrecognised CUE construct; only the file order available | Stop; highlighted prominently in the report | R2 R3 |
| C36 | Plug-in: the next track has not finished verification, or has failed | The whole track is played as CD; no switching within the track | R17 R20 |
| C37 | Disc X with no extension file for track 2 | Track 2 is output as CD; no failure and no error is reported. Track 1 passes | D7 R19 |
| CR1 | Code review | The reader does not read the edge samples in the extension block. It does not repair CD samples. It treats unknown positions only as 0 and does not retry with other values. It does not verify with windows shorter than the whole track. Location uses only the TOC, lead0, tail0, S1, S2 and the whole-track MD5. No external database is consulted, and there is no network access. The extension layer is not applied to a lossy base layer. The trailer record and the extension file contain no value derived from the extension layer and no position or offset. The PCM of session 1 contains no embedded data. The encapsulation tool does not pass `cdalign=none`. Offsets, position differences, shift amounts and trim or padding amounts are not written into any file. The tool stops when ncd ≥ 2^28. Capacity is checked again after the self-check | X6 L12 P4 P7 P8 P18 R1 R5 R15 R18 F7 |
| BT1 | Acceptance test with a real burn (Appendix A.2) | See Appendix A | L3 L4 L5 D3 D6 P15 |

---

## 8. Versions and Extensions

- **Specification version.**
  - This specification is draft version 0. It will be revised after the measurements of Appendix A have been completed.
  - It is published as spec/cdx/SPEC.md under CC BY 4.0.
- **Trailer record version.**
  - The current version is 1.
  - lead0 and tail0 were placed in previously reserved bytes before any publication of the trailer record. The record therefore remains version 1.
  - After publication, any new field or any change requires a new trailer record version. This includes the layout of the trailer record and the definitions of MD5, S1, S2, lead0 and tail0.
  - Readers reject unknown versions ([X7]).
  - Reserved bytes are written as 0 and ignored when read.
- **Extension block types.**
  - 1 = LAEY container v5.
  - 2 = reserved for LAES. The relation between the extension block and the CD layer is defined by the LAES specification.
  - Later LAEY container versions, if any, use new type codes.
  - Type codes identify only the container or the tier.
  - The reader treats the extension block as opaque bytes and passes it unchanged to the corresponding decoder. Therefore the CD layout, the trailer record and the reading rules of this specification need no change when LAES is added.
- **v5 unchanged.** This specification does not require v6.
- **Not in version 0; to be specified later:**
  - keep CD / keep master, and per-layer provenance. This is a future task that this specification does not address. It also covers the case in independent-CD mode in which master and CD differ in non-silent content (version 0 stops; [P10]);
  - a dedicated direct reader (one-step operation on inserting the disc). It is only another kind of input to [R1] and likewise follows Section 6. It may re-read tracks with a slip ([R10]). Details are to be specified later;
  - the interface and host of plug-ins;
  - the transport of LAE streams (protocol, segmentation, buffering);
  - the `.lae` complete file layout ([SPEC] 1.1);
  - the definition of the LAES extension block, and which subset of LAES is placed on a CD-Extra;
  - delivery for pressing (PQ list, the pressing plant's maximum duration);
  - an example burning command with cdrecord on Linux;
  - DDP delivery, and a dedicated burning tool.

---

## Appendix A. Values to Be Measured and Acceptance

### A.1 Values That Only a Real Burn or Rip Can Determine

| # | Item | Value in this draft | Source and reliability | Method of measurement |
|---|---|---|---|---|
| 1 | LBA of INDEX 01 of the data track of session 2 = A2 + 11,400 (6,750 + 4,500 + 150; the last term is the pregap of the data track) | 11,400 sectors | Primary sources (cdrtools, MMC); not yet measured on the burner used for acceptance | After burning, read the TOC and compare it with the address reported by 52h and with B0 |
| 2 | Number of trailing zero sectors of the data track | 150 | Estimate (15 may suffice) | Check whether the last file can be read after burning |
| 3 | Combined offset of the drive and burner used for the test (for the record only) | — | Worst-case combined offset of an uncorrected raw read about +4715 / −4103 (derived, uncertain) | Per-track offsets displayed by the reader |
| 4 | Sector format of the data session | MODE2/2048 preferred; MODE2/2352 if necessary | Preference: sync, header, EDC and ECC generated by the drive; to be measured | See which one ImgBurn accepts. Before using 2352, run an EDC/ECC known-answer test against a real CD |
| 5 | XA subheader values (submode 0x08, EOR/EOF bits); header set to zero when computing the ECC | As stated | Secondary source | Needed only when 2352 is used; verify against a real CD |
| 6 | Volume space size in the PVD = absolute last LBA + 1 | As stated | Estimate (multisession convention) | Check whether Windows, macOS and Linux display session 2 |
| 7 | Whether ImgBurn 2.5.8.0 can burn the disc in one pass. Whether, with a CUE giving "track 1 INDEX 01 00:00:00, no PREGAP", the 2 s pregap of track 1 is written as digital silence. Whether ImgBurn adds the 150-sector pregap of the data track of session 2 automatically, or it must be written in the CUE | — | No source for either pregap; to be measured | Route A |
| 8 | Whether SAO can be used on an appendable disc | — | To be measured | Route B |
| 9 | Capacity of an 80-minute blank disc | 359,849 sectors | Secondary source (ATIP maximum 79:59:74, as reported by cdrecord) | Read the ATIP of the blank disc |
| 10 | Whether Windows 10/11 displays the Joliet files of session 2 | Yes | Secondary source | Inspect with Windows Explorer |
| 11 | Ripping software: whether the pregap of track 1 goes into the file of track 1; whether the disc streams assembled from the three gap modes agree | — | To be measured | Whole-disc and per-track rips with EAC; inspect the reader's report |
| 12 | Which CD players play session 1 of this CD-R | — | To be measured | Try home, car and DVD/BD players; record compatibility problems of CD-R media as such separately |
| 13 | Maximum number of audio tracks | 98 | Derived (the data track occupies one track number) | Read the TOC |

### A.2 Acceptance Procedure

**Success criteria:**

- the image can be generated;
- the CD is recognised and can be read;
- the extension can be restored to the files as they were before encapsulation.

1. **Production side.** Generate the image and pass the self-check ([P16], [P17]).
2. **Check the image.**
   - Open IMAGE.CUE in ImgBurn 2.5.8.0.
   - Compare, one by one, the LBAs of the tracks and the start of the data track of session 2 shown under "image information" with the layout table in the burning and acceptance instructions ([P14]).
   - If they differ, do not burn.
3. **Burn.**
   - First burn in one pass (route A).
   - If this fails, or if session 2 is at the wrong position, use the two-step writing kit (route B).
   - While the disc is still open, Windows may offer to "burn files to disc"; do not accept this offer.
4. **Recognition and reading.**
   - Insert the CD again. Windows Explorer should show README.TXT and LAE\TRACKnn.LAX.
   - Compare these files with the source files in the image using `fc /b`; they should be identical.
5. **Check the TOC.**
   - Read the TOC and check three items: INDEX 01 of track 1 at LBA 0; A2; the LBA of INDEX 01 of the data track of session 2.
   - This step requires the planned TOC-reading tool (discinfo).
6. **Play session 1** on at least one ordinary CD player.
7. **Restore the encapsulation.**
   - Rip the disc twice with EAC: once as a whole-disc image and once per track.
   - Run the reader. Every track should pass.
   - The reconstructed v5 WAV files should have the same SHA-256 as the per-track v5 WAV files given to the encapsulation tool (check with `certutil -hashfile <file> SHA256`).
8. **Return the measured values.**
   - Record the measured results of every item of A.1, including the drive and burner models, the offsets and the addresses displayed by ImgBurn.
   - Return them for the revision of this specification.

**Notes**

- The test discs contain only material derived from Track A and Track B. They are not distributed, and they are disposed of by the tester after acceptance.
- Third-party software (ImgBurn, EAC and so on) is obtained and installed by the tester. It is used only for burning and ripping tests and is not part of the LAE code or repository.

