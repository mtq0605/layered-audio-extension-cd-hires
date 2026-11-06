# laes: the LAE Lossless command-line tool

**English** | [中文](LAES_TOOL.zh.md)

`laes` creates, reads and checks LAES files (LAE Lossless, ID `LAE2`). An LAES file is an ordinary FLAC or WAV file. It contains a **core master**, which every player plays, and one or more further **lossless masters**, which LAE-aware players and `laes` decode bit for bit.

- For a description of LAES, see [OVERVIEW.md](OVERVIEW.md).
- For the file format, see [SPEC.md](SPEC.md). This guide describes only the reference tool.

## Contents

- [Building](#building)
- [Commands at a glance](#commands-at-a-glance)
- [Making a file: `laes enc`](#making-a-file-laes-enc)
- [Tags and pictures](#tags-and-pictures)
- [Changing tags: `laes tag`](#changing-tags-laes-tag)
- [Looking inside: `laes parts`](#looking-inside-laes-parts)
- [Decoding: `laes dec`](#decoding-laes-dec)
- [Checking: `laes verify`](#checking-laes-verify)
- [Time position: `laes align`](#time-position-laes-align)
- [Test helpers](#test-helpers)
- [Text, file names and exit codes](#text-file-names-and-exit-codes)

## Building

The tool consists of a single C++17 source file, `src/laes_tool.cpp`, and its headers. The masters and the core master must be reproduced bit for bit on every platform, so build with strict floating-point semantics.

Windows (Visual Studio 2022, x64 Native Tools prompt):

```
cl /nologo /O2 /EHsc /std:c++17 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux and macOS: the source contains POSIX code paths (`g++ -O2 -std=c++17 -ffp-contract=off -pthread`), but the project's CI does not yet build or test `laes` on these systems. Until it does, use the Windows build for files that you publish.

`laes selftest` runs the built-in regression tests on synthetic signals. Exit code 0 indicates that all tests passed.

## Commands at a glance

| Command | What it does |
|---|---|
| `laes enc <out> ...` | Creates an LAES file from one or more masters. |
| `laes tag <in> <out> ...` | Writes a new file with different tags and pictures, without re-encoding. |
| `laes parts <file>` | Shows the blocks or chunks, the SHA-256 of the audio bytes and of the payload, and the tags. |
| `laes dec <in> <dir> ...` | Writes the core master, the masters and other versions as WAV files. |
| `laes verify <in> <master.wav> ...` | Decodes every master and compares it bit for bit with the given files. |
| `laes align <cd.wav> <master.wav>` | Measures where a master lies on the time line of a released CD. |
| `laes selftest` | Runs the regression tests. |
| `laes excerpt`, `laes reduce`, `laes mkcd` | Test helpers. |

Run `laes` without arguments to display the list.

## Making a file: `laes enc`

```
laes enc <out.flac | out.wav | out.lae> (--derive-core | --core=<core.wav>) <master> [<master> ...] [options]
```

### The core master

- `--derive-core`: the core master is derived from the masters. It is the base level (44.1 or 48 kHz) of a master, at the word length set by `--core-bits`.
  - Default word length: 16 bits for the WAV form; for the FLAC form, the longest word length among the masters, up to a maximum of 24 bits.
  - `--core-bits=16|24|32` selects a different word length. A master with exactly that word length yields the core master unchanged; otherwise, the master with the longest word length is reduced with TPDF dither. The exact rule is given in [SPEC.md](SPEC.md), Section 11.1, so that anyone can check it.
- `--core=<core.wav>`: the core master is supplied, for example as the 16-bit WAV of a released CD or DVD track. A master produced independently of it then requires its time position on that core master (see below).

### Masters

- A master is a 16-, 24- or 32-bit stereo WAV file (RIFF, or RF64 beyond 4 GB) or a FLAC file, at 1, 2, 4 or 8 times 44.1 or 48 kHz. A file can contain up to 16 masters.
- The order on the command line does not matter: the tool chooses the order and the way each master is stored, selecting whichever is smallest.
- `<master>@T`: the time position of the master on the time line of the core master, in base-rate samples (a decimal number, for example `master.wav@-0.37`). The default is 0, which is correct for a derived core master.
- `<master>@auto`: for a master produced independently of the given core master (for example, the hi-res master of a released CD). The tool measures T against `--core` with the same search as `laes align`. If the alignment error is above −60 dB, the tool displays it and asks for confirmation; `--accept` accepts it without asking. If it is not accepted, nothing is written. Two conditions apply: the core master given with `--core` is 16-bit (CD or DVD), and the master to be aligned is supplied as a WAV file, because the search reads WAV; a FLAC master is refused. A derived core master does not need `@auto`: its masters are at T = 0.

### Form and file name

| Name | Form |
|---|---|
| `out.flac` | FLAC form: the core master in the FLAC audio frames, the other masters in `LAE2` APPLICATION blocks. |
| `out.wav` | WAV form: the core master in `data`, the other masters in an `LAE ` chunk (RF64 beyond 4 GB). |
| `out.lae` | The format's own name: `--form=flac` (default) or `--form=wav`. |

- Players identify LAES files by their content, not by their name.
- With `.flac` and `.wav`, any player plays the core master.
- `.lae` files are opened only by players that recognise the name (with an LAE plug-in) or that inspect the content.

### Examples

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_352k_24.wav Piece_176k_24.wav
laes enc Piece_LAES.wav  --derive-core Piece_352k_24.wav
laes enc Track01_LAES.wav --core=Track01_cd.wav Track01_master.wav@auto --accept
laes enc Piece_LAES.lae --form=wav --derive-core Piece_352k_24.wav --tag=TITLE=Piece
```

### What the tool checks before it writes

The file is first written as `<out>.part` and then read back exactly as a player reads it. The tool checks the file structure, the CRC-32 of the payload and the core master. It decodes every master and compares it sample by sample with its input, including the SHA-256 stored for each master. Only if everything matches is `<out>.part` renamed to `<out>`; otherwise, it is removed and nothing is written.

### Memory, disk and resuming

None of these options changes the file: the same masters and options produce the same bytes.

| Option | Effect |
|---|---|
| `--mem=<GB>` | Memory budget; default 2 GB (less on machines with little memory). It sets only the length of the segments processed at a time. |
| `--small-disk` | Requires much less disk space: the largest rate level of each master and the decoded FLAC masters are not kept but are regenerated when read. Slower. |
| `--force-disk` | Starts even if the estimated disk requirement exceeds the free space. |
| `--disk-stop` | When free space falls below 2 GB, stops instead of waiting (the work folder is kept). |
| `--disk-stop-remove` | When free space falls below 2 GB, stops and removes the work folder (the work done so far is lost). |
| `--timing` | Prints the time and peak memory of each stage and the maximum size of the work folder. |

- **Work folder:** intermediate data is written to `<out>.laes.tmp` next to the output and is removed at the end.
- **Disk check:** before starting, the tool estimates the disk space it needs. If this estimate plus the 2 GB that the tool keeps free exceeds the free space, it does not start (`--force-disk` makes it start anyway). During the run, if free space falls below 2 GB, the tool waits until space is available again, unless `--disk-stop` or `--disk-stop-remove` is given.
- **Resuming:** a run that stopped (because the disk was full, or the tool was closed or crashed) keeps its work folder. Running the same command again continues from where it stopped, with the same result as an uninterrupted run. If the arguments, a master, the `--core` file or the tool itself has changed, the run starts again; the same applies to a change of `--small-disk`. Tag options, `--mem`, `--timing`, `--force-disk`, `--disk-stop` and `--disk-stop-remove` are not taken into account and may be changed before resuming.
- **Measured time, memory and disk use** for 6, 10 and 30 minutes with 2 to 12 masters are given in [SPEC.md](SPEC.md), Section 11.5. In those runs, peak memory stayed below the 2 GB budget. The work folder grows with the duration and with the number and rates of the masters: 27.8 GB for 30 minutes with 12 masters, and 15.0 GB with `--small-disk`.

## Tags and pictures

`laes enc` and `laes tag` accept the same options. If none of them is given, no tags are written and the file is byte-for-byte identical to one made without tag support.

| Option | Meaning |
|---|---|
| `--tag=KEY=VALUE` | A tag. KEY is a Vorbis comment name such as `TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `DATE`, `GENRE`, `TRACKNUMBER`, `DISCNUMBER`, `COMPOSER`, `COPYRIGHT`, `COMMENT` or `ISRC`. A key may be given more than once. |
| `--cover=<image>` | The front cover. |
| `--picture=<type>:<image>` | A further picture; any number may be given: 4 back cover, 5 leaflet page, 6 medium (disc label), 7 lead artist, 8 artist, … (the picture types of ID3v2 and FLAC, 0 to 20). |

- **Images:** JPEG or PNG, stored byte for byte (never re-encoded). There is no minimum size. A single image may be up to about 16 MB (the FLAC limit for one metadata block), and all images together up to 256 MB. Very large images make some phone players slow to open the file.
- **Text:** UTF-8, so any language can be used. The key itself is ASCII.
- **Location of the tags:** one set of tags describes the core master and every master.

| Form | Tags | Pictures | Position |
|---|---|---|---|
| FLAC | One `VORBIS_COMMENT` block | One `PICTURE` block per image | After STREAMINFO, before the `LAE2` blocks |
| WAV | A `LIST`/`INFO` chunk and an `id3 ` chunk (ID3v2.4) | `APIC` frames in the `id3 ` chunk | After `fmt `, before `data` |

- WAV `INFO` supports only some keys: `TITLE` → `INAM`, `ARTIST` → `IART`, `ALBUM` → `IPRD`, `DATE` → `ICRD`, `COPYRIGHT` → `ICOP`, `COMMENT` → `ICMT`, `GENRE` → `IGNR`, `TRACKNUMBER` → `ITRK`. Every key is written to `id3 `; keys without a dedicated ID3 frame are written to `TXXX`.
- Older software that reads WAV `INFO` in the system code page displays non-ASCII text incorrectly. `id3 ` is always UTF-8, and players that read both should prefer it.
- The tool identifies itself in the tags as `laes (LAE Lossless)`, without a version number, so that the same input always produces the same file.
- Tags never take part in the checks: the SHA-256 of each master covers only the samples, and tags never determine which version is played.

Example:

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_176k_24.wav "--tag=TITLE=Piece" "--tag=ARTIST=Name" "--tag=DATE=2026" "--tag=COPYRIGHT=(c) 2026 Name. All rights reserved." --cover=cover.jpg --picture=4:back.jpg
```

## Changing tags: `laes tag`

```
laes tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]
```

- Writes a **new** file whose tags and pictures are exactly those given. Without tag options, the new file has no tags at all.
- The old tags are discarded. The audio bytes, the payload and every other block or chunk are copied byte for byte.
- The result is byte-for-byte identical to re-encoding the masters with these tags.
- The new file is then fully decoded, and every master is checked against its stored SHA-256. The audio bytes and the payload are also compared with those of the old file. Any difference causes the new file to be removed.
- The old file is never modified. If `<out>` already exists, nothing is done.
- Supports the FLAC form and the WAV form (including RF64). LAEY files (`LAE1`) are tagged with `lae_enc tag`.

## Looking inside: `laes parts`

```
laes parts <file>
```

Lists the FLAC metadata blocks or the RIFF chunks with their sizes. It prints the SHA-256 of the audio bytes (the FLAC frames or the WAV `data`) and of the payload, and the tags as a player reads them. Two files with the same two SHA-256 values contain the same audio and the same masters, regardless of their tags.

## Decoding: `laes dec`

```
laes dec <in> <out dir> [--levels]
laes dec <in> --list
laes dec <in> <out dir> --pick=<n>[,<n> ...] | --pick=<rate>:<bits>
```

- Without options, it writes the core master (`core.wav`) and every master (`master_<k>_<rate>_<bits>.wav`).
- `--levels` additionally writes every rate level of every master (`master_<k>_level_<rate>_<bits>.wav`). A rate level is the master at half, a quarter or an eighth of its rate, and is lossless.
- `--list` shows every choice with a number. `--pick` writes only the selected choices, by number or by rate and word length.
- **Derived 16-bit masters:** for each rate that has no 16-bit choice of its own, a 16-bit version is offered (`derived16_<rate>_from_<name>.wav`). It is made from the choice with the longest word length at that rate by a fixed TPDF rule ([SPEC.md](SPEC.md), Section 10.1), so every decoder reproduces it bit for bit. It is lossless relative to that rule. It is not lossless relative to the choice it is made from: that choice is requantised to 16 bits with TPDF dither and cannot be recovered from it. It is a derived master, not the original master, and it is written only when picked.
- A master that fails its SHA-256 check is not offered, and neither are its rate levels. The core master and the other masters are not affected.
- Output above 16 bits is written as `WAVE_FORMAT_EXTENSIBLE`.

## Checking: `laes verify`

```
laes verify <in> <master.wav> [<master.wav> ...]
```

Decodes every master in the file and compares it bit for bit with the given files, which may be given in any order. For each file, it prints `IDENTICAL` or `DIFFERENT`; for masters without a corresponding file, it prints `not compared`.

## Time position: `laes align`

```
laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]
```

For a core master that was produced independently, such as a released CD, this command measures the time position T of the master on the CD's time line, including the fractional part. It uses the automatic search of the LAE Lossy encoder (`lae_enc`, which is found next to `laes` or given with `--lae-enc`). An alignment error within −60 dB passes; otherwise, the error is displayed and you decide. The command writes nothing; it prints `use <master>@<T>` for use with `laes enc`. A whole album is cut into tracks by the LAE Lossy planner (`lae_enc plan … laes`).

## Test helpers

| Command | What it does |
|---|---|
| `laes selftest` | Runs regression tests with synthetic signals; exit code 0 means that all tests passed. |
| `laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]` | Cuts an excerpt; 16 = TPDF to 16 bits, 24 = as is, 32 = random low byte. |
| `laes reduce <in.wav> <out.wav> <bits>` | Reduces the word length with TPDF dither and a fixed seed. |
| `laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]` | Creates a test "CD": 16 bits, the master's base level shifted by tau. |

## Text, file names and exit codes

- **Text:** on Windows, all arguments, file names, tags and console output are handled as UTF-8 within the tool. Tags and paths in any language work from cmd, PowerShell and scripts, regardless of the system code page. On other systems, the arguments are taken as given (UTF-8 on typical systems).
- **Exit codes:** 0: success; 1: invalid arguments; 2: the operation failed (a file could not be read or written, a check failed, or the disk was full). After exit code 2, `laes enc` and `laes tag` have written nothing.
- **Messages** are in English; the disk and resume messages also include a line in Chinese.
