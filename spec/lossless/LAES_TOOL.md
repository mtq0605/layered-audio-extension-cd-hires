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
- [Menu order: `--menu-order`](#menu-order---menu-order)
- [Changing master names: `laes names`](#changing-master-names-laes-names)
- [Adding masters: `laes add`](#adding-masters-laes-add)
- [Extracting masters into a new file: `laes extract`](#extracting-masters-into-a-new-file-laes-extract)
- [Checking: `laes verify`](#checking-laes-verify)
- [Time position: `laes align`](#time-position-laes-align)
- [Test helpers](#test-helpers)
- [Text, file names and exit codes](#text-file-names-and-exit-codes)

## Building

The tool consists of a single C++17 source file, `src/laes_tool.cpp`, and its headers. The masters and the core master must be reproduced bit for bit on every platform, so build with strict floating-point semantics.

Windows (Visual Studio 2022, x64 Native Tools prompt):

```
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux and macOS: the source contains POSIX code paths (`g++ -O2 -std=c++17 -ffp-contract=off -pthread`), but the project's CI does not yet build or test `laes` on these systems. Until it does, use the Windows build for files that you publish. On 2026-11-14, `dec`, `verify`, `extract` and `add` were run on Linux x64 (g++ 13.3 under WSL): the decoded WAV files and the files written by `extract` and `add` were byte-identical to those of the Windows build. In builds before that date, `dec` on Linux wrote its WAV files beside the output folder, and `extract` and `add` failed.

`laes selftest` runs the built-in regression tests on synthetic signals. Exit code 0 indicates that all tests passed.

## Commands at a glance

| Command | What it does |
|---|---|
| `laes enc <out> ...` | Creates an LAES file from one or more masters. |
| `laes tag <in> <out> ...` | Writes a new file with different tags and pictures, without re-encoding. |
| `laes names <in> <out> ...` | Writes a new file with different master names, menu order, default or derived-version settings, without re-encoding. |
| `laes add <in> <out> <master> ...` | Adds masters to an existing LAES file and writes the result as a new file; the old parts stay byte for byte unchanged. |
| `laes extract <in> <out> --pick=...` | Packs one or several chosen masters into a new LAES file. |
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
  - `--core-bits=16|24|32` selects a different word length, in either form (a 32-bit core master is allowed in the FLAC form too; see [COMPATIBILITY.md](../COMPATIBILITY.md) for players). If a master has exactly that word length, its base level is used as the core master without requantisation; otherwise, the master with the longest word length is reduced with TPDF dither. The exact rule is given in [SPEC.md](SPEC.md), Section 11.1, so that anyone can check it.
- `--core=<core.wav>`: the core master is supplied, for example as the 16-bit WAV of a released CD or DVD track. A master produced independently of it then requires its time position on that core master (see below).

### Masters

- A master is a 16-, 24- or 32-bit stereo WAV file (RIFF, or RF64 beyond 4 GiB) or a FLAC file, at 1, 2, 4 or 8 times 44.1 or 48 kHz. A file can contain up to 255 masters. Every rate and every word length can hold an independent master, and several masters may have the same rate and word length.
- If the header of a master WAV / RF64 or of the `--core` WAV gives a data length beyond the end of the file, `laes enc` and `laes add` print a `NOTE` that the file may be cut off, read it to its end and continue (since 2026-11-14). A data length of 2^63 bytes or more in the header (RF64 `ds64`) is refused with the reason; `laes parts`, `tag`, `add`, `extract` and `names` refuse such a chunk length in an LAES file in the same way.
- The order on the command line does not change the file size: the tool chooses the order in the file and the way each master is stored, selecting whichever is smallest. The order on the command line is the menu order (see [Menu order](#menu-order---menu-order)).
- `--name=<k>:<text>`: names the k-th master on the command line (counting from 1; repeat the option for other masters). The name is free text from the publisher: UTF-8, at most 255 bytes (about 255 Latin letters or 85 Chinese characters; emoji are allowed), without control characters (line breaks, tabs). Players show it as it is; a name never chooses anything. The tool never fills in a file name, path or date as a name. At the end, it prints "given k -> master n -> name".
- `--no-derive`: no derived 16-bit version at all, neither for the masters nor for the core master. `--no-derive=<k>`: only the k-th master on the command line offers none. `--no-derive=core`: only the core master offers none.
- `--default=<k>`: the k-th master on the command line is what players play when the listener has chosen nothing. `--default=core`, or no `--default`: the core master. The default is always a master itself, never a rate level or a derived version. A player that cannot play it plays the core master. At the end, the tool prints `default: given k -> master n in menus`.
- With a name, `--no-derive`, `--default=<k>`, more than 16 masters, or a menu order other than the file order chosen by the encoder, the file is written in payload version 2; players and tools from before 2026-11-11 play only the core master of such a file. Otherwise, the file is written in payload version 1, byte for byte as before.
- With more than 16 masters, the reference of each master is first tried on four fixed short excerpts, and only the three smallest candidates are coded in full; only one file order is tried. Measured on 10-second excerpts: with 20 masters, the file is byte for byte the one that a full search gives; 64 masters take about 8 minutes.
- `<master>@T`: the time position of the master on the time line of the core master, in base-rate samples (a decimal number, for example `master.wav@-0.37`). The default is 0, which is correct for a derived core master.
- `<master>@auto`: for a master produced independently of the given core master (for example, the hi-res master of a released CD). The tool measures T against `--core` with the same search as `laes align`, which needs `lae_enc` next to `laes` or given with `--lae-enc=<path>`. If the alignment error is above −60 dB, the tool displays it and asks for confirmation; `--accept` accepts it without asking. If it is not accepted, nothing is written. Two conditions apply: the core master given with `--core` is 16-bit (CD or DVD), and the master to be aligned is supplied as a WAV file, because the search reads WAV; a FLAC master is refused. A derived core master does not need `@auto`: its masters are at T = 0. The search runs `lae_enc` through the system's shell, so a path that the shell would change is refused, with the reason, before the search starts: a path with `%` or `"` on Windows, and with `$`, `` ` ``, `"` or `\` elsewhere. Rename the file, or give T by hand (`<master>@<T>`).

### Form and file name

| Name | Form |
|---|---|
| `out.flac` | FLAC form: the core master in the FLAC audio frames, the other masters in `LAE2` APPLICATION blocks. |
| `out.wav` | WAV form: the core master in `data`, the other masters in an `LAE ` chunk (RF64 beyond 4 GiB). |
| `out.lae` | The format's own name: `--form=flac` (default) or `--form=wav`. |

- The LAE plug-ins identify LAES files by their content, not by their name; the name only routes the file to a decoder.
- With `.flac` and `.wav`, any player plays the core master.
- `.lae` files are opened only by players that recognise the name (with an LAE plug-in) or that inspect the content.
- An output name that is one of the inputs (a master or the `--core` file) is refused before anything is written (since 2026-11-14). An existing `<out>` is replaced only after two yes answers at a console or with `--overwrite` (see "Text, file names and exit codes"), and only when the new file is complete.

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
| `--mem=<GB>` | Memory budget; default 2 GB (less on machines with little memory). It sets only the length of the segments processed at a time. It must be a number above 0 and at most 1048576. |
| `--small-disk` | Requires much less disk space: the largest rate level of each master and the decoded FLAC masters are not kept but are regenerated when read. Slower. |
| `--force-disk` | Starts even if the estimated disk requirement exceeds the free space. |
| `--disk-stop` | When free space falls below 2 GB, stops instead of waiting (the work folder is kept). |
| `--disk-stop-remove` | When free space falls below 2 GB, stops and removes the work folder (the work done so far is lost). |
| `--timing` | Prints the time and peak memory of each stage and the maximum size of the work folder. |

- **Work folder:** intermediate data is written to `<out>.laes.tmp` next to the output and is removed at the end. When the folder cannot be made there (a read-only place), it is made in the system temp folder, and the tool says so (since 2026-11-15). Two runs of the same command (the same work folder) cannot run at once: the work folder holds a lock file, and a second run stops at once (exit code 2) and changes nothing.
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

- **Images:** JPEG or PNG, stored byte for byte (never re-encoded). There is no minimum size. A single image may be up to about 16 MB (the FLAC limit for one metadata block; the same limit applies in the WAV form), and all images together up to 256 MB. Very large images make some phone players slow to open the file.
- **Limits:** tags are refused when a VORBIS_COMMENT or PICTURE block would reach 16 MB, or the ID3v2 tag of the WAV form (text and pictures together) 256 MB.
- **Text:** UTF-8, so any language can be used. The key itself is ASCII.
- **Location of the tags:** one set of tags describes the core master and every master.

| Form | Tags | Pictures | Position |
|---|---|---|---|
| FLAC | One `VORBIS_COMMENT` block | One `PICTURE` block per image | After STREAMINFO, before the `LAE2` blocks |
| WAV | A `LIST`/`INFO` chunk and an `id3 ` chunk (ID3v2.4) | `APIC` frames in the `id3 ` chunk | After `fmt `, before `data` |

- WAV `INFO` supports only some keys: `TITLE` → `INAM`, `ARTIST` → `IART`, `ALBUM` → `IPRD`, `DATE` → `ICRD`, `COPYRIGHT` → `ICOP`, `COMMENT` → `ICMT`, `GENRE` → `IGNR`, `TRACKNUMBER` → `ITRK`. Every key is written to `id3 `; keys without a dedicated ID3 frame are written to `TXXX`.
- Older software that reads WAV `INFO` in the system code page displays non-ASCII text incorrectly. `id3 ` is always UTF-8, and players that read both should prefer it.
- When tags are written, the tool identifies itself in them as `laes (LAE Lossless)`, without a version number, so that the same input always produces the same file.
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
- The result is byte-for-byte identical to re-encoding the same masters with the same options and the same version of the tool, with these tags.
- The new file is then fully decoded, and every master is checked against its stored SHA-256. The audio bytes and the payload are also compared with those of the old file. Any difference causes the new file to be removed.
- The old file is never modified. An existing `<out>` is not replaced silently (see "Text, file names and exit codes").
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
laes dec <in> <out dir> --pick=<n>[,<n> ...] | <a>-<b> | <rate>:<bits> | master:<k> | name:<text>
```

- Without options, it writes the core master (`core.wav`) and every master (`master_<k>_<rate>_<bits>.wav`). An output folder name that begins with `-` is refused (since 2026-11-14), so that a forgotten folder does not become a folder named after an option.
- `--levels` additionally writes every rate level of every master (`master_<k>_level_<rate>_<bits>.wav`). A rate level is the master at half, a quarter or an eighth of its rate, and is lossless.
- `--list` shows every choice with a number, in the order of [SPEC.md](SPEC.md), Section 10.3: the core master first, then one group per master (the master itself, then its rate levels and derived versions, marked with "-"). If a master has a name, the name is shown with the master, its levels and its derived versions. `--list` reads the master records only and decodes nothing (since 2026-11-15); the checks belong to decoding and playback. Its exit code is 0, or 2 when the records fail a check.
- `--pick` writes only the selected choices:
  - `<n>`: the n-th entry of the list; several separated by commas, for example `--pick=2,6`;
  - `<a>-<b>`: entries a to b of the list, for example `--pick=6-12`; ranges and single numbers can be mixed, for example `--pick=2-4,9`;
  - `<rate>:<bits>`: masters only, never a rate level or a derived version. If one master matches, it is written; if several match, you tick the ones you want (see "When several masters match" below);
  - `master:<k>`: master k;
  - `name:<text>`: the masters with exactly this name. If several match, you tick them as above; if none matches, the request is refused.
- **Derived 16-bit masters:** every master above 16 bits has one at its own rate and at each of its rate levels (`derived16_<rate>_from_<source>.wav`), shown as "derived from" the master, with the name of the source master; a core master above 16 bits has one as well. A master for which derived versions were turned off with `--no-derive` has none. A derived master is made by a fixed TPDF rule ([SPEC.md](SPEC.md), Section 10.1), so every decoder reproduces it bit for bit. It is lossless relative to that rule. It is not lossless relative to the master it is made from: that master is requantised to 16 bits with TPDF dither and cannot be recovered from it. It is a derived master, not the original master; it never replaces an independent master with the same rate and word length, and it is written only when picked.
- A master that fails its SHA-256 check is not offered, and neither are its rate levels and derived versions. The core master and the other masters are not affected.
- Output above 16 bits is written as `WAVE_FORMAT_EXTENSIBLE`.
- **Disk check:** before anything is written, the size of the chosen WAV files is compared with the free space on the disk of the output folder. If they do not fit, nothing is written and the exit code is 2; `--force-disk` writes anyway.
- **Write failure:** a WAV file that cannot be written completely is removed and reported as `<name>.wav: FAILED, the file was removed: <reason>`. The last line then states that not every file was written, and the exit code is 2.

**When several masters match: the tick list**

When a rate and word length (or a name) match several masters, the tool does not decide for you; it lists the masters for you to tick:

```
--pick: '176400:24' fits 2 masters. Tick the ones to write:
> [ ]    6  176400 Hz 24-bit  master 2 "Original master"
  [ ]   12  176400 Hz 24-bit  master 3 "Original master"
Up / Down: move   Space: tick   Enter: done   Esc: cancel
Or type numbers or ranges (e.g. 6,12 or 6-12), then Enter to tick them:
Numbers:
```

- Each line shows the box, the number, the rate, the word length and the name of the master.
- **Ticking with the keys:** Up and Down move, Space ticks or unticks, and Enter writes the ticked masters. Esc cancels; nothing is written.
- **Typing below the list:** after `Numbers:`, type numbers or ranges and press Enter.
  - Forms: one number, `6`; several separated by commas, `6,12`; a range with a hyphen, `6-12`, meaning 6 to 12 inclusive; mixed, for example `6,8-12`.
  - Enter ticks the candidates whose numbers are in what you typed and unticks the others. Numbers that are not candidates are ignored.
  - A second Enter writes the ticked masters.
  - An error (for example `7-`) is shown, the line is cleared and you can type again; Backspace deletes one character.
- If you press Enter with nothing ticked, you are asked to tick at least one master.
- The tick list appears only in a console window. In a script (input or output redirected), the tool does not wait: it lists the candidates (with the same number, rate, word length and name), writes nothing and exits with code 1. Give them with `--pick=<n>`, `--pick=<n>,<n>` or `--pick=<a>-<b>`.

## Menu order: `--menu-order`

The order of the masters in the file is still chosen by the encoder (for the smallest file); the order in menus and selection is the one that you mark. The two orders are independent.

- `laes enc`: without an option, the menu order is the order in which the masters are given on the command line; for another order, use `--menu-order=3,1,2` (the numbers refer to the masters as given).
- If the menu order equals the file order, nothing is stored and the file can be version 1; otherwise, the menu order is stored and the file is version 2 (players from before 2026-11-11 play only the core master).
- The menu order does not choose the default; the default is set with `--default` (otherwise, the core master). `--list`, `--pick=master:<k>`, the decoded file names `master_<k>_…` and the k of `laes names` all use the menu numbers.

## Changing master names: `laes names`

```
laes names <in> <out> [--name=<k>:<text> ...] [--no-derive[=<k>|core]] [--derive[=<k>|core]] [--menu-order=<k1>,<k2>,...] [--default=<k>|core]
```

- Writes a new file without re-encoding; the old file is kept, and an existing `<out>` is not replaced silently (see "Text, file names and exit codes").
- k is the present number of the master in menus ("master k" in `laes dec --list`). `--name=<k>:` with an empty text removes the name; `--derive` turns derived versions back on; `--menu-order=` gives a new menu order by the present numbers; `--default=<k>` or `--default=core` sets the default; `--no-derive=core` or `--derive=core` turns the derived version of the core master off or on.
- `laes names <in> --in-place --default=<k>|core [--no-derive=core | --derive=core]` changes the file itself instead of writing a new one. Only these options are allowed, because they keep the size of the file: at most 7 bytes change (the payload version, the two header bytes and the CRC-32). The tool prints each changed byte with its old and new value, then decodes every master and checks its SHA-256; on any failure, it writes the old bytes back. The result is byte for byte the file that a direct encode with the same options writes.
- The layers, the core master and the tags are copied byte for byte; only the master records and the CRC-32 change. Afterwards, every master is checked against its SHA-256.
- With every name and flag removed and at most 16 masters, the file is payload version 1 again, byte for byte the file that a direct encode writes.

## Adding masters: `laes add`

```
laes add <in> <out> <new master>[@T] ... [--name=<k>:<text>] [--no-derive[=<k>]] [--menu-order=<k1>,<k2>,...]
```

- Writes a new file: the core master and the record and layers of every old master are copied byte for byte, without re-encoding. The new masters are placed at the end of the payload; each is coded against the core master and every master before it, and the smallest result is kept (as in `laes enc`).
- The k of `--name` and `--no-derive` refers to the new masters as given. By default, the new masters go to the end of the menu; `--menu-order=` gives the menu order of all masters: the old ones by their present numbers, the new ones numbered after them in the order given.
- The old masters are decoded once (they are the references of the new ones); afterwards, every master is checked against its SHA-256. Tags, pictures and the audio bytes of the core master stay as they are.
- A core master above 4 GiB (RF64) is read. Before it starts, `laes add` estimates the disk space it needs and says so when that is more than is free; it goes on.

## Extracting masters into a new file: `laes extract`

```
laes extract <in> <out> --pick=<p>[,<p> ...]      p = master:<k> | name:<text> | <rate>:<bits>
```

- Writes a new file with only the chosen masters (one or several). `<rate>:<bits>` and `name:` take the master that matches; if several match, you tick them in the same tick list as in `laes dec` (there, the numbers are the k of `master:<k>`). `master:<a>-<b>` takes masters a to b.
- The chosen masters are decoded and encoded again in full (with the same core master), each choosing its reference among the masters kept; nothing is cut out of the old payload.
- The order in `--pick` is the menu order of the new file; names and "no derived versions" settings go with their masters; tags, pictures and the audio bytes of the core master stay as they are. Afterwards, every master is checked against its SHA-256.
- Each master keeps its time position T exactly: it is passed to the encoder as a number, not as text (since 2026-11-15). Before it starts, `laes extract` estimates the disk space it needs and says so when that is more than is free; it goes on.
- To get WAV files without packing them, use `laes dec --pick=...`.

## Checking: `laes verify`

```
laes verify <in> <master.wav> [<master.wav> ...]
```

Decodes every master in the file and compares it bit for bit with the given files, which may be given in any order. For each file, it prints `IDENTICAL` or `DIFFERENT`; for masters without a corresponding file, it prints `not compared`. A given file that cannot be opened or that the WAV reader refuses is reported as `<file>: DIFFERENT (not compared: <reason>)`, and the files after it are still compared (since 2026-11-14). `laes verify` and `laes dec` name every master by its number in menus (the menu order of the file), the same number that `--pick=master:<k>` takes (since 2026-11-15). A FLAC file with ID3v2 tags in front or APEv2 / ID3v1 tags behind (tags of other programs) is accepted, as a master and as an LAES file; the tags are not read and stay in the file, and `verify`, `dec` and `parts` name them.

## Time position: `laes align`

```
laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]
```

For a core master that was produced independently, such as a released CD, this command measures the time position T of the master on the CD's time line, including the fractional part. It uses the automatic search of the LAE Lossy encoder (`lae_enc`, which is found next to `laes` or given with `--lae-enc`). A path that the shell would change is refused, as for `@auto`. An alignment error of −60 dB or lower passes; otherwise, the error is displayed and you decide, or `--accept` accepts it without asking. A check block where the master is silent and the core master is not counts as not matching (since 2026-11-15); the tool names it and asks whether to use the alignment. The command writes nothing; when the alignment is accepted, it prints `use <master>@<T>` for use with `laes enc` (exit code 0). A whole album is cut into tracks by the whole-disc planner of LAE Lossy (`lae_enc plan … laes`).

## Test helpers

| Command | What it does |
|---|---|
| `laes selftest` | Runs regression tests with synthetic signals; exit code 0 means that all tests passed. |
| `laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]` | Cuts an excerpt; 16 = TPDF to 16 bits, 24 = as is, 32 = random low byte. |
| `laes reduce <in.wav> <out.wav> <bits>` | Reduces the word length with TPDF dither and a fixed seed. |
| `laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]` | Creates a test "CD": 16 bits, the master's base level shifted by tau. |

## Text, file names and exit codes

- **Text:** on Windows, all arguments, file names, tags and console output are handled as UTF-8 within the tool. Tags and paths in any language work from cmd, PowerShell and scripts, regardless of the system code page. On other systems, the arguments are taken as given (UTF-8 on typical systems).
- **Exit codes:** 0: success; 1: invalid arguments; 2: the operation failed (a file could not be read or written, a check failed, or the disk was full). After exit code 2, `laes enc` and `laes tag` have not written `<out>` (a stopped `laes enc` may keep its work folder `<out>.laes.tmp` for resuming).
- **Existing output files:** an output file that exists is never replaced silently (`enc`, `dec`, `tag`, `add`, `extract`, `names`, and `laes_export`). On a console, the tool asks twice and replaces the file only after two yes answers; otherwise, it refuses with exit code 1 unless `--overwrite` is given. A resumed `laes enc` run is not a replacement. The exact prompts are given in [README.md](../../README.md), Section 14.11.
- **Test switches:** the test switches of `laes` and `laes_export` (`LAES_TEST_KEYS`, `LAES_NO_SHORTLIST`, `LAES_FORCE_RF64`, `LAES_PIECE`, `LAES_SELFCHECK_FLIP`, `LAES_DISK_RESERVE`) act only with `LAE_TEST_MODE=1`; a run with it prints one line naming the active switches. `KEEPTMP` (keep the work folder; no output byte changes) acts without it ([README.md](../../README.md), Section 14.11).
- **Messages** are in English; the disk and resume messages, the questions about an existing output and the note about a work folder in the system temp folder also include a line in Chinese.
