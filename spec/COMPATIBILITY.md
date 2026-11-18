# Playing LAE files without LAE software

**English** | [中文](COMPATIBILITY.zh.md)

For what LAES is, and for its advantages and limits, see [lossless/OVERVIEW.md](lossless/OVERVIEW.md).

An LAE file is an ordinary WAV or FLAC file. A player without LAE support plays the **CD layer** of an LAEY file and the **core master** of an LAES file, and skips the rest. An LAES core master at 16 bit and 44.1 / 48 kHz can be CD-compatible; high-resolution files and FLAC-form files cannot be read by a CD player. This page lists what was tested, where a player without LAE support can fail, and why. **None of these failures comes from the LAE data**: each is a limitation of the decoder concerned, and it appears in the same way with a plain WAV or FLAC file of the same kind. To play the high-resolution layers or the lossless masters, use an LAE plug-in or an LAE player.

## What a player without LAE support sees

| File | Container | Played without LAE (core master, or CD layer for LAEY) | What it skips |
|---|---|---|---|
| LAEY (`.wav`) | RIFF WAVE | the `data` chunk: 16-bit PCM, 44.1 or 48 kHz (the CD layer) | the `"LAE "` chunk (depth and frequency layers) |
| LAEY, FLAC form (`.flac`) | FLAC | the audio frames: 16-bit, 44.1 or 48 kHz (the CD layer) | the `LAE1` APPLICATION blocks (depth and frequency layers, extension packs), which come before the audio frames. Checked 2026-11-10 with GStreamer `flacdec`: the output equals the CD layer of the WAV form. Windows' own FLAC decoder: limitation 1 applies once the extension exceeds about 4 MiB (after a few tens of seconds of music) |
| LAES, WAV form (`.wav`) | RIFF WAVE, or RF64 above 4 GiB | the `data` chunk: the core master (16 bit by default) | the `"LAE "` chunk (the masters) |
| LAES, FLAC form (`.flac`) | FLAC | the audio frames: the core master (up to 24 bit by default) | the `LAE2` APPLICATION blocks (the masters), which come before the audio frames |

## Tested (2026-11-05)

The test files were the largest demo files, together with short excerpts:
- `Grand_LAES.flac`: 938 MB, of which 888 MB is metadata before the first audio frame; a 16-bit core master (the independent CD), 26,667,616 frames = 604.7 s (10:04.7; Kodi shows 10:04); its masters are 609.3 s long (10:09; the DirectShow row below plays a master);
- `Grand_LAES.wav`: 995 MB;
- an LAEY file: 241 MB; 13:22;
- 10-second LAES excerpts with a 24-bit or a 16-bit core master, in RIFF, RF64 and FLAC.

Every result marked "plays" in the table below was compared byte for byte with the reference decode. The software record states its own comparisons.

