# Noise marking, `noise=` and `g`

[中文](NOISE.zh.md)

LAEY can leave out noise that you mark in the ultrasonic band of a master. Three separate steps do this, each with its own tool:

1. **Align.** `lae_enc plan` aligns the master with the CD layer; you confirm the result. Make all marks on the aligned master.
2. **Mark.** LAE Noise Marker (`lae_noise`, `tools/noise_panel.html`; see `plugins/lae_noise_marker/`) lists steady noise and fixed
   narrow lines and lets you choose. It writes a marking file (`.laenoise`). It never removes or exports sound.
3. **Encode.** `lae_enc <master.wav> <out.wav> [mode] noise=<marks.laenoise> [g | g=b | g=c | g=d]`.

Without `noise=` and `g` the encoder writes exactly the same file as before, byte for byte.

## `noise=`

- The encoder checks that the marking file belongs to this master: SHA-256, length and sample rate. If the master was moved
  during alignment, it stops and writes nothing; mark the aligned master instead.
- In the **frequency layer** the master is taken as zero in the marked cells. A cell marked in all 8 of its bins becomes tier A
  (coded as silence).
- With `noise=`, the frequency layer starts at the CD cut-off − 1 kHz, rounded down to a multiple of 8 bins (at most 7 bins lower
  than without it), so that its cells line up with the marked columns.
- **Marks below that point lie in the CD layer.** The encoder lists them and leaves them as they are. The CD layer and the depth
  layer do not change.

## G: what stays in a silent cell

**G = Ground Noise of Imaging.** "Ground" means the floor: a very faint layer at the very bottom of the high-resolution sound.
The usual terms are *interpolation images* or *image components*.

- A decoder plays the high-resolution version as the CD layer up-sampled to the master's rate, plus the frequency layer.
- Up-sampling keeps the original samples and the 0–20 kHz content unchanged. It computes the new samples in between with a filter
  that removes the spectral copies of the CD band. The filter's stopband is 120 dB, so each copy keeps about −120 dB of the music.
  That remainder is G.
- Normally the frequency layer holds "master − up-sampled CD", so it carries −G and the two cancel when decoding.
- In a tier-A cell the frequency layer is zero, so G stays in the decoded output. This happens in two kinds of cell:
  - cells you marked and deleted with `noise=`;
  - cells where the master itself is silent, for example the digital silence between synthesised notes.

Measured on a 352.8 kHz master, in dBFS per column of cells: the deleted noise was −109.5, the G left after deletion −131.5, and
the 24-bit rounding floor of the output is −164.4. So a deleted cell is far below the noise it replaced, but not yet digital
silence.

Some masters have an ultrasonic band that *is* the up-sampled CD. On such a master, `noise=` alone measures no change, because
what remains (G) is exactly what was there. Deleting it there needs `g`.

## `g`

`g` exists only for G. The encoder codes the frequency layer **twice**:

1. The first pass is the normal one: tiers A / B / C / D exactly as without `g`.
2. The second pass codes only −G, and only in first-pass A cells of two kinds:
   - cells you deleted (fully marked);
   - cells where the master itself is silent (at or below the 24-bit rounding floor).

   Cells whose G is below the 24-bit floor are skipped. A cells whose master is not silent are left as they are: their G is the
   master's own content.

The second pass's streams are added to the same frequency layer. Every decoder sums them with the first pass coefficient by
coefficient (spec Section 9.5), so G cancels. Existing decoders play such files without any change.

### Three strengths

| Option | Step of the second pass | Marked cells after `g` (demo master, dBFS per column) |
|---|---|---|
| `g=b` | block peak − 2 bit | −126.4 |
| `g=c` | block peak − 4 bit | −139.0 |
| `g=d` (or `g`) | 1 | −156.2 |

With `noise=` alone the same cells measured −104.0 (on that master they hold G only).

### What `g` costs

Measured on the LAE demo master at 352.8 kHz, mode 3, 90 s with one marked rectangle of 10 s × 2.76 kHz:

| | Whole file | Extension layers only (depth + frequency) |
|---|---|---|
| `noise=` | 22.85 MB | 6.98 MB |
| `noise= g=b` | +0.8 % | +2.7 % |
| `noise= g=c` | +1.0 % | +3.2 % |
| `noise= g=d` | +1.2 % | +3.9 % |
| `g` alone (75 s of synthesised notes with silence between them) | +5.2 % | +15.7 % |

- The cost grows with the marked area and with the amount of silence in the master, not with the length of the piece.
- `g` alone gave no measurable change against the master on that excerpt. We recommend `g` when you have marked cells.
- After every encode with `g`, the encoder prints how many MB the G streams take. That is what you would save without `g`.
- Cells that are neither deleted nor silent: unchanged. CD layer: unchanged. Encoding time: about the same (73–79 s for the 90 s excerpt with or without `g`).

## Disk space while encoding

The encoder keeps its intermediate files in `<out>.tmp` and removes them at the end. At 352.8 kHz they take about 0.8 GB per
minute of music, about 15 % more with `g`. Before it starts, the encoder checks the free space (including what `g` adds) and stops
without writing anything if it is short. At the end it prints the largest size the work folder reached.

## What does not change

- The file format and the decoders.
- The CD layer.
- Files encoded without `noise=` and `g`.
