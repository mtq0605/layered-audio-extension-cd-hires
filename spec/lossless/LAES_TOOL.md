# laes: the LAE Lossless command-line tool

**English** | [中文](LAES_TOOL.zh.md)

`laes` makes, reads and checks LAES files (LAE Lossless, ID `LAE2`). An LAES file is an ordinary FLAC or WAV file. It holds a **core master** that every player plays, and one or more further **lossless masters** that LAE-aware players and `laes` decode bit for bit.

- What LAES is: [OVERVIEW.md](OVERVIEW.md).
- The file format: [SPEC.md](SPEC.md). This guide describes the reference tool only.

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

The tool is one C++17 source file with its headers, `src/laes_tool.cpp`. The masters and the core master must come out bit for bit on every platform, so build with strict floating point.

Windows (Visual Studio 2022, x64 Native Tools prompt):

```
cl /nologo /O2 /EHsc /std:c++17 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux / macOS: the source has POSIX code paths (`g++ -O2 -std=c++17 -ffp-contract=off -pthread`), but `laes` is not yet built or tested by the project's CI on these systems. Use the Windows build for files you publish until it is.

`laes selftest` runs the built-in regression tests on synthetic signals. Exit code 0 means all passed.

## Commands at a glance

| Command | What it does |
|---|---|
| `laes enc <out> ...` | makes an LAES file from one or more masters |
| `laes tag <in> <out> ...` | writes a new file with other tags and pictures, without encoding again |
| `laes parts <file>` | shows the blocks or chunks, the SHA-256 of the audio bytes and of the payload, and the tags |
| `laes dec <in> <dir> ...` | writes the core master, the masters and other versions as WAV files |
| `laes verify <in> <master.wav> ...` | decodes and compares every master bit for bit with the given files |
| `laes align <cd.wav> <master.wav>` | measures where a master lies on a released CD's time line |
| `laes selftest` | runs the regression tests |
| `laes excerpt`, `laes reduce`, `laes mkcd` | test helpers |

Run `laes` without arguments for the list.

## Making a file: `laes enc`

```
laes enc <out.flac | out.wav | out.lae> (--derive-core | --core=<core.wav>) <master> [<master> ...] [options]
```

### The core master

- `--derive-core`: the core master is derived from the masters. It is the base level (44.1 or 48 kHz) of a master, at `--core-bits`.
  - Default word length: 16 bits for the WAV form; for the FLAC form the longest master word length, at most 24 bits.
  - `--core-bits=16|24|32` chooses another. A master with exactly that word length gives the core master unchanged; otherwise the longest master is reduced with TPDF dither. The exact rule is in [SPEC.md](SPEC.md), Section 11.1, so anyone can check it.
- `--core=<core.wav>`: the core master is given, for example the 16-bit WAV of a released CD or DVD track. A master produced apart from it then needs its time position on that core master (see below).

### Masters

- A master is a 16-, 24- or 32-bit stereo WAV (RIFF, or RF64 beyond 4 GB) or a FLAC file, at 1, 2, 4 or 8 times 44.1 or 48 kHz. Up to 16 masters per file.
- The order on the command line does not matter: the tool chooses the order and the way each master is stored, whichever is smallest.
- `<master>@T`: the master's time position on the core master's time line, in base-rate samples (a decimal number, for example `master.wav@-0.37`). Default 0, which is right for a derived core master.
- `<master>@auto`: for a master produced independently of the given core master (for example the hi-res master of a released CD). It measures T against `--core` with the same search as `laes align`. If the alignment error is above −60 dB, the tool shows it and asks; `--accept` accepts without asking. If it is not accepted, nothing is written. Two conditions: the core master given with `--core` is 16 bit (CD or DVD), and the master to be aligned is given as a WAV file, because the search reads WAV; a FLAC master is refused. A derived core master needs no `@auto`: its masters are at T = 0.

### Form and file name

| Name | Form |
|---|---|
| `out.flac` | FLAC form: the core master in the FLAC audio frames, the other masters in `LAE2` APPLICATION blocks |
| `out.wav` | WAV form: the core master in `data`, the other masters in an `LAE ` chunk (RF64 beyond 4 GB) |
| `out.lae` | the format's own name: `--form=flac` (default) or `--form=wav` |

- Players read LAES files by their content, not by their name.
- `.flac` and `.wav` play the core master in any player.
- `.lae` is opened only by players that know the name (with an LAE plug-in) or that look at the content.

### Examples

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_352k_24.wav Piece_176k_24.wav
laes enc Piece_LAES.wav  --derive-core Piece_352k_24.wav
laes enc Track01_LAES.wav --core=Track01_cd.wav Track01_master.wav@auto --accept
laes enc Piece_LAES.lae --form=wav --derive-core Piece_352k_24.wav --tag=TITLE=Piece
```

