# LAE Noise Marker

[中文](README.zh.md)

LAE Noise Marker finds steady noise and fixed narrow lines in a master and lets you mark the parts you want removed. It writes a
marking file (`.laenoise`, see [SPEC_LAENOISE.md](SPEC_LAENOISE.md)). The LAE encoder reads that file and codes the marked cells
as silence.

**LAE Noise Marker only identifies and marks.** It never removes sound and has no export: no command and no button writes a
cleaned copy of your master. The previews for listening are at most 10 seconds per position. If you connect the marking file to
other software that does remove sound, that is your own choice and responsibility.

## What is in the package

| File | What it does |
|---|---|
| `lae_noise` (`lae_noise.exe` on Windows) | The command-line program: `scan`, `spectro`, `preview`, `preview-all`, `mark`, `show`, `level` |
| `noise_panel.html` | The panel: opens in any current browser (Chrome, Edge, Firefox, Safari), offline, Chinese and English |
| `SPEC_LAENOISE.md` / `.zh.md` | The marking file format |
| `LICENSE` | Apache-2.0 |

The program is standard C++17 for Windows, Linux and macOS. It is meant to give the same candidates and the same marking file, byte for
byte, on every platform; this is checked on Windows x64 and x86, Linux x64 (GCC, Clang, 32-bit) and Linux ARM64 (GCC, Clang).

## Before you start

- **Align first.** Make the marks on the master exactly as the encoder will receive it. For LAE: run `lae_enc plan`, confirm the
  alignment, and mark the aligned master. The marking file names the master by its SHA-256, so marks made on any other version
  of the file are refused.
- **Input:** a stereo WAV, PCM 16/24/32 bit or 32-bit float.

## Steps

**1. Scan.** Lists every steady noise and every fixed narrow line that lasts 10 s or more. Short bursts are not listed.

```
lae_noise scan master.wav scan.txt candidates.laenoise
```

`scan.txt` is the report; `candidates.laenoise` holds the same candidates as a marking file. Nothing is marked yet: a candidate
is only a suggestion.

**2. Make the display data and the previews.**

```
lae_noise spectro master.wav spectro.json
lae_noise preview master.wav candidates.laenoise previews
```

For every candidate, `previews` gets three files: `pos_NNN_original.wav` (that span unchanged), `pos_NNN_silenced.wav` (the span
with the marked cells silenced) and `pos_NNN_marked.wav` (only what would be silenced). `preview_check.txt` confirms that
nothing outside the marked cells changes.

**3. Choose in the panel.** Open `noise_panel.html` and load `spectro.json`, `candidates.laenoise` and the `previews` folder.
- Every candidate starts unchecked; check the ones you want removed.
- To add a region by hand, drag a rectangle on the spectrogram or type the times and frequencies. It snaps inward to whole cells.
- Listen with the three buttons of each row.
- **Save marking file** writes only the checked positions.

The panel never processes sound; it only plays the preview files.

**4. Check the file.**

```
lae_noise show master.wav master.laenoise
```

This lists the positions and confirms that the file belongs to this master.

**5. Encode.**

```
lae_enc master.wav out.wav 3 noise=master.laenoise
```

The encoder codes the marked cells of its frequency layer (the ultrasonic band above the CD layer) as silence. Marks in the CD
band are listed by the encoder and left as they are. With the optional `g` (also `g=b`, `g=c`, `g=d`) the encoder also removes
the CD layer's upsampling images left in those cells. That makes the file larger, and the encoder prints how much.

**6. Measure (optional).**

```
lae_noise level decoded.wav master.laenoise
```

This gives the level of the marked cells in any file of the same length: the master, a decode, a copy.

## Without the panel

`lae_noise mark master.wav out.laenoise <t0 s> <t1 s> <f0 Hz> <f1 Hz> [...]` writes a marking file from numbers. Each range
snaps inward to whole cells, and the snapped range is printed.

## Building

The program is standard C++17 with no other libraries. On MSVC use `/fp:strict`; on GCC or Clang use `-ffp-contract=off`; on 32-bit
x86 use SSE2. Build `src/lae_noise.cpp`; it includes the scanner.

License: this text CC-BY-4.0; the program and the panel Apache-2.0.
