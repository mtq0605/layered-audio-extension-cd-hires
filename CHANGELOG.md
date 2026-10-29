# Changelog

This file records the notable changes to LAE (Layered Audio Extension), its lossy tier LAE Lossy (LAEY) and its lossless tier LAE Lossless (LAES), newest first.

## 2026-10-30 (v5-2026-10-30)

The container format and the decoding process of LAEY are unchanged. The encoder writes byte-identical files, faster, block by block and with much less memory.

### Format names

- The first 4 bytes of the `LAE ` chunk name the format: `LAE1` = LAEY (container version 5, unchanged), `LAE2` = LAES, the lossless tier (its own specification). `spec/lossy/SPEC.md` and `spec/cdx/SPEC.md` say so.
- `lae_dec` reports the format it finds. For `LAE2` it says that the file is an LAES file and names the tool to use, instead of the former "unknown LAE chunk version". Its success line ends with `format LAEY (extension LAE1)`.
- `lae_cdx` checks that an extension block of trailer type 1 begins with `LAE1` and one of type 2 with `LAE2` ("extension block does not match its trailer type"). Type 2 is now read and packed (see LAE Lossless below).

### Encoder

- **Faster, same output.** About 3–4 times faster: a 60 s stereo master at 352.8 kHz (mode 3, s1) on a 32-thread Windows x64 machine took 43 s instead of 177 s. Every output file is byte-identical to the 2026-10-24 encoder: all modes and options, independent CD with whole and fractional placement, 88.2 / 96 / 176.4 / 192 / 352.8 kHz, 16 / 24 / 32-bit masters, short tracks; also checked on Linux x64 and Linux ARM64 against the conformance vectors.
  - The partition search no longer allocates a list of cut positions for every rectangle (with many threads these heap allocations waited on each other), reuses its per-block tables, computes the mean D / C energies once per share search, and skips the stream and the planes while it searches the share.
  - The layer signal's periodogram is computed once (it was computed three times, one result unused); layer settings are read once instead of once per cell.
  - The route split's filters are divided across threads, and the two layers of a pair (depth L and R, frequency M and S) can be coded at the same time.
- **Less memory, same output.** One reconstructed plane instead of two identical copies; each octave segment holds only its own bins; signals are freed as soon as they are no longer needed; the master's samples are freed once the CD layer is derived. 60 s at 352.8 kHz: 1.06 GB with the layers one after the other (formerly 1.83 GB), 1.49 GB with two at once.
- **Memory plan and `mem=<GB>`.** At the start the encoder reads the physical memory once, decides from the track length whether each pair of layers is coded at once or one after the other, prints the plan (`memory plan: …`) and follows it. Two at once only on 64-bit builds, with `PARTTHREADS` not 1, and when it fits the memory limit and the available memory. The limit is min(1/2, max(1/4, physical memory − 28 GB)) by default (8 GB on 32 GB, 20 GB on 48 GB, 32 GB on 64 GB); `mem=<GB>` sets it by hand. The block length follows from the limit, at most about 32 s (longer blocks are not faster). The output does not depend on the plan, the limit or the block length.
- **Block-wise encoder, same output.** The encoder reads the master and codes the track block by block (about 32 s per block). The track-wide decisions (floors, tone and transient marks, mean energies, code-length statistics, mode proportions) are added up from all blocks first; then every block is coded by that plan. Every file is byte-identical to the whole-track encoder: all modes and options, all rates, short tracks, independent CD with whole and fractional placement, and a 10.16-minute 352.8 kHz 32-bit master (peak memory 475 MB instead of about 12 GB).
  - Memory no longer grows with the track length apart from small per-cell records (3 bytes per cell). Large intermediate data go to the work folder on disk: 13.2 GB at most for a 10.16-minute 352.8 kHz master (about 1.3 GB per minute); `laes enc` with four masters: 38.6 GB for 10 minutes. Both are removed at the end.
  - The Windows limit of about 12.7 minutes at 352.8 kHz is gone. The limit is that of the v5 container, about 94 minutes at 352.8 kHz.
  - Masters may be WAV, RF64 or Sony Wave64.
  - New option `resume`: an interrupted run (power cut, Ctrl+C, full disc) continues from its last finished step when it is started again with the same master, options and `resume`; the output is the same as that of an uninterrupted run. With `cd=`, the alignment is done again and the steps after it continue.
  - With `cd=`, the master is read through windows of its file too, and its placement and fractional move are written file to file. The alignment search itself is unchanged and works at the CD rate over the whole track: `cdcheck` of a 10-minute 352.8 kHz master against its CD needs 2.36 GB instead of 7.5 GB; every alignment result, line and file is the same as before.
  - The decoder reads the CD audio of its input in place, without the former 32-bit copy of every sample.
