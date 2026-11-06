# LAENOISE 1: the marking file of LAE Noise Marker

Version 1, 2026-11-06. [中文](SPEC_LAENOISE.zh.md)

A marking file identifies rectangles of time and frequency in one master. It holds geometry only: it contains no detector, reason, class or audio. LAE Noise Marker writes the file. The scanner writes candidates in the same format, and the user keeps a subset of them. The LAE Lossy (LAEY) encoder reads the file when given `noise=<file>` and codes the marked cells as silence. Any other program may read the file under the same rules.

## 1. File

- The file is plain ASCII text. Lines end with LF; a CR before the LF is ignored.
- The file name extension is `.laenoise`.
- `#` starts a comment that runs to the end of the line. Comments are for human readers only: a reader must not take any value from them.
- Blank lines are allowed. A reader ignores lines that it does not recognise.

## 2. Lines

LAE Noise Marker writes the lines in the following order:

```
LAENOISE 1
master sha256 <64 hex digits> frames <L> rate <fs>
grid mdct 512 cell 8
pos <m0> <m1> <c0> <c1>
pos ...
```

| Line | Meaning |
|---|---|
| `LAENOISE 1` | Magic line and version. A file without this line is not a marking file. |
| `master sha256 … frames … rate …` | The master to which the marks belong: the SHA-256 of the bytes of its WAV `data` chunk (the PCM samples exactly as stored, and nothing else), its length `L` in sample frames, and its sample rate `fs` in Hz. |
| `grid mdct 512 cell 8` | The grid. Version 1 defines only this grid; a reader refuses any other value. |
| `pos m0 m1 c0 c1` | One rectangle: frames `m0` ≤ m < `m1` and columns `c0` ≤ c < `c1`. The file contains zero or more such lines. |

## 3. The grid

The grid is that of the frequency layer of the LAE Lossy (LAEY) encoder, at the master's own sample rate.

- **Planes.** M = r + ((l − r) >> 1) and S = l − r, where l and r are the left and right samples as 24-bit integers (the master is stereo; samples of another word length are scaled to 24 bits). A rectangle covers M and S together.
- **Frames.** The transform is an MDCT with N = 512 and a sine window. Frame m covers the samples [mN − N, mN + N). Its own hop is [mN − N/2, mN + N/2), which begins at time (mN − N/2) / fs. There are K = ⌊(L + N − 1) / N⌋ + 1 frames, m = 0 … K − 1.
- **Columns.** A column consists of 8 MDCT bins: column c holds bins 8c … 8c + 7, that is, from c · 8 · fs / (2N) Hz up to (c + 1) · 8 · fs / (2N) Hz. There are 64 columns, c = 0 … 63.
- **Cells.** A cell is one frame × one column. At 352.8 kHz a cell is 2 756.25 Hz × 1.45 ms; at 44.1 kHz it is 344.5 Hz × 11.6 ms.

## 4. Rules for a valid file

A reader accepts the file only if all of the following hold:

1. The magic line, the `master` line and the `grid` line are all present.
2. The grid is `mdct 512 cell 8`.
3. `frames` equals the length of the master, and `rate` equals its sample rate.
4. The SHA-256 equals that of the master's `data` chunk.
5. Every `pos` line satisfies 0 ≤ m0 < m1 ≤ K and 0 ≤ c0 < c1 ≤ 64.

If any rule fails, the reader uses none of the file. Rectangles may overlap; the marked area is their union.

## 5. Converting times and frequencies into cells

LAE Noise Marker snaps a range **inward**, so that a rectangle never extends beyond the range the user chose:

- Time [t0, t1) s → the frames whose hop lies inside the range: m0 = ⌈(t0 · fs + N/2) / N⌉ (at least 0) and m1 = ⌊(t1 · fs − N/2) / N⌋ + 1 (at most K).
- Frequency [f0, f1) Hz → the columns whose 8 bins lie inside the range: c0 = ⌈f0 / w⌉ (at least 0) and c1 = ⌊f1 / w⌋ (at most 64), where w = 8 · fs / (2N).
- A range that holds no whole cell (m0 ≥ m1 or c0 ≥ c1) gives no rectangle: LAE Noise Marker skips it.

## 6. Example

```
LAENOISE 1
master sha256 de8b7ca98ccaa8331093c708a7777eacde569b41808487671b33dbec59b136e7 frames 5292000 rate 352800
grid mdct 512 cell 8   # frame m = samples [mN-N, mN+N), its hop [mN-N/2, mN+N/2); column c = bins 8c..8c+7
pos 1999 9027 10 11   # 1: 2.900-13.100 s (10.20 s), 27562.5-30318.8 Hz
```

The file marks one rectangle, 2.900–13.100 s and 27 562.5–30 318.8 Hz, in a 15 s master at 352.8 kHz.

## 7. What the format does not do

- The format does not hold or change audio. Removing what it marks is the task of the program that reads it. In LAE, that program is the encoder, which codes the marked cells of its frequency layer as silence.
- The format does not record why a rectangle was marked.

Licence: this text CC-BY-4.0; the program `lae_noise` and the panel `noise_panel.html` Apache-2.0.
