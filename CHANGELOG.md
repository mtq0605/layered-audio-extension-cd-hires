# Changelog

[中文](CHANGELOG.zh.md)

This file records the notable changes to LAE (Layered Audio Extension), its lossy tier LAE Lossy (LAEY) and its lossless tier LAE Lossless (LAES), newest first.

## Not yet released (after v5-2026-11-14)

The release packages of v5-2026-11-14 do not contain these changes; the sources in `src/` do. The file formats do not change: no new field, no new version number. For files and options that existed before, the encoded files and the decoded samples are byte-identical to those of v5-2026-11-14.

- **New tool `lae_repair` (2026-11-18).** It puts an LAE file that another program changed back into the LAE form, when the LAE data is still complete: an LAE FLAC file that `flac` re-encoded with inter-channel decorrelation (frames coded again by the LAE rules, output FLAC), and an LAE WAV file that `flac --keep-foreign-metadata` packed into FLAC (the original WAV file given back, byte for byte). It checks the audio against the MD5 before it writes, and checks the result as `lae_udec verify` does; when a check fails, nothing is written ([spec/lossless/LAES_TOOL.md](spec/lossless/LAES_TOOL.md), [spec/COMPATIBILITY.md](spec/COMPATIBILITY.md)). The FLAC reader decodes inter-channel decorrelation only for this tool; players and the other tools still refuse such frames, and their message now names `lae_repair`.
- **Name positions (2026-11-18).** `lae_enc master<n>part2=` … `master<n>part5=` and `laes enc / add / names --part2=` … `--part5=<k>:<text>`: optional positions 2 to 5 of a master's or pack's name, free text without a fixed meaning, joined after the name as `<name> - <part2>, <part3>, <part4>, <part5>` (at most 255 bytes together). The file holds only the joined name and is byte for byte the file written with that text as the name ([docs/PACKS.md](docs/PACKS.md), [spec/lossless/LAES_TOOL.md](spec/lossless/LAES_TOOL.md)).
- **Library care (2026-11-18).** `laes verify` looks for the LAE extension before it reads the audio: a plain FLAC file is reported as "no LAE2", an LAEY file as such. `laes_export` carries the pictures of the source file into a one-file export (FLAC PICTURE blocks, or the WAV file's `id3 ` chunk; `--seg` pieces carry none). `laes tag --keep` keeps the old tags and pictures and replaces only the given keys and picture types. When tags are read, ID3v2.4 values in UTF-16 with a byte-order mark each are read correctly (before, the second and later values began with U+FEFF), and a crafted VORBIS_COMMENT vendor length no longer wraps around in the 32-bit programs. An LAE WAV file packed by `flac --keep-foreign-metadata` is reported as such, with `lae_repair` named; LAE2 blocks with too short a payload are reported as a damaged file.
- **Player plug-ins:** foobar2000 `foo_input_lae` 0.31, VLC 0.1.12, Kodi 0.2.13 and DirectShow 0.2.12, built from these sources. They contain the FLAC reader of this update: a file re-encoded with inter-channel decorrelation is still refused, and the message now names `lae_repair`. Playback of valid files is unchanged.
- **Documentation.** `lae_udec` exit codes 0–4 (README Section 14.3); the FLAC form sections of both specifications have an informative paragraph on files changed by other programs. README Sections 12.6 and 17.3 name the ABX tool and the ABX report that have been on the release page of v5-2026-11-07 since 2026-11-13; listening tests with a panel of listeners remain future work.
- **Not tested in this update:** the end-to-end repair of an LAES file with a 32-bit core master (the decoding of inter-channel decorrelation was tested at 16, 24 and 32 bit); the plug-ins were built and loaded, but not played again in their host players; MusicBrainz matching in Picard (only manual tag edits were tested); Jellyfin.

## 2026-11-14 (v5-2026-11-14)

- **Fix in `lae_enc plan`: a passage could be written at twice its level (2026-11-14).** The planners of v5-2026-10-24, v5-2026-10-30 and v5-2026-11-07 are affected. The error occurs when the publisher's CD has digital silence between two tracks, the master joins the two pieces directly, and the positions carry a fraction of a sample. The two pieces then belong to different groups whose ranges in the master overlap by the length of that silence, and each group moved the whole overlap. When both fractions are not zero, the overlap was added twice: in `TRACKnn.master.wav`, a passage as long as that silence, at the start of the later piece or at the end of the earlier one, is 6.02 dB too loud. When only one fraction is not zero, that passage may have been moved by the fraction of the other piece (an error of less than one master sample). The planner reported no error. Each group now moves only the master samples that its own pieces take. Slices made with an earlier build under this condition must be made again with this release, and the files encoded from them must be encoded again. Not affected: plans in which no silence of the CD separates pieces that the master joins, plans whose positions are all whole numbers, and plans made with the option `laes` (nothing is moved). For plans without such an overlap and for plans with `laes`, the slices are byte-identical to those of the earlier build.
- **LAEY FLAC form (2026-11-10).** An LAEY file may be a FLAC file: the CD layer is the FLAC audio, coded with independent channels, and the extension, byte for byte the body of the `LAE ` chunk, is carried in APPLICATION blocks with the ID `LAE1` before the audio ([spec/lossy/SPEC.md](spec/lossy/SPEC.md), Section 3.4). `lae_enc` writes it for a `.flac` output name or with `--form=flac`; `.lae` and `.wav` names give the WAV form, byte-identical to the previous encoder. `lae_dec`, `liblae`, the plug-ins and the Java library read both forms and tell them apart by content. The container version stays 5.
- **LAEY extension packs (2026-11-11 and 11-12).** One file may hold, besides the main body, up to eight further extensions, each made from its own master against the same CD layer and mutually independent (Section 5.6). Encoder options `master2=` … `master9=` with `master<n>align=`, `master<n>name=`, `master<n>accept`, `master<n>edge=`; `master1name=` names the main body (optional `LNAM` record). A decoder without pack support plays the main body; `lae_dec pack=<n>` and `liblae` (`lae_select_pack`) decode another pack, chosen by the user only. Without these options, the output is byte-identical to the previous encoder. Test: `tests/check_packs.sh`.
- **LAES payload version 2 (2026-11-11 and 11-12).** Up to 255 masters; per master a name, a menu position and a flag that turns its derived 16-bit versions off; in the header, a default master and a flag for the core master ([spec/lossless/SPEC.md](spec/lossless/SPEC.md), Sections 6.1, 6.2 and 10). A payload that fits version 1 is still written as version 1, byte for byte as before. Derived 16-bit versions are now one per source (the core master, each master and each of its rate levels) and can start at any frame, bit for bit (GF(2) matrix power). Section 10.3 sets out selection and display; Section 1.3 requires that a master plays only after it and its chain of tiers have matched their SHA-256 values. With more than 16 masters, the encoder ranks candidates on four short excerpts.
- **`laes` (2026-11-11 and 11-12).** `--name=`, `--no-derive`, `--default=` and `--menu-order=` for `laes enc`; new commands `laes names` (names, menu order, default and derived-version flags without re-encoding; `--in-place` for the default and the core master flag), `laes add` (new masters added, the old parts byte for byte unchanged) and `laes extract` (chosen masters packed into a new file); `laes dec --pick=` takes ranges, `master:<k>` and `name:<text>`, and shows a tick list when several masters match ([spec/lossless/LAES_TOOL.md](spec/lossless/LAES_TOOL.md)).
- **Unified decoder `lae_udec` and `liblae` API 9 (2026-11-10 to 11-12).** `lae_any_*` and `lae_probe_*` (API 6, `src/lae_any.cpp`) open LAEY and LAES files through one interface and decide the format by content; API 7 adds the LAES master names and order, API 8 the LAEY extension packs, API 9 the default master. `lae_udec info | dec | verify` lists every choice of a file with the checks of its format and decodes any choice (README Section 13.6). `lae_dec`, `lae_udec` and the Java `LaeDec` label route 16 (no bit-depth layer) as 16-bit; its samples are still delivered in 24-bit words, unchanged.
- **Player plug-ins.** foobar2000 `foo_input_lae` 0.30, VLC 0.1.11, Kodi 0.2.12 and DirectShow 0.2.11: LAEY FLAC form and extension packs (listed by their stored names, each with its own layers), LAES master names, menu order and the default master of the file, derived versions that start at the seek position, and faster playback of long chains of tiers. Each plug-in is built in two standalone versions, English (`-en`) and Chinese (`-zh`). The tier without the bit-depth layer is labelled 16-bit. The plug-ins of v5-2026-11-07 (0.20, 0.1.4, 0.2.5, 0.2.4) are withdrawn.
- **Java library.** Ports of the LAEY FLAC form, extension packs and `master1name=`, and of LAES payload version 2 (names, menu order, default master, no-derive flags), byte-identical with the C++ tools. Java only: `lae_dup()` (a second track over an opened file) and a limit of 16 masters for phones: the player front end `Play` offers the masters of a file with at most 16 masters (a file with more plays its core master), and the Java LAES encoder refuses more than 16 masters before it starts; files with up to 255 masters are made with the PC tool `laes` ([java/README.md](java/README.md)).
- **`lae_noise scan` about 10 times faster (2026-11-13),** with byte-identical output.
- **ABX test tool `lae_abx` (2026-11-12, `tools/lae_abx/`).** A stand-alone ABX listening test for Windows that decodes both sources with `lae_udec` (or an MP3 with Windows Media Foundation), brings them to the same rate and writes a log with the SHA-256 of both files. When one source is an MP3 or AAC file, the tool measures the delay between the two sources, lines them up and shows a delay other than 0; LAE decodes and WAV files start together and are not shifted.
- **`lae_cdx` reads FLAC rips and FLAC base-layer files (2026-11-14).** `lae_cdx read` and `lae_cdx repack` accept 16-bit, 44.1 kHz, 2-channel FLAC files, which they recognise by content (in a CUE sheet under `FILE "…" WAVE`, as ripping programs write it). The reader checks the CRC-8 and CRC-16 of every frame, the sample count, and the audio MD5 of the STREAMINFO block when it is set; a FLAC file that fails a check or has another format stops the reading, and nothing is written. The output is byte-identical to that from the same rip stored as WAV (single file, whole-disc and per-track rips, with and without a CUE sheet; x64, x86 and Linux). `lae_enc plan` and `lae_cdx image … layout=` read a publisher's CUE sheet with FLAC audio through the same code. Limits: a 48 kHz FLAC base-layer file is refused (a 48 kHz WAV is read); `liblae` and the plug-ins pair a WAV base-layer file only. Tested with files written by `flac` 1.3.3 and by the test tool, not with rips of a real disc. The `README.TXT` written to the disc is unchanged.
- **Corrections after a code review (2026-11-14).** For valid files and normal input, nothing changes: the decoded samples and the encoded files are byte-identical to those of the builds before these corrections.
  - **Failed writes are reported.** `lae_dec`, `laes dec` and `laes_export` check every write. When an output cannot be written completely (for example, on a full disk), it is removed, and the tool states the reason and ends with an error code; previously, a truncated file was left and success was reported. `laes dec` first compares the size of its output with the free disk space (`--force-disk` writes anyway). `laes_export` writes each output as `<out>.part` and gives it its name only when it is complete.
  - **`laes_export` gives out a part of a master only after that master has matched its SHA-256** ([spec/lossless/SPEC.md](spec/lossless/SPEC.md), Section 1.3). A run over the whole track is the check and, with `--index=<record>`, notes it in `<record>.verified`. Without that note, the export of a part or of pieces of a part is refused; `--unverified` exports without the check and adds the tag `LAES_UNVERIFIED=1`. Previously, only a whole-track export was checked.
  - **`lae_enc`, `resume` with `cd=`.** The result of the alignment is part of what a resumed run compares. A resumed run whose alignment ends differently (for example, with the other answer to the pad or cut question at the console) starts from the beginning; previously, it could continue with the steps of the other choice and write a file different from that of an uninterrupted run.
  - **`lae_enc` with an independent CD.** The difference between the master and the CD layer is stored in 24-bit values. Values outside that range were limited without notice, and the file then did not decode to the master (for example, with a CD of inverted polarity and a master with peaks above −6 dBFS). The encoder now stops with exit code 2 and writes no file.
  - **`lae_enc`, RF64 and W64 input.** A header that gives a data length of 2^63 bytes or more is refused; previously, reading such a file did not end.
  - **`lae_enc plan`.** On long albums, the planner could refuse the positions that it had itself suggested for the pieces of one group, because a position beyond about 2^29 master samples (25 minutes at 352.8 kHz) lost part of its fraction; the whole part and the fraction are now printed and read separately. The planner states its memory need at the start and holds the joined master three times instead of four (README Section 17.2).
  - **`lae_cdx read` and `lae_cdx repack`** hand every accepted track to the decoder before they write it ([spec/cdx/SPEC.md](spec/cdx/SPEC.md), [R8]), and compare the sample rate and the length in the trailer with the extension block. An extension file with a correct CRC whose content the decoder refuses is no longer reported as verified; the track is written as CD.
  - **Decoders and `liblae`.** The fields K and rate of a stream record header no longer determine how much the decoder stores or how far its one-time check runs; a crafted file of a few kilobytes could exhaust the memory. The 32-bit programs refuse an LAES file whose core master is too long for them; previously, they crashed. A read failure while the block positions of an LAES file are computed is reported as a read failure; previously, it ended the player's process.
  - **`laes` on Linux.** `laes dec` joined the output folder and the file names with a backslash, so `dec` wrote its files beside the folder, and `extract` and `add` failed; they now work, and their output is byte-identical to that of the Windows build (tested on Linux x64). `laes align` and `<master>@auto` refuse, with the reason, a path that the system's shell would change.
  - **DirectShow and VLC plug-ins.** With the highest sample rate set to the base rate (44.1 or 48 kHz), "All layers" plays the CD layer (the core layer in the 48 kHz family) with the bit-depth layer at 24 bit; previously, it played that layer alone at 16 bit.
  - **Noise panel.** A preview is offered only for the row whose rectangle it was made for (matched through `preview_check.txt`); previously, previews were matched by row number, and a row could play the preview of another region.
  - **Java library.** A resumed LAEY encode whose options contain non-ASCII text no longer starts from the beginning; the final rename of the LAEY encoder no longer deletes the existing output when it fails; `LaesTool dec` writes a WAV file beyond 4 GiB in the RF64 form.
- **Changes decided on 2026-11-15.** For normal input, the encoded files and the decoded samples stay byte-identical, except where a point below says otherwise.
  - **Existing output files.** No tool replaces an existing output file silently: at a console it asks twice and replaces the file only after two answers `yes`; without a console it refuses (exit code 1) unless `--overwrite` is given. This holds for `lae_enc`, `lae_dec`, `lae_udec dec`, `lae_cdx`, `laes`, `laes_export`, `lae_noise`, the table generators, `lae_abx --prepare`, `laes_disc.ps1` and the Java tools; a resumed run is not a replacement (README, Section 14.11).
  - **Test switches.** The environment variables that only tests use act only with the master switch `LAE_TEST_MODE=1`, which also prints the active switches (README, Section 14.11).
  - **LAEY decoding.** `lae_dec` and the Java decoder refuse a file whose entropy streams do not each end exactly at their last byte, as the player library does; `lae_dec` keeps no seek checkpoints; `lae_udec verify` decodes every extension pack to its end, one line per pack; the player's record (version 2) keeps the result of every pack checked. The specification states the 64-bit wrap of the signal stages after the IMDCT ([spec/lossy/SPEC.md](spec/lossy/SPEC.md), Section 10.4).
  - **`lae_cdx`.** `pack`, `split`, `image`, `read` and `repack` decode every LAEY extension pack and every LAES master completely before they write anything (the reader decodes an LAES block that does not fit into memory with the stream decoder and temporary files); `read` requires a new or empty output folder; `data` and `read iso:…@N` check the start address against A2 + 11400 (`a2=` or `BURN.TXT`). README.TXT and BURN.TXT say to rip as WAV or FLAC, 16 bit, 44.1 kHz, so the `DATA.ISO` of a new image differs from that of an earlier build (audio, CUE and `.LAX` files are unchanged). When the whole disc needs more than 359,849 sectors (a standard 80-minute blank; possible only with `capacity=` above it), `image` writes a warning in English and Chinese at the top of `BURN.TXT` with the sectors needed and the excess, and prints one line; previously, such a warning had to be added by hand.
  - **`lae_enc tag`** runs the decoder's open check first and refuses a damaged file; blocks and chunks of other software are kept, as before.
  - **`laes`.** The time position T of a master must lie within the length of the track (both decoders refuse a larger one); a T just above a whole number no longer carries to the next sample (the file of such a T changes); a master stored in full keeps its own T in its record (the record bytes of such a file change; decoders do not use them); FLAC files with ID3v2 tags in front or APEv2 / ID3v1 tags behind are accepted and keep their tags; `verify` and `dec` name masters by their menu numbers; a check block where only the master is silent counts as not matching; `dec --list` reads the records only; `add` reads an RF64 core master, and `add` and `extract` estimate the disk space they need; a work folder that cannot be made next to the file goes to the system temp folder; a work folder in use stops a second run; tags that exceed the block limits are refused. `laes_export` writes the INFO tags of a WAV-form file under their Vorbis names.
  - **`lae_noise`.** Previews are limited in the program: 10 s of a position by default, at most 30 s, with at most 1 s of context; `preview-all` at most 30 s.
  - **`laes_disc.ps1`** takes files above 2 GiB on a DVD (UDF 1.02 only). **`lae_abx` 1.1:** kept copies keyed by program version, a button to clear them, the log written from the start of each pair, the rate of every pair in the summary.
  - **Plug-ins.** DirectShow and VLC: the first check of the masters can be cancelled; every player says when the core master plays instead of the chosen master. foobar2000: with a maximum sample rate below twice the base rate, "All layers" plays the CD/core layer with the bit-depth layer (24-bit), as in DirectShow and VLC; the other masters are kept ready only as far as memory allows, and the status bar says when fewer are ready. Kodi and VLC: log lines count extension packs as the menus do ("Pack n"). VLC: the module reports LGPL-3.0-or-later.
  - **Sources and licences.** The four reference modules moved to `tools/reference/`. The Java files `WavIo`, `LaeDec` and `NoiseMarks`, which only the encoder uses, carry the encoder licence. The Java library follows the changes of the tools it has, with byte-identical results.
- **Test tools.** `src/cd_variant.cpp` (exactly defined CD variants for alignment tests), `tests/check_packs.sh` and `tests/check_flacform.sh`; `src/gen_suite.cpp` takes an optional category (same bytes).

## 2026-11-07 (v5-2026-11-07)

- **Test-signal generator platform-identical (2026-11-07).** `src/gen_suite.cpp` computes its signals with `src/lae_math.h` instead of the C library's `sin`, `cos`, `exp`, `pow` and `log`, so Windows, Linux and ARM64 generate byte-identical sources. The sweep and electronic sources changed; their 8 conformance vectors were regenerated (`tests/vectors/MANIFEST.md`), and the transients and imd vectors are unchanged. The encoder conformance check now fails on a different source instead of only reporting it. Decoders are unaffected.
- **Demonstration audio (release attachments).** Three works by the author (*Grand*, *Tolled*, *LAES demo*) as LAES files (`.flac`, `.wav`, `.lae`) and LAEY files, three CD-Extra disc images with CD-TEXT, and every master from which they were made, with the SHA-256 of every file and of the samples of every master, for independent verification (README Section 14.10). All rights in the audio are reserved: it is provided for listening and verification only ([AUDIO_LICENSE.md](AUDIO_LICENSE.md)).
- **Streaming export and data discs for LAES (2026-11-03, published now).** `laes_export` writes a plain FLAC file of one selection from an LAES file (the core master, a master or a rate level), whole or in pieces, for streaming services. `tools/laes_disc.ps1` creates a DVD or BD data-disc image of an album of LAES files (README Section 14.9).
- **Player plug-ins (2026-11-07).** foobar2000 `foo_input_lae` 0.20, VLC `liblae_plugin.dll` 0.1.4, Kodi `audiodecoder.lae` 0.2.5 and DirectShow `lae_source.ax` 0.2.4 play LAEY and LAES files (README Section 13.7). New in this release:
  - the VLC, Kodi and DirectShow plug-ins are published;
  - all four plug-ins read the tags of WAV files (RIFF `LIST/INFO` and `id3 `, `src/lae_wavtags.h`) and of FLAC files;
  - an LAES file is released as soon as it is no longer being played, so that it can be renamed or retagged;
  - the fast first seek of `liblae` is included.
- **Fast first seek in LAEY files (2026-11-06).** When a file is first opened, `liblae` checks the whole file once (the SHA-256 of the file and of every entropy stream up to its exact end, in parallel) and stores a checkpoint every 30 s in the player's own folder (`lae_index`, `LAE_INDEX_DIR`, at most 256 MB, least recently used entries deleted first), never in the media file. Later seeks start from the nearest checkpoint. The output after a seek is bit-identical to that of decoding from the start (README Sections 13.6 and 17.4).
- **Java library (2026-11-04 to 11-07).** A pure-Java port of the LAEY and LAES encoders and decoders, without native code, for Android (API 24 and later) and other Java platforms (`java/`). Its encoded files and decoded samples are byte-for-byte identical to those of the C++ tools of the same commit.
- **LAES (2026-11-05 to 11-07).**
  - Tags and pictures: `laes enc --tag=KEY=VALUE --cover=<image> --picture=<type>:<image>`, `.lae` file names with `--form=flac|wav`, `laes tag` (writes new tags without re-encoding; the audio and the payload are unchanged) and `laes parts`.
  - Specification Section 11.1 now specifies bit-exactly the derivation of a core master from a master (choice of source, level 0, TPDF dither with its seed, and the order of operations). Section 11.5 covers resources and resuming (FLAC masters, memory budget, disk checks, `--small-disk`, `--disk-stop`, `--disk-stop-remove`, `--force-disk`).
  - A resumed run now restores the per-master choices of an unfinished file order of the masters (Specification Section 11.2) and checks their files, instead of redoing that order. The output of an uninterrupted run is unchanged.
  - New documents: [spec/lossless/OVERVIEW.md](spec/lossless/OVERVIEW.md) (an introduction and a comparison with FLAC and WAV), [spec/lossless/LAES_TOOL.md](spec/lossless/LAES_TOOL.md) (the `laes` tool) and [spec/COMPATIBILITY.md](spec/COMPATIBILITY.md) (what plays without LAE software, the decoders tested and their limits).
- **LAES WAV files beyond 4 GiB (2026-11-04).** `laes enc` and `laes dec` write a WAV file whose RIFF size would exceed 4 GiB in the RF64 form (EBU Tech 3306), with a `ds64` chunk; `laes` and the LAES decoder of the plug-ins (`src/laes_play.h`) read RF64 and BW64 ([spec/lossless/SPEC.md](spec/lossless/SPEC.md), Section 7).
- **Project documents.** [SECURITY.md](SECURITY.md), [THIRD_PARTY.md](THIRD_PARTY.md) and [docs/VERSIONS.md](docs/VERSIONS.md). `AUTHORS` records the origin of every encoder file of the Java library.
- **CD-TEXT in CD-Extra images (2026-11-06).** `lae_cdx image` writes the album title, the performer and the track titles as `TITLE` / `PERFORMER` lines in its CUE files (CD-TEXT when burned). The text is taken from the files' tags, from the publisher's CUE, or from `album=`, `performer=`, `cdtitle=` and `cdperformer=`. Only printable ASCII is allowed: Chinese text stops the tool until the user gives the text in ASCII instead; `cdtext=off` writes no text. Without text, the image is byte-identical to that written before.
- **Tags and pictures in LAEY files (2026-11-06).** `lae_enc --tag=KEY=VALUE --cover=<image> --picture=<type>:<image>` writes a RIFF `LIST/INFO` chunk and an `id3 ` chunk (ID3v2.4, UTF-8) between `fmt ` and `data`, using the same code and layout as LAE Lossless. `lae_enc tag <in> <out> ...` writes new tags without re-encoding and checks that the CD audio and the `LAE ` chunk are byte-identical. Without these options, the output is unchanged.
- **Noise marking and `g` (2026-11-04 to 11-06).** Both features are new and optional; without them, the encoder writes byte-for-byte the same files as before ([docs/NOISE.md](docs/NOISE.md)).
  - **LAE Noise Marker** (`plugins/lae_noise_marker/`, `src/lae_noise.cpp`, `tools/noise_panel.html`, in Chinese and English): lists steady noise and fixed narrow lines lasting 10 s or more (`lae_noise scan`), displays them in an offline panel with short previews, and writes the user's selection to a marking file (`.laenoise`, format `LAENOISE 1`). It only identifies and marks: it never removes or exports sound. The scanner produces the same bytes on every platform (`tests/check_noise_linux.sh`).
  - **`lae_enc noise=<marks.laenoise>`**: in the frequency layer, the master is taken as zero in the marked cells, and fully marked cells are class A. The marking file must belong to the master (SHA-256, length, rate) and must be created after alignment. Marks in the CD band are listed and left unchanged.
  - **`g`, `g=b`, `g=c`, `g=d`**: the frequency layer is coded a second time to cancel G, the image components of the up-converted CD layer (about 120 dB below the music), in deleted cells and in cells where the master is silent. The encoder prints the size of the G streams. Measured on the demo master: marked cells at −104 dBFS per column with `noise=` alone, and at −126 / −139 / −156 dBFS per column with `g=b` / `g=c` / `g=d`, at a cost of +0.8 / +1.0 / +1.2 % of the whole file.
  - `spec/lossy/SPEC.md` Section 9.5: the segments of one plane MAY overlap in frequency. Decoders already summed such segments; existing decoders play such files without modification.
- **Encoder disk use (2026-11-05).** The work folder takes about 0.8 GB per minute at 352.8 kHz instead of about 1.3 GB (for a 10-minute master, 8.0 GB instead of 13.2 GB), because segment planes hold only their own bins and intermediate files are removed as soon as no later step needs them. The encoder prints the largest size that its work folder reached, and its free-space check uses the new figures. The output is byte-identical.
- **`resume` fixes (2026-11-05).** A resumed run now restores the dither flags (previously, the bit-depth layer could differ from that of an uninterrupted run), and a run stopped after writing `<output>.part` now writes that file again instead of refusing it. Verified by stopping the encoder after each of its steps: every resumed output is byte-identical to that of an uninterrupted run.
- **Windows file names (2026-11-05).** On Windows, `lae_enc`, `lae_dec` and `lae_noise` take their arguments and open files in UTF-8 (`src/win_utf8.h`), so Chinese and other non-ASCII file names work. Other platforms are unchanged.
- **Encoder bit-identical on every platform (2026-11-03).**
  - Everything that determines the encoder's output uses `src/lae_math.h` (`log2`, `sinpi`, `cospi`, `exp2`, `log10` from IEEE `+ − × ÷`, `sqrt`, `floor`, `frexp`, `ldexp` in a fixed order) instead of the C library. The FFT butterfly and the complex products of the alignment are written out explicitly, and equal tone frequencies are merged in the order in which they are found (`std::stable_sort`).
  - The x64 and x86 builds write byte-identical files; `tests/check_encoder_linux.sh` now fails if a file differs.
  - Previously, the C library's `log2` could differ in the last bit between platforms and change the choice between two partitions of equal cost; the decoded audio was the same. For this reason, 11 of the 16 conformance vectors were regenerated; all their decoded hashes are unchanged.
  - Build the encoder with `/fp:strict` (MSVC) or `-ffp-contract=off` (GCC / Clang). `tools/lae_math_check.cpp` measures these functions against the C library.
- **Independent CD shorter than the master (2026-11-03).** When the master has sound lasting one CD sample or more before the CD starts or after it ends, `lae_enc` with `cd=` no longer simply stops: it reports where and for how long, and lets the user choose. On a console, the encoder asks; in scripts, the option is required.
  - `cdedge=pad`: digital silence is added to the CD layer at those places, and the master is kept whole. The CD's samples are unchanged, but the CD layer is longer than the given CD.
  - `cdedge=cut`: the master is cut to the CD, and its sound beyond the CD is not kept. The CD layer is exactly the given CD.
  - Without a choice, nothing is encoded. The automatic search adds `edge=needed` to `ALIGN_SUGGEST`, and the alignment report records the choice. Encodes without such sound are byte-identical to those written before. Sound that only the CD has still stops the encoder, and the planner still stops on sound that only one side has.
- **IANA registration (2026-10-31).** The FLAC application IDs `LAE1` (0x4C414531, LAE Lossy extension data) and `LAE2` (0x4C414532, LAE Lossless extension data) are registered in the IANA FLAC Application Metadata Block IDs registry (<https://www.iana.org/assignments/flac>). `spec/lossless/SPEC.md` Section 8 records this.

## 2026-10-30 (v5-2026-10-30)

The container format and the decoding process of LAEY are unchanged. The encoder writes byte-identical files faster, block by block, and with much less memory.

### Format names

- The first 4 bytes of the `LAE ` chunk identify the format: `LAE1` = LAEY (container version 5, unchanged), `LAE2` = LAES, the lossless tier (with its own specification). `spec/lossy/SPEC.md` and `spec/cdx/SPEC.md` state this.
- `lae_dec` reports the format it finds. For `LAE2`, it reports that the file is an LAES file and names the tool to use, instead of printing the former "unknown LAE chunk version". Its success line ends with `format LAEY (extension LAE1)`.
- `lae_cdx` checks that an extension block of trailer type 1 begins with `LAE1` and that one of trailer type 2 begins with `LAE2` ("extension block does not match its trailer type"). Type 2 is now read and packed (see LAE Lossless below).

### Encoder

- **Faster, same output.** The encoder is about 3–4 times faster: a 60 s stereo master at 352.8 kHz (mode 3, s1) took 43 s instead of 177 s on a 32-thread Windows x64 machine. Every output file is byte-identical to that of the 2026-10-24 encoder for all modes and options, for an independent CD with whole and fractional placement, at 88.2 / 96 / 176.4 / 192 / 352.8 kHz, for 16 / 24 / 32-bit masters and for short tracks. This was also checked on Linux x64 and Linux ARM64 against the conformance vectors.
  - The partition search no longer allocates a list of cut positions for every rectangle (with many threads, these heap allocations waited on each other). It reuses its per-block tables, computes the mean D / C energies once per share search, and skips the stream and the planes while it searches the share.
  - The periodogram of the layer signal is computed once (it was computed three times, and one result was unused). Layer settings are read once instead of once per cell.
  - The filters of the route split are divided across threads, and the two layers of a pair (depth L and R, frequency M and S) can be coded at the same time.
- **Less memory, same output.** One reconstructed plane is kept instead of two identical copies; each octave segment holds only its own bins; signals are freed as soon as they are no longer needed; and the master's samples are freed once the CD layer has been derived. For 60 s at 352.8 kHz: 1.06 GB with the layers coded one after the other (formerly 1.83 GB), and 1.49 GB with two layers at once.
- **Memory plan and `mem=<GB>`.** At the start, the encoder reads the physical memory once, decides from the track length whether the two layers of each pair are coded at once or one after the other, prints the plan (`memory plan: …`) and follows it. Two layers are coded at once only on 64-bit builds, when `PARTTHREADS` is not 1, and when this fits both the memory limit and the available memory. By default, the limit is min(1/2, max(1/4, physical memory − 28 GB)) (8 GB on 32 GB, 20 GB on 48 GB, 32 GB on 64 GB); `mem=<GB>` sets it manually. The block length follows from the limit and is at most about 32 s (longer blocks are not faster). The output does not depend on the plan, the limit or the block length.
- **Block-wise encoder, same output.** The encoder reads the master and codes the track block by block (about 32 s per block). The track-wide decisions (floors, tone and transient marks, mean energies, code-length statistics, mode proportions) are first accumulated over all blocks; every block is then coded according to that plan. Every file is byte-identical to that of the whole-track encoder for all modes and options, all rates, short tracks, an independent CD with whole and fractional placement, and a 10.16-minute 352.8 kHz 32-bit master (peak memory 475 MB instead of about 12 GB).
  - Apart from small per-cell records (3 bytes per cell), memory no longer grows with the track length. Large intermediate data are written to the work folder on disk: at most 13.2 GB for a 10.16-minute 352.8 kHz master (about 1.3 GB per minute), and 38.6 GB for 10 minutes with `laes enc` and four masters. Both are removed at the end.
  - The Windows limit of about 12.7 minutes at 352.8 kHz no longer applies. The limit is now that of container version 5, about 94 minutes at 352.8 kHz.
  - Masters may be WAV, RF64 or Sony Wave64 files.
  - New option `resume`: an interrupted run (power cut, Ctrl+C, full disk) continues from its last completed step when it is started again with the same master, the same options and `resume`. The output is the same as that of an uninterrupted run. With `cd=`, the alignment is performed again and the steps after it continue.
  - With `cd=`, the master is also read through windows of its file, and its placement and fractional move are written from file to file. The alignment search itself is unchanged and works at the CD rate over the whole track: `cdcheck` of a 10-minute 352.8 kHz master against its CD needs 2.36 GB instead of 7.5 GB. Every alignment result, line and file is the same as before.
  - The decoder reads the CD audio of its input in place, without the former 32-bit copy of every sample.
- `PARTTHREADS=1` now makes the whole encoder single-threaded.
- **Fail-safety in threads.** An error in any worker thread (out of memory, a failed write) is passed to the main thread and handled exactly as before: no output and no `.part` file are written, and the temporary folder is removed (24 forced-failure cases, with the layers coded at once and one after the other).
- The opt-in `TIERSTAT` statistics are now counted per stream.

### LAE Lossless (LAES): first release

- **Specification.** [spec/lossless/SPEC.md](spec/lossless/SPEC.md) (English) and [spec/lossless/SPEC.zh.md](spec/lossless/SPEC.zh.md) (Chinese), payload version 1, draft for public review. It supersedes the design document of 2026-10-18.
- **What it stores.** A core master (16, 24 or 32 bit at 44.1 or 48 kHz) that ordinary players play (limits: [spec/COMPATIBILITY.md](spec/COMPATIBILITY.md)), and 1 to 16 independent masters (16, 24 or 32 bit, at the base rate × 1, 2, 4 or 8). Every master and each of its rate levels is reconstructed bit for bit and checked by SHA-256. A master is stored relative to the core master or to an earlier master (at the base level or at the highest level that both have), or in full. Masters are never moved; their time position is stored.
- **Carriers.** A WAV file (`LAE ` chunk, magic `LAE2`), a FLAC file (the core master in the audio frames, the payload in APPLICATION blocks with ID `LAE2`), or CD-Extra (extension block type 2).
- **Tool `laes`.** The tool `laes` (`src/laes_tool.cpp`) provides `enc` (WAV or FLAC form), `dec` with `--list` / `--pick` (every master and rate level and, on request, a derived master, marked as lossy), `verify`, `align` (the automatic search of `lae_enc`, with the position stored as T) and `selftest`.
- **Planner.** `lae_enc plan … laes` cuts an album master at the CD's track boundaries without moving it; each track has its own T.
- **`lae_cdx`.** `lae_cdx` packs and reads LAES extension files (type 2). Every master must decode and match its SHA-256.
- **Licences.** As for LAEY, the decoder side (`laes_math.h`, `laes_lift.h`, `laes_codec.h`, `laes_file.h`, `laes_flac.h`, `laes_stream.h`) is licensed under Apache-2.0, and the encoder side (`laes_codec_enc.h`, `laes_file_enc.h`, `laes_flac_enc.h`, `laes_stream_enc.h`, `laes_tool.cpp`, `laes_selftest.h`) under GPL-3.0-or-later or the commercial licence.
- **Tests.** `laes selftest` uses synthetic signals: round trips over both file carriers (WAV and FLAC), all word lengths, both rate families and all storage values, as well as damage and parser tests. Measured on 10-second excerpts, 16-, 24- and 32-bit masters of one mix, with all rate levels, take 99.6–99.8 % of the size of a FLAC file of the 32-bit master alone.
- **Memory.** `laes` works block by block (output byte-identical). A 10-minute track with four masters (32 / 24 / 16 bit at 352.8 kHz, 24 bit at 176.4 kHz) and an independent CD as the core master encodes with 2.9 GB peak memory and decodes with 2.2 GB (the whole-track version needed about 57 GB). The memory limit is set with `--mem=<GB>` (default min(1/2, max(1/4, physical memory − 28 GB))); a work folder holds the candidates. A WAV output larger than 4 GB is refused (use the FLAC form). The figures are given in [docs/BUILD.md](docs/BUILD.md), Section 5.
- **Not yet supported.** LAES playback in the foobar2000 component, files larger than 4 GB, and streaming decoding.

## 2026-10-24

This release changes the encoder and the tools, and adds a decoder library and a player component. The container format and the decoding process are unchanged.

### Format

- The container version remains 5. No field, block or semantics of the container has been added or changed.
- Decoders are unchanged. Every container version 5 decoder decodes files written by this encoder, including files aligned to a fraction of a master sample (see Encoder).
- Nothing about the alignment or the fractional move is stored in the file. A decoder never discards a sample.

### Encoder

- **Independent-CD alignment.** With `cd=<CD.wav>` (an independently produced CD as the CD layer), the encoder now aligns the master with the CD. The CD's samples are never changed.
- **Placement across digital silence only.**
  - The master is placed by whole master samples, and only across digital silence (all-zero samples).
  - The side whose sound starts earlier plays first, and the side whose sound ends later ends later. The shorter side is padded with digital silence, so neither side loses a non-silent sample.
  - If the master's sound starts less than one CD sample before the CD's, one CD frame of digital silence is put in front of the CD layer. The CD's own samples are unchanged.
- **Sound present on one side only.** If one side has sound that the other lacks, lasting one CD sample or more, the case is not supported and the encoder stops.
- **Waveform search.** The search uses a fixed coarse-to-fine cascade:
  - Level 0 works at 1/16 of the CD rate. It scans the whole allowed range by cross-correlation and then scans once more around the peak.
  - Each further level halves the blocks, doubles the rate and searches ±32, then ±8 level samples around the previous peak.
  - The last level compares whole master samples within two CD samples by waveform residual.
  - A peak on the edge of a level's range ends the search with "alignment failed".
- **Two searches.** The search runs twice:
  - once with a whitened cross-correlation (GCC-PHAT; C. H. Knapp and G. C. Carter, IEEE Trans. ASSP, 1976);
  - once with a plain, energy-normalised cross-correlation.
  
  Whitening is not better in every case, so both searches are run. The program does not compare the two results or choose between them.
- **Check blocks.** The alignment is checked on three blocks of the CD's sound:
  - Pieces of 40 s or more: 5 s blocks, one starting 10 s after the first sound, one ending 10 s before the last sound, and one centred.
  - Pieces of 3 to 40 s: the middle of each third, at most 5 s each.
  - Pieces under 3 s: one check over the whole sound.
- **Per-block measures.** For each block, the encoder computes:
  - the waveform residual after one least-squares gain per channel; a residual above -3 dB means that the block does not match;
  - the remaining time difference relative to the single fractional best fit of the piece.
- **Drift check.** Each block is also compared at two CD samples on either side of the offset, with the gains kept. A better fit there indicates a different speed, an edit or another take, and the encoder stops.
- **Bands of the remaining time difference.**

  | Remaining time difference | Behaviour |
  |---|---|
  | -40 dB or lower | Aligned. |
  | -40 to -33 dB | The encoder asks for confirmation. |
  | Above -33 dB | The encoder asks for confirmation and warns that the file may become much larger. |
  | No fit | The piece is never encoded. |

- **Channel swap or inverted polarity.** These require the user's confirmation (`cdaccept`), as in the −40 … −33 dB band; the extension codes them.
- **User confirmation of every position.**
  - Without `cdalign=`, the encoder performs both searches. For each, it measures the fraction, tries the placement with the move, and checks again exactly as for an offset given by hand.
  - It prints each result ("aligned at cdalign=<offset>", "not aligned within the limit at …", or "alignment failed …").
  - It then prints the line `ALIGN_SUGGEST whitened=<status>:<offset> plain=<status>:<offset>`.
  - Finally, it stops without writing anything: exit code 1, or 0 with `cdcheck`.
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
  - At hard-cut edges, where sound is present at the first or last master sample, digital silence is added at both ends before the move and removed again where it remains silent, so that what the move carries past the edge is kept. The edge error is then below −66 dBFS (−38.5 dBFS without the added silence).
  - Masters that start and end in digital silence are not affected: their files are byte-identical with and without this step.
- **Console line.** The encoder prints `PHASE t=<whole> phi=0 move=<move> offset=<offset>`. The field `phi` is always 0.
- **Digital silence around the move.**
  - Before a fractional move, 128 CD frames of digital silence are added at both ends of the placed master and of the CD layer.
  - After the move, only what is still digital silence is removed again: at most the added frames, and never a frame of the given CD. Material that the move carries past the master's first or last sample is therefore kept.
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
  | `cdaccept` | Continues after a remaining time difference above −40 dB; otherwise, a console asks. |

- **Thresholds.** The alignment thresholds are defined in `src/cd_align.cpp` (struct `Th`).
- **Fail-safe fixes.** Every file operation and every module result is checked. A full disk, a module error, running out of memory or a failed leak compensation stops the encoder with the reason; no output file is written, and the work folder is removed. Normal output is byte-identical.

### Planner

- **New command.** `lae_enc plan <master.wav> [<master2.wav> …] <publisher.cue> <out dir> [cdalign=<positions>] [cdaccept]` matches a whole master against a publisher's disc.
- **Inputs.**
  - The disc is read as CUE + BIN or as WAV.
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
  - Each piece is searched for in the master around its expected place, within ± max(60 s, piece length).
  - A piece that is digital silence on the CD is not searched for, and its master range must be digital silence.
  - Between pieces, only digital silence may differ.
- **Groups.**
  - Pieces whose master ranges touch at a CD track marker form a group. Such a marker is where the CD splits continuous master material.
  - A group has one whole offset and one fraction, measured as for one piece. Different fractions given for one group stop the planner.
  - Each group is moved from the joined master with its real neighbouring samples and then cut. Pieces that join without a gap are therefore joined in the output exactly as if the whole group had been moved once (tested bit-identical).
- **Spill without a target.** The move can carry samples to where no piece takes master samples: the very start or end of the master, or a gap present only in the master. The planner then:
  - reports where and how many samples are affected;
  - states that these samples are kept if 44 CD frames (about 1 ms) of digital silence is added there to both the master and the CD, with later track markers moved accordingly.
  
  The user decides.
- **Stop conditions.** The planner stops, reporting the time range, when:
  - the master has sound that the CD lacks;
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
- **Fast seek.** Seeking is bit-exact; it uses entropy checkpoints about every 4 s and a warm restart of every signal stage.

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

- **Conformance vectors.** The encoder writes the 16 container version 5 conformance vectors byte-identically, and all 64 outputs of the decoder are identical.
- **Measurement of the fraction.**
  - Test material: CDs made from the master delayed by 0.1 / 0.25 / 0.5 master samples (Kaiser-windowed sinc), at 352.8 / 176.4 / 88.2 kHz, on 2 tracks (18 cases).
  - Measurement error: within ±2.3e-5 master samples; in time, at most 108 ps.
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
- **Linux ARM64.** Added to the verified builds. On aarch64, the decoder (GCC 13 at `-O2` and `-O0`, Clang 18 at `-O2`) gave 192 of 192 results identical to the reference, and the encoder (GCC 13, `-O2`) reproduces all 16 conformance vector files byte for byte.
- **Test suites.** The suites `plan1` and `align1` follow the confirm-then-move flow.
- **liblae end-to-end tests.** Command-line end-to-end tests were added for `liblae`.

### Documentation

- **CD-Extra encapsulation.** Specification: `spec/cdx/SPEC.md` (draft, version 0).
- **Player component.** `plugins/foo_input_lae/README.md` was added.
- **Usage.** README Section 14 describes the alignment options, the planner and the CD-Extra commands. The CD-Extra trailer counts lead0 / tail0 describe content of the CD layer, which is always played and never removed (`spec/cdx/SPEC.md`).
- **README.** Updated for independent-CD alignment, the planner, CD-Extra packaging and reading, the decoder library and the player component. The lossy specification (`spec/lossy/SPEC.md`) is unchanged: container version 5.

### Known limits

- **Search failures on short or narrowband material.** The search can fail, and the encoder then stops, on:
  - a 2 s excerpt of one sustained tone;
  - a 2–3 s passage dominated by one low tone;
  - content entirely above about 1.2 kHz, which lies outside the level-0 band.
  
  The user then gives the position by hand.
- **Periodic signals.** Strictly periodic synthetic signals (tones, saw waves, loops) surrounded by digital silence can be matched one whole period away. For this reason, every position is confirmed by the user.
- **Samples carried past an edge.** In the planner, samples that the move carries to where no piece takes master samples (the very start or end of the master, or a gap that only the master has) are not kept. The planner reports where and how many, and states that adding 1 ms of digital silence there to both the master and the CD keeps them.
- **Real CD / master pairs.** No real independent CD / master pairs from the market have been tested. Tests used only CDs derived from the master with EQ, limiting, gain, delay, noise reduction, gating, edits and drift.
- **Not yet done:**
  - a CD-Extra burn test;
  - measurement of encoder speed and memory. The encoder holds a whole piece in memory, and a whole-disc plan holds the whole master.

## 2026-10-18

- Initial public release (container version 5).