- `PARTTHREADS=1` now makes the whole encoder single-threaded.
- **Fail-safety in threads.** An error inside any worker thread (out of memory, a failed write) is handed to the main thread and handled exactly as before: no output, no `.part` file, the temporary folder removed (24 forced-failure cases, layers at once and one after the other).
- The opt-in `TIERSTAT` statistics now count per stream.

### LAE Lossless (LAES): first release

- **Specification:** [spec/lossless/SPEC.md](spec/lossless/SPEC.md) (English) and [spec/lossless/SPEC.zh.md](spec/lossless/SPEC.zh.md) (Chinese), payload version 1, draft for public review. The design document of 2026-10-18 is superseded.
- **What it stores:** a core (16, 24 or 32 bit at 44.1 or 48 kHz) that every player plays, and 1 to 16 independent masters (16, 24 or 32 bit, at the base rate × 1, 2, 4 or 8). Every master and each of its rate levels is reconstructed bit for bit and checked by SHA-256. A master is stored against the core or an earlier master (at the base level or at the highest level both have), or in full; masters are never moved, their time position is stored.
- **Carriers:** WAV file (`LAE ` chunk, magic `LAE2`), FLAC file (core in the audio frames, payload in APPLICATION blocks with ID `LAE2`; the ID is provisional until registered), CD-Extra (extension block type 2).
- **Tool `laes`** (`src/laes_tool.cpp`): `enc` (WAV or FLAC form), `dec` with `--list` / `--pick` (every master and rate level, and a 16-bit version derived on request, marked lossy), `verify`, `align` (the automatic search of `lae_enc`, the position stored as T), `selftest`.
- **Planner:** `lae_enc plan … laes` cuts an album master at the CD's track boundaries without moving it, each track with its own T.
- **`lae_cdx`** packs and reads LAES extension files (type 2): every master must decode and match its SHA-256.
- **Licences:** as LAEY: the decoder side (`laes_math.h`, `laes_lift.h`, `laes_codec.h`, `laes_file.h`, `laes_flac.h`, `laes_stream.h`) Apache-2.0; the encoder side (`laes_codec_enc.h`, `laes_file_enc.h`, `laes_flac_enc.h`, `laes_stream_enc.h`, `laes_tool.cpp`, `laes_selftest.h`) GPL-3.0-or-later or the commercial licence.
- **Tests:** `laes selftest` (synthetic signals: round trips over both carriers, all word lengths, both rate families, all storage values; damage and parser tests). Measured on 10-second excerpts: 16-, 24- and 32-bit masters of one mix with all rate levels take 99.6–99.8 % of a FLAC of the 32-bit master alone.
- **Memory:** `laes` works block-wise (output byte-identical): a 10-minute track with four masters (32 / 24 / 16 bit at 352.8 kHz, 24 bit at 176.4 kHz) and an independent CD as core encodes with 2.9 GB peak memory and decodes with 2.2 GB (the whole-track version needed about 57 GB). Memory limit `--mem=<GB>` (default min(1/2, max(1/4, physical memory − 28 GB))); a work folder holds the candidates. A WAV output beyond 4 GB is refused (use the FLAC form). Numbers in [docs/BUILD.md](docs/BUILD.md), Section 5.
- Not yet: LAES playback in the foobar2000 component, files beyond 4 GB, streaming decoding.

