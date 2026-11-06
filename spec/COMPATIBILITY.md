# Playing LAE files without LAE software

**English** | [中文](COMPATIBILITY.zh.md)

What LAES is, its advantages and limits: [lossless/OVERVIEW.md](lossless/OVERVIEW.md).

An LAE file is an ordinary WAV or FLAC file. A player without LAE support plays the **CD layer** of an LAEY file and the **core master** of an LAES file, and skips the rest. An LAES core master at 16 bit and 44.1 / 48 kHz can be CD-compatible; hi-res and FLAC-form files cannot be read by a CD player. This page lists what was tested, where a player without LAE support can fail, and why. **None of these failures comes from the LAE data**. Each one is a limitation of that decoder, and it shows the same way with a plain WAV or FLAC file of the same kind. To play the high-resolution layers or the lossless masters, use an LAE plugin or player.

## What a player without LAE support sees

| File | Container | Core master played without LAE | What it skips |
|---|---|---|---|
| LAEY (`.wav`) | RIFF WAVE | the `data` chunk: 16-bit PCM, 44.1 or 48 kHz (the CD layer) | the `"LAE "` chunk (depth and frequency layers) |
| LAES, WAV form (`.wav`) | RIFF WAVE, or RF64 above 4 GiB | the `data` chunk: the core master (16 bit by default) | the `"LAE "` chunk (the masters) |
| LAES, FLAC form (`.flac`) | FLAC | the audio frames: the core master (up to 24 bit by default) | the `LAE2` APPLICATION blocks (the masters), which come before the audio frames |

## Tested (2026-11-05)

The test files were the largest demo files, plus short excerpts:
- `Grand_LAES.flac`: 938 MB; 888 MB of metadata before the first audio frame; a 16-bit core master, 10:05;
- `Grand_LAES.wav`: 995 MB;
- an LAEY file of 241 MB, 13:22;
- 10-second LAES excerpts with a 24-bit or 16-bit core master, in RIFF, RF64 and FLAC.

Every result marked "plays" was compared byte for byte with the reference decode.