### What the tool checks before it writes

The file is written as `<out>.part`. It is then read back exactly as a player reads it. The tool checks the file's structure, the payload's CRC-32 and the core master. It decodes every master and compares it sample by sample with its input, including the SHA-256 stored for each master. Only if everything matches is `<out>.part` renamed to `<out>`. Otherwise it is removed and nothing is written.

### Memory, disk and resuming

None of these options changes the file: the same masters and options give the same bytes.

| Option | Effect |
|---|---|
| `--mem=<GB>` | memory budget, default 2 GB (less on machines with little memory). It sets only the length of the pieces processed at a time. |
| `--small-disk` | needs much less disk: the largest rate level of each master and decoded FLAC masters are not kept but made again when read. Slower. |
| `--force-disk` | starts even if the estimated disk need is more than is free |
| `--disk-stop` | when free space falls below 2 GB, stop (the work folder is kept) instead of waiting |
| `--disk-stop-remove` | when free space falls below 2 GB, stop and remove the work folder (the work done so far is lost) |
| `--timing` | prints the time and peak memory of each stage and the largest size of the work folder |

- **Work folder:** intermediate data goes to `<out>.laes.tmp` next to the output and is removed at the end.
- **Disk check:** before starting, the tool estimates the disk it needs. If that plus the 2 GB it keeps free is more than is free, it does not start (`--force-disk` starts anyway). While it runs, free space below 2 GB makes it wait until there is room again, unless `--disk-stop` or `--disk-stop-remove` is given.
- **Resuming:** a run that stopped (disk full, closed, crashed) keeps its work folder. Run the same command again and it continues where it stopped, with the same result as an uninterrupted run. If the arguments, a master, the `--core` file or the tool itself changed, it starts again; so does a change of `--small-disk`. Tag options, `--mem`, `--timing`, `--force-disk`, `--disk-stop` and `--disk-stop-remove` do not count: you may change them before resuming.
- **Measured time, memory and disk** for 6, 10 and 30 minutes with 2 to 12 masters: [SPEC.md](SPEC.md), Section 11.5. In those runs the peak memory stayed below the 2 GB budget. The work folder grows with the length and with the number and rates of the masters: 27.8 GB for 30 minutes with 12 masters, 15.0 GB with `--small-disk`.

## Tags and pictures

`laes enc` and `laes tag` take the same options. Without any of them, no tags are written and the file is the same, byte for byte, as one made without tag support.

| Option | Meaning |
|---|---|
| `--tag=KEY=VALUE` | a tag; KEY is a Vorbis comment name such as `TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `DATE`, `GENRE`, `TRACKNUMBER`, `DISCNUMBER`, `COMPOSER`, `COPYRIGHT`, `COMMENT`, `ISRC`. A key may be given more than once. |
| `--cover=<image>` | the front cover |
| `--picture=<type>:<image>` | another picture, any number: 4 back cover, 5 leaflet page, 6 medium (disc label), 7 lead artist, 8 artist, … (the picture types of ID3v2 and FLAC, 0 to 20) |

- **Images:** JPEG or PNG, stored byte for byte (never re-encoded). There is no minimum size. One image may be up to about 16 MB (the FLAC limit for one metadata block); all images together up to 256 MB. Very large images make some phone players slow to open the file.
- **Text:** UTF-8, so any language works. The key itself is ASCII.
- **Where the tags go:** one set of tags describes the core master and every master.

| Form | Tags | Pictures | Position |
|---|---|---|---|
| FLAC | one `VORBIS_COMMENT` block | one `PICTURE` block per image | after STREAMINFO, before the `LAE2` blocks |
| WAV | a `LIST`/`INFO` chunk and an `id3 ` chunk (ID3v2.4) | `APIC` frames in the `id3 ` chunk | after `fmt `, before `data` |

- WAV `INFO` knows only some keys: `TITLE` `INAM`, `ARTIST` `IART`, `ALBUM` `IPRD`, `DATE` `ICRD`, `COPYRIGHT` `ICOP`, `COMMENT` `ICMT`, `GENRE` `IGNR`, `TRACKNUMBER` `ITRK`. Every key goes into `id3 `; keys without an ID3 frame of their own go into `TXXX`.
- Old software that reads WAV `INFO` in the system code page shows non-ASCII text wrongly. `id3 ` is always UTF-8, and players that read both should prefer it.
- The tool names itself in the tags as `laes (LAE Lossless)`, without a version, so the same input always gives the same file.
- Tags never take part in the checks: the SHA-256 of each master covers samples only, and tags never decide which version plays.

Example:

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_176k_24.wav "--tag=TITLE=Piece" "--tag=ARTIST=Name" "--tag=DATE=2026" "--tag=COPYRIGHT=(c) 2026 Name. All rights reserved." --cover=cover.jpg --picture=4:back.jpg
```

