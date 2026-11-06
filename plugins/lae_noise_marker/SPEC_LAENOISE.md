# LAENOISE 1: the marking file of LAE Noise Marker

Version 1, 2026-11-06. [中文](SPEC_LAENOISE.zh.md)

A marking file names rectangles of time and frequency in one master. It holds geometry only: no detector, reason, class or
audio. LAE Noise Marker writes it (the scanner writes candidates in the same format; the user keeps a subset). The LAE encoder
reads it with `noise=<file>` and codes the marked cells as silence. Any other program may read it under the same rules.

## 1. File

- Plain text, ASCII, lines end with LF (a CR before the LF is ignored).
- File name extension `.laenoise`.
- `#` starts a comment that runs to the end of the line. Comments are for people only: a reader must not take any value from them.
- Blank lines are allowed. A reader ignores lines it does not recognise.

## 2. Lines

The lines appear in this order:

```
LAENOISE 1
master sha256 <64 hex digits> frames <L> rate <fs>
grid mdct 512 cell 8
pos <m0> <m1> <c0> <c1>
pos ...
```

| Line | Meaning |
|---|---|
| `LAENOISE 1` | Magic and version. A file without it is not a marking file. |
| `master sha256 … frames … rate …` | The master the marks belong to: the SHA-256 of the bytes of its WAV `data` chunk (the PCM samples exactly as stored, nothing else), its length `L` in sample frames, and its sample rate `fs` in Hz. |
| `grid mdct 512 cell 8` | The grid. Version 1 has only this one; a reader refuses any other value. |
| `pos m0 m1 c0 c1` | One rectangle: frames `m0` ≤ m < `m1`, columns `c0` ≤ c < `c1`. Zero or more lines. |

## 3. The grid

The grid is that of the LAE encoder's frequency layer, at the master's own sample rate.

- **Planes.** M = r + ((l − r) >> 1) and S = l − r, with l and r the left and right samples as 24-bit integers. A rectangle covers
  M and S together.
- **Frames.** An MDCT of N = 512 with a sine window. Frame m covers the samples [mN − N, mN + N). Its own hop is
  [mN − N/2, mN + N/2), which begins at time (mN − N/2) / fs. There are K = ⌊(L + N − 1) / N⌋ + 1 frames, m = 0 … K − 1.
- **Columns.** A column is 8 MDCT bins: column c holds bins 8c … 8c + 7, that is from c · 8 · fs / (2N) Hz up to
  (c + 1) · 8 · fs / (2N) Hz. There are 64 columns, c = 0 … 63.
- **Cell.** One frame × one column. At 352.8 kHz a cell is 2 756.25 Hz × 1.45 ms; at 44.1 kHz it is 344.5 Hz × 11.6 ms.

## 4. Rules for a valid file

A reader accepts the file only if all of these hold:

1. The magic line is present, and the `master` line and the `grid` line are both present.
2. The grid is `mdct 512 cell 8`.
3. `frames` equals the master's length and `rate` equals its sample rate.
4. The SHA-256 equals that of the master's `data` chunk.
5. Every `pos` has 0 ≤ m0 < m1 ≤ K and 0 ≤ c0 < c1 ≤ 64.

If any rule fails, the reader uses none of the file. Rectangles may overlap: the marked area is their union.

## 5. Turning times and frequencies into cells

LAE Noise Marker snaps a range **inward**, so a rectangle never reaches outside what the user chose:

- Time [t0, t1) s → the frames whose hop lies inside it: m0 = ⌈(t0 · fs + N/2) / N⌉ (at least 0), m1 = ⌊(t1 · fs − N/2) / N⌋ + 1
  (at most K).
- Frequency [f0, f1) Hz → the columns whose 8 bins lie inside it: c0 = ⌈f0 / w⌉, c1 = ⌊f1 / w⌋, w = 8 · fs / (2N) (c1 at most 64).

## 6. Example

```
LAENOISE 1
master sha256 de8b7ca98ccaa8331093c708a7777eacde569b41808487671b33dbec59b136e7 frames 5292000 rate 352800
grid mdct 512 cell 8   # frame m = samples [mN-N, mN+N), its hop [mN-N/2, mN+N/2); column c = bins 8c..8c+7
pos 1999 9027 10 11   # 1: 2.900-13.100 s (10.20 s), 27562.5-30318.8 Hz
```

One rectangle: 2.900–13.100 s, 27 562.5–30 318.8 Hz, in a 15-s master at 352.8 kHz.

## 7. What the format does not do

- It does not hold or change audio. Removing what it marks is the job of the program that reads it. In LAE that is the encoder,
  which codes the marked cells of its frequency layer as silence.
- It does not say why a rectangle was marked.

License: this text CC-BY-4.0; the program and the panel Apache-2.0.
