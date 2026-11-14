# LAE Noise Marker

[中文](README.zh.md)

LAE Noise Marker detects steady noise and fixed narrow lines in a master and lets the user mark the parts to be removed. It writes a
marking file (`.laenoise`; see [SPEC_LAENOISE.md](SPEC_LAENOISE.md)). The LAE Lossy (LAEY) encoder reads this file and codes the
marked cells of its frequency layer as silence (a cell is one MDCT frame × 8 bins of that layer's grid; see
[SPEC_LAENOISE.md](SPEC_LAENOISE.md), Section 3). Marks in the CD band are only listed. Noise marking applies to LAEY only.

**LAE Noise Marker only identifies and marks.** It never removes sound and has no export function: no command or button writes a
cleaned copy of the master. No command writes over the master: since 2026-11-14, `scan`, `mark`, `spectro`, `preview` and `preview-all` refuse an output
that is the master file itself, and the master stays unchanged. The program limits every listening preview: `preview` covers the first 10 seconds of each position by default and at most the first 30 seconds, with at most 1 second of context on each side, and `preview-all` at most 30 seconds; a longer request is cut to these limits, and the program says so. An output file that already exists is not replaced silently (since 2026-11-15): on a console the program asks twice and replaces it only after two yes answers; otherwise it is refused unless `--overwrite` is given. If the user connects the marking file to
other software that does remove sound, that is the user's own choice and responsibility.

## What is in the package

| File | What it does |
|---|---|
| `lae_noise` (`lae_noise.exe` on Windows) | Command-line program with the subcommands `scan`, `spectro`, `preview`, `preview-all`, `mark`, `show` and `level` |
| `noise_panel.html` | Panel; opens offline in any current browser (Chrome, Edge, Firefox, Safari); available in Chinese and English |
| `SPEC_LAENOISE.md` / `.zh.md` | Specification of the marking file format |
| `LICENSE` | Apache-2.0 |

The program is written in standard C++17 for Windows, Linux and macOS (macOS has not been checked). It is intended to produce the same candidates and the same
marking file, byte for byte, on every platform; this is checked on Windows x64 and x86, Linux x86-64 (GCC, Clang), Linux x86 32-bit and
Linux ARM64 (GCC, Clang).

## Before you start

- **Align first.** Make the marks on the master exactly as the encoder will receive it. For LAE, run `lae_enc plan`, confirm the
  alignment and mark the aligned master. The marking file identifies the master by its SHA-256 hash, so marks made on any other
  version of the file are refused.
- **Input:** a stereo WAV file, 16-, 24- or 32-bit PCM, or 32-bit float.

## Steps

**1. Scan.** The `scan` command lists every steady noise and every fixed narrow line that lasts 10 s or more. Short bursts are not
listed.

```
lae_noise scan master.wav scan.txt candidates.laenoise
```

`scan.txt` is the report; `candidates.laenoise` contains the same candidates as a marking file. Nothing is marked at this stage: a
candidate is only a suggestion.

**2. Generate the display data and the previews.**

```
lae_noise spectro master.wav spectro.json
lae_noise preview master.wav candidates.laenoise previews
```

For every candidate, three files are written to `previews`: `pos_NNN_original.wav` (the span unchanged), `pos_NNN_silenced.wav`
(the span with the cells of that candidate silenced) and `pos_NNN_marked.wav` (only the content that would be silenced). Each preview
covers the first 10 s of the span by default (at most 30 s), with 1 s of context on each side. `preview_check.txt`, written to the same folder,
confirms that nothing outside those cells is changed. Since 2026-11-14, `lae_noise preview` also writes `preview_list.txt`: the
master's SHA-256, length and sample rate, and for each `pos_NNN` its rectangle in cells as in the marking file (geometry only; the
encoder does not read it). `lae_noise preview-all master.wav <file.laenoise> <folder> <t0 s> <t1 s>` writes
one excerpt of at most 30 s with every position silenced together (`all_original.wav`, `all_silenced.wav`, `all_marked.wav`), with the same check.

**3. Select in the panel.** Open `noise_panel.html` and load `spectro.json`, `candidates.laenoise` and the `previews` folder.
- Every candidate is initially unchecked; check those to be removed.
- To add a region manually, drag a rectangle on the spectrogram or enter the times and frequencies. The region snaps inward to
  whole cells.
- Use the three buttons in each row to listen to the previews. With `preview_list.txt`, the panel checks the master by its SHA-256,
  length and sample rate and gives each row the preview of exactly its rectangle. A folder written by an older `lae_noise` has only
  `preview_check.txt`; the panel then matches the master by its file name and each preview by its rectangle in rounded times and
  frequencies. A row without a matching preview has its buttons greyed out. For a region added by hand, save first, run
  `lae_noise preview` on the saved file and load that folder. A preview folder of another master, or one with neither file, plays
  nothing, and the panel says why.
- **Save marking file** writes only the checked positions, for example as `master.laenoise`.

The panel never processes sound; it only plays the preview files.

**4. Check the file.**

```
lae_noise show master.wav master.laenoise
```

This command lists the positions and confirms that the file belongs to this master.

**5. Encode.**

```
lae_enc master.wav out.wav 3 noise=master.laenoise
```

Here `3` is the encoder mode ([README](../../README.md), Section 11); `g` is added as a further argument, for example
`lae_enc master.wav out.wav 3 noise=master.laenoise g`. The encoder codes the marked cells in its frequency layer (the ultrasonic
band above the CD layer) as silence. Marks in the CD band (below the start of the frequency layer) are listed by the encoder; the
audio there is not changed. With the optional `g` (or `g=b`, `g=c`, `g=d`), the encoder also removes the
upsampling images of the CD layer that remain in those cells. This makes the file larger, and the encoder prints the increase.

**6. Measure (optional).**

```
lae_noise level decoded.wav master.laenoise
```

This command reports the level of the marked cells in any file of the same length: the master, a decoded file or a copy. Unlike
the encoder and `show`, it checks only the length and the rate, not the SHA-256, so that it can measure a decoded file.

## Without the panel

`lae_noise mark master.wav out.laenoise <t0 s> <t1 s> <f0 Hz> <f1 Hz> [...]` writes a marking file from numerical values. Each
range snaps inward to whole cells, and the snapped range is printed.

## Building

The program is written in standard C++17 and requires no other libraries. With MSVC, use `/fp:strict`; with GCC or Clang, use
`-ffp-contract=off`; on 32-bit x86, use SSE2. Build `src/lae_noise.cpp` of the LAE repository, which includes the scanner.

Licence: this text, CC-BY-4.0; the program and the panel, Apache-2.0.