## 2026-10-24

This release changes the encoder and the tools and adds a decoder library and a player component. The container format and the decoding process are unchanged.

### Format

- The container version remains 5. No field, block or semantic of the container has been added or changed.
- Decoders are unchanged. Every container version 5 decoder decodes files written by this encoder, including files aligned to a fraction of a master sample (see Encoder).
- Nothing about the alignment or the fractional move is stored in the file. A decoder never discards a sample.

### Encoder

- **Independent-CD alignment.** With `cd=<CD.wav>` (an independently produced CD as the CD layer), the encoder now aligns the master with the CD. The CD's samples are never changed.
- **Placement across digital silence only.**
  - The master is placed by whole master samples, and only across digital silence (all-zero samples).
  - The side whose sound starts earlier plays first, and the side whose sound ends later ends later. The shorter side is padded with digital silence, so no side loses a non-silent sample.
  - If the master's sound starts less than one CD sample before the CD's, one CD frame of digital silence is put in front of the CD layer. The CD's own samples are unchanged.
- **Sound present on one side only.** If one side has sound that the other lacks, over one CD sample or more, the case is not supported and the encoder stops.
- **Waveform search.** The search uses a fixed coarse-to-fine cascade:
  - Level 0 works at 1/16 of the CD rate. It scans the whole allowed range by cross-correlation and then scans once more around the peak.
  - Each further level halves the blocks, doubles the rate and searches ±32, then ±8 level samples around the previous peak.
  - The last level compares whole master samples within two CD samples by waveform residual.
  - A peak on the edge of a level's range ends the search with "alignment failed".
- **Two searches.** The search runs twice:
  - once with a whitened cross-correlation (GCC-PHAT; C. H. Knapp and G. C. Carter, IEEE Trans. ASSP, 1976);
  - once with a plain, energy-normalised cross-correlation.
  
  Whitening is not better in every case, so both are run. The program does not compare the two results or choose between them.
- **Check blocks.** The alignment is checked on three blocks of the CD's sound:
  - Pieces of 40 s or more: 5 s blocks starting 10 s after the first sound, ending 10 s before the last sound, and centred.
  - Pieces of 3 to 40 s: the middle of each third, at most 5 s each.
  - Pieces under 3 s: one check over the whole sound.
- **Per-block measures.** For each block the encoder computes:
  - the waveform residual after one least-squares gain per channel; a residual above -3 dB means the block does not match;
  - the remaining time difference against the piece's single fractional best fit.
- **Drift check.** Each block is also compared at two CD samples either side of the offset, with the gains kept. A better fit there indicates a different speed, an edit or another take, and the encoder stops.
- **Bands of the remaining time difference.**

  | Remaining time difference | Behaviour |
  |---|---|
  | -40 dB or lower | Aligned. |
  | -40 to -33 dB | The encoder asks for confirmation. |
  | Above -33 dB | The encoder asks and warns that the file may become much larger. |
  | No fit | The piece is never encoded. |

- **Channel swap or inverted polarity.** These need the user's confirmation (`cdaccept`), as in the −40 … −33 dB band; the extension codes them.
- **User confirmation of every position.**
  - Without `cdalign=`, the encoder performs both searches. For each it measures the fraction, tries the placement with the move, and checks again exactly as for a hand-given offset.
  - It prints each result ("aligned at cdalign=<offset>", "not aligned within the limit at …", or "alignment failed …").
  - It then prints the line `ALIGN_SUGGEST whitened=<status>:<offset> plain=<status>:<offset>`.
  - Finally it stops without writing anything: exit code 1, or 0 with `cdcheck`.