## Changing tags: `laes tag`

```
laes tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]
```

- Writes a **new** file whose tags and pictures are exactly the ones given. With no tag options, the new file has no tags at all.
- The old tags are dropped. The audio bytes, the payload and every other block or chunk are copied byte for byte.
- The result is the same, byte for byte, as encoding the masters again with these tags.
- Afterwards the new file is decoded completely and every master is checked against its stored SHA-256. The audio bytes and the payload are also compared with the old file. Any difference removes the new file.
- The old file is never changed. If `<out>` already exists, nothing is done.
- Works on the FLAC and the WAV form (also RF64). LAEY files (`LAE1`) are tagged with `lae_enc tag`.

## Looking inside: `laes parts`

```
laes parts <file>
```

Lists the FLAC metadata blocks or the RIFF chunks with their sizes. It prints the SHA-256 of the audio bytes (the FLAC frames, or the WAV `data`) and of the payload, and the tags as a player reads them. Two files with the same two SHA-256 values hold the same audio and the same masters, whatever their tags.

## Decoding: `laes dec`

```
laes dec <in> <out dir> [--levels]
laes dec <in> --list
laes dec <in> <out dir> --pick=<n>[,<n> ...] | --pick=<rate>:<bits>
```

- Without options it writes the core master (`core.wav`) and every master (`master_<k>_<rate>_<bits>.wav`).
- `--levels` adds every rate level of every master (`master_<k>_level_<rate>_<bits>.wav`). A rate level is the master at half, a quarter or an eighth of its rate, lossless.
- `--list` shows every choice with a number. `--pick` writes only the chosen ones, by number or by rate and word length.
- **Derived 16-bit masters:** for each rate without a 16-bit choice of its own, a 16-bit version is offered (`derived16_<rate>_from_<name>.wav`). It is made from the longest-word choice at that rate with a fixed TPDF rule ([SPEC.md](SPEC.md), Section 10.1), so every decoder reproduces it bit for bit. It is lossless relative to that rule. It is not lossless relative to the choice it is made from: that choice is requantised to 16 bits with TPDF dither and cannot be recovered from it. It is a derived master, not the original master, and it is written only when picked.
- A master that fails its SHA-256 is not offered, nor are its rate levels. The core master and the other masters are unaffected.
- Output above 16 bits is written as `WAVE_FORMAT_EXTENSIBLE`.

## Checking: `laes verify`

```
laes verify <in> <master.wav> [<master.wav> ...]
```

Decodes every master in the file and compares it bit for bit with the given files, in any order. It prints `IDENTICAL` or `DIFFERENT` for each file, and `not compared` for masters without a file.

## Time position: `laes align`

```
laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]
```

For a core master that was produced on its own, such as a released CD: measures the master's time position T on the CD's time line, fraction included, with the automatic search of the LAE Lossy encoder (`lae_enc`, found next to `laes` or given with `--lae-enc`). An alignment error within −60 dB passes. Otherwise the error is shown and you decide. It writes nothing; it prints `use <master>@<T>` for `laes enc`. A whole album is cut into tracks by the LAE Lossy planner (`lae_enc plan … laes`).

## Test helpers

| Command | What it does |
|---|---|
| `laes selftest` | regression tests with synthetic signals; exit code 0 = all passed |
| `laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]` | cuts an excerpt; 16 = TPDF to 16 bits, 24 = as is, 32 = random low byte |
| `laes reduce <in.wav> <out.wav> <bits>` | a shorter word length with TPDF dither, fixed seed |
| `laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]` | a test "CD": 16 bits, the master's base level shifted by tau |

## Text, file names and exit codes

- **Text:** on Windows all arguments, file names, tags and console output are UTF-8 inside the tool. Tags and paths in any language work from cmd, PowerShell and scripts, whatever the system code page. On other systems the arguments are taken as given (UTF-8 on usual systems).
- **Exit codes:** 0 success; 1 wrong arguments; 2 the work failed (a file could not be read or written, a check failed, the disk was full). After 2, `laes enc` and `laes tag` have written nothing.
- **Messages** are in English; the disk and resume messages also have a Chinese line.