| Decoder | LAEY WAV | LAES WAV (RIFF) | LAES WAV (RF64) | LAES FLAC, 16/24-bit core master | Plain 32-bit FLAC (for comparison) |
|---|---|---|---|---|---|
| libFLAC 1.3.3 (the reference `flac` command) | — | — | — | plays | — |
| FFmpeg (2025 and 2026 builds) | plays | plays | plays | plays | — |
| Android 13 MediaExtractor / MediaCodec (Sony Xperia) | plays | plays | **does not open** | plays | **not recognised** |
| Windows 11 Media Foundation (Windows' own players, Explorer) | — | — | — | **does not open** (see below) | **opens, does not decode** |

### Software record

This table lists applications tested as a user runs them, without LAE software unless stated otherwise. "Plays" means that the length is shown, playback starts and seeking works. Applications not listed have not been tested.

| Application (version) | Date | File | Result |
|---|---|---|---|
| Kodi 21.3 (Windows x64; built-in decoder: FFmpeg) | 2026-11-05 | `Grand_LAES.flac` (LAES FLAC, 938 MB) | plays the core master: 10:04, 44.1 kHz; seeking works |
| Kodi 21.3 | 2026-11-05 | `Grand_LAES.wav` (LAES WAV, 995 MB) | plays the core master: 10:04, 44.1 kHz; seeking works |
| Kodi 21.3 | 2026-11-05 | `03_LAES_demo.lae.wav` (the work *LAES demo* as an LAEY file, 241 MB) | plays the CD layer: 13:22, 44.1 kHz; seeking works |
| Kodi 21.3 with the LAE add-on 0.2.2 | 2026-11-05 | an LAES `.lae` excerpt | plays the master: 352.8 kHz; seeking works |
| foobar2000 2.26.2 (64-bit) and 1.5.1 (32-bit) | 2026-11-05 | `Grand_LAES.flac`, `Grand_LAES.wav`, `03_LAES_demo.lae.wav` | opens all three and reads them as FLAC / PCM, 44.1 kHz: the core master (LAES) or the CD layer (LAEY). The files were added to a playlist but not played in foobar2000 (no screen access during the test) |
| foobar2000 2.26.2 (64-bit) and 1.5.1 (32-bit) with the LAE component 0.16 | 2026-11-05 | `Grand_LAES.flac`; an LAES FLAC excerpt | recognised as LAES: the list of masters and the master to be played (352.8 kHz) are read. Not played (as above); the component uses the same decoders as the other LAE plug-ins |
| VLC 3.0.24 (64-bit, portable) | 2026-11-05 | `Grand_LAES.flac` | plays the core master; decoded through VLC to a file, byte-identical to the reference |
| VLC 3.0.24 | 2026-11-05 | `Grand_LAES.wav`, `03_LAES_demo.lae.wav` | plays the core master (LAES) and the CD layer (LAEY). Decoded through VLC to a file: identical to the reference, except that the last 346 frames (8 ms) are missing. A plain WAV file loses the same frames in VLC; the cause is VLC's own WAV path, not the LAE data |
| VLC 3.0.24 with the LAE module 0.1.1 | 2026-11-05 | an LAES FLAC excerpt | plays the master `master_2_352800_32`; decoded through VLC to a file, byte-identical to the master except for the last 40 ms (VLC's end-of-stream handling, the same cause as above) |
| Windows 11 (build 26200) Media Foundation: Media Player, Movies & TV, Explorer | 2026-11-05 | `Grand_LAES.flac`, LAES FLAC excerpts | not recognised (limitation 1). A plain 24-bit FLAC file plays; a plain 32-bit FLAC file opens but does not decode |
| Windows 11 Explorer | 2026-11-05 | `Grand_LAES.flac` | length, bit rate and title are empty (limitation 1) |
| Android 13 system decoder (Sony Xperia XQ-BC72), as used by apps that pass files to the system | 2026-11-05 | the files in the table above | see the table above |
| DirectShow players (PotPlayer, MPC-BE, MPC-HC) with the LAE filter 0.2.1 | 2026-11-05 | `Grand_LAES.flac`; `03_LAES_demo.lae.wav` | master: 352.8 kHz, 609 s; seeking works. On the LAEY file, seeking works, but playback resumes about 4 s after the seek point, as in 0.2.0 (tested through the filter itself, without a player window) |

Other players built on libFLAC or FFmpeg, for example mpv and most apps, are expected to behave like those libraries. Only the applications in the software record were tested as applications.

## Known limitations of other decoders

1. **Windows' built-in FLAC decoder and LAES FLAC.**
   - Windows (Media Foundation, which is used by the Media Player and Movies & TV apps, and by Explorer for length and tags) looks for the first audio frame of a FLAC file only within the first 4 MiB of the file. Measured: a first audio frame at byte offset 4,194,308 (4 MiB + 4) or earlier is recognised; from 4,194,309 on, it is not.
   - An LAES FLAC file that contains masters stores them before the audio frames, as RFC 9639 requires for all metadata. Its first frame therefore starts beyond this limit, and Windows does not recognise the file.
   - This decoder does not decode 32-bit FLAC either. An LAES FLAC file is nevertheless a valid FLAC file: libFLAC and FFmpeg play it (tested with 16- and 24-bit core masters; see the table).
   - **Use instead:** the WAV form, a player built on libFLAC or FFmpeg, or the LAE plug-ins.
2. **32-bit FLAC.**
   - Many decoders do not play 32-bit FLAC. Android 13's decoder does not recognise it, and Windows' decoder opens it but does not decode it.
   - The LAES reference encoder therefore writes, by default, a 16-bit core master in the WAV form and a core master of at most 24 bit in the FLAC form; `--core-bits=32` gives a 32-bit one. For the widest compatibility, do not use `--core-bits=32`.
3. **RF64 (LAES WAV form above 4 GiB).**
   - A RIFF WAV file cannot exceed 4 GiB, so a larger LAES WAV file, such as a long album with many masters, is written as RF64.
   - Many WAV readers do not support RF64. Android 13's decoder does not open it.
   - **Use instead:** the FLAC form for such files. LAEY files are always plain RIFF.
4. **The `.lae` extension.**
   - Players that choose a decoder by file name, such as foobar2000, Windows Media Player and many phone apps, do not open `.lae` files without the LAE plug-in.
   - Players that examine the content play the core master (the CD layer for LAEY). To play the core master in most players, rename the file to `.wav` or `.flac`, according to its form (subject to limitations 1–3).
5. **Network streaming of the file itself.**
   - In the FLAC form, the audio frames of the core master come after the data of the other masters. A player that reads the file over the network from the start must load all of the masters before the first sound.
   - For streaming, serve plain FLAC made with `laes_export` (one version: the core master, a master or a rate level, that is, a master brought down to a lower rate by the LAES lifting).
6. **Seeking.**
   - LAES FLAC files carry no SEEKTABLE, by design. Players seek by searching the frames; this works, but can be slower in some players.
   - Seeking in WAV files is unaffected.
7. **Tags in WAV.**
   - LAES writes the same tags twice, as RIFF LIST/INFO and as ID3v2.4 (`id3 ` chunk), both in UTF-8.
   - Most players read the ID3 tags. Old Windows programs that read INFO in the system code page may display non-ASCII text as garbled characters.
   - FLAC tags are Vorbis comments in UTF-8, as FLAC requires.

## Files changed by other programs

Some programs rewrite a file without knowing LAE. Tested with `flac` 1.5.0 and 1.3.3 (2026-11-18):

| Operation | What is left | Repair |
|---|---|---|
| `flac` re-encodes an LAE FLAC file (1.5.0 default and `-8`, 1.3.3) | extension and MD5 kept; the frames use inter-channel decorrelation: refused by the LAE tools | `lae_repair in.flac out.flac` |
| `flac --keep-foreign-metadata` on an LAE WAV file | the `LAE ` chunk in APPLICATION `riff` blocks: plain FLAC audio to the LAE tools | `lae_repair in.flac out.wav`, or `flac -d --keep-foreign-metadata` |
| the same with an `LAE ` chunk above 16 MiB | `flac` 1.5.0 stops (`foreign metadata chunk is too large`), nothing written | — |
| `flac` on an LAE WAV file without that option | only the CD layer / core master, as plain FLAC | not possible (the LAE data is gone) |

`lae_repair` checks the audio against the MD5 before it writes and checks the result as `lae_udec verify` does; when a check fails, nothing is written ([lossless/LAES_TOOL.md](lossless/LAES_TOOL.md), "Repairing a file").


## If a player does not play an LAE file

- **The name ends in `.lae`:** rename the file to `.wav` or `.flac` (according to its form), or install the LAE plug-in.
- **It is a FLAC file and the player is Windows' own:** use the WAV form, another player or the LAE plug-in (limitation 1).
- **It is a FLAC file with a 32-bit core master:** encode the core master at 24 bit (limitation 2).
- **It is a WAV file larger than 4 GiB:** it is RF64. Use the FLAC form (limitation 3).
- **An LAE tool reports inter-channel decorrelation, or "riff" blocks:** the file was re-encoded or packed by `flac`; repair it with `lae_repair` (see "Files changed by other programs").
- **None of these:** check the file with `laes verify` or `laes parts`, and try FFmpeg (`ffmpeg -i file -f null -`). If FFmpeg plays it, the problem lies in that player.