- **Alignment to a fraction of a master sample.**
  - After the whole offset t, the encoder measures the fraction once per piece on the check blocks.
  - The measurement is a fixed descent in steps of 1/8, 1/16, … of a master sample, down to 2^-22 CD sample, with three points per step and at most half a master sample.
  - The master is then moved once by `move` master samples (|move| ≤ 0.5) and checked again at offset 0. The offset is t − move.
- **Interpolation used for the move.**
  - Band-limited interpolation with a 128-tap Kaiser-windowed sinc (β = 10). The taps are computed once from `move` by a closed formula.
  - Digital silence is assumed outside the master.
  - The result is rounded to 24 bits. Hard clipping is reported with a warning and a count.
- **Accuracy of the move** (measured on Track A, +0.3 then −0.3 master samples):
  - Error in 0–20 kHz: −140 dBFS.
  - Ultrasonic error: −147 to −166 dBFS.
  - At hard-cut edges, where sound is present at the first or last master sample: digital silence is added at both ends before the move and removed again where it stays silent, so what the move carries past the edge is kept; the edge error is then below −66 dBFS (−38.5 dBFS without the added silence).
  - Masters that start and end in digital silence are not affected: their files are byte-identical with and without this step.
- **Console line.** The encoder prints `PHASE t=<whole> phi=0 move=<move> offset=<offset>`. The field `phi` is always 0.
- **Digital silence around the move.**
  - Before a fractional move, 128 CD frames of digital silence are added at both ends of the placed master and of the CD layer.
  - After the move, only what is still digital silence is removed again: at most the added frames, never a frame of the given CD. Material that the move carries past the master's first or last sample is therefore kept.
  - A master that starts and ends in digital silence gives a file byte-identical to one encoded without this step.
  - A master cut in the middle of the sound keeps a few frames of digital silence at the ends of the CD layer: 8 at 352.8 kHz, up to 32 at 88.2 kHz.
- **Movement report.** After a successful encode, the encoder writes the plain-text file `<output>.align.txt` beside the output, for the user's records. It contains:
  - the whole offset;
  - the fraction moved, in master samples and in microseconds;
  - the total offset;
  - which side starts earlier;
  - the digital silence added and removed;
  - the check after the move.
  
  No program reads this file, and it is not part of the LAE file or of a disc.
- **Options.**

  | Option | Effect |
  |---|---|
  | `cd=<CD.wav>` | Encodes the extension against an independently produced CD (see above). |
  | `cdalign=<offset>` | Gives the offset in master samples (positive: the CD starts later), as a whole number or with up to 9 decimals, for example `cdalign=3.4999828`. A whole number means fraction 0. Giving back a printed offset yields the same file (tested byte-identical). |
  | `cdcheck` | Performs the same checks without encoding. |
  | `cdaccept` | Continues after a remaining time difference above −40 dB; otherwise a console asks. |

- **Thresholds.** The alignment thresholds are defined in `src/cd_align.cpp` (struct `Th`).
- **Fail-safe fixes.** Every file operation and every module result is checked: a full disk, a module error, running out of memory or a failed leak compensation stops the encoder with the reason, no output file is written and the work folder is removed. Normal output is byte-identical.

### Planner

- **New command.** `lae_enc plan <master.wav> [<master2.wav> …] <publisher.cue> <out dir> [cdalign=<positions>] [cdaccept]` matches a whole master against a publisher's disc.
- **Inputs.**
  - The disc is read as CUE + BIN or WAV.
  - Several master files are joined sample by sample. Their own split points are not used.
  - If the master's split points differ from the CD's, the planner prints "the master is re-split at the CD's positions".
- **Order of operations.**
  1. Join the master files.
  2. Check silence and coverage on the master as it is.
  3. Align.
  4. Obtain the user's confirmation of every position.
  5. Move.
  6. Cut at the CD's track markers.
- **Confirmation.**
  - Without `cdalign=`, the planner runs both searches through the whole plan and prints `PLAN_SUGGEST whitened=<status>:<positions> plain=<status>:<positions>`. Nothing is written (exit code 1).
  - With `cdalign=<p1>,<p2>,…`, the user gives one value per piece: the master sample of the piece's CD frame 0. A fraction is allowed, and `-` marks a piece that is digital silence on the CD.
  - Values given by hand always take precedence.