| Decoder | LAEY WAV | LAES WAV (RIFF) | LAES WAV (RF64) | LAES FLAC, 16/24-bit core master | Plain 32-bit FLAC (for comparison) |
|---|---|---|---|---|---|
| libFLAC 1.3.3 (the reference `flac` command) | — | — | — | plays | — |
| FFmpeg (2025 and 2026 builds) | plays | plays | plays | plays | — |
| Android 13 MediaExtractor / MediaCodec (Sony Xperia) | plays | plays | **does not open** | plays | **not recognised** |
| Windows 11 Media Foundation (Windows' own players, Explorer) | — | — | — | **does not open** (see below) | **opens, does not decode** |

### Software record

The table lists applications tested as a user runs them, without LAE software unless stated. "Plays" means the length is shown, playback starts, and seeking works. Applications not listed have not been tested.

| Application (version) | Date | File | Result |
|---|---|---|---|
| Kodi 21.3 (Windows x64; its own decoder: FFmpeg) | 2026-11-05 | `Grand_LAES.flac` (LAES FLAC, 938 MB) | plays the core master: 10:04, 44.1 kHz; seek works |
| Kodi 21.3 | 2026-11-05 | `Grand_LAES.wav` (LAES WAV, 995 MB) | plays the core master: 10:04, 44.1 kHz; seek works |
| Kodi 21.3 | 2026-11-05 | `03_LAES_demo.lae.wav` (LAEY, 241 MB) | plays the CD layer: 13:22, 44.1 kHz; seek works |
| Kodi 21.3 with the LAE add-on 0.2.2 | 2026-11-05 | an LAES `.lae` excerpt | plays the master: 352.8 kHz; seek works |
| foobar2000 2.26.2 (64-bit) and 1.5.1 (32-bit) | 2026-11-05 | `Grand_LAES.flac`, `Grand_LAES.wav`, `03_LAES_demo.lae.wav` | opens all three and reads them as FLAC / PCM, 44.1 kHz: the core master (LAES) or the CD layer (LAEY). The files were added to a playlist; foobar2000 was not played (no screen access during the test) |
| foobar2000 2.26.2 (64-bit) and 1.5.1 (32-bit) with the LAE component 0.16 | 2026-11-05 | `Grand_LAES.flac`; an LAES FLAC excerpt | recognised as LAES: the list of masters and the master it plays (352.8 kHz) are read. Not played (as above); the component's decoder is the one the other players use |
| VLC 3.0.24 (64-bit, portable) | 2026-11-05 | `Grand_LAES.flac` | plays the core master; decoded through VLC to a file, byte-identical to the reference |
| VLC 3.0.24 | 2026-11-05 | `Grand_LAES.wav`, `03_LAES_demo.lae.wav` | plays the core master (LAES) and the CD layer (LAEY). Decoded through VLC to a file: identical to the reference, except that the last 346 frames (8 ms) are missing. A plain WAV loses the same frames in VLC: VLC's own WAV path, not the LAE data |
| VLC 3.0.24 with the LAE module 0.1.1 | 2026-11-05 | an LAES FLAC excerpt | plays the master `master_2_352800_32`; decoded through VLC to a file, byte-identical to the master apart from the last 40 ms (VLC's end-of-stream handling, as above) |
| Windows 11 (build 26200) Media Foundation: Media Player, Movies & TV, Explorer | 2026-11-05 | `Grand_LAES.flac`, LAES FLAC excerpts | not recognised (limitation 1). A plain 24-bit FLAC plays; a plain 32-bit FLAC opens but does not decode |
| Windows 11 Explorer | 2026-11-05 | `Grand_LAES.flac` | length, bit rate and title empty (limitation 1) |
| Android 13 system decoder (Sony Xperia XQ-BC72), as used by apps that hand files to the system | 2026-11-05 | the files of the table above | see the table above |
| DirectShow players (PotPlayer, MPC-BE, MPC-HC) with the LAE filter 0.2.1 | 2026-11-05 | `Grand_LAES.flac`; `03_LAES_demo.lae.wav` | master 352.8 kHz, 609 s; seeking works. On the LAEY file seeking works, but playback resumes about 4 s after the seek, as in 0.2.0 (tested through the filter itself, without a player window) |

Players built on libFLAC or FFmpeg are expected to behave like those libraries: for example foobar2000, VLC, Kodi, mpv, PotPlayer, MPC-BE and most apps. They were tested at the library level, not in each player's user interface.

## Known limitations of other decoders

1. **Windows' built-in FLAC decoder and LAES FLAC.**
   - Windows (Media Foundation, used by the Media Player and Movies & TV apps and by Explorer for length and tags) finds a FLAC file's first audio frame only within its first 4 MiB. The measured limit is an audio start at byte 4,194,308.
   - An LAES FLAC file with masters stores them before the audio frames, which RFC 9639 requires of all metadata. Its first frame therefore starts later, so Windows does not recognise the file.
   - This decoder does not decode 32-bit FLAC either. The file is still a valid FLAC file: libFLAC and FFmpeg play it.
   - **Use instead:** the WAV form, a player built on libFLAC or FFmpeg, or the LAE plugins.
2. **32-bit FLAC.**
   - Many decoders do not play 32-bit FLAC. Android 13's decoder does not recognise it, and Windows' decoder opens it but does not decode it.
   - The LAES reference encoder therefore writes a core master of at most 24 bit unless `--core-bits=32` is given. For the widest compatibility, do not use `--core-bits=32`.
3. **RF64 (LAES WAV form above 4 GiB).**
   - A RIFF WAV file cannot exceed 4 GiB, so a larger LAES WAV file, such as a long album with many masters, is written as RF64.
   - Many WAV readers do not support RF64. Android 13's decoder does not open it.
   - **Use instead:** the FLAC form for such files. LAEY files are always plain RIFF.
4. **The `.lae` extension.**
   - Players that choose a decoder by the file name, such as foobar2000, Windows Media Player and many phone apps, do not open `.lae` without the LAE plugin.
   - Players that look at the content play the core master (the CD layer for LAEY). Rename the file to `.wav` or `.flac`, matching its form, to play the core master anywhere.
5. **Network streaming of the file itself.** In the FLAC form the core master's audio frames come after the data of the other masters. A player that reads the file over the network from the start must load all of the masters before the first sound. For streaming, serve plain FLAC made with `laes_export` (one version: the core master, a master or a rate level).
6. **Seeking.**
   - LAES FLAC files carry no SEEKTABLE, by design. Players seek by searching the frames: it works, but it can be slower in some players.
   - WAV seeking is unaffected.
7. **Tags in WAV.**
   - LAES writes the same tags twice: as RIFF LIST/INFO and as ID3v2.4 (`id3 ` chunk), both in UTF-8.
   - Most players read the ID3 tags. Old Windows programs that read INFO in the system code page may show non-ASCII text as garbled characters.
   - FLAC tags are Vorbis comments in UTF-8, as FLAC requires.

## If a player does not play an LAE file

- **The name ends in `.lae`:** rename it to `.wav` or `.flac` (matching its form), or install the LAE plugin.
- **It is a FLAC file and the player is Windows' own:** use the WAV form, another player, or the LAE plugin (limitation 1).
- **It is a FLAC file with a 32-bit core master:** encode the core master at 24 bit (limitation 2).
- **It is a WAV file larger than 4 GiB:** it is RF64. Use the FLAC form (limitation 3).
- **None of these:** check the file with `laes_tool verify` or `laes_tool parts`, and try FFmpeg (`ffmpeg -i file -f null -`). If FFmpeg plays it, the problem is in that player.