- **Matching.**
  - Each piece is searched in the master around its expected place, within ± max(60 s, piece length).
  - A piece that is digital silence on the CD is not searched, and its master range must be digital silence.
  - Between pieces, only digital silence may differ.
- **Groups.**
  - Pieces whose master ranges touch at a CD track marker form a group. Such a marker is where the CD splits continuous master material.
  - A group has one whole offset and one fraction, measured as one piece. Different fractions given for one group stop the planner.
  - Each group is moved from the joined master with its real neighbouring samples and then cut. Pieces that join without a gap are therefore joined in the output exactly as if the whole group had been moved once (tested bit-identical).
- **Spill without a target.** The move can carry samples to where no piece takes master samples: the master's very start or end, or a gap present only in the master. The planner then:
  - reports where and how many samples are affected;
  - states that adding at least 1 ms (44 CD frames) of digital silence there to both the master and the CD, with later track markers moving accordingly, keeps them.
  
  The user decides.
- **Stop conditions.** The planner stops, reporting the time range, when:
  - the master has sound the CD lacks;
  - two pieces have sound in the same master range;
  - the order of the pieces differs;
  - the CUE sets FLAGS PRE;
  - there is sound in track 1 INDEX 00.
- **Outputs.**
  - Per piece: `TRACKnn.master.wav` (24-bit, moved) and `TRACKnn.cd.wav` (the publisher's samples).
  - `plan.align.txt`, a plain-text report of the move that no program reads.
- **Encoding the slices.** The slices are encoded with `cdalign=0`.
- **Complete chain.**
  1. `lae_enc plan`
  2. `lae_enc` per piece with `cdalign=0`
  3. `lae_cdx image <dir> TRACK*.v5.wav layout=<publisher.cue>`
  
  Session 1 of the resulting CD-Extra disc is the publisher's program, sample for sample.

### Decoder and library

- **Streaming decoder library.** `src/lae_decoder.h` decodes in a streaming fashion; its memory use is independent of the track length.
- **C API.** The C API `liblae` (`src/lae_api.h`) is at API version 3 and has x64 and x86 builds.
- **API additions.**
  - `lae_open_memory` and `lae_stats`.
  - A per-thread stream-error flag.
  - A static-link build switch.
  - Thread-safe CRC tables, including the trailer CRC table.
- **Fast seek.** Seeking is bit-exact, using entropy checkpoints about every 4 s and a warm restart of every signal stage.

### Tools

- **New tool `lae_cdx`** for CD-Extra encapsulation, with the commands `pack`, `split`, `repack`, `image`, `data`, `read`, `sim`, `selftest` and `test`, and a reusable reader core.
- **Data session.** The data session uses ISO 9660 / Joliet. It carries per-track extension files, each ending in a trailer record.
- **Publisher layout.** `lae_cdx image … layout=<publisher.cue>` builds the disc on the publisher's track layout (see Planner).
- **Portability.** File-size handling in `lae_cdx_read.h` is portable (`fseeko` / `ftello` outside Windows).

### Player component

- **foobar2000 component.** `foo_input_lae` 0.3 is a 32-bit input component for foobar2000 1.4 / 1.5 and 2.x (32-bit), built on `liblae`.
- **Features.** Automatic decoder order, a layer menu, a live bitrate display, and a Chinese / English user interface.
- **Documentation.** See `plugins/foo_input_lae/README.md`.

### Conformance and tests

- **Conformance vectors.** The encoder writes the 16 container version 5 conformance vectors byte-identically, and the decoder gives all 64 outputs identical.
- **Measurement of the fraction.**
  - Test material: CDs made from the master delayed by 0.1 / 0.25 / 0.5 master samples (Kaiser-windowed sinc), at 352.8 / 176.4 / 88.2 kHz, on 2 tracks (18 cases).
  - Measurement error: within ±2.3e-5 master samples (≤ 108 ps).
  - Remaining time difference: −99 dB, the printed floor, in all 18 cases.
- **Track A, CD = master delayed by 3.5 samples.**
  - Measured offset: +3.4999828. Re-check residual: −73.1 dB.
  - Extension size: 1,671,417 B, against 1,807,763 B with the whole offset only (−7.5 %).
  - Error in 0–20 kHz against the master placed and moved independently: −112 dBFS (whole-offset encode: −113 dBFS).
  - The output of a decoder supporting only container version 5 is identical.
- **Track B, CD = master delayed by 0.5 samples.**
  - Measured offset: +0.4999905. Re-check residual: −77.3 dB.
  - Extension size: 2,638,186 B, against 3,169,780 B with the whole offset only (−16.8 %).
  - Error in 0–20 kHz: −111 dBFS.
  - The output of a decoder supporting only container version 5 is identical.
- **Dual search and confirmation** (9 cases on two other test recordings at 352.8, 176.4 and 88.2 kHz).
  - Without `cdalign=`, nothing is written and both results are reported.
  - With the reported offset, the files are byte-identical to those of the previous encoder.
- **Narrowband example.** A 2 s excerpt of a slow 79–182 Hz sweep is found only by the plain correlation: +0.3000226 master samples, against a true offset of +0.3. The whitened search fails.
- **Planner.** Whole-disc case I1 (three tracks that the CD splits from continuous material) passes 13 of 13 checks with confirmed positions and the master moved before cutting; an earlier run with the planner's printed positions passed 11 of 11.
- **Linux ARM64.** Added to the verified builds: on aarch64, the decoder (GCC 13 at `-O2` and `-O0`, Clang 18 at `-O2`) gave 192 of 192 results identical to the reference, and the encoder (GCC 13, `-O2`) reproduces all 16 conformance vector files byte for byte.
- **Test suites.** The suites `plan1` and `align1` follow the confirm-then-move flow.
- **liblae end-to-end tests.** Command-line end-to-end tests were added for `liblae`.

### Documentation

- **CD-Extra encapsulation.** Specification: `spec/cdx/SPEC.md` (draft, version 0).
- **Player component.** `plugins/foo_input_lae/README.md` was added.
- **Usage.** README Section 14 describes the alignment options, the planner and the CD-Extra commands. The CD-Extra trailer counts lead0 / tail0 describe content of the CD layer, which is always played and never removed (`spec/cdx/SPEC.md`).
- **README.** Updated for independent-CD alignment, the planner, CD-Extra packaging and reading, the decoder library and the player component. The lossy specification (`spec/lossy/SPEC.md`) is unchanged: container version 5.

### Known limits

- **Search failures on short or narrowband material.** The search can fail, and then stops, on:
  - a 2 s excerpt of one sustained tone;
  - a 2–3 s passage dominated by one low tone;
  - content entirely above about 1.2 kHz, which lies outside the level-0 band.
  
  The user then gives the position by hand.
- **Periodic signals.** Strictly periodic synthetic signals (tones, saw waves, loops) surrounded by digital silence can be matched one whole period away. For this reason every position is confirmed by the user.
- **Samples carried past an edge.** In the planner, where the move carries samples to where no piece takes master samples (the very start or end of the master, or a gap that only the master has), they are not kept; the planner says where and how many, and that adding 1 ms of digital silence there to both the master and the CD keeps them.
- **Real CD / master pairs.** No real independent CD / master pairs from the market have been tested. Tests used only CDs derived from the master with EQ, limiting, gain, delay, noise reduction, gating, edits and drift.
- **Not yet done:**
  - a CD-Extra burn test;
  - measurement of encoder speed and memory. The encoder holds a whole piece in memory, and a whole-disc plan holds the whole master.

## 2026-10-18

- Initial public release (container version 5).

